#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>

#define DEBUG_WEBSOCKETS_PORT Serial

#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>


const char* ws_host = "steellegend.taila511f4.ts.net";
const uint16_t ws_port = 443;
const char* ws_path = "/";
const bool ws_use_ssl = true;

const char* WIFI_SETUP_AP_NAME = "Potato-GLaDOS-Setup";
const char* WIFI_SETUP_AP_PASS = "potato1234";

#define MIC_WS_PIN    25
#define MIC_SCK_PIN   32
#define MIC_SD_PIN    33

#define AMP_DOUT_PIN  22
#define AMP_BCLK_PIN  27
#define AMP_LRC_PIN   26

#define BUTTON_PIN    4
#define LED_R_PIN     13
#define LED_G_PIN     15
#define LED_B_PIN     14

#define MIC_SAMPLE_RATE 16000
#define SPK_SAMPLE_RATE 22050

#define I2S_MIC_PORT I2S_NUM_0
#define I2S_SPK_PORT I2S_NUM_1

#define MIC_BUF_SAMPLES 512

#define PORTAL_TOGGLE_HOLD_MS 5000
#define WIFI_RESET_HOLD_MS 15000

WebSocketsClient webSocket;
WiFiManager wm;
Preferences prefs;


const char* GLADOS_MODULE_IDS[]    = { "classic",  "wheatley",           "space_core",   "fact_core",   "adventure_core",  "curiosity_core",  "morality_core" };
const char* GLADOS_MODULE_LABELS[] = { "Classic",  "Corrupted Core (Wheatley)", "Space Core",   "Fact Core",   "Adventure Core",  "Curiosity Core",  "Morality Core" };
const int GLADOS_MODULE_COUNT = sizeof(GLADOS_MODULE_IDS) / sizeof(GLADOS_MODULE_IDS[0]);

String selected_glados_module = "classic";
WiFiManagerParameter* moduleParam = nullptr;


bool portal_active = false;

SemaphoreHandle_t portal_state_mutex = nullptr;
volatile bool pending_module_switch = false;
String pending_module_id = "";
volatile unsigned long pending_module_switch_at = 0;
#define PORTAL_ACTION_SEND_DELAY_MS 800


int speaker_volume = 80;
volatile int speaker_volume_live = 80;

volatile bool muted_by_button   = false;
volatile bool muted_by_playback = false;
volatile bool muted_by_processing = false;

volatile bool pending_portal_greeting_replay = false;
volatile unsigned long pending_portal_greeting_replay_at = 0;


volatile unsigned long last_audio_chunk_ms = 0;
#define SPEAKING_GRACE_MS 400

int32_t  mic_raw_buf[MIC_BUF_SAMPLES];
int16_t  mic_pcm16_buf[MIC_BUF_SAMPLES];

#define LEDC_RES_BITS 8
#define LEDC_FREQ_HZ  5000


#define LED_INVERTED false

