# 测试目标只依赖被测模块：
#  - ringbuffer_test 只链接 DecoderPrimitives（不含 FFmpeg）
#  - framequeue_test  直接编入应用层的 framequeue.cpp，只借 Decoder 拿公共 DTO（PacketData.h）
#  - pipeline_smoke   链接 Decoder（公共头 Pipeline.h + PacketData.h）

# Sanitizer 运行环境（显式设置，避免依赖调用者手工 export）：
#  - TSan：加载抑制文件（第三方 Qt/FFmpeg 未插桩产生的已知噪声），并继续跑完全部用例
#  - ASan：打开泄漏检测，并用 lsan 抑制文件过滤第三方一次性分配
set(VD_TEST_ENV "")
if(VD_SANITIZE_THREAD)
    list(APPEND VD_TEST_ENV
        "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/sanitizer/tsan.supp:halt_on_error=0")
elseif(VD_SANITIZE_ADDRESS)
    list(APPEND VD_TEST_ENV
        "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0"
        "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/sanitizer/lsan.supp")
endif()

add_executable(ringbuffer_test ringbuffer_test.cpp)
target_link_libraries(ringbuffer_test PRIVATE DecoderPrimitives)
add_test(NAME ringbuffer COMMAND ringbuffer_test)
set_tests_properties(ringbuffer PROPERTIES TIMEOUT 60 ENVIRONMENT "${VD_TEST_ENV}")

# FrameQueue 属于应用层（不在 Decoder 库里、也不依赖 Qt），因此直接把源码编进测试目标；
# 链接 Decoder 只为拿到公共 DTO（PacketData.h）以及它需要的 FFmpeg 符号。
add_executable(framequeue_test framequeue_test.cpp "${CMAKE_SOURCE_DIR}/framequeue.cpp")
target_include_directories(framequeue_test PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(framequeue_test PRIVATE Decoder)
add_test(NAME framequeue COMMAND framequeue_test)
set_tests_properties(framequeue PROPERTIES TIMEOUT 60 ENVIRONMENT "${VD_TEST_ENV}")

add_executable(pipeline_smoke pipeline_smoke.cpp)
target_link_libraries(pipeline_smoke PRIVATE Decoder)

# 自动生成兜底素材；若本机没有 ffmpeg CLI 则跳过该用例，不阻塞验收。
# 注：本机 FFmpeg 未编译 avdevice（无 lavfi），故用 rawvideo + /dev/zero 造素材。
find_program(VD_FFMPEG_EXECUTABLE
    NAMES ffmpeg
    HINTS "/home/fox/workspace/FFmpeg/install/ffmpeg7-shared/bin"
)

if(VD_FFMPEG_EXECUTABLE)
    set(VD_TEST_MEDIA_DIR "${CMAKE_CURRENT_BINARY_DIR}/testmedia")
    file(MAKE_DIRECTORY "${VD_TEST_MEDIA_DIR}")
    set(VD_TEST_MEDIA "${VD_TEST_MEDIA_DIR}/vd_test.mp4")
    add_test(NAME generate_test_media
        COMMAND "${VD_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
                -f rawvideo -pix_fmt yuv420p -s 640x480 -r 30
                -i /dev/zero -t 3
                -c:v libx264 -pix_fmt yuv420p "${VD_TEST_MEDIA}"
    )
    set_tests_properties(generate_test_media PROPERTIES FIXTURES_SETUP vd_test_media TIMEOUT 120)

    add_test(NAME pipeline_smoke_file COMMAND pipeline_smoke "${VD_TEST_MEDIA}" --frames 60)
    set_tests_properties(pipeline_smoke_file PROPERTIES
        FIXTURES_REQUIRED vd_test_media
        TIMEOUT 120
        ENVIRONMENT "${VD_TEST_ENV}"
    )

    # 回归素材：容器里放 rawvideo(NV12) 流 —— 解码输出**就是 NV12**，因此必然走
    # Decoder“源格式已是目标格式”这条分支。历史上该分支被交给 swscale 做
    # NV12→NV12 同尺寸转换，命中其只拷亮度、不写色度的快速通道，画面整幅发绿。
    # 素材由随机 RGB 转成 NV12，色度必然非常量，故可用 --require-chroma 精确判定。
    set(VD_TEST_MEDIA_NV12 "${VD_TEST_MEDIA_DIR}/vd_test_nv12.mkv")
    add_test(NAME generate_test_media_nv12
        COMMAND "${VD_FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y
                -f rawvideo -pix_fmt rgb24 -s 64x48 -i /dev/urandom -frames:v 16
                -pix_fmt nv12 -c:v rawvideo "${VD_TEST_MEDIA_NV12}"
    )
    set_tests_properties(generate_test_media_nv12 PROPERTIES FIXTURES_SETUP vd_test_media_nv12 TIMEOUT 120)

    add_test(NAME pipeline_smoke_nv12
        COMMAND pipeline_smoke "${VD_TEST_MEDIA_NV12}" --frames 8 --require-chroma
    )
    set_tests_properties(pipeline_smoke_nv12 PROPERTIES
        FIXTURES_REQUIRED vd_test_media_nv12
        TIMEOUT 120
        ENVIRONMENT "${VD_TEST_ENV}"
    )
else()
    message(STATUS "未找到 ffmpeg CLI：跳过自动生成素材的烟测（可用 pipeline_smoke 手动验收）")
endif()
