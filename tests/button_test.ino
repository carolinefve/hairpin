/*
  BUTTON TEST
  Prints what the board reads on the button pin (D0) five times a second.
  Open the Serial Monitor at 115200. Don't touch the button for a few
  seconds, then press and hold it, then let go.
*/

#define BUTTON_PIN 1 // D0

void setup()
{
    Serial.begin(115200);
    delay(1500);
    pinMode(BUTTON_PIN, INPUT_PULLDOWN);
    Serial.println("\n=== BUTTON TEST ===");
}

void loop()
{
    Serial.println(digitalRead(BUTTON_PIN) == HIGH ? "button: PRESSED" : "button: released");
    delay(200);
}