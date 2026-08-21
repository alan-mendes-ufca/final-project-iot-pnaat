# Alan — o que fazer daqui pra frente

Guia de bancada do detector de gestos. As partes de código (Fases 0–2) já estão
prontas e compilando; o que falta são as etapas que exigem você no hardware e no
navegador.

**Onde estamos:** firmware de coleta pronto, no modo `COLLECT`, compilando em
244 KB. Nada foi testado em hardware ainda.

**As 5 classes:** `wave` (tchau) · `guard` (posição de luta) · `typing` (digitar)
· `handshake` (apertar as mãos) · `idle` (repouso)

---

## Passo 1 — Montar o sensor no pulso

> Este é o passo que mais estraga o projeto se for feito de qualquer jeito, e o
> erro **não dá sintoma óbvio**: o modelo treina bem, mostra 90% no Edge Impulse,
> e depois erra tudo no hardware.

`guard`, `typing` e `idle` se distinguem principalmente pela **direção da
gravidade** — ou seja, pela pose do antebraço. Se o sensor girar na pulseira
entre a coleta e a inferência, essa informação vira ruído.

- [ ] Prender o BNO085 no **dorso do pulso** com velcro ou fita, firme o
      bastante para não deslizar ao sacudir o braço
- [ ] Orientar com o **eixo X do sensor apontando para os dedos**
- [ ] **Marcar a orientação com caneta** na placa e na pulseira, para conseguir
      recolocar igual depois
- [ ] Prender a Heltec no antebraço (velcro), fios curtos (~10 cm) até o pulso
- [ ] Cabo USB de ~2 m até o PC
- [ ] Escolher **um braço** e usar sempre o mesmo

Confira também a fiação (não mudou nada aqui):

| BNO085 | ESP32-S3 |
|---|---|
| SDA | GPIO 6 |
| SCL | GPIO 7 |
| RST | GPIO 4 |
| INT | GPIO 5 |
| VCC | 3V3 |
| GND | GND |

Endereço I2C: **0x4A** (não 0x28 — aquilo é BNO055).

---

## Passo 2 — Flash e conferir o stream

```bash
cd ~/Documentos/final-project-iot
idf.py flash monitor
```

- [ ] Aparecem 6 colunas de números, tipo
      `0.1234,-9.7891,0.4412,0.0021,-0.0003,0.0011`
- [ ] O ritmo é de ~50 linhas por segundo
- [ ] Girando o pulso, os 3 últimos números (giroscópio) reagem
- [ ] Com o braço parado, `sqrt(ax²+ay²+az²)` ≈ 9,8

**Saia do monitor** (`Ctrl+]`) — ele segura a porta serial e o forwarder não
consegue abrir.

> Se quiser um teste visual do sensor antes de coletar, dá para trocar para o
> modo de inferência (`idf.py menuconfig` → *Detector de gestos* → *Inferência*)
> e ver `|accel|`, `|gyro|` e a taxa efetiva no OLED. Lembre de voltar para
> *Coleta* antes do Passo 3.

---

## Passo 3 — Coletar o dataset

### 3.1 Conectar o forwarder

```bash
edge-impulse-data-forwarder --frequency 50
```

Ele vai pedir:

- **login** da sua conta Edge Impulse
- **projeto** — crie um novo (`Create new project`) na primeira vez
- **nomes dos eixos** — digite exatamente:
  ```
  accX,accY,accZ,gyrX,gyrY,gyrZ
  ```
- **nome do dispositivo** — qualquer coisa, ex. `heltec-pulso`

- [ ] Ele reporta **"6 sensor axes detected"**

Se detectar número errado de eixos, sobrou log no serial: mantenha o forwarder
rodando e aperte o botão **RST** da placa.

Para reconfigurar do zero: `edge-impulse-data-forwarder --clean`

### 3.2 Gravar

No Studio (`edgeimpulse.com`), vá em **Data acquisition**. Seu dispositivo
aparece na direita. Para cada gravação: preencha **Label**, ponha
**Sample length = 30000** ms, e clique **Start sampling**.

Grave **6 amostras de 30 s por classe** — não gestos isolados de 3 s. Como são
atividades contínuas, o Edge Impulse fatia cada gravação de 30 s em ~140 janelas
de treino. Total: ~15 min de gravação → ~3500 janelas.

| Classe | Como executar durante os 30 s |
|---|---|
| `wave` | Tchau contínuo. Varie altura do braço, velocidade e amplitude |
| `guard` | Guarda de boxe com o bobbing natural. Varie a altura da guarda |
| `typing` | Digitando de verdade num teclado. Varie velocidade, inclua pausas curtas |
| `handshake` | Aperto de mão repetido. Varie o vigor e a duração |
| `idle` | Repouso. **Cubra várias poses**: braço apoiado na mesa, pendente ao lado do corpo, cruzado |

- [ ] `wave` — 6 × 30 s
- [ ] `guard` — 6 × 30 s
- [ ] `typing` — 6 × 30 s
- [ ] `handshake` — 6 × 30 s
- [ ] `idle` — 6 × 30 s

### 3.3 Variações (não pule)

Sem isso o modelo decora a sessão em vez de aprender o gesto:

