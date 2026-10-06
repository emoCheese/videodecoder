#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>

#include <memory>

#include "PacketData.h"  // 库的公共 DTO：avframe::VideoFramePtr
#include "framequeue.h"  // 应用层呈现队列（解码线程 → GUI 线程）

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class QLabel;
class Pipeline;

/// @brief 应用层：交互 + 跨线程投递 + 上屏调度 + 状态展示 + 组装适配器。
///
/// 职责边界（见项目模块边界约定）：
///  - 不做拉流/解码逻辑（Pipeline 负责），不修改帧内容；
///  - 是唯一同时认识"库"与"渲染控件"的地方：把 avframe::VideoFrame 适配成
///    videoframe::Nv12FrameView（控件因此不依赖 FFmpeg）；
///  - 库回调发生在工作线程：帧入 FrameQueue（自带锁、不阻塞），状态用队列投递回 GUI 线程；
///  - 上屏节拍由控件决定（framePending()/frameSwapped()），本类只负责"什么时候把哪一帧交给控件"。
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
    void onFrameSwapped();     ///< vsync 节拍：上一帧已完成上屏，立刻喂下一帧
    void onPaceTimerTick();    ///< 4ms 补漏：队列空转后重新启动上屏链条
    void onStatusTimerTick();

private:
    void bindPipelineCallbacks();
    void handleStatusMessage(const QString& message);
    void presentNextFrame();  ///< 唯一的出队点：控件空闲且队列有帧时交给控件
    void stopPipeline();
    void setConnectedUi(bool connected);
    void updateStatusText();

    Ui::MainWindow* m_ui = nullptr;          ///< UI 对象（含渲染控件），生命周期短于本窗口
    std::unique_ptr<Pipeline> m_pipeline;    ///< 管线唯一入口，先于 UI 停止

    /// 解码线程 → GUI 线程的呈现队列：容量 3，满则丢最旧，积压跨度过大则跳帧追赶。
    FrameQueue m_frames;

    QTimer* m_paceTimer = nullptr;    ///< 4ms：补漏（呈现节拍由控件 frameSwapped 决定）
    QTimer* m_statusTimer = nullptr;  ///< 500ms：刷新状态栏
    QLabel* m_labelResolution = nullptr;
    QLabel* m_labelFps = nullptr;     ///< 到达 fps / 上屏 fps
    QLabel* m_labelDrops = nullptr;   ///< 丢弃明细：队列满 / 过期跳过
    QLabel* m_labelLatency = nullptr;

    /// 到达速率统计（仅 GUI 线程访问）：用累计计数除以真实经过时间，不假设定时器精度。
    QElapsedTimer m_arrivalClock;
    unsigned long long m_lastArrivedFrames = 0;
    double m_arrivalFps = 0.0;

    int m_lastLatencyMs = 0;  ///< 仅 GUI 线程访问
    bool m_streamActive = false;  ///< 已发起连接且尚未收尾（仅 GUI 线程访问）
};

#endif  // MAINWINDOW_H
