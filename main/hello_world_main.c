/**
 * @file hello_world_main.c
 * @brief Detector de gestos de pulso -- BNO085 + Edge Impulse no ESP32-S3.
 *
 * Dois modos, escolhidos em `idf.py menuconfig` -> "Detector de gestos":
 *
 *   COLLECT -- imprime "ax,ay,az,gx,gy,gz" no UART a 50 Hz, uma linha por
 *              amostra, para o `edge-impulse-data-forwarder` consumir.
 *   INFER   -- classifica a janela deslizante e mostra o gesto no OLED.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "i2c_config.h"
#include "imu_config.h"

#if CONFIG_GESTURE_MODE_INFER
#include "gesture_classifier.h"
#include "oled_printf.h"
#include "oled_setup.h"
#endif

static const char TAG[] = "main";

#if CONFIG_GESTURE_MODE_COLLECT

/*
 * O data-forwarder faz auto-deteccao do numero de eixos lendo as primeiras
 * linhas, entao o UART tem que conter SO isto -- qualquer ESP_LOGx no meio
 * quebra a deteccao. O log e silenciado em app_main() antes de a amostragem
 * comecar.
 */
static void collect_sample_cb(const float *sample, void *ctx)
{
    (void)ctx;
    printf("%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
           sample[0], sample[1], sample[2],
           sample[3], sample[4], sample[5]);
}

#else /* CONFIG_GESTURE_MODE_INFER */

#define INFERENCE_QUEUE_DEPTH      2
#define INFERENCE_TASK_STACK_BYTES (16 * 1024)
#define INFERENCE_TASK_PRIORITY    4

typedef struct {
    uint32_t sequence;
    float values[GESTURE_CLASSIFIER_VALUES_PER_SLICE];
} gesture_slice_t;

static QueueHandle_t s_inference_queue;
static TaskHandle_t s_inference_task;
static gesture_slice_t s_pending_slice;
static size_t s_pending_sample_count;
static uint32_t s_next_sequence;
static volatile uint32_t s_dropped_slices;

static void infer_sample_cb(const float *sample, void *ctx)
{
    (void)ctx;

    const size_t offset = s_pending_sample_count * GESTURE_CLASSIFIER_AXES;
    memcpy(&s_pending_slice.values[offset], sample,
           GESTURE_CLASSIFIER_AXES * sizeof(float));
    s_pending_sample_count++;

    if (s_pending_sample_count == GESTURE_CLASSIFIER_SAMPLES_PER_SLICE) {
        s_pending_slice.sequence = s_next_sequence++;
        if (xQueueSend(s_inference_queue, &s_pending_slice, 0) != pdPASS) {
            s_dropped_slices++;
        }
        s_pending_sample_count = 0;
    }
}

static void log_classifier_result(uint32_t sequence,
                                  const gesture_classifier_result_t *result)
{
    ESP_LOGI(TAG,
             "seq=%" PRIu32 " guard=%.3f handshake=%.3f idle=%.3f "
             "typing=%.3f wave=%.3f vencedor=%s conf=%.1f%% "
             "DSP=%" PRIu64 "us NN=%" PRIu64 "us pos=%" PRIu64 "us",
             sequence,
             (double)result->probabilities[0],
             (double)result->probabilities[1],
             (double)result->probabilities[2],
             (double)result->probabilities[3],
             (double)result->probabilities[4],
             gesture_classifier_label(result->best_class),
             (double)(result->confidence * 100.0f),
             result->dsp_us,
             result->classification_us,
             result->postprocessing_us);
}

