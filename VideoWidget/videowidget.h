#pragma once

#include <QElapsedTimer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>

#include "nv12frameview.h"

/// @file videowidget.h
/// @brief NV12 视频渲染控件（OpenGL 上屏）。
///
/// 职责边界：
///  - 只做"拿到 NV12 视图 → 上传纹理 → 画到屏幕"（含宽高比适配与上屏帧率统计）；
///  - 不知道帧从哪来：只依赖 nv12frameview.h 的纯数据契约，不依赖 FFmpeg / Pipeline；
///  - 所有 OpenGL 调用都发生在 GUI 线程（initializeGL / paintGL / resizeGL）。
///
/// 渲染流程：onFrameReady() 只登记待显示帧并请求重绘；
///           paintGL() 里上传纹理并绘制，保证上下文已 current、同一帧只上传一次。

class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT

public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget() override;

    /// 上屏一帧（必须在 GUI 线程调用）。
    /// @param view  像素布局；相邻两帧之间保持有效
    /// @param lease 像素内存保活句柄，控件持有到本帧上传完成
    void onFrameReady(const videoframe::Nv12FrameView& view, const videoframe::FrameLease& lease);

    /// 清空画面为黑屏（断开连接时调用）。
    void clearFrame();

    [[nodiscard]] double renderFps() const noexcept { return m_fps; }
    [[nodiscard]] bool hasFrame() const noexcept { return m_hasFrame; }
    [[nodiscard]] int videoWidth() const noexcept { return m_videoWidth; }
    [[nodiscard]] int videoHeight() const noexcept { return m_videoHeight; }
    [[nodiscard]] bool glReady() const noexcept { return m_glReady; }

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void showEvent(QShowEvent* event) override;

private:
    void initShader();
    void initGeometry();
    void initTextures();
    bool ensureTextures(const videoframe::Nv12FrameView& view);  ///< 尺寸变化时重建纹理
    [[nodiscard]] bool uploadFrame(const videoframe::Nv12FrameView& view);
    void drawFrame();
    void updateFpsCounter();
    void warnIfContextUnavailable();

    QOpenGLShaderProgram m_program;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLuint m_texY = 0;
    GLuint m_texUV = 0;

    int m_texWidth = 0;    ///< 已分配纹理的尺寸
    int m_texHeight = 0;
    int m_videoWidth = 0;  ///< 当前视频尺寸（供状态栏显示）
    int m_videoHeight = 0;

    videoframe::Nv12FrameView m_pendingView{};
    videoframe::FrameLease m_pendingLease;
    bool m_frameDirty = false;
    bool m_hasFrame = false;
    bool m_glReady = false;
    bool m_warnedNoContext = false;  ///< 上下文失败诊断只报一次

    QElapsedTimer m_fpsClock;
    int m_fpsFrames = 0;
    double m_fps = 0.0;
};
