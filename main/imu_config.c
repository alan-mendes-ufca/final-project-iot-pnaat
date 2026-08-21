#include "imu_config.h"

#include <math.h>

#include "esp_log.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bno085.h"
#include "i2c_config.h"

static const char TAG[] = "imu_config";

/* 50 Hz -> 20000 us de intervalo de relatorio, 20 ms de periodo de amostragem. */
#define SAMPLE_RATE_HZ     CONFIG_GESTURE_SAMPLE_RATE_HZ
#define SAMPLE_PERIOD_MS   (1000 / SAMPLE_RATE_HZ)
#define REPORT_INTERVAL_US (1000000 / SAMPLE_RATE_HZ)

/* O sensor produz 2 relatorios por periodo (accel + gyro). Servir o SHTP a
 * 500 Hz da uma ordem de grandeza de folga; como hal_read() so toca o
 * barramento quando H_INTN esta baixo, os ticks ociosos custam um gpio_get_level. */
#define SERVICE_PERIOD_MS 2

static imu_sample_cb_t s_cb;
static void *s_cb_ctx;
static volatile uint32_t s_sample_count;

static TaskHandle_t s_service_task;
static TaskHandle_t s_sample_task;

/* Task de servico: existe so pra drenar o SHTP o mais rapido possivel. Fica
 * numa prioridade acima da task de amostragem porque um relatorio nao drenado
 * a tempo e um relatorio perdido. */
static void imu_service_task(void *arg)
{
    (void)arg;
    for (;;) {
        bno085_service();
        vTaskDelay(pdMS_TO_TICKS(SERVICE_PERIOD_MS));
    }
}

/* Task de amostragem: relogio fixo, independente da chegada dos relatorios. */
static void imu_sample_task(void *arg)
{
    (void)arg;

    const TickType_t period = pdMS_TO_TICKS(SAMPLE_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();
    bool warned_no_data = false;

    for (;;) {
        bno085_imu6_t imu;
        if (bno085_get_imu6(&imu)) {
            const float sample[IMU_SAMPLE_AXES] = {
                imu.accel[0], imu.accel[1], imu.accel[2],
                imu.gyro[0],  imu.gyro[1],  imu.gyro[2],
            };
            s_sample_count++;
            s_cb(sample, s_cb_ctx);
            warned_no_data = false;
        } else if (!warned_no_data) {
            /* So uma vez por "apagao" -- num loop de 50 Hz isso viraria spam
             * no UART, que no modo de coleta corromperia o stream CSV. */
            ESP_LOGW(TAG, "BNO085 ainda sem accel+gyro validos");
            warned_no_data = true;
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

uint32_t imu_config_sample_count(void)
{
    return s_sample_count;
}

esp_err_t imu_config_init(imu_sample_cb_t cb, void *ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cb = cb;
    s_cb_ctx = ctx;

    ESP_LOGI(TAG, "Inicializando IMU (%d Hz, intervalo de relatorio %d us)",
             SAMPLE_RATE_HZ, REPORT_INTERVAL_US);

    /* Barramento proprio, em fast mode: a 100 kHz os ~100 relatorios/s de
     * accel+gyro nao fecham o orcamento de 20 ms por amostra. */
    i2c_port_t bno_i2c_port = I2C_NUM_1;
    initialize_i2c_at(&bno_i2c_port, CONFIG_BNO085_SDA_GPIO,
                      CONFIG_BNO085_SCL_GPIO, I2C_SPEED_FAST_HZ);

    bno085_dev_t dev = {
        .i2c_port = bno_i2c_port,
        .i2c_addr = CONFIG_BNO085_I2C_ADDR,
        .reset_gpio = CONFIG_BNO085_RESET_GPIO,
        .int_gpio = CONFIG_BNO085_INT_GPIO,
    };

    /* Sensor externo em bancada -- fio solto/mau contato na primeira
     * tentativa e funciona na segunda e comum, por isso algumas tentativas
     * antes de desistir de vez. */
    const int max_attempts = 3;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        err = bno085_init(&dev);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "IMU init tentativa %d/%d falhou: %s", attempt,
                 max_attempts, esp_err_to_name(err));
        if (attempt < max_attempts) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro inicializando a IMU: %s", esp_err_to_name(err));
        return err;
    }

    /* Acelerometro COM gravidade (nao linear acceleration): a direcao da
     * gravidade e o que separa pose de antebraco -- digitando vs. guarda vs.
     * repouso. Ver o comentario em bno085.h. */
    err = bno085_enable_sensor(BNO085_SENSOR_ACCELEROMETER, REPORT_INTERVAL_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro habilitando acelerometro: %s", esp_err_to_name(err));
        return err;
    }
    err = bno085_enable_sensor(BNO085_SENSOR_GYROSCOPE, REPORT_INTERVAL_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro habilitando giroscopio: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(imu_service_task, "imu_service", 4 * 1024, NULL, 6,
                    &s_service_task) != pdPASS) {
        ESP_LOGE(TAG, "Nao foi possivel criar a task de servico");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(imu_sample_task, "imu_sample", 4 * 1024, NULL, 5,
                    &s_sample_task) != pdPASS) {
        ESP_LOGE(TAG, "Nao foi possivel criar a task de amostragem");
        vTaskDelete(s_service_task);
        s_service_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "IMU inicializada");
    return ESP_OK;
}
