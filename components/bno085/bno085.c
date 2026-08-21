#include "bno085.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "euler.h"
#include "sh2.h"
#include "sh2_SensorValue.h"
#include "sh2_err.h"

static const char TAG[] = "bno085";

static bno085_dev_t s_dev;
static sh2_Hal_t s_hal;

/* Mapa do enum publico para os IDs do sh2. Fica aqui (e nao no header) porque
 * `sh2` e PRIV_REQUIRES deste componente -- quem inclui bno085.h nao ganha
 * sh2.h transitivamente. */
static const sh2_SensorId_t s_sensor_ids[BNO085_SENSOR_COUNT] = {
    [BNO085_SENSOR_ACCELEROMETER]   = SH2_ACCELEROMETER,
    [BNO085_SENSOR_GYROSCOPE]       = SH2_GYROSCOPE_CALIBRATED,
    [BNO085_SENSOR_ROTATION_VECTOR] = SH2_ROTATION_VECTOR,
};

/* Intervalo configurado por sensor; 0 = desabilitado. Precisa ser um array (e
 * nao um escalar) para que o reenable pos-SH2_RESET restaure TODOS os
 * relatorios ativos, nao so o ultimo que foi habilitado. */
static uint32_t s_enabled_interval_us[BNO085_SENSOR_COUNT];
static volatile bool s_need_reenable;

static bno085_orientation_t s_last_orientation;

/* accel e gyro chegam como relatorios SHTP independentes; guardamos o ultimo
 * de cada e so marcamos a amostra como valida quando os dois ja apareceram. */
static bno085_imu6_t s_imu6;
static bool s_have_accel;
static bool s_have_gyro;

/* Os callbacks do sh2 rodam na task que chama bno085_service(); os getters sao
 * chamados da task de amostragem, que e outra (ver main/imu_config.c). O
 * spinlock protege a copia dessas structs entre as duas. */
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- HAL: framing bruto SHTP sobre I2C, sem registrador ---- */

/*
 * O driver I2C legado do ESP-IDF não tem o limite de ~32 bytes por
 * transação que o Wire do Arduino tem, então ao contrário da HAL de
 * referência (Adafruit_BNO08x), não precisamos fatiar a leitura do
 * payload em vários pedaços: lemos o cabeçalho de 4 bytes pra descobrir
 * o tamanho, depois lemos o pacote inteiro (cabeçalho + payload) numa
 * segunda transação só.
 */
static uint8_t s_i2c_rx_buf[SH2_HAL_MAX_TRANSFER_IN];

/*
 * Datasheet oficial (CEVA, BNO080/85/86, secao "Host Interface"): depois
 * do reset o sensor NAO espera escrita nenhuma -- ele prepara o pacote
 * de "advertisement" SHTP e sinaliza isso baixando H_INTN; "the host may
 * begin communicating with the BNO08X after it has asserted H_INTN", e a
 * primeira transacao tem que ser uma LEITURA. Por isso todo o bring-up
 * (toggle de RESET + espera do INT) acontece em bno085_init(), ANTES de
 * chamar sh2_open() -- hal_open() nao faz I2C nenhum, so devolve OK.
 */
static int hal_open(sh2_Hal_t *self) {
    return 0;
}

static void hal_close(sh2_Hal_t *self) {
    if (s_dev.reset_gpio != GPIO_NUM_NC) {
        gpio_set_level(s_dev.reset_gpio, 0);
    }
}

