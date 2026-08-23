# Detector de Gestos com ESP32-S3

Este projeto implementa um detector de gestos de pulso embarcado em uma
**Heltec WiFi LoRa 32 V3**, baseada no **ESP32-S3**. Um sensor inercial
**BNO085** fornece dados de aceleração e velocidade angular, enquanto um modelo
treinado no **Edge Impulse** classifica a atividade executada pelo usuário.

O firmware foi desenvolvido com **ESP-IDF**, **FreeRTOS**, C e C++. Ele pode
operar tanto como coletor de dados para treinamento quanto como classificador
local. No modo de inferência, todo o processamento ocorre no microcontrolador e
o resultado é mostrado no OLED da própria placa.

O modelo incluído reconhece cinco classes: `guard`, `handshake`, `idle`,
`typing` e `wave`.

## Como o projeto funciona

A execução pode ser entendida em sete partes:

1. Inicialização da placa, dos barramentos I²C e do BNO085
2. Aquisição periódica dos seis eixos da IMU
3. Seleção entre coleta de dados e inferência local
4. Agrupamento das amostras em fatias e janela deslizante
5. Extração de características e classificação com Edge Impulse
6. Controle de continuidade entre aquisição e inferência
7. Exibição do gesto no OLED e registro das probabilidades no terminal

## Arquitetura

O firmware separa aquisição, processamento e apresentação. Essa divisão evita
que o tempo gasto pela rede neural altere o ritmo de leitura do sensor.

```text
┌──────────────┐     I²C 1      ┌─────────────────────────────┐
│   BNO085     │ ─────────────► │ Serviço SHTP + amostragem   │
│ accel + gyro │                │ periódica a 50 Hz           │
└──────────────┘                └──────────────┬──────────────┘
                                              │ 6 valores
                         ┌────────────────────┴────────────────────┐
                         │                                         │
                         ▼                                         ▼
               ┌───────────────────┐                    ┌──────────────────┐
               │ Modo COLLECT      │                    │ Modo INFER       │
               │ CSV pela UART     │                    │ fila de fatias   │
               └─────────┬─────────┘                    └────────┬─────────┘
                         │                                       │
                         ▼                                       ▼
               Edge Impulse Data                    janela de 100 amostras
                   Forwarder                         + modelo TFLite Micro
                                                                 │
                                                                 ▼
                                                       OLED pelo I²C 0
                                                       + log pela UART
```

O OLED interno utiliza o `I2C_NUM_0`. O BNO085 fica em um barramento separado,
o `I2C_NUM_1`, para não interferir na tela e para suportar a taxa combinada dos
relatórios de acelerômetro e giroscópio.

## Hardware e ligações

O projeto foi configurado para os seguintes componentes:

- Heltec WiFi LoRa 32 V3 com ESP32-S3FN8 e 8 MB de flash;
- sensor inercial BNO085;
- OLED integrado à placa Heltec;
- conexão USB para gravação, monitoramento e coleta serial.

Ligação padrão do BNO085:

- `SDA` → `GPIO 6`;
- `SCL` → `GPIO 7`;
- `RST` → `GPIO 4`;
- `INT` → `GPIO 5`;
- `VCC` → `3V3`;
- `GND` → `GND`;
- `ADR` em `GND` → endereço I²C `0x4A`.

O OLED integrado usa `SDA = GPIO 17`, `SCL = GPIO 18` e `RST = GPIO 21`.
Esses pinos não devem ser reutilizados pelo sensor externo. Os valores do
BNO085 podem ser alterados em `idf.py menuconfig`.

Para manter a orientação usada durante a coleta, o sensor deve ficar preso ao
dorso do pulso, sem girar, com o eixo X apontando para os dedos. A direção da
gravidade ajuda o modelo a distinguir poses como `guard`, `typing` e `idle`.

## Organização de pastas e arquivos

```text
final-project-iot/
├── components/
│   ├── bno085/             # driver e configuração do sensor
│   ├── edge_impulse/       # integração do SDK e do modelo no ESP-IDF
│   ├── i2c_config/         # inicialização dos barramentos I²C e Vext
│   ├── oled_printf/        # escrita formatada no display
│   ├── oled_setup/         # configuração do OLED com LVGL
│   └── sh2/                # protocolo SH-2/SHTP usado pelo BNO085
├── main/
│   ├── hello_world_main.c  # ponto de entrada e coordenação das tasks
│   ├── imu_config.c        # aquisição uniforme de accel + gyro
│   ├── imu_config.h        # contrato de amostragem da IMU
│   ├── gesture_classifier.cpp # adaptador C++ para o Edge Impulse
│   ├── gesture_classifier.h   # interface C do classificador
│   ├── Kconfig.projbuild   # seleção dos modos COLLECT e INFER
│   └── CMakeLists.txt
├── ml/edge-impulse/hand-gestures-project/
│   ├── edge-impulse-sdk/   # runtime exportado pelo Edge Impulse
│   ├── model-parameters/   # metadados, eixos e classes
│   └── tflite-model/       # modelo quantizado integrado ao firmware
├── partitions.csv          # partição de aplicação com 3 MB
├── sdkconfig.defaults      # configuração-base para o ESP32-S3
└── CMakeLists.txt
```

