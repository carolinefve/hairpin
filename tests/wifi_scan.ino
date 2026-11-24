/*
  WIFI SCAN TEST
  Lists every WiFi network the board can see, every 10 seconds.
  Nothing else. Open the Serial Monitor at 115200.
  Keep the Personal Hotspot screen open on your iPhone while it runs.
*/

#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(1500);
  WiFi.mode(WIFI_STA);
  Serial.println("\n=== WIFI SCAN ===");
}

void loop() {
  Serial.println("Scanning...");
  int found = WiFi.scanNetworks();
  if (found == 0) Serial.println("  No networks found at all.");
  for (int i = 0; i < found; i++) {
    Serial.printf("  [%s]  signal %d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
  }
  Serial.println();
  delay(10000);
}