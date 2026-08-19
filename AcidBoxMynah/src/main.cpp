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
#include "noise_fx_voice.h"
#include <Wire.h>
#include "soc/rtc_cntl_reg.h"
#include <FS.h>
#include <SD_MMC.h>
#include <string.h>
#include "SETUP.h"
#include "OLED.h"
#include "UI.h"
// NeoPixelBus uses the ESP32-S3 hardware RMT peripheral for non-blocking
// asynchronous transmission — the CPU is NOT stalled while pixels are shifted out.
// NeoPixelBusLg adds luminance (global brightness) control.
#include <NeoPixelBusLg.h>

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
volatile uint32_t s1t, s2t, drt, swt, fxt, s1T, s2T, drT, swT, fxT, art, arT, c0t, c0T, c1t, c1T; // debug timing: if we use less vars, compiler optimizes them
volatile uint32_t prescaler;
uint32_t  last_reset = 0;
float     param[POT_NUM];
uint8_t    ctrl_hold_notes;

// ---- MYNAH HARDWARE GLOBALS ----
// NeoPixelBus: 16-LED strip on GPIO 10, hardware RMT (non-blocking async)
// Neo800KbpsMethod transmits via RMT peripheral — CPU is not stalled.
NeoPixelBusLg<NeoGrbFeature, Neo800KbpsMethod> strip(LED_COUNT, LED_PIN);
volatile uint32_t buttonStates = 0;
volatile uint32_t lastButtonStates = 0;
bool anyStepButtonHeld = false;
bool stepPotAdjusted = false;
bool sdCardAvailable = false;
volatile bool ledsDirty = true;
bool refreshOLED = false;

// Voice mute state (toggled by double-click on F2/F3/F4/F7)
bool muteSynth1 = false;
bool muteSynth2 = false;
bool muteDrums = false;
bool muteSweep = false;

// Audio buffers of all kinds
volatile uint8_t current_gen_buf = 0; // set of buffers for generation
volatile uint8_t current_out_buf = 1 - 0; // set of buffers for output
float synth1_buf[2][DMA_BUF_LEN];    // synth1 mono
float synth2_buf[2][DMA_BUF_LEN];    // synth2 mono
float drums_buf_l[2][DMA_BUF_LEN];   // drums L
float drums_buf_r[2][DMA_BUF_LEN];   // drums R
float sweep_buf_l[2][DMA_BUF_LEN];   // sweep FX L
float sweep_buf_r[2][DMA_BUF_LEN];   // sweep FX R
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
NoiseFxVoice Sweep;

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
void IRAM_ATTR audio_task1(void *userData) {
  
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

      swt = micros();
      sweep_generate();
      swT = micros() - swt;

    }
    
   // taskYIELD();

    taskYIELD();

    c0T = micros() - c0t;
  }
}

// task for Core1, which tipically runs user's code on ESP32
// static void IRAM_ATTR audio_task2(void *userData) {
void IRAM_ATTR audio_task2(void *userData) {
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
        DEBF ("synt1=%dus synt2=%dus drums=%dus sweep=%dus mixer=%dus core0=%dus core1=%dus DMA_BUF=%dus\r\n" , s1T, s2T, drT, swT, fxT, c0T, c1T, DMA_BUF_TIME);
        //    DEBF ("AllTheRestCore1=%dus\r\n" , arT);
#endif
    }    
    
//    taskYIELD();
    arT = micros() - art;
  }
}


/* 
 *  Quite an ordinary SETUP() — now moved to SETUP.cpp *******************************************************************************
 */

void setup(void);

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
 *  Architecture for responsive input without compromising audio:
 *    - Input tasks (buttons, pots) run EVERY tick (~5ms = ~200Hz).
 *    - Slower display tasks (LEDs, OLED) run on a decoupled schedule
 *      so they never block input polling.
 *  Audio runs independently on Core 1 and is unaffected by these changes.
