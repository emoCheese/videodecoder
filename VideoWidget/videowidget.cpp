#include "videowidget.h"

#include <QDebug>
#include <QLoggingCategory>
#include <QShowEvent>
#include <QTimer>

#include <cstddef>

// 参考示例（videowidget）的着色器思路：
//  - 顶点着色器把全屏四边形的位置乘以 uScale，实现宽高比适配（letterbox，不拉伸）；
//  - 片段着色器采样 Y 与 UV 两张纹理，按 BT.601 转成 RGB。
static const char* kVertexShader = R"(#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTex;

uniform vec2 uScale;   // 宽高比适配：视频比控件更宽时压缩 y，更高时压缩 x

out vec2 vTex;

void main() {
    vTex = aTex;
    gl_Position = vec4(aPos * uScale, 0.0, 1.0);
}
)";

static const char* kFragmentShader = R"(#version 330 core
in vec2 vTex;
out vec4 FragColor;

uniform sampler2D texY;
uniform sampler2D texUV;

void main() {
    float y = texture(texY, vTex).r;                      // Y 平面
    vec2 uv = texture(texUV, vTex).rg - vec2(0.5, 0.5);   // U/V 交错平面，转到 [-0.5, 0.5]

    float r = y + 1.402 * uv.y;
    float g = y - 0.344 * uv.x - 0.714 * uv.y;
    float b = y + 1.772 * uv.x;

    FragColor = vec4(r, g, b, 1.0);
}
)";

Q_LOGGING_CATEGORY(lcWidget, "videodecoder.widget")

VideoWidget::VideoWidget(QWidget* parent) : QOpenGLWidget(parent)
{
    m_fpsClock.start();
}

VideoWidget::~VideoWidget()
{
    if (!m_glReady) {
        return;  // 上下文从未建立成功，没有 GL 资源需要释放
    }
    makeCurrent();
    if (m_texY != 0) {
        glDeleteTextures(1, &m_texY);
    }
    if (m_texUV != 0) {
        glDeleteTextures(1, &m_texUV);
    }
    if (m_vbo != 0) {
        glDeleteBuffers(1, &m_vbo);
    }
    if (m_vao != 0) {
        glDeleteVertexArrays(1, &m_vao);
    }
    doneCurrent();
}

void VideoWidget::onFrameReady(const videoframe::Nv12FrameView& view, const videoframe::FrameLease& lease)
{
    // 只登记，不做 GL 调用：真正的上传放在 paintGL（上下文已 current）。
    // 被覆盖的旧帧由 m_pendingLease 释放，天然实现"只保留最新帧"。
    m_pendingView = view;
    m_pendingLease = lease;
    m_frameDirty = view.valid();
    update();
}

void VideoWidget::clearFrame()
{
    m_pendingView = {};
    m_pendingLease.reset();
    m_frameDirty = false;
    m_hasFrame = false;
    update();
}

void VideoWidget::showEvent(QShowEvent* event)
{
    QOpenGLWidget::showEvent(event);
    // QOpenGLWidget 在第一次绘制时才创建上下文：稍后自检，
    // 失败时 initializeGL 不会被调用，这里补一条可执行的诊断，避免只留一个黑窗口。
    QTimer::singleShot(1000, this, [this] { warnIfContextUnavailable(); });
}

void VideoWidget::warnIfContextUnavailable()
{
    if (m_glReady || m_warnedNoContext || isValid()) {
        return;
    }
    m_warnedNoContext = true;
    qCCritical(lcWidget) << "OpenGL 上下文创建失败，画面将无法显示。请检查显示环境与显卡驱动"
                            "（glxinfo -B / qtdiag6）；可尝试 QT_XCB_GL_INTEGRATION=xcb_egl 或 "
                            "LIBGL_ALWAYS_SOFTWARE=1。";
}

void VideoWidget::initializeGL()
{
    if (!initializeOpenGLFunctions()) {
        qCCritical(lcWidget) << "QOpenGLFunctions_3_3_Core 初始化失败：需要 OpenGL 3.3 及以上的 Core Profile 上下文，"
                                "画面将无法显示。可尝试 QT_XCB_GL_INTEGRATION=xcb_egl 或 LIBGL_ALWAYS_SOFTWARE=1。";
        return;
    }

    initShader();
    if (!m_program.isLinked()) {
        qCCritical(lcWidget) << "着色器链接失败，画面将无法显示:" << m_program.log();
        return;
    }

    initGeometry();
    initTextures();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    m_glReady = true;
    qCInfo(lcWidget) << "OpenGL 已就绪，渲染器:" << reinterpret_cast<const char*>(glGetString(GL_RENDERER));
}

