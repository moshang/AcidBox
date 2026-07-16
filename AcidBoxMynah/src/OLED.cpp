#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "config.h"
#include "general.h"
#include "UI.h"
#include "sequencer.h"
#include "SCALES.h"

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

    u8g2.setFont(u8g2_font_helvB12_tf);
    u8g2.setCursor(0, 20);
    u8g2.print("AcidBox");

    u8g2.setFont(u8g2_font_helvR08_tf);
    u8g2.setCursor(0, 37);
    u8g2.print("MYNAH");
    u8g2.setCursor(0, 52);
    u8g2.print(VERSION);

    u8g2.sendBuffer();
    delay(1000); // Hold splash for 1 second
}

void updateOLED()
{
    if (!refreshOLED)
        return;

    u8g2.clearBuffer();

    // ---- UI_SCALE mode: show SCALE on line 1, scale name on line 2 ----
    if (currentUiMode == UI_SCALE)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("SCALE");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        u8g2.print(scaleName);

        u8g2.sendBuffer();
        refreshOLED = false;
        return;
    }

    // ---- UI_ROOT mode: show ROOT on line 1, root note on line 2 ----
    if (currentUiMode == UI_ROOT)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("ROOT");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        char rootBuf[8];
        getRootNoteName(rootBuf, sizeof(rootBuf));
        u8g2.print(rootBuf);

        u8g2.sendBuffer();
        refreshOLED = false;
        return;
    }

    // ---- Normal mode: original display ----
    u8g2.setFont(u8g2_font_helvB14_tf);
    u8g2.setCursor(0, 25);
    u8g2.print(editTypeNames[currentEditType]);

    // Show mute indicator if current voice is muted
    bool isMuted = false;
    switch (currentEditType)
    {
    case Syn1:
        isMuted = muteSynth1;
        break;
    case Syn2:
        isMuted = muteSynth2;
        break;
    case Drm:
        isMuted = muteDrums;
        break;
    default:
        break;
    }

    u8g2.setFont(u8g2_font_helvB10_tf);
    u8g2.setCursor(0, 45);
    if (currentEditType == Drm)
    {
        // In sequencer (EDIT) mode:
        //   - If F1 is held, show the drum edit parameter name (user is changing it)
        //   - Otherwise show the drum lane name (e.g. "BD", "SD", "CH")
        // In jukebox mode show the drum edit parameter name
        if (currentMode == MODE_EDIT && !isButtonPressed(BTN_F1))
        {
            u8g2.print(drumLaneNames[currentDrumLaneIndex]);
        }
        else
        {
            u8g2.print(drumEditModeNames[currentDrumEditMode]);
        }
    }
    else
    {
        u8g2.print(synthEditModeNames[currentEditMode]);
    }

    if (isMuted)
    {
        // u8g2.setFont(u8g2_font_streamline_all_t);
        // u8g2.drawGlyph(65, 50, 326); // lock icon glyph
        u8g2.setFont(u8g2_font_siji_t_6x10);
        u8g2.drawGlyphX2(73, 27, 57423); // mute icon glyph
        u8g2.setFont(u8g2_font_helvR14_tf);
    }

    u8g2.sendBuffer();
    refreshOLED = false;
}
