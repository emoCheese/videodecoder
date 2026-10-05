cmake_minimum_required(VERSION 3.19)
project(VideoDecoder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VD_BUILD_TESTS "构建并注册单元测试" ON)

# ── Sanitizer：显式开关（编译 + 链接都显式指定）──────────────────────────────
# ASan 与 TSan 互斥，必须使用各自独立的构建目录：
#   cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DVD_SANITIZE_ADDRESS=ON -DVD_SANITIZE_UNDEFINED=ON
#   cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DVD_SANITIZE_THREAD=ON
option(VD_SANITIZE_ADDRESS "启用 AddressSanitizer + LeakSanitizer (-fsanitize=address)" OFF)
option(VD_SANITIZE_UNDEFINED "启用 UndefinedBehaviorSanitizer (-fsanitize=undefined)" OFF)
option(VD_SANITIZE_THREAD "启用 ThreadSanitizer (-fsanitize=thread)" OFF)

if(VD_SANITIZE_THREAD AND (VD_SANITIZE_ADDRESS OR VD_SANITIZE_UNDEFINED))
    message(FATAL_ERROR
        "ThreadSanitizer 不能与 Address/Undefined Sanitizer 同时启用：请分别使用 build-asan / build-tsan 目录。")
endif()

set(VD_SANITIZER_FLAGS "")
if(VD_SANITIZE_ADDRESS)
    list(APPEND VD_SANITIZER_FLAGS -fsanitize=address)
endif()
if(VD_SANITIZE_UNDEFINED)
    list(APPEND VD_SANITIZER_FLAGS -fsanitize=undefined)
endif()
if(VD_SANITIZE_THREAD)
    list(APPEND VD_SANITIZER_FLAGS -fsanitize=thread)
endif()

if(VD_SANITIZER_FLAGS)
    list(APPEND VD_SANITIZER_FLAGS -fno-omit-frame-pointer -g)
    add_compile_options(${VD_SANITIZER_FLAGS})
    add_link_options(${VD_SANITIZER_FLAGS})
    message(STATUS "Sanitizer 已启用: ${VD_SANITIZER_FLAGS}")
else()
    message(STATUS "Sanitizer 未启用（-DVD_SANITIZE_ADDRESS=ON / -DVD_SANITIZE_THREAD=ON 可开启）")
endif()

list(APPEND CMAKE_PREFIX_PATH "/home/fox/Qt/6.10.3/gcc_64")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets OpenGL OpenGLWidgets)

# 本项目代码要求零告警（FFmpeg 若在公共头产生告警，用局部 pragma 抑制）。
add_compile_options(-Wall -Wextra -Wpedantic)

# qt_standard_project_setup() 会打开 AUTOMOC/AUTOUIC/AUTORCC，
# 必须在使用 Qt 目标的子目录之前调用；纯 C++ 的 Decoder 库与测试放在它之前，
# 避免为非 Qt 目标做无谓的 moc 扫描。
add_subdirectory(Decoder)

if(VD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

qt_standard_project_setup()

add_subdirectory(VideoWidget)

qt_add_executable(VideoDecoder
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
)

target_link_libraries(VideoDecoder
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        Decoder
        VideoWidget
)

install(TARGETS VideoDecoder
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET VideoDecoder
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
