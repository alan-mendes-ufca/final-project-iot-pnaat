/**
 * @file bno085.h
 * @author Flavian Melquiades (flavian.melquiades@gmail.com)
 * @brief Driver I2C para o BNO085, sobre a lib oficial de protocolo sh2
 *        (CEVA/Hillcrest). Ocupa o mesmo papel que components/mpu6050
 *        ocupava no projeto original.
 * @version 0.1
 * @date 2026-08-19
 *
 * @copyright Copyright (c) 2026
 *
 * Nota: o SH2 é uma lib de sessão única (estado global interno, sem handle
 * por instância) — por isso as funções abaixo não recebem um descriptor por
 * chamada como o mpu6050_*(dev, ...) fazia. O struct bno085_dev_t existe só
 * para deixar a assinatura de bno085_init() explícita sobre o que é
 * necessário configurar (porta/endereço I2C, pinos RESET/INT).
 *
 * Barramento: este driver NÃO inicializa I2C sozinho -- quem chama
 * bno085_init() precisa ter chamado components/i2c_config::initialize_i2c()
 * pra essa porta antes (ver README, seção "Modularidade"). Na Heltec
 * WiFi LoRa 32 V3 isso normalmente é I2C_NUM_1 com os pinos de
 * CONFIG_BNO085_SDA_GPIO/SCL_GPIO (Kconfig deste componente), já que
 * I2C_NUM_0/17/18 são o barramento interno do OLED.
 */
#ifndef __BNO085_H__
#define __BNO085_H__

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BNO085_I2C_ADDR_DEFAULT 0x4A /**< Pino ADR do sensor em GND */
#define BNO085_I2C_ADDR_ALT     0x4B /**< Pino ADR do sensor em 3V3 */

typedef struct {
    i2c_port_t i2c_port; /**< Porta já inicializada por i2c_config::initialize_i2c() */
    uint8_t i2c_addr;
    gpio_num_t reset_gpio; /**< GPIO_NUM_NC se não usar reset por hardware */
    gpio_num_t int_gpio;   /**< GPIO_NUM_NC se não usar pino INT */
} bno085_dev_t;

typedef struct {
    float angle_x; /**< Roll, graus */
    float angle_y; /**< Pitch, graus */
    float angle_z; /**< Yaw, graus */
    float accuracy_rad; /**< Estimativa de erro do rotation vector, radianos */
    bool valid;
} bno085_orientation_t;

/**
 * @brief Relatórios que este driver sabe habilitar e decodificar.
 *
 * Mapeiam para IDs do sh2 internamente -- o header do sh2 não é exposto aqui
 * porque `sh2` é PRIV_REQUIRES no CMakeLists deste componente.
 */
typedef enum {
    BNO085_SENSOR_ACCELEROMETER,   /**< Aceleração COM gravidade, m/s² */
    BNO085_SENSOR_GYROSCOPE,       /**< Giroscópio calibrado, rad/s */
    BNO085_SENSOR_ROTATION_VECTOR, /**< Quaternion fundido com magnetômetro */
    BNO085_SENSOR_COUNT,
} bno085_sensor_t;

/**
 * @brief Última amostra de 6 eixos (accel + gyro).
 *
 * A gravidade é mantida no acelerômetro de propósito: para classificar pose do
 * antebraço (digitando vs. guarda vs. repouso) a direção da gravidade é o sinal
 * discriminante. Usar aceleração linear aqui destruiria essa informação.
 */
typedef struct {
    float accel[3];        /**< m/s², com gravidade */
    float gyro[3];         /**< rad/s */
    uint32_t timestamp_us; /**< timestamp do relatório mais recente dos dois */
    bool valid;            /**< true só depois que accel E gyro chegaram */
} bno085_imu6_t;

/**
 * @brief Configura GPIOs de RESET/INT e abre a sessão SH2 sobre
 *        dev->i2c_port -- essa porta precisa já estar inicializada
 *        (ver nota de barramento acima).
 *
 * Não habilita nenhum relatório sozinho — chame
 * bno085_enable_sensor() depois, uma vez por relatório desejado.
 */
esp_err_t bno085_init(const bno085_dev_t *dev);

/**
 * @brief Habilita um relatório periódico do sensor.
 *
 * Pode ser chamada para vários sensores; todos ficam ativos simultaneamente e
 * são automaticamente reabilitados se o BNO085 resetar sozinho.
 *
 * @param sensor      qual relatório habilitar.
 * @param interval_us intervalo entre relatórios, em microssegundos
 *                    (ex.: 20000 = 50 Hz).
 */
esp_err_t bno085_enable_sensor(bno085_sensor_t sensor, uint32_t interval_us);

/**
 * @brief Atalho para bno085_enable_sensor(BNO085_SENSOR_ROTATION_VECTOR, ...).
 */
esp_err_t bno085_enable_rotation_vector(uint32_t interval_us);

/**
 * @brief Deve ser chamada periodicamente para processar dados pendentes do
 *        sensor e despachar callbacks internos do sh2. Sem chamar isso, o
 *        driver nunca atualiza a orientação lida.
 */
void bno085_service(void);

/**
 * @brief Copia a última orientação decodificada para *out.
 *
 * @return true se havia uma leitura válida, false se o sensor ainda não
 *         reportou nada.
 */
bool bno085_get_orientation(bno085_orientation_t *out);

/**
 * @brief Copia a última amostra de accel+gyro para *out.
 *
 * Requer BNO085_SENSOR_ACCELEROMETER e BNO085_SENSOR_GYROSCOPE habilitados.
 * Os dois chegam como relatórios SHTP independentes; esta função devolve o
 * último valor de cada um, sem tentar alinhá-los por timestamp -- para o
 * classificador, uniformidade da taxa de amostragem importa mais que
 * alinhamento exato entre os eixos.
 *
 * Seguro para chamar de uma task diferente da que roda bno085_service().
 *
 * @return true se accel e gyro já reportaram ao menos uma vez.
 */
bool bno085_get_imu6(bno085_imu6_t *out);

#ifdef __cplusplus
}
#endif

#endif // __BNO085_H__
