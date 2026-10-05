# 测试目标只依赖被测模块：
#  - ringbuffer_test 只链接 DecoderPrimitives（不含 FFmpeg）
#  - pipeline_smoke  链接 Decoder（公共头 Pipeline.h + PacketData.h）

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
else()
    message(STATUS "未找到 ffmpeg CLI：跳过自动生成素材的烟测（可用 pipeline_smoke 手动验收）")
endif()
