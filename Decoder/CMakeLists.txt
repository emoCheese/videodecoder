
set(ENV{PKG_CONFIG_PATH} "/home/fox/workspace/FFmpeg/install/ffmpeg7-shared/lib/pkgconfig")

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
	libavcodec
	libavformat
	libavutil
	libswscale
	libswresample
)

# 最小依赖目标：只提供并发原语（RingBuffer.hpp），供单元测试链接，避免测试被迫依赖 FFmpeg。
add_library(DecoderPrimitives INTERFACE)
target_include_directories(DecoderPrimitives INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(DecoderPrimitives INTERFACE Threads::Threads)

# 拉流/解码库。公共头：Pipeline.h、PacketData.h；其余为内部头文件。
add_library(Decoder STATIC
	Pipeline.h
	Pipeline.cpp
	Demuxer.h
	Demuxer.cpp
	Decoder.h
	Decoder.cpp
	PacketQueue.h
	PacketData.h
	FfmpegError.h
	RingBuffer.hpp
)

# FFmpeg 头文件出现在公共头（PacketData.h）里，故必须是 PUBLIC。
target_include_directories(Decoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Decoder
	PUBLIC
		DecoderPrimitives
		PkgConfig::FFMPEG
)
