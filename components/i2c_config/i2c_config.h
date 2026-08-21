/**
 * @file i2c_config.h
 * @author Flavian Melquiades (flavian.melquiades@gmail.com)
 * @brief 
 * @version 0.1
 * @date 2025-03-31
 * 
 * @copyright Copyright (c) 2025
 * 
 */
#ifndef __I2C_CONFIG_H__
#define __I2C_CONFIG_H__

#define USE_I2C_MASTER  0

#include <stdint.h>

#include "driver/gpio.h"

#if USE_I2C_MASTER == 1
#include "driver/i2c_master.h"
#else
#include "driver/i2c.h"
#endif

/* Pinagem da Heltec WiFi LoRa 32 V3 (ESP32-S3FN8): OLED onboard usa
 * SDA=17/SCL=18/RST=21 (ver components/oled_setup) num barramento
 * INTERNO (I2C_NUM_0), que não sai no header. Esses defines são só o
 * default do OLED -- initialize_i2c() abaixo é genérica e serve pra
 * subir qualquer outro barramento (ex.: I2C_NUM_1 do BNO085, ver
 * components/bno085/Kconfig), bastando passar outros pinos/porta. */
#define PIN_NUM_SDA     17
#define PIN_NUM_SCL     18

#define I2C_HOST        I2C_NUM_0

/* O pino Vext (GPIO36) liga o rail de 3.3V que alimenta o OLED onboard
 * e qualquer sensor externo pendurado no Vext/Grove -- é sequenciamento
 * de energia da placa, não é "coisa de I2C". Por isso é função própria:
 * chame uma vez em app_main, antes de qualquer initialize_i2c(). */
#define VEXT_CTRL_PIN   36
void enable_vext_rail(void);

/* Velocidades de barramento. O OLED onboard roda folgado em standard mode;
 * o BNO085 precisa de fast mode porque accel+gyro a 50 Hz somam ~100 reports
 * SHTP por segundo, e cada leitura custa duas transacoes I2C (cabecalho de 4
 * bytes + pacote). A 100 kHz isso nao fecha o orcamento de 20 ms por amostra. */
#define I2C_SPEED_STANDARD_HZ   100000
#define I2C_SPEED_FAST_HZ       400000

#if USE_I2C_MASTER == 1
void initialize_i2c_at(i2c_master_bus_handle_t *i2c_bus, gpio_num_t sda_gpio, gpio_num_t scl_gpio, uint32_t clk_hz);
void initialize_i2c(i2c_master_bus_handle_t *i2c_bus, gpio_num_t sda_gpio, gpio_num_t scl_gpio);
#else
/** @brief Sobe um barramento I2C na velocidade indicada. */
void initialize_i2c_at(i2c_port_t *i2c_bus, gpio_num_t sda_gpio, gpio_num_t scl_gpio, uint32_t clk_hz);
/** @brief Equivalente a initialize_i2c_at() com I2C_SPEED_STANDARD_HZ. */
void initialize_i2c(i2c_port_t *i2c_bus, gpio_num_t sda_gpio, gpio_num_t scl_gpio);
#endif

#endif // __I2C_CONFIG_H__