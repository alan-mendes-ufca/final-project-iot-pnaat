/**
 * @file imu_config.h
 * @brief Aquisicao do BNO085 a taxa fixa para alimentar o classificador.
 *
 * Sobe o barramento I2C da IMU (I2C_NUM_1, separado do OLED), inicializa o
 * BNO085, habilita accel+gyro e cria duas tasks:
 *
 *   - task de servico   -- so drena o SHTP (bno085_service()), alta prioridade
 *   - task de amostragem -- entrega exatamente CONFIG_GESTURE_SAMPLE_RATE_HZ
 *                           amostras por segundo ao callback
 *
 * Separar as duas e o que torna a taxa uniforme: os relatorios de accel e gyro
 * chegam de forma assincrona e independente, mas o Edge Impulse assume
 * amostragem periodica. A task de amostragem le "o ultimo valor de cada" num
 * relogio fixo, em vez de emitir uma amostra a cada relatorio que chega.
 */
#ifndef __IMU_CONFIG_H__
#define __IMU_CONFIG_H__

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Eixos por amostra: accel(x,y,z) + gyro(x,y,z). */
#define IMU_SAMPLE_AXES 6

/**
 * @brief Chamado pela task de amostragem, uma vez por amostra.
 *
 * @param sample  vetor de IMU_SAMPLE_AXES floats, na ordem
 *                ax, ay, az (m/s², com gravidade), gx, gy, gz (rad/s).
 *                Valido so durante a chamada.
 * @param ctx     o mesmo ponteiro passado a imu_config_init().
 */
typedef void (*imu_sample_cb_t)(const float *sample, void *ctx);

/**
 * @brief Inicializa a IMU e comeca a amostrar.
 *
 * Requer enable_vext_rail() ja chamado. Sobe o proprio barramento I2C
 * (I2C_NUM_1, pinos de CONFIG_BNO085_*), entao nao depende do barramento do
 * OLED nem o perturba.
 *
 * @param cb   callback de amostra; nao pode ser NULL.
 * @param ctx  repassado ao callback.
 */
esp_err_t imu_config_init(imu_sample_cb_t cb, void *ctx);

/**
 * @brief Numero de amostras entregues desde o boot.
 *
 * Serve para conferir a taxa efetiva (deve crescer
 * CONFIG_GESTURE_SAMPLE_RATE_HZ por segundo).
 */
uint32_t imu_config_sample_count(void);

#ifdef __cplusplus
}
#endif

#endif // __IMU_CONFIG_H__
