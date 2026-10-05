#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>

#include <atomic>
#include <memory>
#include <mutex>

#include "PacketData.h"  // 库的公共 DTO：avframe::VideoFramePtr

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class QLabel;
class Pipeline;

/// @brief 应用层：交互 + 跨线程投递 + 状态展示 + 组装适配器。
///
/// 职责边界（见项目模块边界约定）：
///  - 不做拉流/解码逻辑（Pipeline 负责），不修改帧内容；
///  - 是唯一同时认识"库"与"渲染控件"的地方：把 avframe::VideoFrame 适配成
///    videoframe::Nv12FrameView（控件因此不依赖 FFmpeg）；
///  - 库回调发生在工作线程：帧只写"最新帧槽"，状态用队列投递回 GUI 线程。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// 载入初始地址并自动连接（供命令行参数使用，便于无交互验收）。
    void openUrl(const QString& url);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onFrameTimerTick();
    void onStatusTimerTick();

private:
    void bindPipelineCallbacks();
    void handleStatusMessage(const QString& message);
    void stopPipeline();
    void setConnectedUi(bool connected);
    void updateStatusText();

    Ui::MainWindow* m_ui = nullptr;          ///< UI 对象（含渲染控件），生命周期短于本窗口
    std::unique_ptr<Pipeline> m_pipeline;    ///< 管线唯一入口，先于 UI 停止

    /// 解码线程 → GUI 线程的"最新帧槽"：只保留最新一帧，GUI 落后时自动丢帧。
    std::mutex m_frameMutex;
    avframe::VideoFramePtr m_latestFrame;
    std::atomic<unsigned long long> m_droppedFrames{0};

    QTimer* m_frameTimer = nullptr;   ///< 16ms：取最新帧上屏
    QTimer* m_statusTimer = nullptr;  ///< 500ms：刷新状态栏
    QLabel* m_labelResolution = nullptr;
    QLabel* m_labelFps = nullptr;
    QLabel* m_labelLatency = nullptr;

    int m_lastLatencyMs = 0;  ///< 仅 GUI 线程访问
    bool m_streamActive = false;  ///< 已发起连接且尚未收尾（仅 GUI 线程访问）
};

#endif  // MAINWINDOW_H
