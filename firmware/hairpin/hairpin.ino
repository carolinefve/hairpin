/*
  HAIRPIN VOICE RECORDER - Seeed XIAO ESP32-S3
  ---------------------------------------------
  Press button  -> record (also wakes it up and records straight away)
  Press again   -> stop, upload (or save to flash if no WiFi), sleep

  LED:  on = recording | on/off while sending = uploading
        3 quick flashes = done | 2 slow flashes = saved for later
        fast flashing = error  | tiny blip every 3 s = awake, idle

  Wiring:  mic SD->D3  SCK->D4  WS->D5  VDD->3.3V  GND->GND  L/R->GND
           button 3.3V->D0, 10k resistor D0->GND

  Arduino Tools menu:  Board XIAO_ESP32S3 | PSRAM OPI PSRAM | USB CDC On Boot Enabled
*/

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <vector>
#include <algorithm>
#include "driver/i2s_std.h"
#include "driver/rtc_io.h"

// =================== SETTINGS YOU CHANGE ===================

// GAIN: lower number = louder. Try 13 to 15. Change by 1 at a time.
#define GAIN_SHIFT 15

// WiFi networks, upload address and device key live in secrets.h
#include "secrets.h"

#define MAX_SECONDS         225   // longest recording (memory limit ~240)
#define PART_SECONDS        10    // upload in pieces this long
#define IDLE_SLEEP_SECONDS  180   // sleep after this long doing nothing
#define SWEEP_EVERY_SECONDS 300   // while awake, retry saved files this often
#define TIMEZONE "GMT0BST,M3.5.0/1,M10.5.0"   // UK time

// =================== PINS ===================
#define BUTTON_PIN 1              // D0
#define LED_PIN    21             // onboard LED, on when LOW
#define MIC_SD     GPIO_NUM_4     // D3
#define MIC_SCK    GPIO_NUM_5     // D4
#define MIC_WS     GPIO_NUM_6     // D5
#define RATE       16000          // samples per second

// =================== GLOBALS ===================
RTC_DATA_ATTR bool clockSet = false;   // survives deep sleep

WiFiMulti wifiMulti;
WebServer server(80);
i2s_chan_handle_t mic;

int16_t* audio;            // whole recording, in PSRAM
uint8_t* part;             // one WAV part (header + audio), in PSRAM
uint32_t maxSamples, partBytes;
uint32_t numSamples = 0;        // samples recorded so far
uint32_t skip, clipped, lastPrint;
int32_t  peak;
bool recording = false, flashOK = false, webOn = false;
bool lastButton = false;
String session;
uint32_t lastActivity = 0, lastSweep = 0;

// =================== SMALL HELPERS ===================
void led(bool on) { digitalWrite(LED_PIN, on ? LOW : HIGH); }

void blink(int times, int ms) {
  for (int i = 0; i < times; i++) { led(true); delay(ms); led(false); delay(ms); }
}

void fatal(const char* msg) {
  Serial.println(msg);
  while (true) blink(1, 100);
}

bool urlIsSet() { return String(UPLOAD_URL).indexOf("YOUR-") < 0; }

bool buttonDown() { return digitalRead(BUTTON_PIN) == HIGH; }  // button connects D0 to 3.3V

// A fresh press only counts once; holding or bouncing doesn't repeat it.
bool buttonPressed() {
  bool down = buttonDown();
  if (down && !lastButton) {
    delay(30);                                      // debounce
    if (buttonDown()) { lastButton = true; lastActivity = millis(); return true; }
  }
  if (!down) lastButton = false;                    // released
  return false;
}

void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

