#include <Arduino.h>
#include "midi_handler.h"
#include "general.h"
#include "synthvoice.h"
#include "sampler.h"
#include "fx_delay.h"
#ifndef NO_PSRAM
#include "fx_reverb.h"
#endif
#include "compressor.h"
#include <MIDI.h>
#include "esp_timer.h"
#include <Preferences.h>

// The two Mynah variants share the same standard hardware MIDI UART.
// Keep the library parser, but service it in a while(available()) loop: MIDI
// real-time bytes are one-byte messages and MIDI.read() may return false for
// them even though the byte was consumed and its callback was dispatched.
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

static const uint8_t MIDI_CLOCK   = 0xF8;
static const uint8_t MIDI_START   = 0xFA;
static const uint8_t MIDI_CONTINUE = 0xFB;
static const uint8_t MIDI_STOP    = 0xFC;
static const uint8_t CLOCKS_PER_16TH = 6; // 24 PPQN / 4

volatile uint8_t clockSource = CLOCK_SRC_INT;
volatile uint8_t clockOut = 0;
volatile bool midiClockSync = false;
static volatile uint8_t midiClockOffsetMs = 0;
static Preferences midiPreferences;

static volatile uint8_t midiClockCount = 0;
static uint32_t lastClockUs = 0;
static uint64_t clockIntervalAccumUs = 0;
static uint8_t clockIntervalsMeasured = 0;
static uint32_t lastClockPeriodUs = 0;
// The MIDI clock boundary remains at the UART arrival time. This deadline
// delays only AcidBox voice triggering; clock counting, tempo measurement,
// and optional clock-thru remain phase-accurate.
static volatile bool pendingMidiStep = false;
static uint32_t pendingMidiStepDueUs = 0;

// ---------- Absolute-deadline MIDI clock output ----------
static esp_timer_handle_t midiClockTimer = nullptr;
static volatile uint32_t midiClockIntervalUs = 0;
static uint64_t midiClockNextUs = 0;

static void midiClockTimerCallback(void*) {
  if (!clockOut || clockSource != CLOCK_SRC_INT || !globalSeq.isPlaying) return;

  Serial1.write(MIDI_CLOCK);
  const uint32_t intervalUs = midiClockIntervalUs;
  if (intervalUs == 0) return;

  midiClockNextUs += intervalUs;
  const uint64_t nowUs = (uint64_t)esp_timer_get_time();
  if (midiClockNextUs <= nowUs) {
    const uint64_t missed = (nowUs - midiClockNextUs) / intervalUs + 1;
    midiClockNextUs += missed * intervalUs;
  }
  uint64_t delayUs = midiClockNextUs - nowUs;
  if (delayUs == 0) delayUs = 1;
  esp_timer_start_once(midiClockTimer, delayUs);
}

static void ensureMidiClockTimer() {
  if (midiClockTimer != nullptr) return;
  esp_timer_create_args_t args = {
    .callback = &midiClockTimerCallback,
    .arg = nullptr,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "acidbox_midi_clock"
  };
  esp_timer_create(&args, &midiClockTimer);
}

static void resetMidiClockTracking() {
  midiClockCount = 0;
  clockIntervalsMeasured = 0;
  lastClockUs = 0;
  clockIntervalAccumUs = 0;
  lastClockPeriodUs = 0;
  pendingMidiStep = false;
}

static void dispatchMidiStep() {
  if (currentMode == MODE_JUKEBOX) jukebox_midi_step();
  else sequencer_midi_step();
}