*/
void uiCoreTask(void* parameter) {
  uint8_t slowPhase = 0;
  while (true) {
    // === HIGH-FREQUENCY INPUT POLLING (every tick) ===
    updateButtons();
    processButtons();
    updatePot();
    midiPatternSyncService();

    // === LOW-FREQUENCY DISPLAY UPDATES (decoupled round-robin) ===
    switch (slowPhase) {
      case 0: updateLEDS(); break;
      case 1: updateOLED(); break;
      // case 2: (idle) — gives CPU back to audio on shared core
    }
    slowPhase = (slowPhase + 1) % 3;

    // 5ms tick — input tasks now run at ~200Hz (was 40Hz with old round-robin)
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---- SEQUENCER PATTERN DISPLAY (EDIT Mode) ----
// Default: show BD (kick) lane
uint16_t drumViewMask = 1 << 0;

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
    // Extract the instrument index from the current 16-slot kit.
    // Legacy jukebox notes retain their existing offsets within the group.
    uint8_t drumInstr = note % DRUM_SLOT_COUNT;
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

// ---- SEQUENCER PATTERN DISPLAY ----
// Two rows of 8 NeoPixels:
//   Top row:    LED 0-7  = steps 1-8  (step indices 0-7)
//   Bottom row: LED 8-15 = steps 9-16 (step indices 8-15)
// Step 1 is top-left (LED 0), step 16 is bottom-right (LED 15)
void sequencerDisplayTick() {
  for (int step = 0; step < 16; step++) {
    bool active = false;
    RgbColor stepColor(0, 0, 0); // off by default

    // Determine step activity and color based on current edit type
    switch (currentEditType) {
      case Syn1: {
        SynthStep& s = globalSeq.synth1.steps[step];
        if (s.active && s.note > 0) {
          active = true;
          if (s.slide && s.accent) {
            // Both slide and accent: bright yellow
            stepColor = RgbColor(255, 255, 0);
          } else if (s.slide) {
            // Slide only: gold/yellow
            stepColor = RgbColor(255, 200, 0);
          } else if (s.accent) {
            // Accent only: bright white
            stepColor = RgbColor(255, 255, 255);
          } else {
            // Normal active step: cyan
            stepColor = RgbColor(0, 200, 220);
          }
        }
        break;
      }
      case Syn2: {
        SynthStep& s = globalSeq.synth2.steps[step];
        if (s.active && s.note > 0) {
          active = true;
          if (s.slide && s.accent) {
            // Both slide and accent: bright yellow
            stepColor = RgbColor(255, 255, 0);
          } else if (s.slide) {
            // Slide only: gold/yellow
            stepColor = RgbColor(255, 200, 0);
          } else if (s.accent) {
            // Accent only: bright white
            stepColor = RgbColor(255, 255, 255);
          } else {
            // Normal active step: magenta
            stepColor = RgbColor(220, 0, 200);
          }
        }
        break;
      }
      case Drm: {
        uint16_t mask = globalSeq.drum.steps[step];
        if (mask & drumViewMask) {
          active = true;
          // Color based on which drum lane is being viewed
          switch (drumViewMask) {
            case 1 << 0:  stepColor = RgbColor(255, 255, 255); break; // BD  - white
            case 1 << 1:  stepColor = RgbColor(255, 220, 0);   break; // SD  - yellow
            case 1 << 2:  stepColor = RgbColor(0, 255, 60);    break; // CH  - green
            case 1 << 3:  stepColor = RgbColor(0, 220, 40);    break; // OH  - green
            case 1 << 4:  stepColor = RgbColor(255, 120, 0);   break; // CLAP- orange
            case 1 << 5:  stepColor = RgbColor(180, 100, 60);  break; // LT  - brown
            case 1 << 6:  stepColor = RgbColor(200, 130, 80);  break; // MT  - tan
            case 1 << 7:  stepColor = RgbColor(220, 160, 100); break; // HT  - light tan
            case 1 << 8:  stepColor = RgbColor(200, 200, 200); break; // CR  - grey
            case 1 << 9:  stepColor = RgbColor(180, 180, 255); break; // RIM - light blue
            case 1 << 10: stepColor = RgbColor(255, 200, 0);   break; // MAR - gold
            case 1 << 11: stepColor = RgbColor(200, 100, 0);   break; // CLAV- dark orange
            case 1 << 12: stepColor = RgbColor(255, 150, 50);  break; // COW - pumpkin
            case 1 << 13: stepColor = RgbColor(255, 255, 100); break; // CY  - pale yellow
            case 1 << 14: stepColor = RgbColor(150, 80, 40);   break; // CONG- brown
            case 1 << 15: stepColor = RgbColor(180, 60, 60);   break; // TIMB- rust
            default:      stepColor = RgbColor(200, 200, 200); break; // default grey
          }
        }
        break;
      }
      default:
        break;
    }

    // --- Check if this step's button is physically held down ---
    bool stepHeld = (buttonStates & (1UL << step)) != 0;

    // --- Playhead (blue) overrides everything except held step ---
    if (stepHeld) {
      // Held step: bright white (overrides playhead and pattern)
      strip.SetPixelColor(step, RgbColor(255, 255, 255));
    } else if (step == globalSeq.currentStep && globalSeq.isPlaying) {
      // Playhead: bright blue
      strip.SetPixelColor(step, RgbColor(0, 0, 255));
    } else if (active) {
      // Active step: dimmed to ~35% so pattern is visible but doesn't overwhelm
      strip.SetPixelColor(step, RgbColor(
        (uint8_t)((float)stepColor.R * 0.35f),
        (uint8_t)((float)stepColor.G * 0.35f),
        (uint8_t)((float)stepColor.B * 0.35f)
      ));
    } else {
      // Inactive step: off
      strip.SetPixelColor(step, RgbColor(0, 0, 0));
    }
  }
  ledsDirty = true;
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
  strip.SetLuminance(NEOPIXEL_BRIGHTNESS);
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

  // ---- AcidBox Bank/Song/Pattern select modes ----
  // Color scheme (matching MYNAH project):
  //   Current slot           : White (255,255,255)
  //   Current slot, modified : Yellow/orange (200,150,0) [pattern only]
  //   Occupied slot          : Green (0,100,0)
  //   Empty slot             : Dim purple (20,0,20) [PATTERN_SELECT]
  //                          : Dim blue   (0,0,30)  [SONG_SELECT]
  //                          : Dim red    (30,0,0)  [BANK_SELECT]
  //   Playback position      : 20% of the slot's own colour (sequencer running only)
  if (currentUiMode == UI_PATTERN_SELECT || currentUiMode == UI_SONG_SELECT || currentUiMode == UI_BANK_SELECT)
  {
    for (int i = 0; i < 16; i++)
    {
      bool isCurrent = false;
      bool slotExists = false;

      if (currentUiMode == UI_PATTERN_SELECT)
      {
        slotExists = acidBoxSaveLoad.patternExistsCache[i];
        isCurrent = (i == acidBoxSaveLoad.currentPattern);
      }
      else if (currentUiMode == UI_SONG_SELECT)
      {
        slotExists = acidBoxSaveLoad.songExistsCache[i];
        isCurrent = (i == acidBoxSaveLoad.currentSong);
      }
      else if (currentUiMode == UI_BANK_SELECT)
      {
        slotExists = acidBoxSaveLoad.bankExistsCache[i];
        isCurrent = (i == acidBoxSaveLoad.currentBank);
      }

      if (isCurrent)
      {
        if (currentUiMode == UI_PATTERN_SELECT && acidBoxSaveLoad.modified)
        {
          // Current slot with unsaved changes: yellow/orange
          strip.SetPixelColor(i, RgbColor(200, 150, 0));
        }
        else
        {
          // Current slot: white
          strip.SetPixelColor(i, RgbColor(255, 255, 255));
        }
      }
      else if (slotExists)
      {
        // Occupied slot: green
        strip.SetPixelColor(i, RgbColor(0, 100, 0));
      }
      else
      {
        // Empty slot: dim colour per mode
        if (currentUiMode == UI_SONG_SELECT)
          strip.SetPixelColor(i, RgbColor(0, 0, 30));   // dim blue
        else if (currentUiMode == UI_BANK_SELECT)
          strip.SetPixelColor(i, RgbColor(30, 0, 0));   // dim red
        else
          strip.SetPixelColor(i, RgbColor(20, 0, 20));  // dim purple (pattern)
      }
    }

    // Playback position overlay: dim the current step to 20% of its set colour
    // so the sequencer position is visible even while browsing slots.
    if (globalSeq.isPlaying)
    {
      uint8_t step = globalSeq.currentStep;
      RgbColor col = strip.GetPixelColor(step);
      uint8_t r = col.R / 5;
      uint8_t g = col.G / 5;
      uint8_t b = col.B / 5;
      strip.SetPixelColor(step, RgbColor(r, g, b));
    }

    strip.Show();
    ledsDirty = false;
    return;
  }

if (currentMode == MODE_JUKEBOX)
{
  // In jukebox mode the neopixel visualizer drives the LEDs.
  // visualizerTick decays brightnesses, applies colours, and sets ledsDirty = true
  // for continuous animated updates on every UI tick.
  visualizerTick();
}
else
{
  // Sequencer/EDIT mode: show the pattern steps on the neopixel grid
  // Two rows of 8 LEDs = direct 1:1 mapping to 16 steps
  sequencerDisplayTick();
}

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
  // Pattern generation (fills globalSeq and plays via legacy MIDI)
  run_tick();
  myRandomAddEntropy((uint16_t)(micros() & 0x0000FFFF));
}
#endif


void regular_checks() {
  timer1_fired = false;
  
  midi_read();
  midiClockService();

  // Process any deferred drum kit loads (avoids WDT timeout during Init())
  processDeferredKitLoad();
  
  
#ifdef JUKEBOX
  if (currentMode == MODE_JUKEBOX) {
    // Jukebox generation + sequencer engine
    // MIDI follower mode is driven directly from the real-time clock callback.
    if (midiClockSource() != CLOCK_SRC_MIDI) jukebox_tick();
  } else {
    // EDIT mode: jukebox generation suspended,
    // sequencer loops the patterns in globalSeq continuously
    sequencer_service();
  }
#endif


}
