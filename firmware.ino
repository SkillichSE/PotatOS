#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <Preferences.h>

const char* ws_host = "0.tcp.ngrok.io";
const uint16_t ws_port = 12345;
const char* ws_path = "/";
const bool ws_use_ssl = false;

const char* WIFI_SETUP_AP_NAME = "Potato-GLaDOS-Setup";
const char* WIFI_SETUP_AP_PASS = "potato1234";

#define MIC_WS_PIN    25
#define MIC_SCK_PIN   32
#define MIC_SD_PIN    33

#define AMP_DOUT_PIN  22
#define AMP_BCLK_PIN  27
#define AMP_LRC_PIN   26

#define BUTTON_PIN    0
#define LED_R_PIN     13
#define LED_G_PIN     14
#define LED_B_PIN     15

#define MIC_SAMPLE_RATE 16000
#define SPK_SAMPLE_RATE 22050

#define I2S_MIC_PORT I2S_NUM_0
#define I2S_SPK_PORT I2S_NUM_1

#define MIC_BUF_SAMPLES 512

#define WIFI_RESET_HOLD_MS 5000

WebSocketsClient webSocket;
WiFiManager wm;
Preferences prefs;

// Список модулей должен совпадать по id со словарём glados_modules в test.py
const char* GLADOS_MODULE_IDS[]    = { "classic",      "wheatley",      "space_core",         "fact_core",             "adventure_core",       "curiosity_core",       "morality_core" };
const char* GLADOS_MODULE_LABELS[] = { "Классический", "Повреждённый модуль интеллекта", "Космическое ядро",   "Информационное ядро",   "Ядро приключений",     "Ядро любопытства",     "Ядро морали" };
const int GLADOS_MODULE_COUNT = sizeof(GLADOS_MODULE_IDS) / sizeof(GLADOS_MODULE_IDS[0]);

String selected_glados_module = "classic";
WiFiManagerParameter* moduleParam = nullptr;

volatile bool muted_by_button   = false;
volatile bool muted_by_playback = false;

int32_t  mic_raw_buf[MIC_BUF_SAMPLES];
int16_t  mic_pcm16_buf[MIC_BUF_SAMPLES];

#define LEDC_RES_BITS 8
#define LEDC_FREQ_HZ  5000
#define LEDC_CH_R 0
#define LEDC_CH_G 1
#define LEDC_CH_B 2