static void handleMidiClock() {
  if (clockSource != CLOCK_SRC_MIDI || !midiClockSync) return;

  // If the main loop was briefly busy and a swung step became due before
  // this next incoming clock byte, commit it first so the pending step cannot
  // be overwritten by the next boundary calculation.
  if (pendingMidiStep && (int32_t)(micros() - pendingMidiStepDueUs) >= 0) {
    pendingMidiStep = false;
    dispatchMidiStep();
  }

  const uint32_t nowUs = micros();
  if (lastClockUs != 0) {
    lastClockPeriodUs = (uint32_t)(nowUs - lastClockUs);
    clockIntervalAccumUs += lastClockPeriodUs;
    clockIntervalsMeasured++;
  }
  lastClockUs = nowUs;

  if (clockOut) Serial1.write(MIDI_CLOCK); // optional clock-thru in follower mode

  midiClockCount++;
  if ((midiClockCount % CLOCKS_PER_16TH) == 0) {
    // AcidBox's swing model delays odd 16th steps by swing/200 of a
    // straight 16th interval. Use the latest incoming clock period so the
    // delay follows the master immediately, before the BPM window updates.
    const uint8_t nextStep = (uint8_t)((globalSeq.currentStep + 1) & 0x0F);
    if (globalSeq.swing > 0.0f && (nextStep & 1u)) {
      const uint32_t straightStepUs = (lastClockPeriodUs > 0)
          ? lastClockPeriodUs * CLOCKS_PER_16TH
          : (uint32_t)(15000000.0f / max(1.0f, globalSeq.bpm));
      const uint32_t delayUs = (uint32_t)(straightStepUs * globalSeq.swing / 200.0f);
      const uint32_t offsetUs = (uint32_t)midiClockOffsetMs * 1000UL;
      pendingMidiStepDueUs = nowUs + offsetUs + delayUs;
      pendingMidiStep = true;
    } else {
      const uint32_t offsetUs = (uint32_t)midiClockOffsetMs * 1000UL;
      if (offsetUs == 0) dispatchMidiStep();
      else {
        pendingMidiStepDueUs = nowUs + offsetUs;
        pendingMidiStep = true;
      }
    }
  }

  // Update tempo once per quarter note. Summing a complete 24-clock window
  // rejects polling jitter and keeps the AcidBox delay/automation tempo aligned.
  if (midiClockCount >= 24) {
    if (clockIntervalAccumUs > 0) {
      const float detectedBpm = (clockIntervalsMeasured > 0)
          ? 60000000.0f * (float)clockIntervalsMeasured / (float)clockIntervalAccumUs
          : 0.0f;
      if (detectedBpm >= 20.0f && detectedBpm <= 300.0f) {
        globalSeq.bpm = detectedBpm;
        bpm = detectedBpm;
        Delay.SetBPM(detectedBpm);
      }
    }
    midiClockCount = 0;
    clockIntervalAccumUs = 0;
    clockIntervalsMeasured = 0;
  }
}

static void handleMidiStart() {
  if (clockSource != CLOCK_SRC_MIDI) return;

  midiClockSync = true;
  resetMidiClockTracking();
  globalSeq.isPlaying = true;
  globalSeq.currentStep = 15;

  if (currentMode == MODE_JUKEBOX) {
    jukebox_midi_start();
  }

  // Keep MIDI Start immediate for transport semantics; delay only the first
  // generated voice step by the configured slave audio offset.
  const uint32_t offsetUs = (uint32_t)midiClockOffsetMs * 1000UL;
  if (offsetUs == 0) dispatchMidiStep();
  else {
    pendingMidiStepDueUs = micros() + offsetUs;
    pendingMidiStep = true;
  }
}

static void handleMidiStop() {
  if (clockSource != CLOCK_SRC_MIDI) return;
  midiClockSync = false;
  if (currentMode == MODE_JUKEBOX) jukebox_midi_stop();
  else sequencer_stop();
}

static void handleMidiContinue() {
  if (clockSource != CLOCK_SRC_MIDI) return;
  midiClockSync = true;
  globalSeq.isPlaying = true;
  if (currentMode == MODE_JUKEBOX) jukebox_midi_continue();
}

const uint8_t synthEditCC[16] = {CC_303_CUTOFF, CC_303_RESO, CC_303_WAVEFORM, CC_303_ENVMOD_LVL, CC_303_PAN, CC_303_DELAY_SEND, CC_303_REVERB_SEND, CC_303_VOLUME, CC_303_PORTATIME, CC_303_ACCENT_LVL, CC_303_ATTACK, CC_303_DECAY, CC_303_DISTORTION, CC_303_OVERDRIVE, CC_303_SATURATOR, CC_303_TUNING}; // MIDI channels for synth1, synth2, drums

// Forward declaration of do_midi_stop defined in AcidBanger.cpp
void do_midi_stop();

void handleNoteOn(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity);
void handleNoteOff(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity);
void handleCC(uint8_t inChannel, uint8_t cc_number, uint8_t cc_value);
void handlePitchBend(uint8_t inChannel, int number);
void handleProgramChange(uint8_t inChannel, uint8_t number);

