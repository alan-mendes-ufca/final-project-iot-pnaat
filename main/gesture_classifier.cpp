#include "gesture_classifier.h"

#include <algorithm>
#include <cstring>

#include "sdkconfig.h"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

static_assert(EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME == GESTURE_CLASSIFIER_AXES,
              "O modelo precisa ter os seis eixos da IMU");
static_assert(EI_CLASSIFIER_RAW_SAMPLE_COUNT == GESTURE_CLASSIFIER_WINDOW_SAMPLES,
              "O modelo precisa usar uma janela de 100 amostras");
static_assert(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE == GESTURE_CLASSIFIER_WINDOW_VALUES,
              "O modelo precisa receber 600 valores por janela");
static_assert(EI_CLASSIFIER_LABEL_COUNT == GESTURE_CLASSIFIER_CLASS_COUNT,
              "A interface C espera exatamente cinco classes");
static_assert(EI_CLASSIFIER_FREQUENCY == 50,
              "O modelo exportado precisa ter sido treinado a 50 Hz");
static_assert(CONFIG_GESTURE_SAMPLE_RATE_HZ == EI_CLASSIFIER_FREQUENCY,
              "A taxa da IMU precisa coincidir com a taxa do modelo");

namespace {

float s_window[GESTURE_CLASSIFIER_WINDOW_VALUES];
size_t s_write_offset;
size_t s_values_available;
bool s_initialized;

int window_get_data(size_t offset, size_t length, float *out_ptr)
{
    if (out_ptr == nullptr || offset + length > GESTURE_CLASSIFIER_WINDOW_VALUES) {
        return ei::EIDSP_OUT_OF_BOUNDS;
    }

    const size_t logical_start =
        (s_write_offset + offset) % GESTURE_CLASSIFIER_WINDOW_VALUES;
    const size_t first_length =
        std::min(length, GESTURE_CLASSIFIER_WINDOW_VALUES - logical_start);

    std::memcpy(out_ptr, &s_window[logical_start], first_length * sizeof(float));
    if (first_length < length) {
        std::memcpy(out_ptr + first_length, s_window,
                    (length - first_length) * sizeof(float));
    }

    return ei::EIDSP_OK;
}

void append_slice(const float *values)
{
    const size_t first_length =
        std::min(static_cast<size_t>(GESTURE_CLASSIFIER_VALUES_PER_SLICE),
                 static_cast<size_t>(GESTURE_CLASSIFIER_WINDOW_VALUES) - s_write_offset);

    std::memcpy(&s_window[s_write_offset], values, first_length * sizeof(float));
    if (first_length < GESTURE_CLASSIFIER_VALUES_PER_SLICE) {
        std::memcpy(s_window, values + first_length,
                    (GESTURE_CLASSIFIER_VALUES_PER_SLICE - first_length) * sizeof(float));
    }

    s_write_offset =
        (s_write_offset + GESTURE_CLASSIFIER_VALUES_PER_SLICE) %
        GESTURE_CLASSIFIER_WINDOW_VALUES;
    s_values_available =
        std::min(static_cast<size_t>(GESTURE_CLASSIFIER_WINDOW_VALUES),
                 s_values_available + GESTURE_CLASSIFIER_VALUES_PER_SLICE);
}

} // namespace

extern "C" int gesture_classifier_init(void)
{
    s_initialized = true;
    gesture_classifier_reset();
    return GESTURE_CLASSIFIER_OK;
}

extern "C" void gesture_classifier_reset(void)
{
    s_write_offset = 0;
    s_values_available = 0;
}

extern "C" void gesture_classifier_deinit(void)
{
    gesture_classifier_reset();
    s_initialized = false;
}

extern "C" int gesture_classifier_run_slice(
    const float *values,
    size_t value_count,
    gesture_classifier_result_t *result)
{
    if (values == nullptr || result == nullptr ||
        value_count != GESTURE_CLASSIFIER_VALUES_PER_SLICE) {
        return GESTURE_CLASSIFIER_ERR_INVALID_ARGUMENT;
    }
    if (!s_initialized) {
        return GESTURE_CLASSIFIER_ERR_NOT_INITIALIZED;
    }

    std::memset(result, 0, sizeof(*result));
    append_slice(values);

    if (s_values_available < GESTURE_CLASSIFIER_WINDOW_VALUES) {
        return GESTURE_CLASSIFIER_OK;
    }

    signal_t signal;
    signal.total_length = GESTURE_CLASSIFIER_WINDOW_VALUES;
    signal.get_data = window_get_data;

    ei_impulse_result_t ei_result = {};
    const EI_IMPULSE_ERROR error = run_classifier(&signal, &ei_result, false);
    if (error != EI_IMPULSE_OK) {
        return static_cast<int>(error);
    }

    result->ready = true;
    for (size_t index = 0; index < GESTURE_CLASSIFIER_CLASS_COUNT; ++index) {
        const float probability = ei_result.classification[index].value;
        result->probabilities[index] = probability;
        if (index == 0 || probability > result->confidence) {
            result->best_class = index;
            result->confidence = probability;
        }
    }

    result->accepted = result->confidence >= EI_CLASSIFIER_THRESHOLD;
    result->dsp_us = static_cast<uint64_t>(ei_result.timing.dsp_us);
    result->classification_us =
        static_cast<uint64_t>(ei_result.timing.classification_us);
    result->postprocessing_us =
        static_cast<uint64_t>(ei_result.timing.postprocessing_us);

    return GESTURE_CLASSIFIER_OK;
}

extern "C" const char *gesture_classifier_label(size_t class_index)
{
    if (class_index >= GESTURE_CLASSIFIER_CLASS_COUNT) {
        return "unknown";
    }
    return ei_classifier_inferencing_categories[class_index];
}
