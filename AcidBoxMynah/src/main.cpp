/*

  AcidBox
  ESP32 acid combo of 303 + 303 + 808 like synths. MIDI driven. I2S output to DAC. No indication. Uses both cores of ESP32.

  To build the thing
  You will need an ESP32 with PSRAM (ESP32 WROVER module). Preferrable an external DAC, like PCM5102. In ArduinoIDE Tools menu select:

* * Board: "ESP32 Dev Module" or "ESP32S3 Dev Module"
* * Partition scheme: No OTA (1MB APP/ 3MB SPIFFS)
* * PSRAM: "enabled" or "OPI PSRAM" or what type you have


  !!!!!!!! ATTENTION !!!!!!!!!
  You will need to upload samples from /data folder to the ESP32 flash, otherwise you'll only have 40kB samples from samples.h. 
  To upload samples follow the instructions:
  
  https://github.com/lorol/LITTLEFS#arduino-esp32-littlefs-filesystem-upload-tool
  And then use Tools -> ESP32 Sketch Data Upload

*/
#include <Arduino.h>
#include "config.h"
#include "general.h"
#include "fx_delay.h"
#ifndef NO_PSRAM
#include "fx_reverb.h"
#endif
#include "compressor.h"
#include "synthvoice.h"
#include "sampler.h"
#include <Wire.h>
#include "soc/rtc_cntl_reg.h"
#include <Adafruit_NeoPixel.h>
#include <FS.h>
#include <SD_MMC.h>
#include <string.h>


// lookuptables
float midi_pitches[128];
float midi_phase_steps[128];
float midi_tbl_steps[128];
float exp_square_tbl[TABLE_SIZE+1];
//static float square_tbl[TABLE_SIZE+1];
float saw_tbl[TABLE_SIZE+1];
float exp_tbl[TABLE_SIZE+1];
float knob_tbl[TABLE_SIZE+1]; // exp-like curve
float shaper_tbl[TABLE_SIZE+1]; // illinear tanh()-like curve
float lim_tbl[TABLE_SIZE+1]; // diode soft clipping at about 1.0
float sin_tbl[TABLE_SIZE+1];
float norm1_tbl[16][16]; // cutoff-reso pair gain compensation
float norm2_tbl[16][16]; // wavefolder-overdrive gain compensation
//static float (*tables[])[TABLE_SIZE+1] = {&exp_square_tbl, &square_tbl, &saw_tbl, &exp_tbl};

// service variables and arrays
volatile uint32_t s1t, s2t, drt, fxt, s1T, s2T, drT, fxT, art, arT, c0t, c0T, c1t, c1T; // debug timing: if we use less vars, compiler optimizes them
volatile uint32_t prescaler;
uint32_t  last_reset = 0;
float     param[POT_NUM];
uint8_t    ctrl_hold_notes;

// ---- MYNAH HARDWARE GLOBALS ----
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
volatile uint32_t buttonStates = 0;
volatile uint32_t lastButtonStates = 0;
bool anyStepButtonHeld = false;
bool sdCardAvailable = false;
volatile bool ledsDirty = false;

// Audio buffers of all kinds
volatile uint8_t current_gen_buf = 0; // set of buffers for generation
volatile uint8_t current_out_buf = 1 - 0; // set of buffers for output
float synth1_buf[2][DMA_BUF_LEN];    // synth1 mono
float synth2_buf[2][DMA_BUF_LEN];    // synth2 mono
float drums_buf_l[2][DMA_BUF_LEN];   // drums L
float drums_buf_r[2][DMA_BUF_LEN];   // drums R
float mix_buf_l[2][DMA_BUF_LEN];     // mix L channel
float mix_buf_r[2][DMA_BUF_LEN];     // mix R channel
out_buf_u out_buf[2];                               // i2s L+R output buffer
size_t bytes_written;                       // i2s result

volatile boolean processing = false;
#ifndef NO_PSRAM
volatile float rvb_k1, rvb_k2, rvb_k3;
#endif
volatile float dly_k1, dly_k2, dly_k3;

// tasks for Core0 and Core1
TaskHandle_t SynthTask1;
TaskHandle_t SynthTask2;
TaskHandle_t uiTaskHandle = NULL;

// 303-like synths
SynthVoice Synth1(0); // instance 0 to recognize from the inside
SynthVoice Synth2(1); // instance 1 to recognize from the inside

// 808-like drums
Sampler Drums( DEFAULT_DRUMKIT ); // argument: starting drumset [0 .. total-1]

// Global effects
FxDelay Delay;
#ifndef NO_PSRAM
FxReverb Reverb;
#endif
Compressor Comp;