// Standard 44-byte WAV header: 16 kHz, 16-bit, mono.
void wavHeader(uint8_t* h, uint32_t dataBytes) {
  memcpy(h, "RIFF", 4);       put32(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
  put16(h + 20, 1);           put16(h + 22, 1);
  put32(h + 24, RATE);        put32(h + 28, RATE * 2);
  put16(h + 32, 2);           put16(h + 34, 16);
  memcpy(h + 36, "data", 4);  put32(h + 40, dataBytes);
}

// =================== WIFI + CLOCK ===================
bool connectWiFi() {
  static bool added = false;
  if (!added) { for (auto& w : WIFI_LIST) wifiMulti.addAP(w[0], w[1]); added = true; }
  WiFi.persistent(false);          // never save WiFi settings to flash
  WiFi.mode(WIFI_STA);
  Serial.println("[wifi] Looking for a known network...");
  if (wifiMulti.run(12000) == WL_CONNECTED) {
    Serial.printf("[wifi] Connected to '%s', IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("[wifi] No known WiFi found. Carrying on offline.");
  WiFi.mode(WIFI_OFF);
  return false;
}

void setClock() {
  if (clockSet && time(nullptr) > 1700000000) return;
  configTzTime(TIMEZONE, "pool.ntp.org");
  for (int i = 0; i < 50 && time(nullptr) < 1700000000; i++) delay(200);
  clockSet = time(nullptr) > 1700000000;
  Serial.println(clockSet ? "[clock] Clock set from the internet" : "[clock] Couldn't get the time");
}

String makeName() {
  char buf[32];
  time_t now = time(nullptr);
  if (now > 1700000000) strftime(buf, sizeof(buf), "%y%m%d-%H%M%S", localtime(&now));
  else snprintf(buf, sizeof(buf), "NOTIME-%lu-%04X", millis(), (unsigned)(esp_random() & 0xFFFF));
  return String(buf);   // the random tag keeps NOTIME names unique after every sleep
}

// =================== FLASH STORAGE ===================
void mountFlash() {
  if (flashOK) return;
  const char* names[] = { "spiffs", "littlefs", "storage" };
  for (const char* n : names) {
    if (LittleFS.begin(true, "/littlefs", 5, n)) {
      flashOK = true;
      Serial.printf("[flash] Ready: %u KB total, %u KB free\n",
                    (unsigned)(LittleFS.totalBytes() / 1024),
                    (unsigned)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));
      return;
    }
    LittleFS.end();
  }
  Serial.println("[flash] Couldn't open flash storage. Offline saving is off.");
}

bool saveToFlash(const String& name, uint32_t len) {
  if (!flashOK || LittleFS.totalBytes() - LittleFS.usedBytes() < len + 8192) return false;
  File f = LittleFS.open("/" + name, FILE_WRITE);
  if (!f) return false;
  bool ok = f.write(part, len) == len;
  f.close();
  Serial.printf("[flash] Saved %s, %u KB free\n", name.c_str(),
                (unsigned)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));
  return ok;
}

// =================== UPLOAD ===================
bool upload(const String& name, uint32_t len) {
  if (!urlIsSet() || WiFi.status() != WL_CONNECTED) return false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();                 // encrypted, certificate not checked
    client.setHandshakeTimeout(30);
    HTTPClient http;
    http.setConnectTimeout(30000);
    http.setTimeout(30000);
    http.begin(client, UPLOAD_URL);
    http.addHeader("Content-Type", "audio/wav");
    http.addHeader("X-Filename", name);
    http.addHeader("X-Device-Key", DEVICE_KEY);
    led(true);
    int code = http.POST(part, len);
    led(false);
    http.end();
    Serial.printf("[upload] %s attempt %d: HTTP %d\n", name.c_str(), attempt, code);
    if (code >= 200 && code < 300) return true;
    delay(2000 * attempt);
  }
  return false;
}

// Send anything waiting on flash. Delete each file only after it's sent.
void sweep() {
  lastSweep = millis();
  if (!flashOK) return;
  std::vector<String> files;
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) files.push_back(f.name());
  root.close();
  std::sort(files.begin(), files.end());       // oldest first, parts in order

  for (String& name : files) {
    File f = LittleFS.open("/" + name, FILE_READ);
    uint32_t len = f.size();
    if (len < 44 + 16000) {                     // under half a second: junk
      f.close(); LittleFS.remove("/" + name);
      Serial.println("[sweep] Deleted tiny file " + name);
      continue;
    }
    if (len > partBytes) { f.close(); continue; }
    f.read(part, len);
    f.close();
    if (!upload(name, len)) break;              // no luck now, try again later
    LittleFS.remove("/" + name);
    Serial.println("[sweep] Sent and deleted " + name);
  }
  Serial.printf("[sweep] Done. %d file(s) were on flash.\n", (int)files.size());
}

