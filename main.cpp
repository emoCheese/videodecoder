#include "mainwindow.h"

#include <QApplication>
#include <QSurfaceFormat>

namespace {

/// 渲染控件使用 QOpenGLFunctions_3_3_Core 与 #version 330 着色器，
/// 因此必须在创建任何 OpenGL 上下文之前把默认格式设为 3.3 Core。
void configureDefaultSurfaceFormat()
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(0);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
}

}  // namespace

int main(int argc, char* argv[])
{
    configureDefaultSurfaceFormat();

    QApplication app(argc, argv);

    MainWindow window;
    window.show();

    // 可选：命令行直接给出地址（rtsp:// | rtp 的 SDP 路径 | 本地文件），启动后自动连接。
    if (argc > 1) {
        window.openUrl(QString::fromLocal8Bit(argv[1]));
    }
    return QApplication::exec();
}