hw_timer_t * timer1 = NULL;            // Timer variables
hw_timer_t * timer2 = NULL;            // Timer variables
portMUX_TYPE timer1Mux = portMUX_INITIALIZER_UNLOCKED; 
portMUX_TYPE timer2Mux = portMUX_INITIALIZER_UNLOCKED; 
volatile boolean timer1_fired = false;   // Update battery icon flag
volatile boolean timer2_fired = false;   // Update battery icon flag

/*
 * Timer interrupt handler **********************************************************************************************************************************
*/

void IRAM_ATTR onTimer1() {
   portENTER_CRITICAL_ISR(&timer1Mux);
   timer1_fired = true;
   portEXIT_CRITICAL_ISR(&timer1Mux);
}

void IRAM_ATTR onTimer2() {
   portENTER_CRITICAL_ISR(&timer2Mux);
   timer2_fired = true;
   portEXIT_CRITICAL_ISR(&timer2Mux);
}



/* 
 * Core Tasks ************************************************************************************************************************
*/
// forward declaration
void IRAM_ATTR mixer() ;
void regular_checks();
void readPots();
void paramChange(uint8_t paramNum, float paramVal);
// Core0 task 
// static void audio_task1(void *userData) {
static void IRAM_ATTR audio_task1(void *userData) {
  
  while (true) {
    taskYIELD(); 
    if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) { // we need all the generators to fill the buffers here, so we wait
      c0t = micros();
      
//      taskYIELD(); 
      
      current_gen_buf = current_out_buf;      // swap buffers
      current_out_buf = 1 - current_gen_buf;
      
      xTaskNotifyGive(SynthTask2);            // if we are here, then we've already received a notification from task2
      
      s1t = micros();
      synth1_generate();
      s1T = micros() - s1t;
      
  //    taskYIELD(); 

      drt = micros();
      drums_generate();
      drT = micros() - drt;

    }
    
   // taskYIELD();

    taskYIELD();

    c0T = micros() - c0t;
  }
}

// task for Core1, which tipically runs user's code on ESP32
// static void IRAM_ATTR audio_task2(void *userData) {
static void IRAM_ATTR audio_task2(void *userData) {
  while (true) {
    taskYIELD();
    
    if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) { // wait for the notification from the SynthTask1
      c1t = micros();
      fxt = micros();
      mixer(); 
      i2s_output();
      fxT = micros() - fxt;
      
      taskYIELD();
    
      s2t = micros();
      synth2_generate();
      s2T = micros() - s2t;
      
      xTaskNotifyGive(SynthTask1); 
    }    
    
    c1T = micros() - c1t;

    art = micros();
    
    if (timer2_fired) {
      timer2_fired = false;

#ifdef TEST_POTS      
       readPots();
#endif
        
#ifdef DEBUG_TIMING
        DEBF ("synt1=%dus synt2=%dus drums=%dus mixer=%dus DMA_BUF=%dus\r\n" , s1T, s2T, drT, fxT, DMA_BUF_TIME);
        //    DEBF ("TaskCore0=%dus TaskCore1=%dus DMA_BUF=%dus\r\n" , c0T , c1T , DMA_BUF_TIME);
        //    DEBF ("AllTheRestCore1=%dus\r\n" , arT);
#endif
    }    
    
//    taskYIELD();
    arT = micros() - art;
  }
}


/* 
 *  Quite an ordinary SETUP() *******************************************************************************************************************************
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
  Serial.println("✓ OLED initialized");

  // ---------- NEOPIXEL INIT ----------
  Serial.println("Initializing NeoPixel...");
  neopixelInit();
  Serial.println("✓ NeoPixel initialized");

  // ---------- BUTTONS (Shift Register) ----------
  Serial.println("Initializing buttons...");
  initShiftRegister();
  Serial.println("✓ Buttons initialized");

  // ---------- SD CARD INIT ----------
  Serial.println("Initializing SD Card (SDMMC 1-bit mode)...");
  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);
  if (!SD_MMC.begin("/sdcard", true))
  {
    Serial.println("❌ SD Card Mount Failed!");
    Serial.println("   Check:");
    Serial.println("   - SD card is inserted");
    Serial.println("   - SD card is formatted (FAT32)");
    Serial.println("   - Wiring: CLK=18, CMD=17, D0=16 (SDMMC mode)");
    sdCardAvailable = false;
  }
  else
  {
    Serial.println("✓ SD Card Initialized via SDMMC (High Speed)");
    sdCardAvailable = true;
  }

  if (!sdCardAvailable) {
    Serial.println("⚠️  SD card not available - samples cannot be loaded from SD");
  }

  MidiInit(); // init midi input and handling of midi events

  buildTables();

  for (int i = 0; i < POT_NUM; i++) pinMode( POT_PINS[i] , INPUT);

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
  // i2s_write(i2s_num, out_buf[current_out_buf]._signed, sizeof(out_buf[current_out_buf]._signed), &bytes_written, portMAX_DELAY);

  //xTaskCreatePinnedToCore( audio_task1, "SynthTask1", 8000, NULL, (1 | portPRIVILEGE_BIT), &SynthTask1, 0 );
  //xTaskCreatePinnedToCore( audio_task2, "SynthTask2", 8000, NULL, (1 | portPRIVILEGE_BIT), &SynthTask2, 1 );
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
  Serial.println("✓ UI task created on Core 0");

  // somehow we should allow tasks to run
  xTaskNotifyGive(SynthTask1);
  //  xTaskNotifyGive(SynthTask2);
  processing = true;

#if ESP_ARDUINO_VERSION_MAJOR < 3 
  // timer interrupt
  /*
  timer1 = timerBegin(0, 80, true);               // Setup timer for midi
  timerAttachInterrupt(timer1, &onTimer1, true);  // Attach callback
  timerAlarmWrite(timer1, 4000, true);            // 4000us, autoreload
  timerAlarmEnable(timer1);
  */
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

