cmake_minimum_required(VERSION 3.13.1)

# O pacote exportado permanece fora de components/. No modo de coleta este
# componente fica deliberadamente vazio, evitando incluir o modelo e o SDK na
# imagem final.
if(NOT CONFIG_GESTURE_MODE_INFER)
    # As dependencias precisam ser declaradas tambem durante a fase de
    # expansao inicial do ESP-IDF, quando as opcoes Kconfig ainda podem nao
    # estar disponiveis.
    idf_component_register(REQUIRES freertos esp_timer heap)
    return()
endif()

set(EI_MODEL_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../ml/edge-impulse/hand-gestures-project")
set(EI_SDK_DIR "${EI_MODEL_DIR}/edge-impulse-sdk")

include("${EI_SDK_DIR}/cmake/utils.cmake")

# Esta selecao segue o exemplo ESP-IDF oficial do Edge Impulse. CMSIS nao e
# usado no ESP32-S3; ESP-NN e esp-dsp fornecem as implementacoes otimizadas.
RECURSIVE_FIND_FILE_EXCLUDE_DIR(EI_CPP_SOURCES "${EI_SDK_DIR}" "CMSIS" "*.cpp")
RECURSIVE_FIND_FILE_EXCLUDE_DIR(EI_CC_SOURCES  "${EI_SDK_DIR}" "CMSIS" "*.cc")
RECURSIVE_FIND_FILE_EXCLUDE_DIR(EI_C_SOURCES   "${EI_SDK_DIR}" "CMSIS" "*.c")
RECURSIVE_FIND_FILE(EI_ASM_SOURCES "${EI_SDK_DIR}/porting/espressif/ESP-NN" "*.S")
RECURSIVE_FIND_FILE_APPEND(EI_ASM_SOURCES
    "${EI_SDK_DIR}/porting/espressif/ESP-NN" "*.s")
RECURSIVE_FIND_FILE_APPEND(EI_ASM_SOURCES
    "${EI_SDK_DIR}/porting/espressif/esp-dsp/modules/fft" "*.S")
RECURSIVE_FIND_FILE_APPEND(EI_ASM_SOURCES
    "${EI_SDK_DIR}/porting/espressif/esp-dsp/modules/fft" "*.s")
RECURSIVE_FIND_FILE(EI_MODEL_SOURCES "${EI_MODEL_DIR}/tflite-model" "*.cpp")

set(EI_SOURCES
    ${EI_CPP_SOURCES}
    ${EI_CC_SOURCES}
    ${EI_C_SOURCES}
    ${EI_ASM_SOURCES}
    ${EI_MODEL_SOURCES})

idf_component_register(
    SRCS ${EI_SOURCES}
    INCLUDE_DIRS
        "${EI_MODEL_DIR}"
        "${EI_MODEL_DIR}/model-parameters"
        "${EI_MODEL_DIR}/tflite-model"
        "${EI_SDK_DIR}"
        "${EI_SDK_DIR}/porting"
        "${EI_SDK_DIR}/porting/espressif/ESP-NN"
        "${EI_SDK_DIR}/porting/espressif/ESP-NN/include"
        "${EI_SDK_DIR}/porting/espressif/ESP-NN/src/common"
        "${EI_SDK_DIR}/porting/espressif/esp-dsp/modules/common/include"
        "${EI_SDK_DIR}/porting/espressif/esp-dsp/modules/fft/include"
    REQUIRES freertos esp_timer heap)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=1
    EIDSP_USE_ESP_DSP=1)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -mlongcalls
    "-Wa,-I${EI_MODEL_DIR}"
    -fno-unroll-loops
    -Wno-unused-function
    -Wno-error=maybe-uninitialized
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