void setupLed() {
  ledcSetup(LEDC_CH_R, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcSetup(LEDC_CH_G, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcSetup(LEDC_CH_B, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcAttachPin(LED_R_PIN, LEDC_CH_R);
  ledcAttachPin(LED_G_PIN, LEDC_CH_G);
  ledcAttachPin(LED_B_PIN, LEDC_CH_B);
}

void setLedRGB(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(LEDC_CH_R, r);
  ledcWrite(LEDC_CH_G, g);
  ledcWrite(LEDC_CH_B, b);
}

void potatoFlicker() {
  static unsigned long next_flicker_check = 0;
  static unsigned long flicker_until = 0;
  static bool in_dip = false;

  unsigned long now = millis();
  float t = now / 900.0;
  int breathe = 55 + (int)(45.0 * (sin(t) + 1.0) / 2.0);

  if (in_dip) {
    if (now >= flicker_until) in_dip = false;
  } else if (now >= next_flicker_check) {
    next_flicker_check = now + random(1500, 4500);
    if (random(0, 100) < 35) {
      in_dip = true;
      flicker_until = now + random(40, 160);
    }
  }

  uint8_t level = in_dip ? (uint8_t)random(0, 15) : (uint8_t)breathe;
  setLedRGB(level, (uint8_t)(level * 0.55), 0);
}

void wifiSetupBlink() {
  unsigned long now = millis();
  uint8_t level = (now / 300) % 2 == 0 ? 160 : 10;
  setLedRGB(level, 0, level);
}

void updateLed() {
  if (!webSocket.isConnected()) {
    setLedRGB(180, 0, 0);
    return;
  }
  if (muted_by_playback) {
    return;
  }
  if (muted_by_button) {
    setLedRGB(90, 0, 0);
    return;
  }
  potatoFlicker();
}

String buildGladosModuleHtml() {
  String html = "<p style='margin-top:16px;'><b>Модуль ГЛаДОС</b></p>";
  for (int i = 0; i < GLADOS_MODULE_COUNT; i++) {
    html += "<label style='display:block;margin:4px 0;'>";
    html += "<input type='radio' name='glados_module' value='";
    html += GLADOS_MODULE_IDS[i];
    html += "'";
    if (selected_glados_module == GLADOS_MODULE_IDS[i]) {
      html += " checked";
    }
    html += "> ";
    html += GLADOS_MODULE_LABELS[i];
    html += "</label>";
  }
  return html;
}

void loadGladosModule() {
  prefs.begin("glados", false);
  selected_glados_module = prefs.getString("module", "classic");
  prefs.end();
}

void saveGladosModule(const String& moduleId) {
  selected_glados_module = moduleId;
  prefs.begin("glados", false);
  prefs.putString("module", selected_glados_module);
  prefs.end();
}

void onWmSaveParams() {
  if (wm.server == nullptr) return;
  String requested = wm.server->arg("glados_module");
  for (int i = 0; i < GLADOS_MODULE_COUNT; i++) {
    if (requested == GLADOS_MODULE_IDS[i]) {
      saveGladosModule(requested);
      Serial.print("[glados] выбран модуль: ");
      Serial.println(selected_glados_module);
      return;
    }
  }
}

void onWifiConfigPortalStarted(WiFiManager* wifiManager) {
  Serial.print("[wifi] портал настройки открыт, точка доступа: ");
  Serial.println(WIFI_SETUP_AP_NAME);
  Serial.println("[wifi] подключись к этой сети и открой 192.168.4.1");
}

void setupMicI2S() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = MIC_BUF_SAMPLES,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pins = {
    .bck_io_num = MIC_SCK_PIN,
    .ws_io_num = MIC_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD_PIN
  };

  i2s_driver_install(I2S_MIC_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &pins);
  i2s_set_clk(I2S_MIC_PORT, MIC_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_MONO);
}

void setupSpkI2S() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SPK_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pins = {
    .bck_io_num = AMP_BCLK_PIN,
    .ws_io_num = AMP_LRC_PIN,
    .data_out_num = AMP_DOUT_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_SPK_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_SPK_PORT, &pins);
  i2s_set_clk(I2S_SPK_PORT, SPK_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}

void playAudio(uint8_t* data, size_t len) {
  muted_by_playback = true;
  setLedRGB(0, 0, 220);

  size_t written = 0;
  size_t offset = 0;
  while (offset < len) {
    size_t chunk = min((size_t)1024, len - offset);
    size_t w = 0;
    i2s_write(I2S_SPK_PORT, data + offset, chunk, &w, portMAX_DELAY);
    offset += w;
    written += w;
  }

  muted_by_playback = false;
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[ws] подключено к серверу");
      webSocket.sendTXT("MODULE:" + selected_glados_module);
      Serial.print("[glados] отправлен модуль: ");
      Serial.println(selected_glados_module);
      break;

    case WStype_DISCONNECTED:
      Serial.println("[ws] отключено, жду переподключения...");
      break;

    case WStype_BIN:
      Serial.printf("[ws] получен ответ, %u байт\n", (unsigned)length);
      playAudio(payload, length);
      break;

    case WStype_ERROR:
      Serial.println("[ws] ошибка соединения");
      break;

    default:
      break;
  }
}

void handleButton() {
  static bool last_reading = HIGH;
  static unsigned long last_change = 0;
  const unsigned long debounce_ms = 50;

  static bool stable_state = HIGH;
  static unsigned long press_started_at = 0;
  static bool long_press_fired = false;

  bool reading = digitalRead(BUTTON_PIN);
  if (reading != last_reading) {
    last_change = millis();
    last_reading = reading;
  }

  if ((millis() - last_change) > debounce_ms && stable_state != reading) {
    stable_state = reading;
    if (stable_state == LOW) {
      press_started_at = millis();
      long_press_fired = false;
    } else {
      if (!long_press_fired) {
        muted_by_button = !muted_by_button;
        Serial.println(muted_by_button ? "[mic] выключен кнопкой" : "[mic] включён кнопкой");
      }
    }
  }

  if (stable_state == LOW && !long_press_fired &&
      (millis() - press_started_at) > WIFI_RESET_HOLD_MS) {
    long_press_fired = true;
    Serial.println("[wifi] долгое удержание — сброс настроек Wi-Fi и перезагрузка");
    setLedRGB(255, 255, 255);
    delay(300);
    wm.resetSettings();
    delay(200);
    ESP.restart();
  }
}

void micReadAndSend() {
  if (muted_by_button || muted_by_playback || !webSocket.isConnected()) return;

  size_t bytes_read = 0;
  i2s_read(I2S_MIC_PORT, mic_raw_buf, sizeof(mic_raw_buf), &bytes_read, 0);
  if (bytes_read == 0) return;

  size_t samples = bytes_read / sizeof(int32_t);
  for (size_t i = 0; i < samples; i++) {
    int32_t s = mic_raw_buf[i] >> 14;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    mic_pcm16_buf[i] = (int16_t)s;
  }

  webSocket.sendBIN((uint8_t*)mic_pcm16_buf, samples * sizeof(int16_t));
}

void setupWifi() {
  WiFi.mode(WIFI_STA);

  wm.setAPCallback(onWifiConfigPortalStarted);
  wm.setConfigPortalTimeout(180);

  bool connected;
  if (WIFI_SETUP_AP_PASS != nullptr) {
    connected = wm.autoConnect(WIFI_SETUP_AP_NAME, WIFI_SETUP_AP_PASS);
  } else {
    connected = wm.autoConnect(WIFI_SETUP_AP_NAME);
  }

  if (!connected) {
    Serial.println("[wifi] не удалось подключиться (таймаут портала), перезагрузка...");
    delay(1000);
    ESP.restart();
  }

  Serial.print("[wifi] подключено, IP: ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  delay(200);

  setupLed();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  setLedRGB(0, 0, 120);

  setupMicI2S();
  setupSpkI2S();

  loadGladosModule();
  String moduleHtml = buildGladosModuleHtml();
  moduleParam = new WiFiManagerParameter(moduleHtml.c_str());
  wm.addParameter(moduleParam);
  wm.setSaveParamsCallback(onWmSaveParams);

  setupWifi();

  if (ws_use_ssl) {
    webSocket.beginSSL(ws_host, ws_port, ws_path);
  } else {
    webSocket.begin(ws_host, ws_port, ws_path);
  }
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(3000);
}

void loop() {
  webSocket.loop();
  handleButton();
  micReadAndSend();
  updateLed();
}