static int hal_read(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len, uint32_t *t_us) {
    /* shtp_service() chama isso sem condicao nenhuma, a cada tick do
     * service loop -- e a HAL quem tem que respeitar o datasheet ("host
     * pode ler depois que H_INTN foi assertado"). Sem esse gate, a gente
     * fica martelando o barramento com uma leitura de verdade mesmo com
     * o sensor sem nada pra mandar. */
    if (s_dev.int_gpio != GPIO_NUM_NC && gpio_get_level(s_dev.int_gpio) != 0) {
        ESP_LOGD(TAG, "hal_read: INT alto (sem dado), pulando leitura");
        return 0;
    }

    esp_err_t err = i2c_master_read_from_device(s_dev.i2c_port, s_dev.i2c_addr,
                                                 s_i2c_rx_buf, 4, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "hal_read: falha lendo cabecalho: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t packet_size = (uint16_t)s_i2c_rx_buf[0] | ((uint16_t)s_i2c_rx_buf[1] << 8);
    packet_size &= ~0x8000; /* bit 15 = continuation, não usamos */

    if (packet_size == 0 || packet_size > len || packet_size > sizeof(s_i2c_rx_buf)) {
        ESP_LOGD(TAG, "hal_read: cabecalho com packet_size=%u (len=%u) -- descartando",
                 packet_size, len);
        return 0;
    }

    err = i2c_master_read_from_device(s_dev.i2c_port, s_dev.i2c_addr,
                                       s_i2c_rx_buf, packet_size, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "hal_read: falha lendo payload (%u bytes): %s", packet_size,
                 esp_err_to_name(err));
        return 0;
    }

    memcpy(pBuffer, s_i2c_rx_buf, packet_size);
    if (t_us) {
        *t_us = (uint32_t)esp_timer_get_time();
    }
    ESP_LOGD(TAG, "hal_read: pacote de %u bytes recebido", packet_size);
    return (int)packet_size;
}

static int hal_write(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len) {
    esp_err_t err = i2c_master_write_to_device(s_dev.i2c_port, s_dev.i2c_addr,
                                                pBuffer, len, pdMS_TO_TICKS(100));
    ESP_LOGD(TAG, "hal_write: %u bytes (report id 0x%02X) -- %s", len,
             len > 0 ? pBuffer[0] : 0, esp_err_to_name(err));
    return (err == ESP_OK) ? (int)len : 0;
}

static uint32_t hal_getTimeUs(sh2_Hal_t *self) {
    return (uint32_t)esp_timer_get_time();
}

/* ---- Callbacks sh2 ---- */

static void event_callback(void *cookie, sh2_AsyncEvent_t *event) {
    ESP_LOGD(TAG, "event_callback: eventId=%lu", (unsigned long)event->eventId);
    if (event->eventId == SH2_RESET) {
        ESP_LOGW(TAG, "BNO085 resetou, relatorios precisam ser reabilitados");
        s_need_reenable = true;
    }
}

static void sensor_callback(void *cookie, sh2_SensorEvent_t *event) {
    sh2_SensorValue_t value;
    if (sh2_decodeSensorEvent(&value, event) != SH2_OK) {
        ESP_LOGD(TAG, "sensor_callback: falha ao decodificar evento");
        return;
    }
    ESP_LOGD(TAG, "sensor_callback: sensorId=0x%02X", value.sensorId);

    switch (value.sensorId) {
    case SH2_ACCELEROMETER:
        taskENTER_CRITICAL(&s_state_mux);
        s_imu6.accel[0] = value.un.accelerometer.x;
        s_imu6.accel[1] = value.un.accelerometer.y;
        s_imu6.accel[2] = value.un.accelerometer.z;
        s_imu6.timestamp_us = value.timestamp;
        s_have_accel = true;
        s_imu6.valid = s_have_gyro;
        taskEXIT_CRITICAL(&s_state_mux);
        break;

    case SH2_GYROSCOPE_CALIBRATED:
        taskENTER_CRITICAL(&s_state_mux);
        s_imu6.gyro[0] = value.un.gyroscope.x;
        s_imu6.gyro[1] = value.un.gyroscope.y;
        s_imu6.gyro[2] = value.un.gyroscope.z;
        s_imu6.timestamp_us = value.timestamp;
        s_have_gyro = true;
        s_imu6.valid = s_have_accel;
        taskEXIT_CRITICAL(&s_state_mux);
        break;

    case SH2_ROTATION_VECTOR: {
        float roll, pitch, yaw;
        q_to_ypr(value.un.rotationVector.real, value.un.rotationVector.i,
                 value.un.rotationVector.j, value.un.rotationVector.k,
                 &roll, &pitch, &yaw);

        taskENTER_CRITICAL(&s_state_mux);
        s_last_orientation.angle_x = roll * (180.0f / (float)M_PI);
        s_last_orientation.angle_y = pitch * (180.0f / (float)M_PI);
        s_last_orientation.angle_z = yaw * (180.0f / (float)M_PI);
        s_last_orientation.accuracy_rad = value.un.rotationVector.accuracy;
        s_last_orientation.valid = true;
        taskEXIT_CRITICAL(&s_state_mux);
        break;
    }

    default:
        /* Relatorio que nao pedimos (ou que ainda estava na fila do SHTP
         * quando desabilitamos). Ignora. */
        break;
    }
}

