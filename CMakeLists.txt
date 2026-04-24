cmake_minimum_required(VERSION 3.20)

project(HyperMosaic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HYPERMOSAIC_COPY_FFMPEG_DLLS "Copy FFmpeg runtime DLLs next to hyper_mosaic.exe" ON)
option(HYPERMOSAIC_ENABLE_OPENCV_CASCADE "Enable OpenCV CascadeClassifier face detector" ON)
set(HYPERMOSAIC_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/ffmpeg-8.1" CACHE PATH "Path to FFmpeg root (source tree or prebuilt package)")

if(MSVC)
    add_compile_options(/W4 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(hyper_mosaic
    src/main.cpp
    src/ffmpeg/ffmpeg_raii.cpp
    src/pipeline/demux_stage.cpp
    src/pipeline/decode_stage.cpp
    src/pipeline/track_render_stage.cpp
    src/pipeline/encode_stage.cpp
    src/tracker/tracker_core.cpp
)

if(WIN32)
    set_target_properties(hyper_mosaic PROPERTIES
        WIN32_EXECUTABLE FALSE
    )
endif()


target_include_directories(hyper_mosaic PRIVATE src)

set(_ffmpeg_candidate_roots
    "${HYPERMOSAIC_FFMPEG_ROOT}"
    "${HYPERMOSAIC_FFMPEG_ROOT}/ffmpeg-8.1"
)

if(DEFINED ENV{FFMPEG_ROOT})
    list(APPEND _ffmpeg_candidate_roots "$ENV{FFMPEG_ROOT}")
endif()

set(_ffmpeg_include_candidates "")
set(_ffmpeg_lib_candidates "")
set(_ffmpeg_bin_candidates "")
set(_ffmpeg_detected_source_root "")

foreach(_root IN LISTS _ffmpeg_candidate_roots)
    if(NOT EXISTS "${_root}")
        continue()
    endif()

    # Prebuilt package layout: <root>/include, <root>/lib, <root>/bin
    if(EXISTS "${_root}/include/libavcodec/avcodec.h")
        list(APPEND _ffmpeg_include_candidates "${_root}/include")
    endif()

    # Source tree layout: contains ./configure and libavcodec headers at root.
    if(EXISTS "${_root}/configure" AND EXISTS "${_root}/libavcodec/avcodec.h")
        if(_ffmpeg_detected_source_root STREQUAL "")
            set(_ffmpeg_detected_source_root "${_root}")
        endif()
        list(APPEND _ffmpeg_include_candidates "${_root}")
    endif()

    list(APPEND _ffmpeg_lib_candidates
        "${_root}/lib"
        "${_root}/build/lib"
        "${_root}/out/lib"
        "${_root}/local-install/lib"
    )

    list(APPEND _ffmpeg_bin_candidates
        "${_root}/bin"
        "${_root}/build/bin"
        "${_root}/out/bin"
        "${_root}/local-install/bin"
    )
endforeach()

list(REMOVE_DUPLICATES _ffmpeg_include_candidates)
list(REMOVE_DUPLICATES _ffmpeg_lib_candidates)
list(REMOVE_DUPLICATES _ffmpeg_bin_candidates)

# 强制清空缓存值，确保每次都重新搜索
unset(FFMPEG_INCLUDE_DIR CACHE)
unset(AVCODEC_LIBRARY CACHE)
unset(AVFORMAT_LIBRARY CACHE)
unset(AVUTIL_LIBRARY CACHE)

message(STATUS "=== Starting FFmpeg search ===")

find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
    PATHS ${_ffmpeg_include_candidates}
    NO_DEFAULT_PATH
)
find_library(AVCODEC_LIBRARY NAMES avcodec avcodec-62
    PATHS ${_ffmpeg_lib_candidates}
    NO_DEFAULT_PATH
)
find_library(AVFORMAT_LIBRARY NAMES avformat avformat-62
    PATHS ${_ffmpeg_lib_candidates}
    NO_DEFAULT_PATH
)
find_library(AVUTIL_LIBRARY NAMES avutil avutil-60
    PATHS ${_ffmpeg_lib_candidates}
    NO_DEFAULT_PATH
)


message(STATUS "FFMPEG_INCLUDE_DIR found: ${FFMPEG_INCLUDE_DIR}")
message(STATUS "AVCODEC_LIBRARY found: ${AVCODEC_LIBRARY}")
message(STATUS "AVFORMAT_LIBRARY found: ${AVFORMAT_LIBRARY}")
message(STATUS "AVUTIL_LIBRARY found: ${AVUTIL_LIBRARY}")

# Fallback to system/vcpkg search paths.
if(NOT FFMPEG_INCLUDE_DIR)
    find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
endif()
if(NOT AVCODEC_LIBRARY)
    find_library(AVCODEC_LIBRARY NAMES avcodec avcodec-61)
endif()
if(NOT AVFORMAT_LIBRARY)
    find_library(AVFORMAT_LIBRARY NAMES avformat avformat-61)
endif()
if(NOT AVUTIL_LIBRARY)
    find_library(AVUTIL_LIBRARY NAMES avutil avutil-59)
endif()

if(NOT FFMPEG_INCLUDE_DIR OR NOT AVCODEC_LIBRARY OR NOT AVFORMAT_LIBRARY OR NOT AVUTIL_LIBRARY)
    if(NOT _ffmpeg_detected_source_root STREQUAL "")
        message(FATAL_ERROR
            "Detected FFmpeg source tree at: ${_ffmpeg_detected_source_root}\n"
            "But no usable prebuilt libraries were found (avcodec/avformat/avutil).\n"
            "Build FFmpeg first, then reconfigure CMake.\n"
            "Recommended helper script: powershell -ExecutionPolicy Bypass -File scripts/build_ffmpeg_msvc.ps1\n"
            "Or set HYPERMOSAIC_FFMPEG_ROOT to a prebuilt FFmpeg package path with include/lib/bin."
        )
    else()
        message(FATAL_ERROR
            "FFmpeg not found.\n"
            "Set HYPERMOSAIC_FFMPEG_ROOT to your FFmpeg package path, or install with vcpkg: vcpkg install ffmpeg"
        )
    endif()
endif()

target_include_directories(hyper_mosaic PRIVATE ${FFMPEG_INCLUDE_DIR})
target_link_libraries(hyper_mosaic PRIVATE
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
)

if(HYPERMOSAIC_ENABLE_OPENCV_CASCADE)
    find_package(OpenCV QUIET COMPONENTS core objdetect)
    if(OpenCV_FOUND)
        message(STATUS "OpenCV found: ${OpenCV_VERSION}")
        target_compile_definitions(hyper_mosaic PRIVATE HYPERMOSAIC_WITH_OPENCV=1)
        target_include_directories(hyper_mosaic PRIVATE ${OpenCV_INCLUDE_DIRS})
        target_link_libraries(hyper_mosaic PRIVATE ${OpenCV_LIBS})
    else()
        message(WARNING "OpenCV not found. Face detection will fallback to heuristic detector.")
    endif()
endif()

if(WIN32 AND HYPERMOSAIC_COPY_FFMPEG_DLLS)
    find_path(FFMPEG_BIN_DIR
        NAMES avcodec-61.dll avcodec.dll
        PATHS ${_ffmpeg_bin_candidates}
        NO_DEFAULT_PATH
    )

    if(NOT FFMPEG_BIN_DIR)
        find_path(FFMPEG_BIN_DIR NAMES avcodec-61.dll avcodec.dll)
    endif()

    if(FFMPEG_BIN_DIR)
        file(GLOB FFMPEG_RUNTIME_DLLS "${FFMPEG_BIN_DIR}/*.dll")
        if(FFMPEG_RUNTIME_DLLS)
            add_custom_command(TARGET hyper_mosaic POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${FFMPEG_RUNTIME_DLLS}
                    $<TARGET_FILE_DIR:hyper_mosaic>
                COMMENT "Copy FFmpeg runtime DLLs to output directory"
            )
        endif()
    endif()
endif()
