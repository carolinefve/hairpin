/*
  ===================================================================
  MIC TEST  -  XIAO ESP32-S3 + INMP441
  ===================================================================
  What this does:
    Reads the microphone and prints a "level" bar to the Serial
    Monitor ten times a second. Nothing else. No WiFi, no recording.

  What you should see (Serial Monitor set to 115200 baud):
    Silence  ->  small number, short bar      e.g.  level    40 |#
    Talking  ->  much bigger number, long bar e.g.  level  1800 |##########
    If talking makes the number jump to several times the silent
    number, the mic works.

  Wiring this sketch expects (from your schematic):
    INMP441 VDD -> 3.3V
    INMP441 GND -> GND
    INMP441 L/R -> GND   (this is the "SEL" pin - MUST be GND)
    INMP441 SD  -> D3  (GPIO4)   data from the mic
    INMP441 SCK -> D4  (GPIO5)   bit clock
    INMP441 WS  -> D5  (GPIO6)   word select / left-right clock

  ===================================================================
*/

#include "driver/i2s_std.h"   // the ESP32's built-in audio (I2S) driver

// ---- Pins (GPIO numbers, not the D-numbers printed on the board) ----
#define MIC_SD_PIN   GPIO_NUM_4   // D3
#define MIC_SCK_PIN  GPIO_NUM_5   // D4
#define MIC_WS_PIN   GPIO_NUM_6   // D5

#define SAMPLE_RATE  16000        // 16,000 samples per second

i2s_chan_handle_t micChannel;     // "handle" = our connection to the mic

int32_t rawBlock[256];            // one small block of raw 32-bit samples

// Running totals for the current 100 ms window
int64_t sumOfSamples   = 0;
int64_t sumOfSquares   = 0;
int32_t peak           = 0;
uint32_t sampleCount   = 0;
uint32_t zeroCount     = 0;       // how many samples were exactly 0
uint32_t lastPrintMs   = 0;

void setup() {
  Serial.begin(115200);
  delay(1500);                    // give the Serial Monitor time to connect
  Serial.println();
  Serial.println("=== MIC TEST starting ===");

  // 1. Create a receive channel on I2S port 0, with the ESP32 as the clock master
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chanCfg, NULL, &micChannel) != ESP_OK) {
    Serial.println("ERROR: could not create I2S channel");
    while (true) delay(1000);
  }

  // 2. Describe the audio format: 16 kHz, 32-bit slots, one channel (mono)
  i2s_std_config_t stdCfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = MIC_SCK_PIN,
      .ws   = MIC_WS_PIN,
      .dout = I2S_GPIO_UNUSED,    // we never send audio OUT
      .din  = MIC_SD_PIN,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  // L/R is wired to GND, so the mic talks in the LEFT slot. Listen there.
  stdCfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  // 3. Apply the settings and switch the mic on
  if (i2s_channel_init_std_mode(micChannel, &stdCfg) != ESP_OK ||
      i2s_channel_enable(micChannel) != ESP_OK) {
    Serial.println("ERROR: could not start I2S. Check the pin numbers.");
    while (true) delay(1000);
  }

  Serial.println("Mic started. Stay quiet for a few seconds, then talk.");
  Serial.println();
}

void loop() {
  // Read one small block. Wait at most 20 ms so loop() never gets stuck.
  size_t bytesRead = 0;
  i2s_channel_read(micChannel, rawBlock, sizeof(rawBlock), &bytesRead, pdMS_TO_TICKS(20));
  int samples = bytesRead / sizeof(int32_t);

  for (int i = 0; i < samples; i++) {
    // The mic sends 24-bit audio sitting in the top of a 32-bit number.
    // Shifting right by 16 gives a normal 16-bit-sized value.
    int32_t s = rawBlock[i] >> 16;
    if (rawBlock[i] == 0) zeroCount++;
    sumOfSamples += s;
    sumOfSquares += (int64_t)s * s;
    sampleCount++;
  }

  // Every 100 ms, print one line
  if (millis() - lastPrintMs >= 100 && sampleCount > 0) {
    lastPrintMs = millis();

    // Remove the mic's small constant offset, then measure loudness (RMS)
    double mean     = (double)sumOfSamples / sampleCount;
    double meanSq   = (double)sumOfSquares / sampleCount;
    double variance = meanSq - mean * mean;
    int level       = variance > 0 ? (int)sqrt(variance) : 0;

    // Draw a bar: one # per 100 units, max 50
    int bars = min(level / 100, 50);
    char bar[52];
    memset(bar, '#', bars);
    bar[bars] = '\0';

    Serial.printf("level %5d |%s\n", level, bar);

    if (zeroCount == sampleCount) {
      Serial.println("   ^ every sample is exactly 0 - check that L/R goes to GND and the wiring");
    }

    sumOfSamples = sumOfSquares = 0;
    sampleCount = zeroCount = 0;
  }
}