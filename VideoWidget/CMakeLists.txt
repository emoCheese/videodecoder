# 渲染控件独立目标：只依赖 Qt 与纯数据契约（nv12frameview.h）。
# 本目标不链接 Decoder、也不带它的 include 目录，
# 因此"控件不得依赖 FFmpeg/Pipeline"这条边界由构建系统直接保证。
qt_add_library(VideoWidget STATIC
	nv12frameview.h
	videowidget.h
	videowidget.cpp
)

target_include_directories(VideoWidget PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(VideoWidget
	PUBLIC
		Qt6::Core
		Qt6::Widgets
		Qt6::OpenGL
		Qt6::OpenGLWidgets
)
