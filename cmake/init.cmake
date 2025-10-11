message(STATUS "Building ffai")

find_package(PkgConfig REQUIRED)

set(ENV{PKG_CONFIG_PATH} "${CMAKE_SOURCE_DIR}/lib/pkgconfig")

list(APPEND TARGET_FILES
  "${CMAKE_SOURCE_DIR}/src/main.cpp"
  "${CMAKE_SOURCE_DIR}/src/input/src/InputHandler.cpp"
  "${CMAKE_SOURCE_DIR}/src/decoder/src/DecoderHandler.cpp"
  "${CMAKE_SOURCE_DIR}/src/filter/src/FilterHandler.cpp"
  "${CMAKE_SOURCE_DIR}/src/filter/src/personseg_ort.cpp"
  "${CMAKE_SOURCE_DIR}/src/filter/src/vf_personseg.c"
  "${CMAKE_SOURCE_DIR}/src/encoder/src/EncoderHandler.cpp"
  "${CMAKE_SOURCE_DIR}/src/output/src/OutputHandler.cpp"
  "${CMAKE_SOURCE_DIR}/src/core/src/Controller.cpp"
  "${CMAKE_SOURCE_DIR}/src/util/src/Logger.cpp"
)

list(APPEND HEADER_FILES
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/src/input/inc"
  "${CMAKE_SOURCE_DIR}/src/decoder/inc"
  "${CMAKE_SOURCE_DIR}/src/filter/inc"
  "${CMAKE_SOURCE_DIR}/src/encoder/inc"
  "${CMAKE_SOURCE_DIR}/src/output/inc"
  "${CMAKE_SOURCE_DIR}/src/util/inc"
  "${CMAKE_SOURCE_DIR}/src/core/inc"
)

set(PKG_CONFIG_USE_STATIC OFF)

pkg_check_modules(AVCODEC REQUIRED libavcodec) # .pc 파일을 이름으로 찾아서 AVCODEC이라는 접두어에 연결하는 것, 만약 없으면 빌드 오류 (REQUIRED)
pkg_check_modules(SVTAV1ENC REQUIRED SvtAv1Enc)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVFILTER REQUIRED libavfilter)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(VMAF REQUIRED libvmaf)
pkg_check_modules(ONNXRUNTIME REQUIRED libonnxruntime)

find_package(Boost COMPONENTS log REQUIRED) # .pc 파일 대신 cmake가 외부 라이브러리/패키지를 찾아서 사용 준비 시킴
# Boost 패키지에서 log 컴포넌트를 반드시 찾도록 명령

# include 경로 설정
include_directories(
  ${AVCODEC_INCLUDE_DIRS}
  ${AVFORMAT_INCLUDE_DIRS}
  ${AVFILTER_INCLUDE_DIRS}
  ${AVUTIL_INCLUDE_DIRS}
  ${SWRESAMPLE_INCLUDE_DIRS}
  ${SVTAV1ENC_INCLUDE_DIRS}
  ${SWSCALE_INCLUDE_DIRS}
  ${ONNXRUNTIME_INCLUDE_DIRS}
  ${VMAF_INCLUDE_DIRS}
  ${HEADER_FILES}
  ${Boost_INCLUDE_DIRS}
)

link_directories(
  ${AVCODEC_LIBRARY_DIRS}
  ${AVFORMAT_LIBRARY_DIRS}
  ${AVFILTER_LIBRARY_DIRS}
  ${AVUTIL_LIBRARY_DIRS}
  ${SWRESAMPLE_LIBRARY_DIRS}
  ${SVTAV1ENC_LIBRARY_DIRS}
  ${SWSCALE_LIBRARY_DIRS}
  ${ONNXRUNTIME_LIBRARY_DIRS}
  ${VMAF_LIBRARY_DIRS}
  ${Boost_LIBRARY_DIRS}
)

add_executable(ffai ${TARGET_FILES})

 # PRIVATE이므로 ffai에만 적용하고 ffai에 의존하는 다른 타겟에는 적용 안됨
 # -Wl : 컴파일러를 통해 링커(ld)에게 옵션 전달함
 # -no-as-needed : 사용되지 않아 보이는 라이브러리라도 버리지말고 링크, (빌드는 되지만 실제로 링크하지 않는 경우 해결)
target_link_options(ffai PRIVATE "-Wl,--no-as-needed")


target_link_libraries(ffai PRIVATE
  ${AVCODEC_LIBRARIES}
  ${AVFORMAT_LIBRARIES}
  ${AVFILTER_LIBRARIES}
  ${AVUTIL_LIBRARIES}
  ${SWRESAMPLE_LIBRARIES}
  ${SVTAV1ENC_LIBRARIES}
  ${SWSCALE_LIBRARIES}
  ${ONNXRUNTIME_LIBRARIES}
  ${VMAF_LIBRARIES}
  ${Boost_LIBRARIES}
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET ffai PROPERTY CXX_STANDARD 20)
endif()