## Papel dos arquivos principais

### `main/hello_world_main.c`

É o ponto de entrada do firmware. A função `app_main()` inicializa o NVS, liga
o barramento de alimentação Vext e escolhe o fluxo correspondente ao modo de
compilação.

No modo de inferência, esse arquivo também:

- configura o OLED;
- inicializa o classificador;
- cria a fila de fatias;
- inicia a task de inferência;
- encaminha cada amostra recebida da IMU;
- reinicia a janela se detectar perda de fatias;
- mostra o resultado no OLED e no terminal.

### `main/imu_config.c`

Inicializa o BNO085, habilita acelerômetro e giroscópio e cria duas tasks do
FreeRTOS:

- `imu_service_task` atende o protocolo SHTP a cada 2 ms;
- `imu_sample_task` entrega uma amostra em relógio fixo, normalmente a 50 Hz.

Os relatórios do BNO085 chegam de forma assíncrona. A task de amostragem usa o
valor mais recente de cada sensor para produzir um fluxo uniforme, como o
modelo espera.

Cada amostra respeita esta ordem:

```c
{ ax, ay, az, gx, gy, gz }
```

As acelerações são expressas em `m/s²`, com a gravidade, e as velocidades
angulares em `rad/s`.

### `main/gesture_classifier.cpp`

Adapta a API C++ do Edge Impulse para uma interface que pode ser chamada pelo
código C da aplicação. O arquivo mantém uma janela circular, chama
`run_classifier()` e transforma a resposta do SDK em uma estrutura com:

- probabilidade de cada classe;
- classe vencedora;
- confiança;
- indicação de resultado pronto e aceito;
- tempos de DSP, classificação e pós-processamento.

As verificações `static_assert` impedem a compilação quando o novo modelo não
respeita o contrato esperado: seis eixos, 100 amostras, cinco classes e 50 Hz.

### `components/edge_impulse/CMakeLists.txt`

Inclui o SDK, o modelo TFLite e as implementações otimizadas de ESP-NN e
ESP-DSP apenas no modo de inferência. No modo de coleta, o modelo não entra na
imagem final.

## Aquisição dos dados

O acelerômetro e o giroscópio são configurados para produzir relatórios a 50
Hz. Em cada período de 20 ms, o firmware entrega ao consumidor seis valores:

```text
ax, ay, az, gx, gy, gz
```

O callback de aquisição é independente do destino. Assim, o mesmo caminho do
BNO085 alimenta o stream CSV no modo de coleta e a fila do classificador no
modo de inferência.

## Modo de coleta

O modo `COLLECT` prepara o firmware para gerar o conjunto de dados usado no
treinamento. Todos os logs do ESP-IDF são silenciados e a UART passa a conter
somente linhas CSV:

```text
0.1234,-9.7891,0.4412,0.0021,-0.0003,0.0011
```

Essa restrição é importante porque mensagens extras fariam o Edge Impulse Data
Forwarder detectar incorretamente o número de eixos.

Com o firmware gravado e o monitor serial fechado, execute:

```bash
edge-impulse-data-forwarder --frequency 50
```

Quando solicitado, informe os nomes dos eixos exatamente nesta ordem:

```text
accX,accY,accZ,gyrX,gyrY,gyrZ
```

## Modo de inferência

O modo `INFER` acumula 25 amostras por fatia. Cada fatia contém 150 valores e é
enviada, sem espera, para uma fila de profundidade 2.

```c
#define GESTURE_CLASSIFIER_AXES              6U
#define GESTURE_CLASSIFIER_SAMPLES_PER_SLICE 25U
#define GESTURE_CLASSIFIER_WINDOW_SAMPLES    100U
```

Quatro fatias consecutivas formam a janela de 100 amostras, equivalente a dois
segundos de sinal a 50 Hz. O primeiro resultado aparece depois que essa janela
é preenchida; em seguida, uma nova inferência ocorre a cada fatia, ou seja, a
cada 500 ms.

```text
25 amostras ─┐
25 amostras ─┼─► janela de 100 amostras ─► DSP ─► rede neural ─► resultado
25 amostras ─┤
25 amostras ─┘
```

Se a task de inferência não acompanhar a aquisição, a fatia é descartada sem
bloquear a IMU. Ao perceber uma quebra na sequência, o consumidor limpa a
janela para não classificar dados temporalmente descontínuos.

## Modelo de aprendizado de máquina

O modelo incluído foi exportado como biblioteca C++ do Edge Impulse e usa
TensorFlow Lite Micro com quantização INT8. Sua entrada possui 600 valores:
100 amostras multiplicadas pelos seis eixos da IMU.

As classes, na ordem produzida pelo modelo, são:

