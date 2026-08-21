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
#include <math.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "i2c_config.h"
#include "imu_config.h"

#if CONFIG_GESTURE_MODE_INFER
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

/*
 * Enquanto o modelo do Edge Impulse nao estiver integrado (Fase 5), o modo de
 * inferencia mostra os modulos de accel/gyro e a taxa efetiva. Isso ja valida
 * na pratica o que a coleta depende: sensor preso na orientacao certa,
 * reportando, e na taxa configurada.
 */
static volatile float s_accel_mag;
static volatile float s_gyro_mag;

static void infer_sample_cb(const float *sample, void *ctx)
{
    (void)ctx;
    s_accel_mag = sqrtf(sample[0] * sample[0] + sample[1] * sample[1] +
                        sample[2] * sample[2]);
    s_gyro_mag = sqrtf(sample[3] * sample[3] + sample[4] * sample[4] +
                       sample[5] * sample[5]);
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
#endif

    /* A IMU sobe o proprio barramento (I2C_NUM_1), ver imu_config.c. */
#if CONFIG_GESTURE_MODE_COLLECT
    esp_err_t err = imu_config_init(collect_sample_cb, NULL);
#else
    esp_err_t err = imu_config_init(infer_sample_cb, NULL);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha inicializando a IMU: %s", esp_err_to_name(err));
        return;
    }

#if CONFIG_GESTURE_MODE_COLLECT
    /* Nada a fazer: a task de amostragem imprime sozinha. */
    vTaskSuspend(NULL);
#else
    ESP_LOGI(TAG, "Entrando no loop principal...");

    uint32_t last_count = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t count = imu_config_sample_count();
        uint32_t rate = count - last_count;
        last_count = count;

        printf_oled("acc=%.1f\ngyr=%.2f\n%" PRIu32 " Hz\n",
                    (double)s_accel_mag, (double)s_gyro_mag, rate);
        ESP_LOGI(TAG, "|accel|=%.2f m/s2  |gyro|=%.2f rad/s  taxa=%" PRIu32 " Hz",
                 (double)s_accel_mag, (double)s_gyro_mag, rate);
    }
#endif
}
