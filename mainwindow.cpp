#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QString>

#include <chrono>
#include <string>
#include <utility>

#include "Pipeline.h"
#include "videowidget.h"

Q_LOGGING_CATEGORY(lcApp, "videodecoder.app")

namespace {

constexpr int kFrameIntervalMs = 16;   // ≈60Hz：上屏节流，天然"只保留最新帧"
constexpr int kStatusIntervalMs = 500;

/// 适配器：库的 VideoFrame（FFmpeg/NV12）→ 渲染控件的纯数据视图。
/// 这层耦合是刻意的：只有应用层同时认识库与控件。
videoframe::Nv12FrameView toNv12View(const avframe::VideoFrame& frame)
{
    videoframe::Nv12FrameView view;
    const AVFrame* raw = frame.frame.get();
    if (raw == nullptr || raw->data[0] == nullptr || raw->data[1] == nullptr) {
        return view;  // 返回无效视图，控件会忽略
    }
    view.width = frame.width;
    view.height = frame.height;
    view.y = videoframe::Nv12Plane{raw->data[0], raw->linesize[0]};
    view.uv = videoframe::Nv12Plane{raw->data[1], raw->linesize[1]};
    view.ptsSeconds = frame.ptsSeconds;
    return view;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), m_ui(new Ui::MainWindow)
{
    m_ui->setupUi(this);

    m_labelResolution = new QLabel(this);
    m_labelFps = new QLabel(this);
    m_labelLatency = new QLabel(this);
    statusBar()->addPermanentWidget(m_labelResolution);
    statusBar()->addPermanentWidget(m_labelFps);
    statusBar()->addPermanentWidget(m_labelLatency);
    statusBar()->showMessage(QStringLiteral("就绪"));

    m_pipeline = std::make_unique<Pipeline>();
    bindPipelineCallbacks();

    connect(m_ui->btnConnect, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(m_ui->btnDisconnect, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);
    connect(m_ui->editUrl, &QLineEdit::returnPressed, this, &MainWindow::onConnectClicked);

    m_frameTimer = new QTimer(this);
    m_frameTimer->setInterval(kFrameIntervalMs);
    connect(m_frameTimer, &QTimer::timeout, this, &MainWindow::onFrameTimerTick);
    m_frameTimer->start();

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusIntervalMs);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::onStatusTimerTick);
    m_statusTimer->start();

    setConnectedUi(false);
    updateStatusText();
}

MainWindow::~MainWindow()
{
    m_frameTimer->stop();
    m_statusTimer->stop();

    // 必须先停管线：保证没有任何库线程还会进入回调，再销毁 UI（含渲染控件）。
    stopPipeline();
    delete m_ui;
}

void MainWindow::bindPipelineCallbacks()
{
    // 帧回调运行在解码线程：只写"最新帧槽"，绝不触碰 GUI 对象。
    m_pipeline->setFrameCallback([this](avframe::VideoFramePtr frame) {
        std::lock_guard lock(m_frameMutex);
        if (m_latestFrame) {
            m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        }
        m_latestFrame = std::move(frame);
    });

    // 状态回调同样在库线程：用队列投递回 GUI 线程后再更新界面。
    m_pipeline->setStatusCallback([this](std::string message) {
        const QString text = QString::fromStdString(message);
        QMetaObject::invokeMethod(
            this, [this, text] { handleStatusMessage(text); }, Qt::QueuedConnection);
    });
}

void MainWindow::openUrl(const QString& url)
{
    m_ui->editUrl->setText(url);
    // 让窗口先显示出来，再开始连接（连接与流探测会阻塞 GUI 线程）。
    QTimer::singleShot(0, this, &MainWindow::onConnectClicked);
}