1. `guard` — posição de guarda;
2. `handshake` — movimento de aperto de mão;
3. `idle` — repouso;
4. `typing` — digitação;
5. `wave` — aceno de tchau.

A classe de maior probabilidade é aceita quando sua confiança atinge o limiar
de `0.6`. Abaixo disso, o OLED mostra `incerto`, embora todas as probabilidades
continuem disponíveis no log serial.

## Resultado no OLED e na UART

Enquanto a primeira janela ainda está sendo preenchida, o OLED informa o
progresso de coleta. Depois da inferência, mostra o nome do gesto e a confiança
aproximada:

```text
wave
conf: 92%
```

O terminal apresenta as probabilidades das cinco classes, a vencedora, a
confiança e os tempos de cada etapa. Esse registro ajuda a medir latência e a
investigar confusões do modelo.

## Como executar o projeto

### Requisitos

- Heltec WiFi LoRa 32 V3 e BNO085 conectados conforme a seção de hardware;
- ESP-IDF instalado e com o ambiente exportado no terminal;
- cabo USB compatível com gravação e monitor serial;
- Edge Impulse CLI apenas se o modo de coleta for usado.

O manifesto aceita ESP-IDF `>= 4.1.0`, mas a compatibilidade final também
depende das versões resolvidas dos componentes LVGL e ESP-IDF Lib Helpers.

### 1. Clone o repositório

```bash
git clone https://github.com/alan-mendes-ufca/final-project-iot-pnaat.git
cd final-project-iot-pnaat
```

### 2. Selecione o ESP32-S3

```bash
idf.py set-target esp32s3
```

### 3. Escolha o modo

```bash
idf.py menuconfig
```

Abra `Detector de gestos` → `Modo da aplicação` e escolha:

- `Coleta` para transmitir CSV pela UART;
- `Inferência` para executar o modelo e mostrar o gesto no OLED.

O `sdkconfig` versionado está configurado em `COLLECT`. Para gerar o firmware
de inferência é necessário trocar a opção no menu antes da compilação.

### 4. Compile, grave e monitore

```bash
idf.py build
idf.py flash monitor
```

Use `Ctrl+]` para encerrar o monitor. No modo de coleta, feche-o antes de abrir
o Data Forwarder, pois os dois programas disputam a mesma porta serial.

## Como substituir o modelo

Um novo modelo deve ser exportado pelo Edge Impulse como **C++ library** e
manter o contrato usado pelo firmware:

- fusão dos eixos `accX`, `accY`, `accZ`, `gyrX`, `gyrY` e `gyrZ`;
- frequência de 50 Hz;
- janela de 100 amostras;
- exatamente cinco classes, ou atualização correspondente da interface C;
- modelo compatível com TensorFlow Lite Micro no ESP32-S3.

Substitua o conteúdo de `ml/edge-impulse/hand-gestures-project/`, revise os
nomes das classes e compile novamente no modo de inferência. As asserções em
`gesture_classifier.cpp` apontam incompatibilidades de dimensões durante a
compilação.

## Orientações para contribuições

Ao alterar aquisição, modelo ou concorrência, preserve estes contratos:

- a ordem dos seis eixos deve ser igual na coleta, no treinamento e na
  inferência;
- a taxa de amostragem deve coincidir com a frequência do modelo;
- o callback da IMU não deve ser bloqueado pela inferência;
- o modo de coleta não pode emitir logs junto ao CSV;
- perdas de fatias devem invalidar a janela temporal acumulada;
- mudanças no comportamento devem ser refletidas neste README e nas opções de
  Kconfig relacionadas.

Valide separadamente os dois modos com `idf.py build`. Alterações em pinagem,
sensor ou modelo também precisam ser verificadas no hardware.

## Estado atual e limitações

O repositório contém os fluxos de coleta e inferência, o modelo quantizado, a
integração com o BNO085 e a exibição no OLED. Entretanto, não há testes
automatizados nem resultados de uma validação física registrados no projeto.

Antes de considerar o detector pronto para uso, recomenda-se medir no hardware:

- acurácia por classe e matriz de confusão;
- latência de DSP e classificação;
- estabilidade por execução prolongada;
- sensibilidade à retirada e recolocação do sensor no pulso;
- comportamento quando a fila de inferência descarta fatias.

## Resumo da lógica

- o ESP32-S3 liga a alimentação externa e inicializa o BNO085;
- uma task atende o protocolo do sensor e outra amostra os seis eixos a 50 Hz;
- no modo `COLLECT`, cada amostra é enviada como CSV limpo pela UART;
- no modo `INFER`, grupos de 25 amostras seguem para uma fila não bloqueante;
- quatro grupos formam a janela de dois segundos usada pelo Edge Impulse;
- o modelo calcula as probabilidades de cinco gestos;
- resultados com confiança menor que 60% são apresentados como `incerto`;
- o OLED mostra gesto e confiança, enquanto a UART registra probabilidades e
  tempos de processamento;
- qualquer descontinuidade entre fatias reinicia a janela do classificador.