// =================== WEB PAGE ===================
void pageList() {
  lastActivity = millis();
  String html = "<html><body style='font-family:sans-serif'><h2>Recordings on the device</h2>";
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String n = f.name();
    html += "<p>" + n + " (" + String((f.size() - 44) / (RATE * 2)) + " s) "
          + "<a href='/dl?f=" + n + "'>download</a> | <a href='/del?f=" + n + "'>delete</a></p>";
  }
  root.close();
  server.send(200, "text/html", html + "</body></html>");
}

void pageDownload() {
  lastActivity = millis();
  String n = server.arg("f");
  if (n.indexOf('/') >= 0 || !LittleFS.exists("/" + n)) { server.send(404, "text/plain", "Not found"); return; }
  File f = LittleFS.open("/" + n, FILE_READ);
  server.streamFile(f, "audio/wav");
  f.close();
}

void pageDelete() {
  lastActivity = millis();
  String n = server.arg("f");
  if (n.indexOf('/') < 0) LittleFS.remove("/" + n);
  server.sendHeader("Location", "/");
  server.send(303);
}

void startWebPage() {
  server.on("/", pageList);
  server.on("/dl", pageDownload);
  server.on("/del", pageDelete);
  server.begin();
  webOn = true;
  Serial.println("[web] Recordings page: http://" + WiFi.localIP().toString() + "/");
}

// =================== MICROPHONE ===================
bool setupMic() {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = 8;
  chan.dma_frame_num = 512;                 // ~0.25 s of buffer inside the chip
  if (i2s_new_channel(&chan, NULL, &mic) != ESP_OK) return false;

  i2s_std_config_t cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED, .bclk = MIC_SCK, .ws = MIC_WS,
      .dout = I2S_GPIO_UNUSED, .din = MIC_SD,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;   // L/R is on GND = LEFT. Wrong slot = silence.
  return i2s_channel_init_std_mode(mic, &cfg) == ESP_OK;
}

// =================== RECORDING ===================
void startRecording() {
  if (webOn) { server.stop(); webOn = false; }
  WiFi.mode(WIFI_OFF);                      // radio off while recording
  session = makeName();
  numSamples = clipped = peak = 0;
  skip = RATE / 10;                         // drop the mic's first 0.1 s (start-up thump)
  lastPrint = millis();
  i2s_channel_enable(mic);
  recording = true;
  led(true);
  Serial.println("\n[rec] RECORDING " + session);
}

void goToSleep();
void stopRecording();


// Read one small block from the mic into the PSRAM buffer.
void recordBlock() {
  static int32_t raw[512];
  size_t bytes = 0;
  i2s_channel_read(mic, raw, sizeof(raw), &bytes, pdMS_TO_TICKS(10));

  for (int i = 0; i < (int)(bytes / 4); i++) {
    if (skip) { skip--; continue; }
    int32_t s = raw[i] >> GAIN_SHIFT;                 // 32-bit -> 16-bit, with gain
    if (s > 32767)  { s = 32767;  clipped++; }        // clamp: no wrap-around clicks
    if (s < -32768) { s = -32768; clipped++; }
    if (abs(s) > peak) peak = abs(s);
    audio[numSamples++] = s;
    if (numSamples >= maxSamples) { Serial.println("[rec] Memory full"); stopRecording(); return; }
  }
  if (millis() - lastPrint > 5000) {
    lastPrint = millis();
    Serial.printf("[rec] %u s\n", (unsigned)(numSamples / RATE));
  }
}

