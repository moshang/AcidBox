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
#include <FS.h>
#include <SD_MMC.h>
#include <string.h>

// NeoPixelBus uses the ESP32-S3 hardware RMT peripheral for non-blocking
// asynchronous transmission — the CPU is NOT stalled while pixels are shifted out.
#include <NeoPixelBus.h>

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
// NeoPixelBus: 16-LED strip on GPIO 10, hardware RMT (non-blocking async)
// Neo800KbpsMethod transmits via RMT peripheral — CPU is not stalled.
NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod> strip(LED_COUNT, LED_PIN);
volatile uint32_t buttonStates = 0;
volatile uint32_t lastButtonStates = 0;
bool anyStepButtonHeld = false;
bool sdCardAvailable = false;
volatile bool ledsDirty = true;

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

  //for (int i = 0; i < POT_NUM; i++) pinMode( POT_PINS[i] , INPUT);

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
      case 3: updateLEDS(); break;
    }
    phase = (phase + 1) % 4;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---- NEOPIXEL VISUALIZER STATE & FUNCTIONS ----
// 16 LED states, one per physical LED on the ring
LedState ledStates[16];
bool visualizerCurrentSlide = false;

// Colour mapping per voice (RGB values 0-255)
static const RgbColor COLOR_SYNTH1(0, 200, 220);   // cyan
static const RgbColor COLOR_SYNTH1_SAW(0, 140, 200); // deeper cyan for saw
static const RgbColor COLOR_SYNTH2(220, 0, 200);   // magenta
static const RgbColor COLOR_SYNTH2_SAW(180, 0, 140); // deeper magenta for saw
static const RgbColor COLOR_DRUM_KICK(255, 255, 255);   // white
static const RgbColor COLOR_DRUM_SNARE(255, 220, 0);    // yellow
static const RgbColor COLOR_DRUM_CH(0, 255, 60);        // green
static const RgbColor COLOR_DRUM_OH(0, 220, 40);        // green (slightly dimmer)
static const RgbColor COLOR_DRUM_PERC(255, 120, 0);     // orange

// Arc indices: synth1 LEDs 0-5, synth2 LEDs 6-10, drums LEDs 11-15
#define ARC_SYNTH1_START 0
#define ARC_SYNTH1_END   5
#define ARC_SYNTH2_START 6
#define ARC_SYNTH2_END   10
#define ARC_DRUMS_START  11
#define ARC_DRUMS_END    15

// Map voice (0=synth1, 1=synth2) to an arc's LED range
static inline void getSynthArc(uint8_t voice, uint8_t* start, uint8_t* end) {
  if (voice == 0) { *start = ARC_SYNTH1_START; *end = ARC_SYNTH1_END; }
  else            { *start = ARC_SYNTH2_START; *end = ARC_SYNTH2_END; }
}

void initVisualizer() {
  for (int i = 0; i < 16; i++) {
    ledStates[i].brightness = 0.05f; // dim baseline
    ledStates[i].decay_rate = 0.92f;
    ledStates[i].ramp_target = -1.0f; // no ramp
    ledStates[i].ramp_step = 0.0f;
    ledStates[i].base_color = RgbColor(0, 0, 0);
    ledStates[i].current_color = RgbColor(0, 0, 0);
  }
}

void visualizerTick() {
  for (int i = 0; i < 16; i++) {
    LedState* ls = &ledStates[i];

    // Handle brightness ramping (for slide interpolation and accent boost)
    if (ls->ramp_target >= 0.0f) {
      if (ls->brightness < ls->ramp_target) {
        ls->brightness += ls->ramp_step;
        if (ls->brightness > ls->ramp_target) ls->brightness = ls->ramp_target;
      } else if (ls->brightness > ls->ramp_target) {
        ls->brightness -= ls->ramp_step;
        if (ls->brightness < ls->ramp_target) ls->brightness = ls->ramp_target;
      } else {
        ls->ramp_target = -1.0f; // ramp complete
      }
    } else {
      // Normal decay
      ls->brightness *= ls->decay_rate;
    }

    // Clamp to dim baseline — never go below 5% so ring is always visible
    if (ls->brightness < 0.05f && ls->brightness > 0.0f) {
      ls->brightness = 0.05f;
    }

    // Apply brightness to base colour
    float bb = ls->brightness;
    bb = (bb > 1.0f) ? 1.0f : bb;
    ls->current_color = RgbColor(
      (uint8_t)((float)ls->base_color.R * bb),
      (uint8_t)((float)ls->base_color.G * bb),
      (uint8_t)((float)ls->base_color.B * bb)
    );

    strip.SetPixelColor(i, ls->current_color);
  }
  ledsDirty = true;
}

