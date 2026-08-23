cmake_minimum_required(VERSION 3.5)

set(MAIN_SRCS
    "hello_world_main.c"
    "imu_config.c")

if(CONFIG_GESTURE_MODE_INFER)
    list(APPEND MAIN_SRCS "gesture_classifier.cpp")
endif()

idf_component_register(SRCS ${MAIN_SRCS} INCLUDE_DIRS ".")

if(CONFIG_GESTURE_MODE_INFER)
    # Cabecalhos do SDK exportado geram avisos com GCC 14. Eles nao devem
    # impedir a compilacao do adaptador, mas continuam visiveis no log.
    target_compile_options(${COMPONENT_LIB} PRIVATE
        "-Wa,-I${CMAKE_CURRENT_LIST_DIR}/../ml/edge-impulse/hand-gestures-project"
        -Wno-error=format
        -Wno-error=uninitialized
        -Wno-error=maybe-uninitialized
        -Wno-error=unused-but-set-parameter)
endif()
