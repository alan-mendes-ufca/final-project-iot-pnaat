/**
 * @file gesture_classifier.h
 * @brief Interface C para o classificador C++ exportado pelo Edge Impulse.
 */
#ifndef GESTURE_CLASSIFIER_H
#define GESTURE_CLASSIFIER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GESTURE_CLASSIFIER_AXES              6U
#define GESTURE_CLASSIFIER_SAMPLES_PER_SLICE 25U
#define GESTURE_CLASSIFIER_VALUES_PER_SLICE  \
    (GESTURE_CLASSIFIER_AXES * GESTURE_CLASSIFIER_SAMPLES_PER_SLICE)
#define GESTURE_CLASSIFIER_WINDOW_SAMPLES    100U
#define GESTURE_CLASSIFIER_WINDOW_VALUES     \
    (GESTURE_CLASSIFIER_AXES * GESTURE_CLASSIFIER_WINDOW_SAMPLES)
#define GESTURE_CLASSIFIER_CLASS_COUNT       5U
#define GESTURE_CLASSIFIER_SLICES_PER_WINDOW \
    (GESTURE_CLASSIFIER_WINDOW_SAMPLES / GESTURE_CLASSIFIER_SAMPLES_PER_SLICE)

enum {
    GESTURE_CLASSIFIER_OK = 0,
    GESTURE_CLASSIFIER_ERR_INVALID_ARGUMENT = -1000,
    GESTURE_CLASSIFIER_ERR_NOT_INITIALIZED = -1001,
};

typedef struct {
    float probabilities[GESTURE_CLASSIFIER_CLASS_COUNT];
    size_t best_class;
    float confidence;
    bool ready;
    bool accepted;
    uint64_t dsp_us;
    uint64_t classification_us;
    uint64_t postprocessing_us;
} gesture_classifier_result_t;

/** Inicializa e esvazia a janela do classificador. */
int gesture_classifier_init(void);

/** Descarta o contexto temporal acumulado. */
void gesture_classifier_reset(void);

/** Libera o contexto logico do classificador. */
void gesture_classifier_deinit(void);

/**
 * Adiciona uma fatia de 25 amostras. A inferencia comeca quando quatro fatias
 * consecutivas preencherem a janela de 100 amostras.
 */
int gesture_classifier_run_slice(const float *values,
                                 size_t value_count,
                                 gesture_classifier_result_t *result);

/** Retorna o nome da classe, ou "unknown" para um indice invalido. */
const char *gesture_classifier_label(size_t class_index);

#ifdef __cplusplus
}
#endif

#endif /* GESTURE_CLASSIFIER_H */
