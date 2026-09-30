# Desktop build of the Android PyroWave decoder and renderer for testing
# without a device. Needs a Vulkan loader and any Vulkan 1.3 driver (Mesa's
# lavapipe works), and moonlight-common-c's headers:
#
#   cmake -S app/src/main/jni/pyrowave/android/tests -B build/pyrowave-test \
#         -DMOONLIGHT_COMMON_C=app/src/main/jni/moonlight-core/moonlight-common-c
#   cmake --build build/pyrowave-test && build/pyrowave-test/pw_render_test

cmake_minimum_required(VERSION 3.16)
project(pyrowave_android_test CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(MOONLIGHT_COMMON_C "${PW_ROOT}/../moonlight-core/moonlight-common-c" CACHE PATH "moonlight-common-c checkout")
get_filename_component(MOONLIGHT_COMMON_C "${MOONLIGHT_COMMON_C}" ABSOLUTE)

file(STRINGS "${PW_ROOT}/codec_sources.txt" PW_CODEC_SOURCES)
list(TRANSFORM PW_CODEC_SOURCES PREPEND "${PW_ROOT}/")
set(GRANITE "${PW_ROOT}/pyrowave/Granite")

add_library(pyrowave-codec STATIC ${PW_CODEC_SOURCES} "${PW_ROOT}/volk_platform.c")
target_include_directories(pyrowave-codec PUBLIC
    "${PW_ROOT}/pyrowave" "${PW_ROOT}/pyrowave/shaders"
    "${GRANITE}/video" "${GRANITE}/vulkan" "${GRANITE}/util" "${GRANITE}/math"
    "${GRANITE}/application/global" "${GRANITE}/third_party/volk"
    "${GRANITE}/third_party/renderdoc" "${GRANITE}/third_party/khronos/vulkan-headers/include")
target_compile_definitions(pyrowave-codec PUBLIC GRANITE_SHIPPING GRANITE_RENDERDOC_CAPTURE PYROWAVE_PRECISION=1)
target_compile_options(pyrowave-codec PRIVATE -w)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    # Granite's math needs SSE3 or better (Android x86_64 guarantees SSE4.2)
    target_compile_options(pyrowave-codec PUBLIC -msse4.2)
endif()
target_link_libraries(pyrowave-codec PUBLIC dl pthread)

add_library(pyrowave-android STATIC
    "${PW_ROOT}/android/pyrowaveframing.cpp"
    "${PW_ROOT}/android/pw_vulkan.cpp"
    "${PW_ROOT}/android/pw_decoder.cpp"
    "${PW_ROOT}/android/pw_presenter.cpp")
target_include_directories(pyrowave-android PUBLIC "${PW_ROOT}/android" "${MOONLIGHT_COMMON_C}/src")
target_compile_options(pyrowave-android PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(pyrowave-android PUBLIC pyrowave-codec)

add_executable(pw_render_test pw_render_test.cpp)
target_link_libraries(pw_render_test PRIVATE pyrowave-android)

# moonlight-qt's framing parser tests, unchanged apart from the include path
add_executable(tst_pyrowaveframing tst_pyrowaveframing.cpp "${PW_ROOT}/android/pyrowaveframing.cpp")
target_include_directories(tst_pyrowaveframing PRIVATE "${PW_ROOT}/android")
