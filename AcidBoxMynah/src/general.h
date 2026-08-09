#ifndef GENERAL_H
#define GENERAL_H

#include <Arduino.h>
#include "config.h"
#include "midi_config.h"
#include "sequencer.h"
#include "UI.h"
#include "SAVE.h"
#include "fx_delay.h"
#include <NeoPixelBusLg.h>

// Forward declare the classes so we can use pointers/references
class SynthVoice;
class Sampler;
class FxDelay;
class FxReverb;
class Compressor;

// Core generators and mixer functions (non-static)
void drums_generate();
void synth1_generate();
void synth2_generate();
void IRAM_ATTR mixer();

// Easing
float easeInExpo(float x);

// OLED / Display
void oledInit();

// Other functions defined in various compilation units
void i2sInit();
void i2sDeinit();
void i2s_output();
void MidiInit();
void buildTables();
void init_midi();
void run_tick();
void jukebox_reset_parameters();
void jukebox_midi_start();
void jukebox_midi_stop();
void jukebox_midi_continue();
void jukebox_midi_step();
void jukebox_generate_part(EditType part);
void jukebox_generate_all();
uint16_t myRandomAddEntropy(uint16_t entropy);
void midi_read();
void midiClockService();
void midi_send_noteon(uint8_t chan, uint8_t note, uint8_t vol);
void midi_send_noteoff(uint8_t chan, uint8_t note);
void handleNoteOn(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity);
void handleNoteOff(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity);
void handleCC(uint8_t inChannel, uint8_t cc_number, uint8_t cc_value);

// Shared MIDI clock/transport state and controls.
#define CLOCK_SRC_INT  0
#define CLOCK_SRC_MIDI 1
extern volatile uint8_t clockSource;
extern volatile uint8_t clockOut;
extern volatile bool midiClockSync;
void midiClockSetSource(uint8_t source);
void midiClockSetOutput(uint8_t enabled);
void midiClockSetOffset(uint8_t offsetMs);
void midiClockBpmChanged(float newBpm);
void midiClockTransportStart();
void midiClockTransportStop();
bool midiClockIsSynchronized();
uint8_t midiClockSource();
uint8_t midiClockOutput();
uint8_t midiClockOffset();

// ---- MYNAH HARDWARE FUNCTION DECLARATIONS ----
void neopixelInit();
void initShiftRegister();
uint32_t readShiftRegister();
void updateButtons();
bool isButtonPressed(uint8_t buttonNum);
bool isButtonJustPressed(uint8_t buttonNum);
bool isButtonJustReleased(uint8_t buttonNum);
void processButtons();
void midi_toggle_play();
void printButtonStates();
void updatePot();
void handlePot(uint16_t potVal);
void potLock();
bool isPotLocked();  // true when pot is locked (automation interpolation should be used)
void updateLEDS();
void uiCoreTask(void* parameter);

// ---- MYNAH HARDWARE EXTERN GLOBALS ----
// NeoPixelBus strip on GPIO 10, GRB (WS2812/SK6812), 800Kbps via RMT (async, non-blocking)
extern NeoPixelBusLg<NeoGrbFeature, Neo800KbpsMethod> strip;
extern volatile uint32_t buttonStates;
extern volatile uint32_t lastButtonStates;
extern bool anyStepButtonHeld;
extern bool stepPotAdjusted;  // true if pot was adjusted while a step button was held
extern bool sdCardAvailable;

// UI
extern volatile bool ledsDirty;
extern bool refreshOLED;
extern EditType currentEditType;
extern SynthEditMode currentEditMode;
extern const char* editTypeNames[4];
extern const char* synthEditModeNames[16];
extern const uint8_t midiChn[4];
extern const uint8_t synthEditCC[16];

// Voice mute state (toggled by double-click on F2/F3/F4)
extern bool muteSynth1;
extern bool muteSynth2;
extern bool muteDrums;

// Pot-used-with-F-key flags (suppress F-key release actions after pot adjustment)
extern bool f2PotUsed;
extern bool f3PotUsed;
extern bool f4PotUsed;
extern bool f8PotUsed;

// Master volume (0.0 - 1.0, set via F8+Step16)
extern float masterVolume;

// Shared instances and variables
extern Sampler Drums;
extern SynthVoice Synth1;
extern SynthVoice Synth2;
extern FxDelay Delay;
#ifndef NO_PSRAM
extern FxReverb Reverb;
#endif
extern Compressor Comp;

extern volatile uint32_t s1t, s2t, drt, fxt, s1T, s2T, drT, fxT, art, arT, c0t, c0T, c1t, c1T;
extern volatile uint32_t prescaler;
extern uint32_t last_reset;
extern float param[POT_NUM];
extern uint8_t ctrl_hold_notes;

extern volatile uint8_t current_gen_buf;
extern volatile uint8_t current_out_buf;
extern float synth1_buf[2][DMA_BUF_LEN];
extern float synth2_buf[2][DMA_BUF_LEN];
extern float drums_buf_l[2][DMA_BUF_LEN];
extern float drums_buf_r[2][DMA_BUF_LEN];
extern float mix_buf_l[2][DMA_BUF_LEN];
extern float mix_buf_r[2][DMA_BUF_LEN];

union out_buf_u {
  int16_t _signed[DMA_BUF_LEN * 2];
  uint16_t _unsigned[DMA_BUF_LEN * 2];
};
extern out_buf_u out_buf[2];
extern size_t bytes_written;

extern volatile float rvb_k1, rvb_k2, rvb_k3;
extern volatile float dly_k1, dly_k2, dly_k3;

