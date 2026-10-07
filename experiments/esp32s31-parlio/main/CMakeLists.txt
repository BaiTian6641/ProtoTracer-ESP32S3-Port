idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "../../../src"
                       REQUIRES arduino hub75 esp_psram esp_driver_parlio)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE VECTOR2D_USE_DSP VECTOR3D_USE_DSP)
