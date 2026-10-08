set(app_requires arduino hub75 esp_psram esp_driver_parlio)
# IDF's early requirements pass does not inherit arbitrary parent options.
if("$ENV{PROTOTRACER_S31_FULL_BUILD}" STREQUAL "ON")
    list(APPEND app_requires prototracer_deps espressif__esp_codec_dev esp_driver_i2s esp_driver_i2c)
endif()
if(PROTOTRACER_FULL_FIRMWARE)
    file(GLOB network_sources "${CMAKE_CURRENT_LIST_DIR}/../../../src/Network/*.cpp")
    idf_component_register(SRCS "../../../src/main.cpp" ${network_sources}
        INCLUDE_DIRS "../../../src" REQUIRES ${app_requires})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TASESP32S31 PRINTINFO
        USE_PSRAM_FOR_FACE_JSON=1 OTA_BTN=61)
    if(PROTOTRACER_S31_OFFLINE_BOOT)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE PROTOTRACER_BOOT_OFFLINE=1)
    endif()
    if(PROTOTRACER_S31_MICROPHONE STREQUAL "ES8311")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MIC_USE_ES8311=1)
    endif()
    if(PROTOTRACER_S31_PROBE_SECOND_BANK)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE S31_HUB75_PROBE_SECOND_BANK=1)
    endif()
    # Keep existing engine copy/clamp diagnostics visible without rewriting
    # render behavior as part of the SDK port. Other errors remain fatal.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=deprecated-copy
        -Wno-error=type-limits -Wno-error=class-memaccess)
else()
    idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "../../../src"
                           REQUIRES ${app_requires})
endif()
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE VECTOR2D_USE_DSP VECTOR3D_USE_DSP)