void setupLed() {
  ledcAttach(LED_R_PIN, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcAttach(LED_G_PIN, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcAttach(LED_B_PIN, LEDC_FREQ_HZ, LEDC_RES_BITS);
}

void setLedRGB(uint8_t r, uint8_t g, uint8_t b) {
  if (LED_INVERTED) {
    r = 255 - r;
    g = 255 - g;
    b = 255 - b;
  }
  ledcWrite(LED_R_PIN, r);
  ledcWrite(LED_G_PIN, g);
  ledcWrite(LED_B_PIN, b);
}

void potatoFlicker() {
  static unsigned long next_flicker_check = 0;
  static unsigned long flicker_until = 0;
  static bool in_dip = false;
  static uint8_t stutters_left = 0;
  static unsigned long next_stutter_at = 0;

  unsigned long now = millis();


  float t = now / 1400.0;
  float wobble = sin(t * 1.7) * 0.15;
  int breathe = 30 + (int)(35.0 * ((sin(t) + 1.0) / 2.0 + wobble));
  breathe = constrain(breathe, 4, 70);

  if (in_dip) {
    if (now >= flicker_until) {
      in_dip = false;
      if (stutters_left > 0) {
        stutters_left--;
        next_stutter_at = now + random(20, 90);
      }
    }
  } else if (stutters_left > 0 && now >= next_stutter_at) {
    in_dip = true;
    flicker_until = now + random(15, 60);
  } else if (stutters_left == 0 && now >= next_flicker_check) {
    next_flicker_check = now + random(1200, 4000);
    if (random(0, 100) < 30) {
      in_dip = true;
      flicker_until = now + random(30, 140);
      stutters_left = (random(0, 100) < 40) ? random(1, 3) : 0;
    }
  }

  uint8_t level = in_dip ? (uint8_t)random(0, 10) : (uint8_t)breathe;


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

  if (muted_by_playback && (millis() - last_audio_chunk_ms > SPEAKING_GRACE_MS)) {
    muted_by_playback = false;
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
  String html = "<p style='margin-top:16px;'><b>GLaDOS Module</b></p>";
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


  html += "<p style='margin-top:16px;'><b>Speaker volume</b></p>";
  html += "<input type='range' name='speaker_volume' min='0' max='100' step='5' value='";
  html += String(speaker_volume);
  html += "' oninput='this.nextElementSibling.value=this.value+\"%\"' style='width:100%;'>";
  html += "<output style='display:block;text-align:right;font-size:0.85em;color:#666;'>";
  html += String(speaker_volume);
  html += "%</output>";

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

void loadSpeakerVolume() {
  prefs.begin("glados", false);
  speaker_volume = prefs.getInt("volume", 80);
  prefs.end();
  speaker_volume = constrain(speaker_volume, 0, 100);
  speaker_volume_live = speaker_volume;
}

void saveSpeakerVolume(int volume) {
  speaker_volume = constrain(volume, 0, 100);
  speaker_volume_live = speaker_volume;
  prefs.begin("glados", false);
  prefs.putInt("volume", speaker_volume);
  prefs.end();
}

void onWmSaveParams() {
  if (wm.server == nullptr) return;

  String requested = wm.server->arg("glados_module");
  for (int i = 0; i < GLADOS_MODULE_COUNT; i++) {
    if (requested == GLADOS_MODULE_IDS[i]) {
      bool changed = (requested != selected_glados_module);
      saveGladosModule(requested);
      Serial.print("[glados] module selected: ");
      Serial.println(selected_glados_module);


      if (changed) {
        xSemaphoreTake(portal_state_mutex, portMAX_DELAY);
        pending_module_id = selected_glados_module;
        pending_module_switch = true;
        pending_module_switch_at = millis();
        xSemaphoreGive(portal_state_mutex);
      }
      break;
    }
  }

  String volume_str = wm.server->arg("speaker_volume");
  if (volume_str.length() > 0) {
    int vol = constrain(volume_str.toInt(), 0, 100);
    if (vol != speaker_volume) {
      saveSpeakerVolume(vol);
      Serial.print("[glados] speaker volume set to: ");
      Serial.print(speaker_volume);
      Serial.println("%");
    }
  }
}

void onWifiConfigPortalStarted(WiFiManager* wifiManager) {
  Serial.print("[wifi] setup portal opened, access point: ");
  Serial.println(WIFI_SETUP_AP_NAME);
  Serial.println("[wifi] connect to this network and open 192.168.4.1");
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
    .mck_io_num = I2S_PIN_NO_CHANGE,
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
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = AMP_BCLK_PIN,
    .ws_io_num = AMP_LRC_PIN,
    .data_out_num = AMP_DOUT_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_SPK_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_SPK_PORT, &pins);
  i2s_set_clk(I2S_SPK_PORT, SPK_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}


#define AUDIO_QUEUE_CAPACITY 66150
#define AUDIO_QUEUE_HIGH_WATERMARK (AUDIO_QUEUE_CAPACITY - 8000)
#define AUDIO_PLAYBACK_PIECE_BYTES 1024


#define AUDIO_PREBUFFER_BYTES 39690


#define AUDIO_PREBUFFER_TIMEOUT_MS 600

#define AUDIO_PREBUFFER_MIN_BYTES 19845
#define AUDIO_PREBUFFER_MAX_BYTES 61740
#define AUDIO_PREBUFFER_STEP_BYTES 8000
#define AUDIO_PREBUFFER_BYTES_PER_MS 44.1
#define AUDIO_PREBUFFER_HEALTHY_MS 15000

uint8_t audio_queue[AUDIO_QUEUE_CAPACITY];
size_t  audio_queue_head = 0;
size_t  audio_queue_len  = 0;


volatile bool audio_priming = true;
volatile unsigned long audio_priming_start_ms = 0;

volatile size_t audio_prebuffer_target = AUDIO_PREBUFFER_BYTES;
volatile unsigned long audio_last_underrun_ms = 0;

volatile unsigned long audio_underrun_count = 0;


SemaphoreHandle_t audio_queue_mutex = nullptr;

void audioQueuePush(const uint8_t* data, size_t len) {
  xSemaphoreTake(audio_queue_mutex, portMAX_DELAY);

  if (audio_queue_len == 0 && len > 0) {
    audio_priming_start_ms = millis();
  }

  if (len > AUDIO_QUEUE_CAPACITY) {
    data += (len - AUDIO_QUEUE_CAPACITY);
    len = AUDIO_QUEUE_CAPACITY;
  }
  size_t free_space = AUDIO_QUEUE_CAPACITY - audio_queue_len;
  if (len > free_space) {
    size_t to_drop = len - free_space;
    audio_queue_head = (audio_queue_head + to_drop) % AUDIO_QUEUE_CAPACITY;
    audio_queue_len -= to_drop;
    Serial.println("[audio] queue overflow (unexpected), dropping oldest buffered audio");
  }
  size_t tail = (audio_queue_head + audio_queue_len) % AUDIO_QUEUE_CAPACITY;
  size_t first_part = min(len, AUDIO_QUEUE_CAPACITY - tail);
  memcpy(audio_queue + tail, data, first_part);
  if (len > first_part) {
    memcpy(audio_queue, data + first_part, len - first_part);
  }
  audio_queue_len += len;

  xSemaphoreGive(audio_queue_mutex);
}

size_t audioQueuePop(uint8_t* out, size_t max_len) {
  xSemaphoreTake(audio_queue_mutex, portMAX_DELAY);

  size_t n = min(max_len, audio_queue_len);
  size_t first_part = min(n, AUDIO_QUEUE_CAPACITY - audio_queue_head);
  memcpy(out, audio_queue + audio_queue_head, first_part);
  if (n > first_part) {
    memcpy(out + first_part, audio_queue, n - first_part);
  }
  audio_queue_head = (audio_queue_head + n) % AUDIO_QUEUE_CAPACITY;
  audio_queue_len -= n;

  xSemaphoreGive(audio_queue_mutex);
  return n;
}


void audioPlaybackTask(void* pvParameters) {
  float led_level = 0.0f;

  for (;;) {
    if (audio_queue_len == 0) {
      if (!audio_priming) {
        audio_underrun_count++;
        unsigned long now = millis();
        audio_last_underrun_ms = now;
        if (audio_prebuffer_target + AUDIO_PREBUFFER_STEP_BYTES <= AUDIO_PREBUFFER_MAX_BYTES) {
          audio_prebuffer_target += AUDIO_PREBUFFER_STEP_BYTES;
          Serial.print("[audio] underrun, growing prebuffer target to ");
          Serial.print(audio_prebuffer_target);
          Serial.println(" bytes");
        }
      }
      audio_priming = true;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    if (audio_priming) {
      if (audio_prebuffer_target > AUDIO_PREBUFFER_MIN_BYTES &&
          (millis() - audio_last_underrun_ms) >= AUDIO_PREBUFFER_HEALTHY_MS) {
        audio_prebuffer_target -= min((size_t)AUDIO_PREBUFFER_STEP_BYTES,
                                       audio_prebuffer_target - AUDIO_PREBUFFER_MIN_BYTES);
        audio_last_underrun_ms = millis();
        Serial.print("[audio] healthy stretch, shrinking prebuffer target to ");
        Serial.print(audio_prebuffer_target);
        Serial.println(" bytes");
      }

      bool have_cushion = audio_queue_len >= audio_prebuffer_target;
      unsigned long scaled_timeout = AUDIO_PREBUFFER_TIMEOUT_MS +
          (unsigned long)((audio_prebuffer_target > AUDIO_PREBUFFER_BYTES
                                ? (audio_prebuffer_target - AUDIO_PREBUFFER_BYTES)
                                : 0) / AUDIO_PREBUFFER_BYTES_PER_MS);
      bool timed_out = (millis() - audio_priming_start_ms) >= scaled_timeout;
      if (!have_cushion && !timed_out) {
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      audio_priming = false;
    }

    uint8_t piece[AUDIO_PLAYBACK_PIECE_BYTES];
    size_t n = audioQueuePop(piece, sizeof(piece));
    if (n == 0) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    muted_by_playback = true;

    size_t samples_in_piece = n / sizeof(int16_t);
    if (samples_in_piece > 0) {
      int16_t* samples = (int16_t*)piece;

      int vol = speaker_volume_live;
      if (vol != 100) {
        for (size_t i = 0; i < samples_in_piece; i++) {
          int32_t s = (int32_t)samples[i] * vol / 100;
          if (s > 32767) s = 32767;
          if (s < -32768) s = -32768;
          samples[i] = (int16_t)s;
        }
      }

      uint32_t sum_abs = 0;
      for (size_t i = 0; i < samples_in_piece; i++) {
        sum_abs += (uint32_t)abs((int)samples[i]);
      }
      float amplitude = (float)sum_abs / samples_in_piece / 32768.0f;


      if (amplitude > led_level) {
        led_level = amplitude;
      } else {
        led_level = led_level * 0.75f + amplitude * 0.25f;
      }

      float perceptual = sqrtf(constrain(led_level, 0.0f, 1.0f));
      uint8_t level = (uint8_t)(25 + perceptual * 230.0f);


      setLedRGB(level, (uint8_t)(level * 0.75f), 0);
    }


    size_t w = 0;
    i2s_write(I2S_SPK_PORT, piece, n, &w, portMAX_DELAY);

    last_audio_chunk_ms = millis();
  }
}


void portalTask(void* pvParameters) {
  for (;;) {
    wm.process();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


#define WS_BIN_EXPAND_PIECE_BYTES 256
static uint8_t ws_bin_expand_buf[WS_BIN_EXPAND_PIECE_BYTES * 2];

bool first_connection_since_boot = true;

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[ws] connected to server");
      Serial.print("[diag] free heap on connect: ");
      Serial.println(ESP.getFreeHeap());
      if (first_connection_since_boot) {
        webSocket.sendTXT("MODULE:" + selected_glados_module);
        Serial.print("[glados] module sent (boot greeting will play): ");
        Serial.println(selected_glados_module);
        first_connection_since_boot = false;
      } else {
        Serial.println("[glados] reconnect, not a fresh boot - skipping greeting replay");
      }
      break;

    case WStype_DISCONNECTED:
      Serial.println("[ws] disconnected, waiting to reconnect...");
      Serial.print("[diag] free heap on disconnect: ");
      Serial.print(ESP.getFreeHeap());
      Serial.print(" bytes, largest contiguous block: ");
      Serial.print(ESP.getMaxAllocHeap());
      Serial.println(" bytes");
      Serial.print("[diag] RSSI at disconnect: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");

      muted_by_processing = false;
      break;

    case WStype_TEXT: {
      String msg((const char*)payload, length);
      if (msg == "MUTE") {
        muted_by_processing = true;
        Serial.println("[mic] muted - server is processing a command");
      } else if (msg == "UNMUTE") {
        muted_by_processing = false;
        Serial.println("[mic] unmuted - server abandoned processing without a reply");
      }
      break;
    }

    case WStype_BIN: {
      muted_by_processing = false;

      Serial.printf("[ws] response received, %u bytes (8-bit)\n", (unsigned)length);
      if (audio_queue_len == 0) {
        Serial.print("[diag] free heap at start of reply: ");
        Serial.print(ESP.getFreeHeap());
        Serial.print(" bytes, largest contiguous block: ");
        Serial.print(ESP.getMaxAllocHeap());
        Serial.println(" bytes");
      }

      size_t remaining = length;
      size_t src_off = 0;
      while (remaining > 0) {
        size_t take = min(remaining, (size_t)WS_BIN_EXPAND_PIECE_BYTES);
        for (size_t i = 0; i < take; i++) {
          int16_t sample = ((int16_t)payload[src_off + i] - 128) * 256;
          ws_bin_expand_buf[i * 2]     = (uint8_t)(sample & 0xFF);
          ws_bin_expand_buf[i * 2 + 1] = (uint8_t)((sample >> 8) & 0xFF);
        }
        audioQueuePush(ws_bin_expand_buf, take * 2);
        src_off += take;
        remaining -= take;
      }
      break;
    }

    case WStype_ERROR:
      Serial.println("[ws] connection error");
      Serial.print("[diag] free heap at error: ");
      Serial.print(ESP.getFreeHeap());
      Serial.print(" bytes, largest contiguous block: ");
      Serial.print(ESP.getMaxAllocHeap());
      Serial.println(" bytes");
      break;

    default:
      break;
  }
}

void enablePortal();
void disablePortal();
void togglePortal();

void handleButton() {
  static bool last_reading = HIGH;
  static unsigned long last_change = 0;
  const unsigned long debounce_ms = 50;

  static bool stable_state = HIGH;
  static unsigned long press_started_at = 0;
  static bool portal_toggle_fired = false;
  static bool wifi_reset_fired = false;

  bool reading = digitalRead(BUTTON_PIN);
  if (reading != last_reading) {
    last_change = millis();
    last_reading = reading;
  }

  if ((millis() - last_change) > debounce_ms && stable_state != reading) {
    stable_state = reading;
    if (stable_state == LOW) {
      press_started_at = millis();
      portal_toggle_fired = false;
      wifi_reset_fired = false;
    } else {
      if (!portal_toggle_fired && !wifi_reset_fired) {
        muted_by_button = !muted_by_button;
        Serial.println(muted_by_button ? "[mic] muted by button" : "[mic] unmuted by button");
      }
    }
  }

  if (stable_state == LOW) {
    unsigned long held_ms = millis() - press_started_at;

    if (!portal_toggle_fired && !wifi_reset_fired && held_ms > PORTAL_TOGGLE_HOLD_MS) {
      portal_toggle_fired = true;
      Serial.println("[wifi] 5s hold - toggling setup portal");
      setLedRGB(0, 200, 255);
      delay(150);
      togglePortal();
    }

    if (!wifi_reset_fired && held_ms > WIFI_RESET_HOLD_MS) {
      wifi_reset_fired = true;
      Serial.println("[wifi] long press held — resetting Wi-Fi settings and rebooting");
      setLedRGB(255, 255, 255);
      delay(300);
      wm.resetSettings();
      delay(200);
      ESP.restart();
    }
  }
}

void micReadAndSend() {
  static bool was_paused_for_portal = false;
  if (portal_active != was_paused_for_portal) {
    Serial.println(portal_active
      ? "[mic] portal is open - going quiet (mic paused, GLaDOS won't speak)"
      : "[mic] portal closed - back to normal");
    was_paused_for_portal = portal_active;
  }
  if (portal_active) return;

  if (muted_by_button || muted_by_playback || muted_by_processing || !webSocket.isConnected()) return;

  size_t bytes_read = 0;
  i2s_read(I2S_MIC_PORT, mic_raw_buf, sizeof(mic_raw_buf), &bytes_read, 0);
  if (bytes_read == 0) return;


  if (bytes_read > sizeof(mic_raw_buf)) {
    Serial.println("[mic] anomalous bytes_read, skipping frame");
    return;
  }

  size_t samples = bytes_read / sizeof(int32_t);
  for (size_t i = 0; i < samples; i++) {
    int32_t s = mic_raw_buf[i] >> 14;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    mic_pcm16_buf[i] = (int16_t)s;
  }

  webSocket.sendBIN((uint8_t*)mic_pcm16_buf, samples * sizeof(int16_t));
}

void enablePortal() {
  if (portal_active) return;

  int ap_channel = WiFi.channel();
  if (ap_channel < 1 || ap_channel > 13) {
    ap_channel = 1;
  }

  bool ap_ok;
  if (WIFI_SETUP_AP_PASS != nullptr) {
    ap_ok = WiFi.softAP(WIFI_SETUP_AP_NAME, WIFI_SETUP_AP_PASS, ap_channel);
  } else {
    ap_ok = WiFi.softAP(WIFI_SETUP_AP_NAME, nullptr, ap_channel);
  }
  WiFi.mode(WIFI_AP_STA);
  wm.startWebPortal();
  portal_active = true;

  Serial.print("[wifi] portal enabled, softAP() result: ");
  Serial.print(ap_ok ? "OK" : "FAILED");
  Serial.print(", channel: ");
  Serial.println(ap_channel);
  Serial.print("[wifi] AP IP (expected 192.168.4.1): ");
  Serial.println(WiFi.softAPIP());
}

void disablePortal() {
  if (!portal_active) return;

  wm.stopWebPortal();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portal_active = false;

  pending_portal_greeting_replay = true;
  pending_portal_greeting_replay_at = millis();

  Serial.println("[wifi] portal disabled - setup access point turned off, greeting will replay");
}

void togglePortal() {
  if (portal_active) {
    disablePortal();
  } else {
    enablePortal();
  }
}

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  wm.setAPCallback(onWifiConfigPortalStarted);
  wm.setConfigPortalTimeout(180);

  bool connected;
  if (WIFI_SETUP_AP_PASS != nullptr) {
    connected = wm.autoConnect(WIFI_SETUP_AP_NAME, WIFI_SETUP_AP_PASS);
  } else {
    connected = wm.autoConnect(WIFI_SETUP_AP_NAME);
  }

  if (!connected) {
    Serial.println("[wifi] failed to connect (portal timeout), rebooting...");
    delay(1000);
    ESP.restart();
  }

  Serial.print("[wifi] connected, IP: ");
  Serial.println(WiFi.localIP());


  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
              IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));

  Serial.print("[wifi] setup access point available (hold button 5s to toggle): ");
  Serial.println(WIFI_SETUP_AP_NAME);


  Serial.print("[wifi] Wi-Fi mode (1 = STA, 3 = AP_STA): ");
  Serial.println((int)WiFi.getMode());
  Serial.print("[wifi] AP MAC: ");
  Serial.println(WiFi.softAPmacAddress());
}

const char* resetReasonToStr(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software (restart())";
    case ESP_RST_PANIC:     return "PANIC (exception/crash)";
    case ESP_RST_INT_WDT:   return "interrupt watchdog (task starved core too long)";
    case ESP_RST_TASK_WDT:  return "task watchdog (a task didn't yield in time)";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply voltage dipped under load)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.print("[boot] reset reason: ");
  Serial.println(resetReasonToStr(esp_reset_reason()));

  setupLed();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  setLedRGB(0, 0, 120);

  setupMicI2S();
  setupSpkI2S();

  audio_queue_mutex = xSemaphoreCreateMutex();
  portal_state_mutex = xSemaphoreCreateMutex();
  audio_last_underrun_ms = millis();


  xTaskCreatePinnedToCore(audioPlaybackTask, "audioPlayback", 4096, NULL, 2, NULL, 1);

  loadGladosModule();
  loadSpeakerVolume();


  static String moduleHtml = buildGladosModuleHtml();
  moduleParam = new WiFiManagerParameter(moduleHtml.c_str());
  wm.addParameter(moduleParam);
  wm.setSaveParamsCallback(onWmSaveParams);


  wm.setParamsPage(true);

  setupWifi();

  Serial.print("[diag] free heap before TLS connection: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");
  Serial.print("[diag] largest contiguous block: ");
  Serial.print(ESP.getMaxAllocHeap());
  Serial.println(" bytes");

  if (ws_use_ssl) {
    webSocket.beginSSL(ws_host, ws_port, ws_path);
  } else {
    webSocket.begin(ws_host, ws_port, ws_path);
  }
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(3000);
  webSocket.enableHeartbeat(15000, 5000, 3);


  xTaskCreatePinnedToCore(portalTask, "portal", 8192, NULL, 1, NULL, 0);
}

void loop() {
  webSocket.loop();

  if (pending_module_switch && !portal_active && (millis() - pending_module_switch_at) >= PORTAL_ACTION_SEND_DELAY_MS) {
    if (webSocket.isConnected()) {
      xSemaphoreTake(portal_state_mutex, portMAX_DELAY);
      String module_to_send = pending_module_id;
      pending_module_switch = false;
      pending_portal_greeting_replay = false;
      xSemaphoreGive(portal_state_mutex);

      webSocket.sendTXT("MODULE:" + module_to_send);
      Serial.println("[glados] notified server of module change, greeting will replay");
    } else {
      Serial.println("[glados] module switch pending, websocket not connected yet - will retry");
    }
  }

  if (pending_portal_greeting_replay && !portal_active && (millis() - pending_portal_greeting_replay_at) >= PORTAL_ACTION_SEND_DELAY_MS) {
    if (webSocket.isConnected()) {
      pending_portal_greeting_replay = false;
      webSocket.sendTXT("MODULE:" + selected_glados_module);
      Serial.println("[glados] portal closed, replaying greeting");
    } else {
      Serial.println("[glados] portal-close greeting pending, websocket not connected yet - will retry");
    }
  }

  handleButton();
  micReadAndSend();
  updateLed();

  static unsigned long next_ap_diag = 0;
  if (millis() >= next_ap_diag) {
    next_ap_diag = millis() + 10000;
    Serial.print("[diag] clients on setup access point: ");
    Serial.println(WiFi.softAPgetStationNum());
    Serial.print("[diag] audio underruns so far: ");
    Serial.print(audio_underrun_count);
    Serial.print(", current prebuffer target: ");
    Serial.print(audio_prebuffer_target);
    Serial.println(" bytes");


    if (WiFi.softAPgetStationNum() > 0) {
      WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
                  IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
    }
  }
}