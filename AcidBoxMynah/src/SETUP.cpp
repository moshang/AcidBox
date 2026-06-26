/*
 *  SETUP.cpp - Hardware initialization for AcidBox MYNAH
 */

#include "SETUP.h"
#include <Arduino.h>
#include "config.h"
#include "general.h"
#include "fx_delay.h"
#include "compressor.h"
#include "synthvoice.h"
#include "sampler.h"
#include <Wire.h>
#include "soc/rtc_cntl_reg.h"
#include <FS.h>
#include <SD_MMC.h>
#include <string.h>
#include <NeoPixelBus.h>

#ifndef NO_PSRAM
#include "fx_reverb.h"
#endif

// Task handles defined in main.cpp
extern TaskHandle_t SynthTask1;
extern TaskHandle_t SynthTask2;
extern TaskHandle_t uiTaskHandle;

// Processing flag defined in main.cpp
extern volatile boolean processing;

// Forward declarations for audio/UI tasks defined in main.cpp
extern void IRAM_ATTR audio_task1(void *userData);
extern void IRAM_ATTR audio_task2(void *userData);
extern void uiCoreTask(void* parameter);

// Timer variables defined in main.cpp
extern hw_timer_t * timer2;
extern void IRAM_ATTR onTimer2();

/*
 *  Hardware and system initialization
 */
void setup(void) {

  // ---------- BROWNOUT DETECTOR: disabled ----------
  // Prevents false brownout resets from transient current spikes
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  btStop(); // we don't want bluetooth to consume our precious cpu time 

  Serial.begin(115200);
  Serial.setTimeout(100);
  
  // Wait for Serial connection (max 3 seconds)
  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait < 3000)) {
    delay(10);
  }
  delay(200);
  
  Serial.println("=================================");
  Serial.println("AcidBox Mynah starting...");
  Serial.println("=================================");

  // ---------- OLED SPLASH ----------
  Serial.println("Initializing OLED...");
  oledInit();
  Serial.println("OLED initialized");

  // ---------- NEOPIXEL INIT ----------
  Serial.println("Initializing NeoPixel...");
  neopixelInit();
  Serial.println("NeoPixel initialized");

  // ---------- BUTTONS (Shift Register) ----------
  Serial.println("Initializing buttons...");
  initShiftRegister();
  Serial.println("Buttons initialized");

  // ---------- SD CARD INIT ----------
  Serial.println("Initializing SD Card (SDMMC 1-bit mode)...");
  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);
  if (!SD_MMC.begin("/sdcard", true))
  {
    Serial.println("SD Card Mount Failed!");
    Serial.println("   Check:");
    Serial.println("   - SD card is inserted");
    Serial.println("   - SD card is formatted (FAT32)");
    Serial.println("   - Wiring: CLK=18, CMD=17, D0=16 (SDMMC mode)");
    sdCardAvailable = false;
  }
  else
  {
    Serial.println("SD Card Initialized via SDMMC (High Speed)");
    sdCardAvailable = true;
  }

  if (!sdCardAvailable) {
    Serial.println("SD card not available - samples cannot be loaded from SD");
  }

  
  MidiInit(); // init midi input and handling of midi events

  buildTables();

  Serial.println("Initializing Synth1...");
  Synth1.Init();
  Serial.println("Initializing Synth2...");
  Synth2.Init();
  Serial.println("Initializing Drums...");
  Drums.Init();
#ifndef NO_PSRAM
  Serial.println("Initializing Reverb...");
  Reverb.Init();
#endif
  Serial.println("Initializing Delay...");
  Delay.Init();
  Serial.println("Initializing Compressor...");
  Comp.Init(SAMPLE_RATE);
#ifdef JUKEBOX
  init_midi(); // AcidBanger function
#endif

  // silence while we haven't loaded anything reasonable
  for (int i = 0; i < DMA_BUF_LEN; i++) {
    drums_buf_l[current_gen_buf][i] = 0.0f ;
    drums_buf_r[current_gen_buf][i] = 0.0f ;
    synth1_buf[current_gen_buf][i] = 0.0f ;
    synth2_buf[current_gen_buf][i] = 0.0f ;
    out_buf[current_out_buf]._signed[i * 2] = 0 ;
    out_buf[current_out_buf]._signed[i * 2 + 1] = 0 ;
    mix_buf_l[current_out_buf][i] = 0.0f;
    mix_buf_r[current_out_buf][i] = 0.0f;
  }

  Serial.println("Initializing I2S...");
  i2sInit();
  Serial.println("I2S initialized");

  Serial.println("Creating audio tasks...");
  xTaskCreatePinnedToCore( audio_task1, "SynthTask1", 8192, NULL, 1, &SynthTask1, 0 );
  xTaskCreatePinnedToCore( audio_task2, "SynthTask2", 8192, NULL, 1, &SynthTask2, 1 );

  // ---- UI TASK ON CORE 0 ----
  // Separate UI task for button/pot/neopixel polling on core 0 (same core as SynthTask1)
  Serial.println("Creating UI task on Core 0...");
  xTaskCreatePinnedToCore(
      uiCoreTask,       // Task function
      "UI_Core_Task",   // Task name
      4096,             // Stack size
      NULL,             // Parameters
      0,                // Priority (lower than SynthTask1)
      &uiTaskHandle,    // Task handle
      0                 // Core 0
  );
  Serial.println("UI task created on Core 0");

  // somehow we should allow tasks to run
  xTaskNotifyGive(SynthTask1);
  //  xTaskNotifyGive(SynthTask2);
  processing = true;

#if ESP_ARDUINO_VERSION_MAJOR < 3 
  // timer interrupt
  timer2 = timerBegin(1, 80, true);               // Setup general purpose timer
  timerAttachInterrupt(timer2, &onTimer2, true);  // Attach callback
  timerAlarmWrite(timer2, 200000, true);          // 200ms, autoreload
  timerAlarmEnable(timer2);
#else 
  timer2 = timerBegin(1000000);               // Setup general purpose timer
  timerAttachInterrupt(timer2, &onTimer2);  // Attach callback
  timerAlarm(timer2, 200000, true, 0);          // 200ms, autoreload
#endif

  Serial.println("=================================");
  Serial.println("Setup complete!");
  Serial.println("=================================");
}