void MidiInit() {
  pinMode(MIDIRX_PIN, INPUT_PULLDOWN);
  pinMode(MIDITX_PIN, OUTPUT);
  Serial1.begin(31250, SERIAL_8N1, MIDIRX_PIN, MIDITX_PIN);

  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.setHandleNoteOff(handleNoteOff);
  MIDI.setHandleControlChange(handleCC);
  MIDI.setHandlePitchBend(handlePitchBend);
  MIDI.setHandleProgramChange(handleProgramChange);
  MIDI.setHandleClock(handleMidiClock);
  MIDI.setHandleStart(handleMidiStart);
  MIDI.setHandleStop(handleMidiStop);
  MIDI.setHandleContinue(handleMidiContinue);
  MIDI.setHandleActiveSensing([]() {});
  MIDI.turnThruOff();
  MIDI.begin(MIDI_CHANNEL_OMNI);
  // OFFSET is a device setting, not pattern data. Keep it across reboots.
  midiPreferences.begin("acidbox", false);
  midiClockOffsetMs = midiPreferences.getUChar("offset_ms", 0);
  if (midiClockOffsetMs > 20) midiClockOffsetMs = 0;
  resetMidiClockTracking();
}

void midi_read() {
  while (Serial1.available() > 0) MIDI.read();
}

void midiClockService() {
  if (!pendingMidiStep || !midiClockSync) return;
  if ((int32_t)(micros() - pendingMidiStepDueUs) < 0) return;
  pendingMidiStep = false;
  dispatchMidiStep();
}

void midi_send_noteon(uint8_t chan, uint8_t note, uint8_t vol) {
  MIDI.sendNoteOn(note, vol, chan);
}

void midi_send_noteoff(uint8_t chan, uint8_t note) {
  MIDI.sendNoteOff(note, 0, chan);
}

void midiClockSetSource(uint8_t source) {
  source = (source == CLOCK_SRC_MIDI) ? CLOCK_SRC_MIDI : CLOCK_SRC_INT;
  if (source == clockSource) return;

  const bool wasInternalMaster = (clockSource == CLOCK_SRC_INT && clockOut);
  if (midiClockTimer != nullptr) esp_timer_stop(midiClockTimer);
  pendingMidiStep = false;
  if (source == CLOCK_SRC_MIDI) {
    midiClockSync = false;
    resetMidiClockTracking();
    if (currentMode == MODE_JUKEBOX) jukebox_midi_stop();
    else sequencer_stop();
    // sequencer_stop() sends this for EDIT mode; JUKEBOX has its own stop
    // path, so ensure the external follower is stopped in both cases.
    if (wasInternalMaster && currentMode == MODE_JUKEBOX) Serial1.write(MIDI_STOP);
  }
  clockSource = source;
  if (source == CLOCK_SRC_INT && clockOut && globalSeq.isPlaying) {
    midiClockTransportStart();
  }
}

void midiClockSetOutput(uint8_t enabled) {
  if (!enabled && clockOut && clockSource == CLOCK_SRC_INT) {
    Serial1.write(MIDI_STOP);
  }
  clockOut = enabled ? 1 : 0;
  if (!clockOut) {
    if (midiClockTimer != nullptr) esp_timer_stop(midiClockTimer);
    return;
  }
  if (clockSource == CLOCK_SRC_INT && globalSeq.isPlaying) {
    midiClockTransportStart();
  }
}

void midiClockSetOffset(uint8_t offsetMs) {
  if (offsetMs > 20) offsetMs = 20;
  if (midiClockOffsetMs == offsetMs) return;
  midiClockOffsetMs = offsetMs;
  midiPreferences.putUChar("offset_ms", midiClockOffsetMs);
}

void midiClockBpmChanged(float newBpm) {
  if (newBpm < 1.0f) newBpm = 1.0f;
  midiClockIntervalUs = (uint32_t)(60000000.0f / (newBpm * 24.0f));
}

void midiClockTransportStart() {
  if (clockSource != CLOCK_SRC_INT || !clockOut) return;
  ensureMidiClockTimer();
  if (midiClockTimer == nullptr) return;
  midiClockBpmChanged(globalSeq.bpm);
  esp_timer_stop(midiClockTimer);
  midiClockNextUs = (uint64_t)esp_timer_get_time() + midiClockIntervalUs;
  Serial1.write(MIDI_START);
  esp_timer_start_once(midiClockTimer, midiClockIntervalUs);
}

void midiClockTransportStop() {
  if (midiClockTimer != nullptr) esp_timer_stop(midiClockTimer);
  if (clockSource == CLOCK_SRC_INT && clockOut) Serial1.write(MIDI_STOP);
}

bool midiClockIsSynchronized() { return midiClockSync; }
uint8_t midiClockSource() { return clockSource; }
uint8_t midiClockOutput() { return clockOut; }
uint8_t midiClockOffset() { return midiClockOffsetMs; }