extern float shaper_tbl[TABLE_SIZE+1];
extern float sin_tbl[TABLE_SIZE+1];
extern float knob_tbl[TABLE_SIZE+1];
extern float exp_tbl[TABLE_SIZE+1];
extern float saw_tbl[TABLE_SIZE+1];
extern float exp_square_tbl[TABLE_SIZE+1];
extern float lim_tbl[TABLE_SIZE+1];
extern float norm1_tbl[16][16];
extern float norm2_tbl[16][16];
extern float midi_pitches[128];
extern float midi_phase_steps[128];
extern float midi_tbl_steps[128];
extern const float tuning[128];

// ---- NEOPIXEL VISUALIZER (Jukebox Mode) ----
struct LedState {
  float brightness;
  float decay_rate;
  float ramp_target;   // >0 = ramping toward this brightness target (for slide + accent effects)
  float ramp_step;     // per-tick increment toward ramp_target
  RgbColor base_color;
  RgbColor current_color;
};
extern LedState ledStates[16];
extern bool visualizerCurrentSlide;
void visualizerTick();
void visualizerNoteOn(uint8_t voice, uint8_t note, bool accent, bool slide);
void visualizerNoteOff(uint8_t voice, uint8_t note);

// ---- SEQUENCER PATTERN DISPLAY (EDIT Mode) ----
extern uint16_t drumViewMask;  // which drum bit to display on the neopixel grid in sequencer mode (default: 1<<0 = BD)
extern uint16_t currentDrumLane;
extern uint8_t  currentDrumLaneIndex;
extern const char* drumLaneNames[16];
void sequencerDisplayTick();   // render the current pattern on the neopixels
void setDrumLane(uint8_t laneIndex);  // 0-15, maps to 1<<laneIndex

// Utility math and lookup functions defined as inline
inline float fclamp(float in, float min, float max) {
    return fmin(fmax(in, min), max);
}

inline float dB2amp(float dB) {
    return expf(dB * 0.11512925464970228420089957273422f);
}

inline float amp2dB(float amp) {
    return 8.6858896380650365530225783783321f * logf(amp);
}

inline float lookupTable(float (&table)[TABLE_SIZE+1], float index ) {
  static float v1, v2, res;
  static int32_t i;
  static float f;
  i = (int32_t)index;
  f = (float)index - i;
  v1 = (table)[i];
  v2 = (table)[i+1];
  res = (float)f * (float)(v2-v1) + v1;
  return res;
}

inline float bilinearLookup(float (&table)[16][16], float x, float y) {
  static float kmap = 0.1181f;
  int32_t i,j;
  float fi,fj;
  float v1,v2,v3,v4;
  float res1,res2,res3;
  x *= kmap;
  y *= kmap;
  i = (int32_t)x;
  j = (int32_t)y;
  fi = (float)x - i;
  fj = (float)y - j;
  v1 = table[i][j];
  v2 = table[i+1][j];
  v3 = table[i][j+1];
  v4 = table[i+1][j+1];  
  res1 = (float)fi * (float)(v2-v1) + v1;
  res2 = (float)fi * (float)(v4-v3) + v3;
  res3 = (float)fj * (float)(res2-res1) + res1;
  return res3;
}

inline float fast_shape(float x){
    float sign = 1.0f;
    if (x<0) {
      x = -x;
      sign = -1.0f;
    }
    if (x>=4.95f) {
      return sign;
    }
    return sign * lookupTable(shaper_tbl, (x*SHAPER_LOOKUP_COEF));
}

inline float fast_sin(const float x) {
  const float argument = ((x * ONE_DIV_TWOPI) * TABLE_SIZE);
  const float res = lookupTable(sin_tbl, CICLE_INDEX(argument)+((float)argument-(int32_t)argument));
  return res;
}

inline float fast_cos(const float x) {  
  const float argument = ((x * ONE_DIV_TWOPI + 0.25f) * TABLE_SIZE);
  const float res = lookupTable(sin_tbl, CICLE_INDEX(argument)+((float)argument-(int32_t)argument));
  return res;
}

inline void fast_sincos(const float x, float* sinRes, float* cosRes){
    *sinRes = fast_sin(x);
    *cosRes = fast_cos(x);
}

static __attribute__((always_inline)) inline float one_div(float a) {
    float result;
    asm volatile (
        "wfr f1, %1"          "\n\t"
        "recip0.s f0, f1"     "\n\t"
        "const.s f2, 1"       "\n\t"
        "msub.s f2, f1, f0"   "\n\t"
        "maddn.s f0, f0, f2"  "\n\t"
        "const.s f2, 1"       "\n\t"
        "msub.s f2, f1, f0"   "\n\t"
        "maddn.s f0, f0, f2"  "\n\t"
        "rfr %0, f0"          "\n\t"
        : "=r" (result)
        : "r" (a)
        : "f0","f1","f2"
    );
    return result;
}

inline float linToLin(float in, float inMin, float inMax, float outMin, float outMax){
  float tmp = (in-inMin) * one_div(inMax-inMin);
  tmp *= (outMax-outMin);
  tmp += outMin;
  return tmp;
}

inline float linToExp(float in, float inMin, float inMax, float outMin, float outMax){
  float tmp = (in-inMin) * one_div(inMax-inMin);
  return outMin * expf( tmp*(logf(outMax * one_div(outMin))) );
}

inline float expToLin(float in, float inMin, float inMax, float outMin, float outMax){
  float tmp = logf(in * one_div(inMin)) * one_div( logf(inMax * one_div(inMin)));
  return outMin + tmp * (outMax-outMin);
}

inline float knobMap(float in, float outMin, float outMax) {
  return outMin + lookupTable(knob_tbl, (int)(in * TABLE_SIZE)) * (outMax - outMin);
}

#endif