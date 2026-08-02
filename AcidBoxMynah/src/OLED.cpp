#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "config.h"
#include "general.h"
#include "sampler.h"
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

    // ---- UI_BPM mode: show BPM on line 1, value on line 2 ----
    if (currentUiMode == UI_BPM)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("BPM");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", globalSeq.bpm);
        u8g2.print(buf);

        u8g2.sendBuffer();
        refreshOLED = false;
        return;
    }

    // ---- UI_SWING mode: show SWING on line 1, value on line 2 ----
    if (currentUiMode == UI_SWING)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("SWING");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f%%", globalSeq.swing);
        u8g2.print(buf);

        u8g2.sendBuffer();
        refreshOLED = false;
        return;
    }

    // ---- UI_KITS mode: show KITS on line 1, kit folder name on line 2 ----
    if (currentUiMode == UI_KITS)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("KITS");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        int kc = Drums.GetKitCount();
        if (kc > 0)
        {
            int ki = Drums.GetKitIndex();
            char buf[32];
            snprintf(buf, sizeof(buf), "%s", Drums.GetKitName(ki));
            u8g2.print(buf);
        }
        else
        {
            u8g2.print("NO KITS");
        }

        u8g2.sendBuffer();
        refreshOLED = false;
        return;
    }

    // ---- UI_MASTERVOL mode: show VOLUME on line 1, value on line 2 ----
    if (currentUiMode == UI_MASTERVOL)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("VOLUME");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", (int)(masterVolume * 100.0f));
        u8g2.print(buf);

        u8g2.sendBuffer();
        refreshOLED = false;
        return;
    }

    // ---- UI_PATTERN_SELECT mode ----
    if (currentUiMode == UI_PATTERN_SELECT)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("PATTERN");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        // Show current pattern slot (1-16) and bank/song context
        char buf[32];
        snprintf(buf, sizeof(buf), "P%02d  S%02d B%02d",
                 acidBoxSaveLoad.currentPattern + 1,
                 acidBoxSaveLoad.currentSong + 1,
                 acidBoxSaveLoad.currentBank + 1);
        u8g2.print(buf);

        u8g2.sendBuffer();
        refreshOLED = false;
        return;
    }

    // ---- UI_SONG_SELECT mode ----
    if (currentUiMode == UI_SONG_SELECT)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("SONG");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        char buf[32];
        snprintf(buf, sizeof(buf), "S%02d  B%02d",
                 acidBoxSaveLoad.currentSong + 1,
                 acidBoxSaveLoad.currentBank + 1);
        u8g2.print(buf);

        u8g2.sendBuffer();
        refreshOLED = false;
        return;
    }

    // ---- UI_BANK_SELECT mode ----
    if (currentUiMode == UI_BANK_SELECT)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("BANK");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        char buf[32];
        snprintf(buf, sizeof(buf), "B%02d", acidBoxSaveLoad.currentBank + 1);
        u8g2.print(buf);

        u8g2.sendBuffer();
        refreshOLED = false;
        return;
    }

    // ---- UI_CLEARPART mode: show "PART" on line 1, "CLEAR [F5]" on line 2 ----
    if (currentUiMode == UI_CLEARPART)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("PART");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        u8g2.print("CLEAR [F5]");

        u8g2.sendBuffer();
        refreshOLED = false;
        return;
    }

    // ---- UI_CLEARPATTERN mode: show "PATTERN" on line 1, "CLEAR [F6]" on line 2 ----
    if (currentUiMode == UI_CLEARPATTERN)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("PATTERN");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        u8g2.print("CLEAR [F6]");

        u8g2.sendBuffer();
        refreshOLED = false;
        return;
    }

    // ---- UI_PARTGEN mode: show "PART" on line 1, "CREATE [F5]" on line 2 ----
    if (currentUiMode == UI_PARTGEN)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("PART");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        u8g2.print("CREATE [F5]");

        u8g2.sendBuffer();
        refreshOLED = false;
        return;
    }

    // ---- UI_PATTERNGEN mode: show "PATTERN" on line 1, "CREATE [F6]" on line 2 ----
    if (currentUiMode == UI_PATTERNGEN)
    {
        u8g2.setFont(u8g2_font_helvB14_tf);
        u8g2.setCursor(0, 25);
        u8g2.print("PATTERN");

        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.setCursor(0, 45);
        u8g2.print("CREATE [F6]");

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