void handleNoteOn(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity) {
#ifdef DEBUG_MIDI
  DEB("MIDI note on ");
  DEBUG(inNote);
#endif
  if (inChannel == DRUM_MIDI_CHAN )         {Drums.NoteOn(inNote, inVelocity);}
  else if (inChannel == SYNTH1_MIDI_CHAN )  {Synth1.on_midi_noteON(inNote, inVelocity);}
  else if (inChannel == SYNTH2_MIDI_CHAN )  {Synth2.on_midi_noteON(inNote, inVelocity);}

#ifdef JUKEBOX
  // Hook neopixel visualizer on note events
  {
    // Determine voice index: 0=synth1, 1=synth2, 2=drums
    uint8_t voice = 2; // drums by default
    if (inChannel == SYNTH1_MIDI_CHAN) voice = 0;
    else if (inChannel == SYNTH2_MIDI_CHAN) voice = 1;

    // Derive accent from velocity >= 80
    bool accent = (inVelocity >= 80);
    // Read slide flag set by AcidBanger's instr_noteon_raw, then reset it
    bool slide = visualizerCurrentSlide;
    visualizerCurrentSlide = false;
    visualizerNoteOn(voice, inNote, accent, slide);
  }
#endif
}

void handleNoteOff(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity) {
  if (inChannel == DRUM_MIDI_CHAN )         {Drums.NoteOff(inNote);}
  else if (inChannel == SYNTH1_MIDI_CHAN )  {Synth1.on_midi_noteOFF(inNote, inVelocity);}
  else if (inChannel == SYNTH2_MIDI_CHAN )  {Synth2.on_midi_noteOFF(inNote, inVelocity);}

#ifdef JUKEBOX
  {
    uint8_t voice = 2;
    if (inChannel == SYNTH1_MIDI_CHAN) voice = 0;
    else if (inChannel == SYNTH2_MIDI_CHAN) voice = 1;
    visualizerNoteOff(voice, inNote);
  }
#endif
}

void handleCC(uint8_t inChannel, uint8_t cc_number, uint8_t cc_value) {
  switch (cc_number) { // global parameters yet set via ANY channel CCs
    case CC_ANY_COMPRESSOR:
      Comp.SetRatio(3.0f + cc_value * 0.307081f);
      DEBF("Set Comp Ratio %d\r\n", cc_value);
      break;
    case CC_ANY_DELAY_TIME:
      Delay.SetLength(cc_value * MIDI_NORM);
      break;
    case CC_ANY_DELAY_FB:
      Delay.SetFeedback(cc_value * MIDI_NORM);
      break;
    case CC_ANY_DELAY_LVL:
      Delay.SetLevel(cc_value * MIDI_NORM);
      break;
    case CC_ANY_RESET_CCS:
    case CC_ANY_NOTES_OFF:
    case CC_ANY_SOUND_OFF:
        if (inChannel == SYNTH1_MIDI_CHAN && millis()-last_reset>1000 ) {
#ifdef JUKEBOX
          do_midi_stop();
#endif
          Synth1.allNotesOff();
          Synth2.allNotesOff();
          last_reset = millis();
        }
      break;
#ifndef NO_PSRAM
    case CC_ANY_REVERB_TIME:
      Reverb.SetTime(cc_value * MIDI_NORM);
      break;
    case CC_ANY_REVERB_LVL:
      Reverb.SetLevel(cc_value * MIDI_NORM);
      break;
#endif
    default:
      if (inChannel == DRUM_MIDI_CHAN )         {Drums.ParseCC(cc_number, cc_value);}
      else if (inChannel == SYNTH1_MIDI_CHAN )  {Synth1.ParseCC(cc_number, cc_value);}
      else if (inChannel == SYNTH2_MIDI_CHAN )  {Synth2.ParseCC(cc_number, cc_value);}
  }
}

void handleProgramChange(uint8_t inChannel, uint8_t number) {
  if (inChannel == DRUM_MIDI_CHAN) {     Drums.SetProgram(number);  }
}

void handlePitchBend(uint8_t inChannel, int number) {
  if (inChannel == DRUM_MIDI_CHAN )         {Drums.PitchBend(number);}
  else if (inChannel == SYNTH1_MIDI_CHAN )  {Synth1.PitchBend(number);}
  else if (inChannel == SYNTH2_MIDI_CHAN )  {Synth2.PitchBend(number);}
}