void VideoWidget::initShader()
{
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)) {
        qCCritical(lcWidget) << "顶点着色器编译失败:" << m_program.log();
        return;
    }
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)) {
        qCCritical(lcWidget) << "片段着色器编译失败:" << m_program.log();
        return;
    }
    if (!m_program.link()) {
        qCCritical(lcWidget) << "着色器链接失败:" << m_program.log();
    }
}

void VideoWidget::initGeometry()
{
    // 全屏四边形（GL_TRIANGLE_STRIP）：x, y, u, v
    // 纹理坐标 v 轴翻转，因为 OpenGL 纹理原点在左下。
    static constexpr float kVertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<const void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void VideoWidget::initTextures()
{
    glGenTextures(1, &m_texY);
    glGenTextures(1, &m_texUV);

    for (const GLuint texture : {m_texY, m_texUV}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    // 逐行上传时按行宽取数据：行宽不一定是 4 的倍数，故按 1 字节对齐解释像素行。
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

void VideoWidget::resizeGL(int width, int height)
{
    glViewport(0, 0, width, height);
}

bool VideoWidget::ensureTextures(const videoframe::Nv12FrameView& view)
{
    if (view.width == m_texWidth && view.height == m_texHeight) {
        return true;
    }

    m_texWidth = view.width;
    m_texHeight = view.height;

    glBindTexture(GL_TEXTURE_2D, m_texY);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, view.width, view.height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, m_texUV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, view.width / 2, view.height / 2, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

bool VideoWidget::uploadFrame(const videoframe::Nv12FrameView& view)
{
    if (!view.valid()) {
        qCWarning(lcWidget) << "非法 NV12 视图，跳过该帧";
        return false;
    }

    ensureTextures(view);

    // 保存原有 pixel store 状态，避免污染其他代码
    GLint oldAlign = 4;
    GLint oldRowLength = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlign);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &oldRowLength);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // Y 平面：一次上传整帧
    glBindTexture(GL_TEXTURE_2D, m_texY);

    // Y 每像素 1 字节，所以 ROW_LENGTH 就是 stride 字节数
    glPixelStorei(GL_UNPACK_ROW_LENGTH, view.y.stride);

    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0, 0,
                    view.width, view.height,
                    GL_RED,
                    GL_UNSIGNED_BYTE,
                    view.y.data);

    // UV 平面：一次上传整个交错 UV 平面
    glBindTexture(GL_TEXTURE_2D, m_texUV);

    // GL_RG 每像素 2 字节，ROW_LENGTH 单位是像素数
    glPixelStorei(GL_UNPACK_ROW_LENGTH, view.uv.stride / 2);

    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0, 0,
                    view.width / 2, view.height / 2,
                    GL_RG,
                    GL_UNSIGNED_BYTE,
                    view.uv.data);

    // 恢复状态
    glPixelStorei(GL_UNPACK_ROW_LENGTH, oldRowLength);
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlign);

    glBindTexture(GL_TEXTURE_2D, 0);

    m_videoWidth = view.width;
    m_videoHeight = view.height;
    return true;
}

void VideoWidget::drawFrame()
{
    // 宽高比适配：视频更宽 → 上下留黑边；视频更高 → 左右留黑边。
    const float widgetAspect =
        (height() > 0) ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
    const float videoAspect =
        (m_videoHeight > 0) ? static_cast<float>(m_videoWidth) / static_cast<float>(m_videoHeight) : 1.0f;

    float scaleX = 1.0f;
    float scaleY = 1.0f;
    if (videoAspect > widgetAspect) {
        scaleY = widgetAspect / videoAspect;
    }
    else if (videoAspect < widgetAspect) {
        scaleX = videoAspect / widgetAspect;
    }

    m_program.bind();
    m_program.setUniformValue("uScale", scaleX, scaleY);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texY);
    m_program.setUniformValue("texY", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_texUV);
    m_program.setUniformValue("texUV", 1);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_program.release();
}

void VideoWidget::updateFpsCounter()
{
    ++m_fpsFrames;
    const qint64 elapsed = m_fpsClock.elapsed();
    if (elapsed >= 1000) {
        m_fps = static_cast<double>(m_fpsFrames) * 1000.0 / static_cast<double>(elapsed);
        m_fpsFrames = 0;
        m_fpsClock.restart();
    }
}

void VideoWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    if (!m_glReady) {
        return;
    }

    if (m_frameDirty) {
        m_hasFrame = uploadFrame(m_pendingView);
        if (m_hasFrame) {
            updateFpsCounter();
        }
        m_frameDirty = false;
        m_pendingLease.reset();  // 像素已进入 GPU 纹理，可以释放保活句柄
    }

    if (m_hasFrame) {
        drawFrame();
    }
}
