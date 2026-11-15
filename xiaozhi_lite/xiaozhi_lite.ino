/*
 * Xiaozhi-Lite untuk ESP32-S3 CAM
 * - Mic INMP441 (dengar), LCD ST7789 240x240 (jawaban teks), kamera OV2640/OV5640
 * - Tanpa speaker (MAX98357A tidak dipakai)
 * - 1 panggilan OpenRouter untuk: suara -> jawaban (model multimodal, mis. Gemini Flash)
 * - Setup WiFi + API key lewat WiFi AP "Xiaozhi-Setup" -> buka 192.168.4.1
 *
 * Library (Library Manager): Adafruit GFX, Adafruit ST7789, ArduinoJson (v7)
 * Board : ESP32S3 Dev Module, Arduino-ESP32 core 3.x, PSRAM = Enabled (OPI/QUAD sesuai board),
 *         USB CDC On Boot = Enabled, Partition = Huge APP / 8MB+ (opsional)
 *
 * Pemakaian:
 *   - Bicara saja -> otomatis terdeteksi -> jawaban muncul di layar
 *   - Tekan tombol BOOT sebentar -> ambil foto, (opsional tanya lewat suara 5 dtk), AI menjelaskan foto
 *   - Tahan tombol BOOT 3 detik -> reset WiFi & masuk mode setup lagi
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <vector>
#include "esp_camera.h"
#include "driver/i2s_std.h"
#include "mbedtls/base64.h"

// ================== PIN (sesuai tabel kamu) ==================
#define LCD_SCK   19   // SCL
#define LCD_MOSI  20   // SDA
#define LCD_RST   21   // RES
#define LCD_DC    47
#define LCD_BLK   38

#define I2S_WS    1    // INMP441 WS
#define I2S_SCK   2    // INMP441 SCK
#define I2S_SD    42   // INMP441 SD   (L/R ke GND = kanal kiri)

#define BTN_PIN   0    // tombol BOOT

// ================== PIN KAMERA (default: Freenove ESP32-S3 WROOM CAM) ==================
// Kalau board kamu beda, ganti angka di bawah ini.
#define CAM_PWDN  -1
#define CAM_RESET -1
#define CAM_XCLK  15
#define CAM_SIOD  4
#define CAM_SIOC  5
#define CAM_Y9    16
#define CAM_Y8    17
#define CAM_Y7    18
#define CAM_Y6    12
#define CAM_Y5    10
#define CAM_Y4    8
#define CAM_Y3    9
#define CAM_Y2    11
#define CAM_VSYNC 6
#define CAM_HREF  7
#define CAM_PCLK  13

// ================== SETTING ==================
// Boleh diisi langsung di sini, atau (lebih aman) diisi lewat halaman setup WiFi.
#define DEFAULT_API_KEY ""
#define DEFAULT_MODEL   "openrouter/free"

#define AP_NAME        "Xiaozhi-Setup"
#define SAMPLE_RATE    16000
#define CH             512                       // sample per chunk (32 ms)
#define MAX_SECONDS    8
#define MAX_SAMPLES    (SAMPLE_RATE * MAX_SECONDS)
#define MIN_SAMPLES    12000                     // ucapan minimal ~0.75 dtk
#define SILENCE_CH     31                        // ~1 dtk diam = selesai bicara
#define PRE_CH         4                         // pre-roll biar awal kata tidak terpotong
#define GAIN_SHIFT     14                        // makin kecil = makin keras
#define VAD_MIN        250.0f                    // ambang minimum suara (naikkan kalau sering salah picu)

const char* SYSTEM_PROMPT =
  "Kamu asisten suara kecil di perangkat ESP32 dengan layar kecil. "
  "Jawab dengan bahasa yang dipakai pengguna (default Indonesia). "
  "Jawaban singkat, maksimal 3 kalimat atau 250 karakter. "
  "Tanpa emoji, tanpa markdown, tanpa simbol aneh, hanya huruf biasa.";

// ================== GLOBAL ==================
Adafruit_ST7789 tft(&SPI, -1, LCD_DC, LCD_RST);   // CS tidak dipakai
Preferences prefs;
WebServer server(80);
DNSServer dns;
i2s_chan_handle_t rxChan = NULL;

String cfgSsid, cfgPass, cfgKey, cfgModel;
uint8_t* pcmBuf = NULL;                 // 44 byte header WAV + PCM
int16_t* pcm = NULL;
float noiseFloor = 60.0f;
bool camOK = false;

// ================== LCD ==================
void showStatus(const char* s, uint16_t color) {
  tft.fillRect(0, 0, 240, 26, color);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(6, 5);
  tft.print(s);
}

void showBody(const String& t, uint16_t color = ST77XX_WHITE) {
  tft.fillRect(0, 28, 240, 212, ST77XX_BLACK);
  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.setCursor(0, 34);
  tft.print(t);
}

void showAnswer(String t) {
  String clean;
  for (size_t i = 0; i < t.length(); i++) {
    unsigned char c = t[i];
    if (c == '\n' || c == '\r' || c == '\t') clean += ' ';
    else if (c >= 32 && c < 127 && c != '*' && c != '#' && c != '`') clean += (char)c;
  }
  const int COLS = 20, ROWS = 12;
  std::vector<String> lines;
  String cur;
  int i = 0, L = clean.length();
  while (i < L) {
    int sp = clean.indexOf(' ', i);
    String w = (sp < 0) ? clean.substring(i) : clean.substring(i, sp);
    i = (sp < 0) ? L : sp + 1;
    if (w.length() == 0) continue;
    while ((int)w.length() > COLS) {
      if (cur.length()) { lines.push_back(cur); cur = ""; }
      lines.push_back(w.substring(0, COLS));
      w = w.substring(COLS);
    }
    if ((int)(cur.length() + (cur.length() ? 1 : 0) + w.length()) > COLS) { lines.push_back(cur); cur = w; }
    else { if (cur.length()) cur += ' '; cur += w; }
  }
  if (cur.length()) lines.push_back(cur);
  if (lines.empty()) lines.push_back("(kosong)");

  int pages = (lines.size() + ROWS - 1) / ROWS;
  for (int p = 0; p < pages; p++) {
    tft.fillRect(0, 28, 240, 212, ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    for (int r = 0; r < ROWS; r++) {
      int idx = p * ROWS + r;
      if (idx >= (int)lines.size()) break;
      tft.setCursor(0, 34 + r * 16);
      tft.print(lines[idx]);
    }
    if (pages > 1) {
      tft.setTextSize(1);
      tft.setTextColor(ST77XX_CYAN);
      tft.setCursor(200, 230);
      tft.printf("%d/%d", p + 1, pages);
    }
    if (p < pages - 1) delay(4500);
  }
}

// ================== WIFI PORTAL ==================
String htmlEsc(String s) {
  s.replace("&", "&amp;"); s.replace("\"", "&quot;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  return s;
}

String portalPage() {
  String h = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>Xiaozhi Setup</title><style>"
               "body{font-family:sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:16px}"
               ".c{max-width:420px;margin:auto;background:#1e293b;padding:20px;border-radius:14px}"
               "h2{margin-top:0}label{display:block;margin:14px 0 4px;font-size:14px;color:#94a3b8}"
               "input{width:100%;box-sizing:border-box;padding:12px;border-radius:8px;border:1px solid #334155;"
               "background:#0f172a;color:#fff;font-size:16px}"
               "button{width:100%;margin-top:20px;padding:14px;border:0;border-radius:8px;background:#22c55e;"
               "color:#000;font-size:17px;font-weight:bold}a{color:#38bdf8}</style></head><body><div class='c'>"
               "<h2>Setup Xiaozhi-Lite</h2><form method='POST' action='/save'>"
               "<label>Nama WiFi (SSID)</label><input name='ssid' list='ssids' required value='");
  h += htmlEsc(cfgSsid);
  h += F("'><datalist id='ssids'>");
  int n = WiFi.scanComplete();
  for (int i = 0; i < n; i++) { h += "<option value='" + htmlEsc(WiFi.SSID(i)) + "'>"; }
  h += F("</datalist><label>Password WiFi</label><input name='pass' type='password' value='");
  h += htmlEsc(cfgPass);
  h += F("'><label>OpenRouter API Key</label><input name='key' placeholder='sk-or-v1-...' value='");
  h += htmlEsc(cfgKey);
  h += F("'><label>Model (harus support audio + gambar)</label><input name='model' value='");
  h += htmlEsc(cfgModel);
  h += F("'><button type='submit'>Simpan &amp; Sambungkan</button></form>"
         "<p><a href='/'>Refresh daftar WiFi</a></p></div></body></html>");
  return h;
}

void runPortal() {
  showStatus("Setup WiFi", ST77XX_ORANGE);
  showBody("1. Sambung HP ke WiFi:\n\n  " AP_NAME "\n\n2. Buka browser:\n\n  192.168.4.1");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.scanNetworks(true);                 // scan di background
  WiFi.softAP(AP_NAME);
  delay(300);
  dns.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", portalPage()); });
  server.on("/save", HTTP_POST, []() {
    prefs.putString("ssid", server.arg("ssid"));
    prefs.putString("pass", server.arg("pass"));
    prefs.putString("key", server.arg("key"));
    String m = server.arg("model"); m.trim();
    prefs.putString("model", m.length() ? m : String(DEFAULT_MODEL));
    server.send(200, "text/html",
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<body style='font-family:sans-serif;background:#0f172a;color:#fff;padding:30px'>"
      "<h2>Tersimpan!</h2><p>Perangkat restart & menyambung ke WiFi. Kamu bisa tutup halaman ini.</p>");
    delay(1500);
    ESP.restart();
  });
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
  while (true) {                           // tunggu sampai disimpan (lalu restart)
    dns.processNextRequest();
    server.handleClient();
    delay(2);
  }
}

bool connectWiFi() {
  showStatus("Sambung WiFi", ST77XX_YELLOW);
  showBody(cfgSsid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

// ================== TOMBOL ==================
// return 0 = tidak ditekan, 1 = klik pendek. Tahan 3 dtk = reset WiFi.
int checkButton() {
  if (digitalRead(BTN_PIN) == HIGH) return 0;
  uint32_t t = millis();
  while (digitalRead(BTN_PIN) == LOW) {
    if (millis() - t > 3000) {
      showStatus("Reset WiFi...", ST77XX_RED);
      prefs.remove("ssid"); prefs.remove("pass");
      delay(800);
      ESP.restart();
    }
    delay(10);
  }
  return (millis() - t > 30) ? 1 : 0;
}

// ================== MIC ==================
void micInit() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 6;
  cc.dma_frame_num = 256;
  i2s_new_channel(&cc, NULL, &rxChan);
  i2s_std_config_t sc = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_SCK,
      .ws   = (gpio_num_t)I2S_WS,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)I2S_SD,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;   // L/R pin = GND
  i2s_channel_init_std_mode(rxChan, &sc);
  i2s_channel_enable(rxChan);
}

// baca 1 chunk, return level rata-rata (atau -1 kalau gagal)
float readChunk(int16_t* out) {
  static int32_t raw[CH];
  static float dc = 0;
  size_t br = 0;
  if (i2s_channel_read(rxChan, raw, sizeof(raw), &br, 1000) != ESP_OK || br == 0) return -1;
  int n = br / 4;
  float sum = 0;
  for (int i = 0; i < n; i++) {
    float v = (float)(raw[i] >> GAIN_SHIFT);
    dc += (v - dc) * 0.002f;               // buang DC offset
    v -= dc;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    out[i] = (int16_t)v;
    sum += fabsf(v);
  }
  for (int i = n; i < CH; i++) out[i] = 0;
  return sum / (n ? n : 1);
}

void flushMic() {
  int16_t tmp[CH];
  for (int i = 0; i < 8; i++) readChunk(tmp);
}

// return: jumlah sample, 0 = tidak ada suara, -1 = tombol BOOT ditekan
int recordSpeech(uint32_t maxWaitMs, bool pollBtn) {
  static int16_t pre[PRE_CH][CH];
  int16_t chunk[CH];
  int preIdx = 0, preCount = 0, hot = 0;
  uint32_t t0 = millis();
  uint32_t lastLog = 0;
  float thr = 0;

  while (true) {
    if (pollBtn && checkButton() == 1) return -1;
    if (maxWaitMs && millis() - t0 > maxWaitMs) return 0;
    float lvl = readChunk(chunk);
    if (lvl < 0) continue;
    memcpy(pre[preIdx], chunk, sizeof(chunk));
    preIdx = (preIdx + 1) % PRE_CH;
    if (preCount < PRE_CH) preCount++;
    thr = noiseFloor * 3.0f + VAD_MIN;
    if (lvl > thr) hot++;
    else { hot = 0; noiseFloor = noiseFloor * 0.98f + lvl * 0.02f; }
    if (millis() - lastLog > 2000) {        // bantu tuning di Serial Monitor
      lastLog = millis();
      Serial.printf("level=%.0f  noise=%.0f  thr=%.0f\n", lvl, noiseFloor, thr);
    }
    if (hot >= 3) break;
  }

  showStatus("Merekam...", ST77XX_RED);
  size_t n = 0;
  int start = (preIdx - preCount + PRE_CH) % PRE_CH;
  for (int i = 0; i < preCount; i++) {
    memcpy(pcm + n, pre[(start + i) % PRE_CH], CH * 2);
    n += CH;
  }
  int quiet = 0;
  while (n + CH <= MAX_SAMPLES) {
    float lvl = readChunk(chunk);
    if (lvl < 0) continue;
    memcpy(pcm + n, chunk, CH * 2);
    n += CH;
    if (lvl > thr * 0.7f) quiet = 0; else quiet++;
    if (quiet > SILENCE_CH) break;
  }
  return (n < MIN_SAMPLES) ? 0 : (int)n;
}

void writeWavHeader(uint8_t* b, uint32_t dataLen) {
  uint32_t v32; uint16_t v16;
  memcpy(b, "RIFF", 4);  v32 = 36 + dataLen; memcpy(b + 4, &v32, 4);
  memcpy(b + 8, "WAVEfmt ", 8);
  v32 = 16;          memcpy(b + 16, &v32, 4);
  v16 = 1;           memcpy(b + 20, &v16, 2);
  v16 = 1;           memcpy(b + 22, &v16, 2);
  v32 = SAMPLE_RATE; memcpy(b + 24, &v32, 4);
  v32 = SAMPLE_RATE * 2; memcpy(b + 28, &v32, 4);
  v16 = 2;           memcpy(b + 32, &v16, 2);
  v16 = 16;          memcpy(b + 34, &v16, 2);
  memcpy(b + 36, "data", 4);
  v32 = dataLen;     memcpy(b + 40, &v32, 4);
}

// ================== KAMERA ==================
bool camInit() {
  camera_config_t c;
  memset(&c, 0, sizeof(c));
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = CAM_Y2; c.pin_d1 = CAM_Y3; c.pin_d2 = CAM_Y4; c.pin_d3 = CAM_Y5;
  c.pin_d4 = CAM_Y6; c.pin_d5 = CAM_Y7; c.pin_d6 = CAM_Y8; c.pin_d7 = CAM_Y9;
  c.pin_xclk = CAM_XCLK; c.pin_pclk = CAM_PCLK;
  c.pin_vsync = CAM_VSYNC; c.pin_href = CAM_HREF;
  c.pin_sccb_sda = CAM_SIOD; c.pin_sccb_scl = CAM_SIOC;
  c.pin_pwdn = CAM_PWDN; c.pin_reset = CAM_RESET;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_VGA;            // 640x480 cukup untuk dijelaskan AI
  c.jpeg_quality = 12;
  c.fb_count = 1;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  return esp_camera_init(&c) == ESP_OK;
}

// ================== OPENROUTER ==================
static size_t b64Len(size_t n) { return 4 * ((n + 2) / 3); }

static size_t appendStr(uint8_t* dst, size_t pos, const String& s) {
  memcpy(dst + pos, s.c_str(), s.length());
  return pos + s.length();
}

static size_t appendB64(uint8_t* dst, size_t pos, const uint8_t* src, size_t len) {
  size_t olen = 0;
  mbedtls_base64_encode(dst + pos, b64Len(len) + 1, &olen, src, len);
  return pos + olen;
}

String askLLM(const uint8_t* wav, size_t wavLen, const uint8_t* jpg, size_t jpgLen, const char* prompt) {
  if (cfgKey.length() < 10) return "API key belum diisi. Tahan tombol BOOT 3 dtk untuk setup ulang.";

  String head = String("{\"model\":\"") + cfgModel + "\",\"max_tokens\":400,\"messages\":["
                "{\"role\":\"system\",\"content\":\"" + SYSTEM_PROMPT + "\"},"
                "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"" + prompt + "\"}";
  String imgHead = ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
  String imgTail = "\"}}";
  String audHead = ",{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"";
  String audTail = "\",\"format\":\"wav\"}}";
  String tail = "]}]}";

  size_t total = head.length() + tail.length() + 16;
  if (jpg) total += imgHead.length() + b64Len(jpgLen) + imgTail.length();
  if (wav) total += audHead.length() + b64Len(wavLen) + audTail.length();

  uint8_t* body = (uint8_t*)ps_malloc(total + 8);
  if (!body) return "Memori tidak cukup (PSRAM aktif?)";

  size_t p = 0;
  p = appendStr(body, p, head);
  if (jpg) { p = appendStr(body, p, imgHead); p = appendB64(body, p, jpg, jpgLen); p = appendStr(body, p, imgTail); }
  if (wav) { p = appendStr(body, p, audHead); p = appendB64(body, p, wav, wavLen); p = appendStr(body, p, audTail); }
  p = appendStr(body, p, tail);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://openrouter.ai/api/v1/chat/completions");
  http.setTimeout(60000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + cfgKey);
  http.addHeader("HTTP-Referer", "https://github.com/xiaozhi-lite");
  http.addHeader("X-Title", "Xiaozhi-Lite ESP32");

  int code = http.POST(body, p);
  String resp = http.getString();
  http.end();
  free(body);
  Serial.printf("HTTP %d, %u byte\n", code, (unsigned)resp.length());

  if (code <= 0) return String("Gagal koneksi: ") + HTTPClient::errorToString(code);

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return String("Respon tidak valid (HTTP ") + code + ")";
  if (code != 200) {
    const char* em = doc["error"]["message"] | "error";
    return String("Error ") + code + ": " + em;
  }
  const char* txt = doc["choices"][0]["message"]["content"] | "";
  return String(txt);
}

// ================== ALUR UTAMA ==================
void doPhoto() {
  if (!camOK) { showStatus("Kamera error", ST77XX_RED); showBody("Kamera tidak terdeteksi.\nCek pin kamera."); delay(2500); return; }
  showStatus("Foto...", ST77XX_CYAN);
  camera_fb_t* fb = esp_camera_fb_get();   // buang frame lama
  if (fb) esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();
  if (!fb) { showStatus("Foto gagal", ST77XX_RED); delay(2000); return; }

  showStatus("Tanya? (5 dtk)", ST77XX_GREEN);
  showBody("Foto diambil.\n\nTanya lewat suara\n(atau diam saja = AI\nmenjelaskan foto)", ST77XX_CYAN);
  flushMic();
  int n = recordSpeech(5000, false);

  showStatus("Berpikir...", ST77XX_YELLOW);
  String r;
  if (n > 0) {
    writeWavHeader(pcmBuf, n * 2);
    r = askLLM(pcmBuf, 44 + n * 2, fb->buf, fb->len,
               "Ini foto dari kamera. Audio berikut adalah pertanyaan pengguna tentang foto ini. Jawab pertanyaannya.");
  } else {
    r = askLLM(NULL, 0, fb->buf, fb->len, "Jelaskan apa yang terlihat di foto ini secara singkat.");
  }
  esp_camera_fb_return(fb);
  showStatus("Jawaban", ST77XX_GREEN);
  showAnswer(r);
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LCD_BLK, OUTPUT);
  digitalWrite(LCD_BLK, HIGH);

  SPI.begin(LCD_SCK, -1, LCD_MOSI, -1);
  tft.init(240, 240, SPI_MODE3);
  tft.setRotation(0);                       // coba 1/2/3 kalau layar terbalik
  tft.fillScreen(ST77XX_BLACK);
  showStatus("Booting...", ST77XX_WHITE);

  if (!psramFound()) { showBody("PSRAM tidak aktif!\nTools > PSRAM > Enabled", ST77XX_RED); while (1) delay(1000); }
  pcmBuf = (uint8_t*)ps_malloc(44 + MAX_SAMPLES * 2);
  pcm = (int16_t*)(pcmBuf + 44);

  camOK = camInit();
  Serial.println(camOK ? "Kamera OK" : "Kamera GAGAL");

  prefs.begin("cfg", false);
  cfgSsid  = prefs.getString("ssid", "");
  cfgPass  = prefs.getString("pass", "");
  cfgKey   = prefs.getString("key", DEFAULT_API_KEY);
  cfgModel = prefs.getString("model", DEFAULT_MODEL);

  if (cfgSsid.length() == 0 || !connectWiFi() || cfgKey.length() < 10) {
    runPortal();                            // tidak return (restart setelah simpan)
  }

  micInit();
  showStatus("Siap!", ST77XX_GREEN);
  showBody("WiFi tersambung.\n\nBicara langsung ke\nmic, atau tekan BOOT\nuntuk foto.", ST77XX_CYAN);
  delay(1500);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    showStatus("WiFi putus", ST77XX_RED);
    WiFi.reconnect();
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(250);
    return;
  }

  showStatus("Mendengarkan", ST77XX_GREEN);
  flushMic();
  int n = recordSpeech(0, true);

  if (n == -1) { doPhoto(); return; }
  if (n > 0) {
    showStatus("Berpikir...", ST77XX_YELLOW);
    writeWavHeader(pcmBuf, n * 2);
    String r = askLLM(pcmBuf, 44 + n * 2, NULL, 0,
                      "Dengarkan audio berikut (suara pengguna) lalu jawab langsung pertanyaan atau permintaannya.");
    showStatus("Jawaban", ST77XX_GREEN);
    showAnswer(r);
  }
}