// Map a MIDI note (range ~24–84) to an LED index within an arc [start..end]
static inline uint8_t noteToLedInArc(uint8_t note, uint8_t arcStart, uint8_t arcEnd) {
  uint8_t arcLen = arcEnd - arcStart + 1;
  // Clamp note to reasonable range
  if (note < 24) note = 24;
  if (note > 96) note = 96;
  // Map 24-84 range to 0..arcLen-1
  uint8_t idx = (uint8_t)(((float)(note - 24) / 60.0f) * (float)arcLen);
  if (idx >= arcLen) idx = arcLen - 1;
  return arcStart + idx;
}

void visualizerNoteOn(uint8_t voice, uint8_t note, bool accent, bool slide) {
    if (voice < 2) {
    // Synth1 or Synth2
    uint8_t arcStart, arcEnd;
    getSynthArc(voice, &arcStart, &arcEnd);
    uint8_t led = noteToLedInArc(note, arcStart, arcEnd);
    LedState* ls = &ledStates[led];

    // Determine base colour — could be driven by waveform (saw vs square) later
    // For now use the main synth colour; the AcidBanger API could pass waveform if needed.
    RgbColor baseColor = (voice == 0) ? COLOR_SYNTH1 : COLOR_SYNTH2;
    ls->base_color = baseColor;

    float targetBrightness = accent ? 1.0f : 0.7f;

    if (slide) {
      // Slide: ramp brightness from current to target over ~5 UI ticks
      visualizerCurrentSlide = true;
      ls->ramp_target = targetBrightness;
      float diff = targetBrightness - ls->brightness;
      ls->ramp_step = diff / 5.0f;
      if (ls->ramp_step < 0.0f) ls->ramp_step = -ls->ramp_step;
      ls->decay_rate = 0.97f; // slower decay during slide
    } else {
      // Immediate set
      ls->ramp_target = -1.0f;
      ls->brightness = targetBrightness;
      ls->decay_rate = 0.92f; // normal decay
    }

    // Accent boost: briefly boost all LEDs in the arc
    if (accent) {
      for (uint8_t i = arcStart; i <= arcEnd; i++) {
        if (i != led) {
          LedState* other = &ledStates[i];
          // Temporary +30% boost for one tick via ramp (decay will handle rest)
          other->ramp_target = other->brightness * 1.3f;
          if (other->ramp_target > 1.0f) other->ramp_target = 1.0f;
        }
      }
    }
  } else {
    // Drums (voice 2): 5 drum voices mapped to LEDs 11..15
    // The MIDI note = current_drumkit + drum_instrument_index.
    // Extract the instrument index: drum_instrument = note % 12
    // Drum instrument numbers: 0=kick, 1=snare, 6=CH, 7=OH, 9=crash, 11=perc
    uint8_t drumInstr = note % 12;
    uint8_t led;
    uint8_t drumType;

    // Map instrument to LED 11-15
    // Instrument 0 (KICK) → LED 11
    // Instrument 1 (SNARE) → LED 12
    // Instrument 6 (CH)    → LED 13
    // Instrument 7 (OH)    → LED 14
    // Instrument 9 (CRASH) or 11 (PERC) → LED 15
    if (drumInstr == 0) {
      led = ARC_DRUMS_START;      // 11
      drumType = 0;
    } else if (drumInstr == 1) {
      led = ARC_DRUMS_START + 1;  // 12
      drumType = 1;
    } else if (drumInstr == 6) {
      led = ARC_DRUMS_START + 2;  // 13
      drumType = 2;
    } else if (drumInstr == 7) {
      led = ARC_DRUMS_START + 3;  // 14
      drumType = 3;
    } else {
      led = ARC_DRUMS_START + 4;  // 15 — perc/crash
      drumType = 4;
    }

    LedState* ls = &ledStates[led];
    ls->brightness = 1.0f; // full flash on hit
    ls->ramp_target = -1.0f;

    // Assign colour and decay based on drum type
    switch (drumType) {
      case 0: // Kick
        ls->base_color = COLOR_DRUM_KICK;
        ls->decay_rate = 0.94f; // slowest decay
        break;
      case 1: // Snare
        ls->base_color = COLOR_DRUM_SNARE;
        ls->decay_rate = 0.92f;
        break;
      case 2: // Closed hat
        ls->base_color = COLOR_DRUM_CH;
        ls->decay_rate = 0.88f; // fastest decay
        break;
      case 3: // Open hat
        ls->base_color = COLOR_DRUM_OH;
        ls->decay_rate = 0.95f; // longer decay
        break;
      default: // Perc/crash
        ls->base_color = COLOR_DRUM_PERC;
        ls->decay_rate = 0.93f;
        break;
    }
  }
}

