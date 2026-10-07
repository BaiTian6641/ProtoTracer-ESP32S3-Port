cmake_minimum_required(VERSION 3.16)
# Arduino's public include set can exceed Windows CreateProcess limits.
set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON CACHE BOOL "Use response files for the probe")
set(IDF_TARGET "esp32s31" CACHE STRING "S31 bring-up target")
if(NOT IDF_TARGET STREQUAL "esp32s31")
    message(FATAL_ERROR "This probe targets ESP32-S31 only")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/arduino/CMakeLists.txt" OR
   NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/hub75/CMakeLists.txt")
    message(FATAL_ERROR "Run tools/prepare_s31_probe.py before configuring")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(prototracer_s31_parlio_probe)

# IDF 6.1 S31 register headers and Arduino RC1 Ethernet use valid partial
# aggregate initialization. Keep those diagnostics as warnings in this probe;
# retain all other warning/error policy and do not change the S3 build.
foreach(component arduino hub75 main)
    idf_component_get_property(component_lib ${component} COMPONENT_LIB)
    target_compile_options(${component_lib} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-Wno-error=missing-field-initializers>")
endforeach()
