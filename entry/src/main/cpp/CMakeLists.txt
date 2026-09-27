cmake_minimum_required(VERSION 3.5.0)
project(harmony_ipcamera)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(streaming SHARED
        rtsp_server.cpp
        test_source.cpp
        camera_streamer.cpp
        mic_streamer.cpp
        rtmp_publisher.cpp
        recorder.cpp
        snapshot.cpp
        web_stream.cpp
        jpeg_encoder.cpp
        http_server.cpp
        voice_player.cpp
        osd_state.cpp
        osd_renderer.cpp
        osd_pipeline.cpp
        napi_init.cpp)

target_include_directories(streaming PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(streaming PUBLIC
        libace_napi.z.so
        libhilog_ndk.z.so
        libnative_media_core.so
        libnative_media_codecbase.so
        libnative_media_venc.so
        libnative_media_vdec.so
        libnative_media_adec.so
        libnative_media_avmuxer.so
        libnative_media_aenc.so
        libohcamera.so
        libohaudio.so
        libnative_window.so
        libnative_image.so
        libnative_buffer.so
        libnative_drawing.so
        libpixelmap.so
        libace_ndk.z.so)