static void inference_task(void *arg)
{
    (void)arg;

    gesture_slice_t slice;
    uint32_t previous_sequence = 0;
    uint32_t consecutive_slices = 0;
    uint32_t reported_dropped_slices = 0;
    bool has_previous = false;

    for (;;) {
        if (xQueueReceive(s_inference_queue, &slice, portMAX_DELAY) != pdPASS) {
            continue;
        }

        if (has_previous && slice.sequence != previous_sequence + 1U) {
            const uint32_t missing = slice.sequence - previous_sequence - 1U;
            ESP_LOGW(TAG,
                     "Descontinuidade: %" PRIu32 " fatia(s) perdida(s), "
                     "reiniciando janela",
                     missing);
            gesture_classifier_reset();
            consecutive_slices = 0;
        }

        previous_sequence = slice.sequence;
        has_previous = true;

        gesture_classifier_result_t result;
        const int error = gesture_classifier_run_slice(
            slice.values, GESTURE_CLASSIFIER_VALUES_PER_SLICE, &result);
        if (error != GESTURE_CLASSIFIER_OK) {
            ESP_LOGE(TAG, "Falha na inferencia (codigo %d)", error);
            gesture_classifier_reset();
            consecutive_slices = 0;
            printf_oled("erro de\ninferencia");
            continue;
        }

        if (consecutive_slices < GESTURE_CLASSIFIER_SLICES_PER_WINDOW) {
            consecutive_slices++;
        }

        if (!result.ready) {
            printf_oled("Coletando janela\n%" PRIu32 "/%u",
                        consecutive_slices,
                        GESTURE_CLASSIFIER_SLICES_PER_WINDOW);
            continue;
        }

        const char *display_label = result.accepted
                                        ? gesture_classifier_label(result.best_class)
                                        : "incerto";
        printf_oled("%s\nconf: %.0f%%", display_label,
                    (double)(result.confidence * 100.0f));
        log_classifier_result(slice.sequence, &result);

        const uint32_t dropped = s_dropped_slices;
        if (dropped != reported_dropped_slices) {
            ESP_LOGW(TAG, "Total de fatias descartadas pela fila: %" PRIu32,
                     dropped);
            reported_dropped_slices = dropped;
        }
    }
}

#endif

void app_main(void)
{
#if CONFIG_GESTURE_MODE_COLLECT
    /* Antes de qualquer outra coisa: a partir daqui o UART e do stream CSV. */
    esp_log_level_set("*", ESP_LOG_NONE);
#endif

    /* oled_setup le a rotacao gravada no NVS; sem isso ele falha com
     * ESP_ERR_NVS_NOT_INITIALIZED e loga erro no boot. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    enable_vext_rail(); /* rail de energia da placa, antes de qualquer I2C */

#if CONFIG_GESTURE_MODE_INFER
    i2c_port_t oled_port = I2C_NUM_0;
    initialize_i2c(&oled_port, PIN_NUM_SDA, PIN_NUM_SCL); /* barramento do OLED */
    configure_oled_screen(&oled_port);
    oled_printf_init(local_disp);

    s_inference_queue = xQueueCreate(INFERENCE_QUEUE_DEPTH,
                                     sizeof(gesture_slice_t));
    if (s_inference_queue == NULL) {
        ESP_LOGE(TAG, "Nao foi possivel criar a fila de inferencia");
        printf_oled("erro: fila");
        return;
    }

    const int classifier_error = gesture_classifier_init();
    if (classifier_error != GESTURE_CLASSIFIER_OK) {
        ESP_LOGE(TAG, "Nao foi possivel inicializar o classificador: %d",
                 classifier_error);
        vQueueDelete(s_inference_queue);
        s_inference_queue = NULL;
        printf_oled("erro: modelo");
        return;
    }

    if (xTaskCreate(inference_task, "gesture_infer",
                    INFERENCE_TASK_STACK_BYTES, NULL,
                    INFERENCE_TASK_PRIORITY, &s_inference_task) != pdPASS) {
        ESP_LOGE(TAG, "Nao foi possivel criar a task de inferencia");
        gesture_classifier_deinit();
        vQueueDelete(s_inference_queue);
        s_inference_queue = NULL;
        printf_oled("erro: task");
        return;
    }
#endif

    /* A IMU sobe o proprio barramento (I2C_NUM_1), ver imu_config.c. */
#if CONFIG_GESTURE_MODE_COLLECT
    esp_err_t err = imu_config_init(collect_sample_cb, NULL);
#else
    esp_err_t err = imu_config_init(infer_sample_cb, NULL);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha inicializando a IMU: %s", esp_err_to_name(err));
#if CONFIG_GESTURE_MODE_INFER
        vTaskDelete(s_inference_task);
        s_inference_task = NULL;
        gesture_classifier_deinit();
        vQueueDelete(s_inference_queue);
        s_inference_queue = NULL;
#endif
        return;
    }

#if CONFIG_GESTURE_MODE_COLLECT
    /* Nada a fazer: a task de amostragem imprime sozinha. */
    vTaskSuspend(NULL);
#else
    ESP_LOGI(TAG, "Classificador iniciado; aguardando a primeira janela");
    vTaskSuspend(NULL);
#endif
}
