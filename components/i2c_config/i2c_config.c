#include "i2c_config.h"

#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char TAG[] = "i2c_config";

void enable_vext_rail(void)
{
    static bool s_vext_enabled = false;
    if (s_vext_enabled) {
        return;
    }

    gpio_config_t vext_conf = {
        .pin_bit_mask = 1ULL << VEXT_CTRL_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&vext_conf);
    gpio_set_level(VEXT_CTRL_PIN, 0); /* Vext liga em nivel baixo */
    vTaskDelay(pdMS_TO_TICKS(50));    /* tempo pro rail estabilizar */

    s_vext_enabled = true;
}

#if USE_I2C_MASTER == 1
void initialize_i2c_at(i2c_master_bus_handle_t *i2c_bus, gpio_num_t sda_gpio, gpio_num_t scl_gpio, uint32_t clk_hz)
{
    /* No driver i2c_master a velocidade e por dispositivo, nao por barramento,
     * entao clk_hz nao se aplica aqui -- fica so pra manter a mesma assinatura
     * dos dois ramos. */
    (void)clk_hz;

    ESP_LOGI(TAG, "Initialize I2C bus");

    // i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_HOST,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, i2c_bus));

    ESP_LOGI(TAG, "Initialize I2C bus done");
}
#else
void initialize_i2c_at(i2c_port_t *i2c_bus, gpio_num_t sda_gpio, gpio_num_t scl_gpio, uint32_t clk_hz)
{
    ESP_LOGI(TAG, "Initialize I2C bus (port %d, SDA %d, SCL %d, %" PRIu32 " Hz)",
             (int)*i2c_bus, (int)sda_gpio, (int)scl_gpio, clk_hz);

    // i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = clk_hz,
        .clk_flags = 0,
    };
    ESP_ERROR_CHECK(i2c_param_config(*i2c_bus, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(*i2c_bus, conf.mode, 0, 0, 0));

    ESP_LOGI(TAG, "Initialize I2C bus done");
}
#endif

/* Mantem o call site historico funcionando: quem nao se importa com a
 * velocidade continua chamando initialize_i2c() e ganha standard mode. */
#if USE_I2C_MASTER == 1
void initialize_i2c(i2c_master_bus_handle_t *i2c_bus, gpio_num_t sda_gpio, gpio_num_t scl_gpio)
#else
void initialize_i2c(i2c_port_t *i2c_bus, gpio_num_t sda_gpio, gpio_num_t scl_gpio)
#endif
{
    initialize_i2c_at(i2c_bus, sda_gpio, scl_gpio, I2C_SPEED_STANDARD_HZ);
}