void MainWindow::onConnectClicked()
{
    const QString url = m_ui->editUrl->text().trimmed();
    if (url.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("请输入地址或选择本地视频文件"));
        return;
    }

    stopPipeline();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::string error;
    const bool started = m_pipeline->start(url.toStdString(), &error);
    QApplication::restoreOverrideCursor();

    if (!started) {
        const QString message = QString::fromStdString(error);
        qCWarning(lcApp) << "启动失败:" << message;
        statusBar()->showMessage(QStringLiteral("启动失败: %1").arg(message), 8000);
        QMessageBox::warning(this, QStringLiteral("启动失败"), message);
        setConnectedUi(false);
        return;
    }

    m_ui->videoWidget->clearFrame();
    {
        std::lock_guard lock(m_frameMutex);
        m_latestFrame.reset();
    }
    m_droppedFrames.store(0, std::memory_order_relaxed);
    m_lastLatencyMs = 0;
    m_streamActive = true;

    setConnectedUi(true);
    statusBar()->showMessage(QStringLiteral("已连接: %1").arg(url), 3000);
    qCInfo(lcApp) << "已连接" << url << "codec" << QString::fromStdString(m_pipeline->codecName());
}

void MainWindow::onDisconnectClicked()
{
    stopPipeline();
    statusBar()->showMessage(QStringLiteral("已断开"), 3000);
}

void MainWindow::onFrameTimerTick()
{
    avframe::VideoFramePtr frame;
    {
        std::lock_guard lock(m_frameMutex);
        frame = std::move(m_latestFrame);
    }
    if (!frame) {
        return;  // 没有新帧：不做任何事（不重复上传同一帧）
    }

    // 端到端延迟：编码包进入管线 → 即将上屏。
    const auto now = std::chrono::steady_clock::now();
    m_lastLatencyMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - frame->recvAt).count());

    // 交给渲染控件：视图描述像素，保活句柄保证上传前内存有效。
    m_ui->videoWidget->onFrameReady(toNv12View(*frame), std::static_pointer_cast<const void>(frame));
}

void MainWindow::onStatusTimerTick()
{
    // 上游结束/出错后不能立刻收尾：解码线程还要把残留帧回调完。
    // isRunning() 转 false 说明管线彻底收工，此时再停线程 + 清屏 + 恢复按钮。
    if (m_streamActive && m_pipeline && !m_pipeline->isRunning()) {
        stopPipeline();
    }
    updateStatusText();
}

void MainWindow::handleStatusMessage(const QString& message)
{
    // 库上报的"流已结束/出错"只是信息：此时解码线程可能还在回调残留帧，
    // 因此这里只提示，真正的收尾交给状态定时器在 isRunning() 转 false 后统一处理。
    qCInfo(lcApp) << "库状态:" << message;
    statusBar()->showMessage(message, 8000);
}

void MainWindow::stopPipeline()
{
    m_streamActive = false;
    if (m_pipeline) {
        m_pipeline->stop();  // 幂等：内部先解阻塞再 join，500ms 内返回
    }
    if (m_ui != nullptr && m_ui->videoWidget != nullptr) {
        m_ui->videoWidget->clearFrame();
    }
    {
        std::lock_guard lock(m_frameMutex);
        m_latestFrame.reset();
    }
    setConnectedUi(false);
    updateStatusText();
}

void MainWindow::setConnectedUi(bool connected)
{
    m_ui->btnConnect->setEnabled(!connected);
    m_ui->btnDisconnect->setEnabled(connected);
    m_ui->editUrl->setEnabled(!connected);
}

void MainWindow::updateStatusText()
{
    const int videoWidth = m_ui->videoWidget->videoWidth();
    const int videoHeight = m_ui->videoWidget->videoHeight();

    m_labelResolution->setText(videoWidth > 0 && videoHeight > 0
                                   ? QStringLiteral("分辨率 %1x%2").arg(videoWidth).arg(videoHeight)
                                   : QStringLiteral("分辨率 -"));
    m_labelFps->setText(QStringLiteral("上屏 %1 fps / 丢 %2")
                            .arg(m_ui->videoWidget->renderFps(), 0, 'f', 1)
                            .arg(m_droppedFrames.load(std::memory_order_relaxed)));
    m_labelLatency->setText(m_lastLatencyMs > 0 ? QStringLiteral("延迟 %1 ms").arg(m_lastLatencyMs)
                                                : QStringLiteral("延迟 -"));
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    stopPipeline();
    QMainWindow::closeEvent(event);
}
