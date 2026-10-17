/* Type AT commands to the A7672S through the ESP32's USB port.
   Flash this first: it proves the UART wiring and lets you run the
   bring-up commands by hand before trusting the main firmware.
   Pins come from the real config.h so there is one source of truth. */
#include "../../firmware/multisense_s3_a7672s/config.h"

void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial1.begin(MODEM_BAUD, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
#if PIN_MODEM_PWRKEY >= 0
  pinMode(PIN_MODEM_PWRKEY, OUTPUT);
  digitalWrite(PIN_MODEM_PWRKEY, HIGH); delay(100);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);  delay(1200);   // >1 s = power on
  digitalWrite(PIN_MODEM_PWRKEY, HIGH);
#endif
  Serial.println("\nA7672S passthrough. Set the Serial Monitor to 115200 and "
                 "line ending = Both NL & CR, then type: AT");
}

void loop() {
  while (Serial.available())  Serial1.write(Serial.read());
  while (Serial1.available()) Serial.write(Serial1.read());
}
