#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QOpenGLWidget>  // frameSwapped()：上屏节拍信号
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

/// 补漏/启动周期：呈现节拍由控件的 frameSwapped()（≈vsync）决定，本定时器只负责
/// "队列里有帧、控件却空闲"时把链条重新启动起来，因此它只影响启动延迟（≤4ms），
/// 不影响上屏节拍。相比原来的 16ms 固定节拍，去掉了与 60Hz(16.67ms) 的拍频。
constexpr int kPaceIntervalMs = 4;
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
    m_labelDrops = new QLabel(this);
    m_labelLatency = new QLabel(this);
    statusBar()->addPermanentWidget(m_labelResolution);
    statusBar()->addPermanentWidget(m_labelFps);
    statusBar()->addPermanentWidget(m_labelDrops);
    statusBar()->addPermanentWidget(m_labelLatency);
    statusBar()->showMessage(QStringLiteral("就绪"));

    m_pipeline = std::make_unique<Pipeline>();
    bindPipelineCallbacks();

    connect(m_ui->btnConnect, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(m_ui->btnDisconnect, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);
    connect(m_ui->editUrl, &QLineEdit::returnPressed, this, &MainWindow::onConnectClicked);

    // 上屏节拍 = 控件的 frameSwapped()（一帧真正完成上屏后立刻喂下一帧），
    // 因此不需要"猜"上屏时刻的固定周期定时器；本定时器只做补漏（见 kPaceIntervalMs）。
    m_paceTimer = new QTimer(this);
    m_paceTimer->setTimerType(Qt::PreciseTimer);
    m_paceTimer->setInterval(kPaceIntervalMs);
    connect(m_paceTimer, &QTimer::timeout, this, &MainWindow::onPaceTimerTick);
    connect(m_ui->videoWidget, &QOpenGLWidget::frameSwapped, this, &MainWindow::onFrameSwapped);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusIntervalMs);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::onStatusTimerTick);
    m_statusTimer->start();

    m_arrivalClock.start();  // 到达速率基准（构造期 updateStatusText() 会立刻读它）

    setConnectedUi(false);
    updateStatusText();
}

MainWindow::~MainWindow()
{
    m_paceTimer->stop();
    m_statusTimer->stop();

    // 必须先停管线：保证没有任何库线程还会进入回调，再销毁 UI（含渲染控件）。
    stopPipeline();
    delete m_ui;
}

void MainWindow::bindPipelineCallbacks()
{
    // 帧回调运行在解码线程：只入队（队列自带锁、不阻塞、不丢弃上游包），绝不触碰 GUI 对象。
    // 为什么不是"单槽覆盖"：实测带音轨的 RTSP 流会把视频帧成簇投递（两帧贴脸 + 空档），
    // 单槽会把同一周期内到达的第 2 帧直接覆盖丢掉；队列则把它们摊到相邻两次上屏。
    m_pipeline->setFrameCallback([this](avframe::VideoFramePtr frame) {
        m_frames.push(std::move(frame));
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
    m_frames.clear();        // 新会话：丢弃上一路残留的帧
    m_frames.resetStats();   // 计数从零开始（丢帧语义拆开统计的前提）
    m_lastArrivedFrames = 0;
    m_arrivalFps = 0.0;
    m_arrivalClock.restart();
    m_lastLatencyMs = 0;
    m_streamActive = true;
    m_paceTimer->start();  // 只在连接期间补漏，空闲时零唤醒

    setConnectedUi(true);
    statusBar()->showMessage(QStringLiteral("已连接: %1").arg(url), 3000);
    qCInfo(lcApp) << "已连接" << url << "codec" << QString::fromStdString(m_pipeline->codecName());
}

void MainWindow::onDisconnectClicked()
{
    stopPipeline();
    statusBar()->showMessage(QStringLiteral("已断开"), 3000);
}

void MainWindow::onFrameSwapped()
{
    // vsync 节拍：控件刚完成一次上屏（swap），此刻喂下一帧能拿到完整的刷新周期，
    // 既不会像固定 16ms 定时器那样与 60Hz 拍频空转，也不会连喂两帧导致覆盖丢帧。
    presentNextFrame();
}

void MainWindow::onPaceTimerTick()
{
    // 补漏：队列被取空后 frameSwapped 链条就停了，需要"下一帧到达"重新启动。
    // 这里只做补漏，不决定上屏节拍（节拍由 frameSwapped 决定）。
    presentNextFrame();
}

void MainWindow::presentNextFrame()
{
    // 控件里还有一帧没上传完：此刻再喂会被覆盖，正是要消灭的丢帧来源。
    if (m_ui->videoWidget->framePending()) {
        return;
    }

    avframe::VideoFramePtr frame = m_frames.popForDisplay();
    if (!frame) {
        return;  // 队列为空：不重复上传同一帧，等下一帧到达再补漏
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
    if (m_paceTimer != nullptr) {
        m_paceTimer->stop();  // 链条的启动器：断开后不再喂帧，自然不再有上屏动作
    }
    if (m_pipeline) {
        m_pipeline->stop();  // 幂等：内部先解阻塞再 join，500ms 内返回
    }
    if (m_ui != nullptr && m_ui->videoWidget != nullptr) {
        m_ui->videoWidget->clearFrame();
    }
    m_frames.clear();  // 释放队列里保活的帧（丢弃属于生命周期动作，不计入丢弃统计）
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

    // 到达速率：累计计数 ÷ 真实经过时间（不假设定时器精度，也不受界面卡顿影响）。
    const FrameQueue::Stats stats = m_frames.stats();
    const qint64 elapsedMs = m_arrivalClock.restart();
    if (elapsedMs > 0) {
        const auto delta = stats.arrived - m_lastArrivedFrames;
        m_arrivalFps = static_cast<double>(delta) * 1000.0 / static_cast<double>(elapsedMs);
        m_lastArrivedFrames = stats.arrived;
    }

    m_labelResolution->setText(videoWidth > 0 && videoHeight > 0
                                   ? QStringLiteral("分辨率 %1x%2").arg(videoWidth).arg(videoHeight)
                                   : QStringLiteral("分辨率 -"));
    m_labelFps->setText(QStringLiteral("到达 %1 / 上屏 %2 fps")
                            .arg(m_arrivalFps, 0, 'f', 1)
                            .arg(m_ui->videoWidget->renderFps(), 0, 'f', 1));
    // 两类丢弃分开显示：满=上游产出快于上屏消费；过期=本地呈现落后、主动跳帧追赶。
    m_labelDrops->setText(QStringLiteral("丢 满 %1 / 过期 %2")
                              .arg(stats.overflowDropped)
                              .arg(stats.staleSkipped));
    m_labelLatency->setText(m_lastLatencyMs > 0 ? QStringLiteral("延迟 %1 ms").arg(m_lastLatencyMs)
                                                : QStringLiteral("延迟 -"));
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    stopPipeline();
    QMainWindow::closeEvent(event);
}