- [ ] Gravar em **≥ 2 sessões separadas**, tirando e recolocando a pulseira
      entre elas
- [ ] Gravar em **≥ 2 posturas**: sentado e em pé
- [ ] Em `idle`, incluir a pose de **antebraço apoiado na mesa** — é a mais
      parecida com `typing` e é onde o modelo mais erra

### 3.4 Split

- [ ] No **Data acquisition** → **Perform train/test split** (80/20)

---

## Passo 4 — Treinar no Edge Impulse

### 4.1 Create impulse

- [ ] **Time series data**: Window size **2000 ms**, Window increase **200 ms**,
      Frequency **50 Hz**, zero-pad **desligado**
- [ ] Processing block: **Spectral Analysis** (nos 6 eixos)
- [ ] Learning block: **Classification**
- [ ] **Save Impulse**

> Não use *Flatten* — ele descarta o conteúdo de frequência, que é justamente o
> que separa `wave`, `handshake` e `typing`. E *MFE* é bloco de áudio.

### 4.2 Spectral features

- [ ] Filter type: **low**, cutoff **~12 Hz**
- [ ] FFT length: **64**
- [ ] Take log of spectrum: **ligado**
- [ ] **Save parameters** → **Generate features**

> **Não use high-pass.** Ele remove a componente DC, que é a gravidade — e sem
> gravidade `guard`, `typing` e `idle` viram a mesma coisa.

**Pare aqui e olhe o Feature Explorer.** Os 5 grupos de cor devem formar
agrupamentos distinguíveis. Se `typing` e `idle` estiverem completamente
sobrepostos, o problema é **dado**, não modelo — volte ao Passo 3 antes de
treinar.

### 4.3 Classifier

- [ ] 2 camadas densas: **40** e **20** neurônios
- [ ] Dropout **0.25**
- [ ] Training cycles **100**, learning rate **0.0005**
- [ ] **Start training**

Metas: treino ≥ 90%, teste ≥ 85%.

### 4.4 Se a acurácia não bater

| Sintoma | O que fazer |
|---|---|
| `typing` ↔ `idle` confundindo | Mais `idle` com antebraço apoiado na mesa; FFT length 128 |
| `wave` ↔ `handshake` confundindo | Conferir se o sensor não girou no pulso entre as sessões |
| Tudo abaixo de 80% | Rodar o **EON Tuner** — resolve mais rápido que ajuste manual |
| Bom no treino, ruim no teste | Faltam variações — voltar ao Passo 3.3 |

- [ ] **Model testing** → **Classify all** → anotar a acurácia de teste

### 4.5 Exportar

- [ ] **Deployment** → **C++ library**
- [ ] Quantização: **INT8**
- [ ] **Build** → baixa um `.zip`

> **Não escolha "Arduino library".** Ela não serve para ESP-IDF.

---

## Passo 5 — Me avisar

Quando tiver o `.zip` baixado, me diga — a Fase 5 (integrar o SDK no projeto,
escrever o loop de inferência e mandar o gesto para o OLED) é código, eu faço.

Me passe junto:

- [ ] Onde o `.zip` está (ex. `~/Downloads/...-cpp-...zip`)
- [ ] A acurácia de teste que você obteve
- [ ] Quais pares de classe apareceram confundidos na matriz de confusão

---

## Passo 6 — Validar no hardware

Depois que eu integrar o modelo, você faz a validação real:

```bash
idf.py menuconfig     # Detector de gestos -> Inferência
idf.py build flash monitor
```

Faça **10 repetições de cada classe** e preencha:

| Executado \ Detectado | wave | guard | typing | handshake | idle |
|---|---|---|---|---|---|
| **wave** | | | | | |
| **guard** | | | | | |
| **typing** | | | | | |
| **handshake** | | | | | |
| **idle** | | | | | |

- [ ] Acurácia total ≥ **80%**
- [ ] Latência (`dsp_ms + cls_ms` no serial) < **100 ms**
- [ ] Roda 1 h sem reiniciar
- [ ] **Repetir a matriz depois de tirar e recolocar a pulseira** — se despencar,
      o modelo decorou a montagem e falta variação no dataset

Se esta matriz divergir muito da do Edge Impulse, a causa quase sempre é
orientação do sensor ou superfície de apoio diferente entre coleta e uso.

---

## Se algo der errado

| Problema | Causa provável | Solução |
|---|---|---|
| `BNO085 nunca assertou INT apos reset` | Alimentação ou fiação RST/INT | Conferir 3V3/GND, GPIO 4 e GPIO 5 |
| `sh2_open falhou` | SDA/SCL ou endereço | Conferir GPIO 6/7 e que o ADR está em GND (0x4A) |
| Forwarder detecta ≠ 6 eixos | Log sujando o UART | Manter o forwarder aberto e apertar RST |
| Forwarder não abre a porta | Monitor ainda rodando | `Ctrl+]` para sair do `idf.py monitor` |
| Taxa muito abaixo de 50 Hz | Relatórios sendo perdidos | Me avisar — dá para baixar para 25 Hz ou trocar o polling do INT por interrupção |
| `IMU ainda sem accel+gyro validos` | Sensor não está reportando | Ver as duas primeiras linhas desta tabela |
