#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "config.h"

// OLED display (128x64, hardware I2C) — same SSD1306 as MYNAH/RGB
// HW_I2C constructor only takes rotation + reset pin. I2C pins are set via Wire.begin().
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2, U8X8_PIN_NONE);

// ---------- OLED SPLASH ----------
// Shows "AcidBox" on the display during boot.
// Call this as early as possible in setup().
void oledInit()
{
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

    u8g2.begin();
    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_helvB10_tf);
    u8g2.setCursor(0, 20);
    u8g2.print("AcidBox");

    u8g2.setFont(u8g2_font_helvR08_tf);
    u8g2.setCursor(0, 37);
    u8g2.print("MYNAH");
    u8g2.setCursor(0, 52);
    u8g2.print(VERSION);

    u8g2.sendBuffer();
    delay(1000);  // Hold splash for 1 second
}