static uint32_t last_ms = micros();

/* 
 *  Finally, the LOOP () ***********************************************************************************************************
*/

void loop() { // default loopTask running on the Core1
  // The UI task on Core 0 handles buttons, pots, and neopixel updates.
  // The audio tasks handle synth/drums/mixer/i2s.
  // Core 1's loop just runs regular checks (MIDI, jukebox tick).
  regular_checks();
  taskYIELD();
}

/* 
 *  UI CORE 0 TASK - Button, Pot, and NeoPixel polling *****************************************************************************
*/
void uiCoreTask(void* parameter) {
  uint8_t phase = 0;
  while (true) {
    switch (phase) {
      case 0: updateButtons(); break;
      case 1: processButtons(); break;
      case 2: updatePot(); break;
      //case 3: updateLEDS(); break;
    }
    phase = (phase + 1) % 4;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

/* 
 *  MYNAH Hardware Functions *****************************************************************************************************************************
*/

void neopixelInit()
{
  strip.begin();
  strip.setBrightness(20);
  strip.clear();
  strip.show();
  Serial.println("NeoPixel initialized (16 LEDs)");
}

void updateLEDS() {
  if (!ledsDirty) return;
  // Simple NeoPixel display: show which step buttons are pressed
  // Step buttons map to bits 0-15 of buttonStates
  strip.clear();

  // Map the 16 step buttons to the 16 LEDs
  for (int i = 0; i < 16; i++) {
    if ((buttonStates >> i) & 0x01) {
      // Step button pressed - show white
      strip.setPixelColor(i, 255, 255, 255);
    } else {
      // Step button not pressed - off
      strip.setPixelColor(i, 0, 0, 0);
    }
  }

  strip.setBrightness(20);
  strip.show();
  ledsDirty = false;
}

/* 
 *  Some debug and service routines *****************************************************************************************************************************
*/

void readPots() {
  static const float snap = 0.003f;
  static uint8_t i = 0;
  static float tmp;
  static const float NORMALIZE_ADC = 1.0f / 4096.0f;
//read one pot per call
  tmp = (float)analogRead(POT_PINS[i]) * NORMALIZE_ADC;
  if (fabs(tmp - param[i]) > snap) {
    param[i] = tmp;
    paramChange(i, tmp);
  }

  i++;
  // if (i >= POT_NUM) i=0;
  i %= POT_NUM;
}

void paramChange(uint8_t paramNum, float paramVal) {
  // paramVal === param[paramNum];
  DEBF ("param %d val %0.4f\r\n" , paramNum, paramVal);
  paramVal *= 127.0;
  switch (paramNum) {
    case 0:
      //set_bpm( 40.0f + (paramVal * 160.0f));
      Synth2.ParseCC(CC_303_CUTOFF, paramVal);
      break;
    case 1:
      Synth2.ParseCC(CC_303_RESO, paramVal);
      break;
    case 2:
      Synth2.ParseCC(CC_303_OVERDRIVE, paramVal);
      Synth2.ParseCC(CC_303_DISTORTION, paramVal);
      break;
    case 3:
      Synth2.ParseCC(CC_303_ENVMOD_LVL, paramVal);
      break;
    case 4:
      Synth2.ParseCC(CC_303_ACCENT_LVL, paramVal);
      break;
    default:
      {}
  }
}


#ifdef JUKEBOX
void jukebox_tick() {
  run_tick();
  myRandomAddEntropy((uint16_t)(micros() & 0x0000FFFF));
}
#endif


void regular_checks() {
  timer1_fired = false;
  
  midi_read();
  
#ifdef JUKEBOX
  jukebox_tick();
#endif


}