/* ---- API pública ---- */

esp_err_t bno085_init(const bno085_dev_t *dev) {
    s_dev = *dev;
    memset(&s_last_orientation, 0, sizeof(s_last_orientation));
    memset(&s_imu6, 0, sizeof(s_imu6));
    memset(s_enabled_interval_us, 0, sizeof(s_enabled_interval_us));
    s_have_accel = false;
    s_have_gyro = false;
    s_need_reenable = false;

    /* Barramento I2C (dev->i2c_port) já vem inicializado pelo chamador
     * via i2c_config::initialize_i2c() -- este driver só faz device I/O
     * (ver hal_read/hal_write acima), nunca traz o barramento à vida. */

    if (s_dev.reset_gpio != GPIO_NUM_NC) {
        gpio_config_t reset_cfg = {
            .pin_bit_mask = 1ULL << s_dev.reset_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&reset_cfg);
    }
    if (s_dev.int_gpio != GPIO_NUM_NC) {
        gpio_config_t int_cfg = {
            .pin_bit_mask = 1ULL << s_dev.int_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&int_cfg);
    }

    /* Reset por hardware: LOW pulsa o reset, HIGH libera. Datasheet
     * pede so >=10ns de pulso -- 10ms e bem generoso/seguro. */
    if (s_dev.reset_gpio != GPIO_NUM_NC) {
        gpio_set_level(s_dev.reset_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(s_dev.reset_gpio, 1);
    }

    /* Espera H_INTN assertar (nivel baixo) -- so leitura de GPIO, nao
     * pode dar NACK, e distingue "sensor nunca respondeu" (problema de
     * alimentacao/RESET/INT) de "respondeu mas a leitura I2C falhou"
     * (problema de SDA/SCL) de forma bem mais clara que tentar uma
     * escrita as cegas. Timing do datasheet: init interno ~90ms +
     * config ~4ms -- 500ms da bastante folga. */
    if (s_dev.int_gpio != GPIO_NUM_NC) {
        const TickType_t timeout_ticks = pdMS_TO_TICKS(500);
        TickType_t start_tick = xTaskGetTickCount();
        bool int_asserted = false;
        while ((xTaskGetTickCount() - start_tick) < timeout_ticks) {
            if (gpio_get_level(s_dev.int_gpio) == 0) {
                int_asserted = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (!int_asserted) {
            ESP_LOGE(TAG,
                     "BNO085 nunca assertou INT apos reset (GPIO %d) -- "
                     "confira alimentacao 3V3/GND e a fiacao do "
                     "RESET (GPIO %d)/INT",
                     s_dev.int_gpio, s_dev.reset_gpio);
            return ESP_ERR_TIMEOUT;
        }
    }

    s_hal.open = hal_open;
    s_hal.close = hal_close;
    s_hal.read = hal_read;
    s_hal.write = hal_write;
    s_hal.getTimeUs = hal_getTimeUs;

    int status = sh2_open(&s_hal, event_callback, NULL);
    if (status != SH2_OK) {
        ESP_LOGE(TAG,
                 "sh2_open falhou (%d) -- INT respondeu mas a leitura I2C "
                 "falhou, confira SDA/SCL (porta %d), pull-up e o endereco "
                 "0x%02X (ADR em GND=0x4A, em 3V3=0x4B)",
                 status, s_dev.i2c_port, s_dev.i2c_addr);
        return ESP_FAIL;
    }

    sh2_setSensorCallback(sensor_callback, NULL);

    /* O handshake inicial de sh2_open() acima passa pelo mesmo
     * SH2_RESET que event_callback() usa pra saber que precisa
     * reabilitar relatorios -- mas nesse ponto nada foi habilitado
     * ainda (isso e feito depois, por bno085_enable_sensor()).
     * Zera aqui pra nao disparar um reenable redundante no primeiro
     * bno085_service(). */
    s_need_reenable = false;

    ESP_LOGI(TAG, "BNO085 inicializado (addr 0x%02X)", s_dev.i2c_addr);
    return ESP_OK;
}

/* Aplica a config no sensor sem mexer no bookkeeping -- usada tanto pelo
 * caminho publico quanto pelo reenable pos-reset. */
static esp_err_t apply_sensor_config(bno085_sensor_t sensor, uint32_t interval_us) {
    sh2_SensorConfig_t config = {0};
    config.reportInterval_us = interval_us;

    ESP_LOGD(TAG, "sh2_setSensorConfig: id=0x%02X interval_us=%lu",
             s_sensor_ids[sensor], (unsigned long)interval_us);
    int status = sh2_setSensorConfig(s_sensor_ids[sensor], &config);
    if (status != SH2_OK) {
        ESP_LOGE(TAG, "sh2_setSensorConfig(id=0x%02X) falhou: %d",
                 s_sensor_ids[sensor], status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t bno085_enable_sensor(bno085_sensor_t sensor, uint32_t interval_us) {
    if (sensor >= BNO085_SENSOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = apply_sensor_config(sensor, interval_us);
    if (err != ESP_OK) {
        return err;
    }

    s_enabled_interval_us[sensor] = interval_us;
    return ESP_OK;
}

esp_err_t bno085_enable_rotation_vector(uint32_t interval_us) {
    return bno085_enable_sensor(BNO085_SENSOR_ROTATION_VECTOR, interval_us);
}

void bno085_service(void) {
    ESP_LOGD(TAG, "bno085_service tick");
    sh2_service();

    if (!s_need_reenable) {
        return;
    }

    /* O reset limpa a config de TODOS os relatorios, entao restaura cada um
     * que estava ativo. So limpa a flag se todos voltarem -- se algum falhar,
     * tenta de novo no proximo service. */
    bool all_ok = true;
    for (int i = 0; i < BNO085_SENSOR_COUNT; i++) {
        if (s_enabled_interval_us[i] == 0) {
            continue;
        }
        ESP_LOGW(TAG, "Reabilitando relatorio 0x%02X apos reset assincrono",
                 s_sensor_ids[i]);
        if (apply_sensor_config((bno085_sensor_t)i, s_enabled_interval_us[i]) != ESP_OK) {
            all_ok = false;
        }
    }
    if (all_ok) {
        s_need_reenable = false;
    }
}

bool bno085_get_orientation(bno085_orientation_t *out) {
    bool valid;
    taskENTER_CRITICAL(&s_state_mux);
    valid = s_last_orientation.valid;
    if (valid) {
        *out = s_last_orientation;
    }
    taskEXIT_CRITICAL(&s_state_mux);
    return valid;
}

bool bno085_get_imu6(bno085_imu6_t *out) {
    bool valid;
    taskENTER_CRITICAL(&s_state_mux);
    valid = s_imu6.valid;
    if (valid) {
        *out = s_imu6;
    }
    taskEXIT_CRITICAL(&s_state_mux);
    return valid;
}