void stopRecording() {
  i2s_channel_disable(mic);
  recording = false;
  led(false);

  Serial.printf("[rec] Stopped: %u s, loudest %d%%, clipped %u\n",
                (unsigned)(numSamples / RATE), (int)(peak * 100 / 32767), (unsigned)clipped);
  if (clipped > numSamples / 1000) Serial.printf("[gain] Too loud. Try GAIN_SHIFT %d\n", GAIN_SHIFT + 1);
  else if (peak < 5000)       Serial.printf("[gain] Too quiet. Try GAIN_SHIFT %d\n", GAIN_SHIFT - 1);
  else                        Serial.println("[gain] Level looks good");

  if (numSamples < RATE) { Serial.println("[rec] Under 1 s, discarded"); goToSleep(); }

  // Recording is over, so flash and WiFi are allowed again.
  mountFlash();
  bool online = connectWiFi();
  if (online) setClock();
  if (!urlIsSet()) Serial.println("[upload] No upload URL yet, saving to flash instead");

  uint32_t perPart = PART_SECONDS * RATE;
  int parts = (numSamples + perPart - 1) / perPart;
  int sent = 0, saved = 0, lost = 0;

  for (int p = 0; p < parts; p++) {
    uint32_t first = p * perPart;
    uint32_t n = min(perPart, numSamples - first);
    wavHeader(part, n * 2);
    memcpy(part + 44, audio + first, n * 2);
    uint32_t len = 44 + n * 2;
    char suffix[16];
    snprintf(suffix, sizeof(suffix), "_p%02dof%02d.wav", p + 1, parts);
    String name = session + suffix;

    if (upload(name, len))           sent++;
    else if (saveToFlash(name, len)) saved++;
    else { lost++; Serial.println("[ERROR] No room on flash, lost " + name); }
  }
  Serial.printf("[upload] %d sent, %d saved for later, %d lost\n", sent, saved, lost);

  if (online) sweep();
  if (lost)       blink(20, 100);   // error
  else if (saved) blink(2, 600);    // saved for later
  else            blink(3, 150);    // done
  goToSleep();
}

// =================== SLEEP ===================
void goToSleep() {
  Serial.println("[sleep] Waiting for button release...");
  while (buttonDown()) delay(10);
  WiFi.mode(WIFI_OFF);
  if (flashOK) LittleFS.end();
  led(false);
  Serial.println("[sleep] Sleeping. Press the button to record.");
  Serial.flush();
  rtc_gpio_pullup_dis(GPIO_NUM_1);
  rtc_gpio_pulldown_en(GPIO_NUM_1);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_1, 1);   // wake when button pin goes HIGH
  esp_deep_sleep_start();
}

// =================== SETUP + LOOP ===================
void setup() {
  bool wokeByButton = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
  pinMode(LED_PIN, OUTPUT);
  led(false);
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  lastButton = buttonDown();              // a held button isn't a new press
  WiFi.persistent(false);
  setenv("TZ", TIMEZONE, 1);
  tzset();

  Serial.begin(115200);
  if (!wokeByButton) delay(1500);         // time to open Serial Monitor
  Serial.println("\n=== Hairpin recorder ===");

  maxSamples = MAX_SECONDS * RATE;
  partBytes = 44 + PART_SECONDS * RATE * 2;
  audio = (int16_t*)ps_malloc(maxSamples * 2);
  part  = (uint8_t*)ps_malloc(partBytes);
  if (!audio || !part) fatal("[ERROR] No PSRAM. Set Tools > PSRAM > OPI PSRAM and upload again.");
  Serial.printf("[mem] PSRAM %u KB, room for %d s of audio\n", (unsigned)(ESP.getPsramSize() / 1024), MAX_SECONDS);
  if (!setupMic()) fatal("[ERROR] Microphone didn't start. Check the pins.");

  if (wokeByButton) { startRecording(); return; }   // woken by a press: record now

  mountFlash();
  if (connectWiFi()) { setClock(); sweep(); startWebPage(); }
  lastActivity = millis();
  Serial.println("[idle] Ready. Press the button to record.");
}

void loop() {
  if (buttonPressed()) {
    if (recording) stopRecording();
    else startRecording();
  }
  if (recording) { recordBlock(); return; }   // nothing else while recording

  if (webOn) server.handleClient();
  if (WiFi.status() == WL_CONNECTED && millis() - lastSweep > SWEEP_EVERY_SECONDS * 1000UL) sweep();
  led(millis() % 3000 < 40);                  // tiny blip = awake
  if (millis() - lastActivity > IDLE_SLEEP_SECONDS * 1000UL) {
    Serial.println("[idle] Nothing happening, going to sleep");
    goToSleep();
  }
}