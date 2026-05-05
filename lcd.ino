#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup() {
  Wire.begin();
  delay(100);
  lcd.init();
  lcd.init();   // tweede keer fix voor UNO R4 timing
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("LCD Test OK!");
  lcd.setCursor(0, 1);
  lcd.print("Kromhout WP");
}

void loop() {}