void visualizerNoteOff(uint8_t voice, uint8_t note) {
  if (voice < 2) {
    uint8_t arcStart, arcEnd;
    getSynthArc(voice, &arcStart, &arcEnd);
    uint8_t led = noteToLedInArc(note, arcStart, arcEnd);
    LedState* ls = &ledStates[led];
    // Do not snap off — let decay handle it (mirroring synth envelope release)
    // Just ensure ramp is disabled so normal decay takes over
    ls->ramp_target = -1.0f;
    ls->decay_rate = 0.92f; // ~100ms decay at 20ms ticks: 0.92^5 ≈ 0.66 (dim enough)
  }
}

/* 
 *  MYNAH Hardware Functions *****************************************************************************************************************************
 */

void neopixelInit()
{
  strip.Begin();
  strip.ClearTo(RgbColor(0, 0, 0));
  strip.Show();
#ifdef JUKEBOX
  initVisualizer();
  Serial.println("NeoPixel visualizer initialized (jukebox mode)");
#endif
  Serial.println("NeoPixel initialized (16 LEDs, RMT async)");
}

void updateLEDS() {
  if (!ledsDirty) return;

#ifdef JUKEBOX
  // In jukebox mode the neopixel visualizer drives the LEDs.
  // visualizerTick decays brightnesses, applies colours, and sets ledsDirty = true
  // for continuous animated updates on every UI tick.
  visualizerTick();
#else
  // Non-jukebox mode: show button states as dim white on held keys
  for (int i = 0; i < 16; i++) {
    if ((buttonStates >> i) & 0x01) {
      strip.SetPixelColor(i, RgbColor(20, 20, 20));
    } else {
      strip.SetPixelColor(i, RgbColor(0, 0, 0));
    }
  }
#endif

  // Show() uses hardware RMT — it transmits in the background
  // via the ESP32-S3 RMT peripheral without blocking the CPU.
  strip.Show();
  ledsDirty = false;
}

/* 
 *  Some debug and service routines *****************************************************************************************************************************
*/

// in AcidBoxMynah we read our single pot in POTS.cpp, so this is not used anymore
// void readPots() {
//   static const float snap = 0.003f;
//   static uint8_t i = 0;
//   static float tmp;
//   static const float NORMALIZE_ADC = 1.0f / 4096.0f;
// //read one pot per call
//   tmp = (float)analogRead(POT_PINS[i]) * NORMALIZE_ADC;
//   if (fabs(tmp - param[i]) > snap) {
//     param[i] = tmp;
//     paramChange(i, tmp);
//   }

//   i++;
//   // if (i >= POT_NUM) i=0;
//   i %= POT_NUM;
// }

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