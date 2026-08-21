#include <Arduino.h>
#include "sequencer.h"
#include "general.h"
#include "config.h"
#include "midi_config.h"
#include "synthvoice.h"
#include "sampler.h"
#include "noise_fx_voice.h"
#include "SAVE.h"

// ============================================================
// Global instances
// ============================================================
SequencerState globalSeq;
PlaybackMode    currentMode = MODE_EDIT;
uint16_t        currentScale = SCALE_CHROMATIC;  // default: all notes allowed

// ============================================================
// External references (defined in AcidBanger.cpp)
// ============================================================
extern uint8_t current_drumkit;

// ============================================================
// Internal timing variables
// ============================================================
static uint32_t lastTickUs = 0;        // micros() when the last 16th-note tick fired
static uint32_t nextTickIntervalUs = 0; // microseconds to wait until the next tick

// Track last note played per synth channel (matching jukebox's playing_note)
static uint8_t lastNote1 = 0;
static uint8_t lastNote2 = 0;

// ============================================================
// Cutoff interpolation state (EDIT mode only)
// ============================================================
// "from" = previous step's cutoff value, "to" = current step's cutoff value
// During the step, cutoff smoothly ramps from "from" to "to".
static uint8_t cutoffFrom_1 = 0;
static uint8_t cutoffTo_1 = 0;
static bool   cutoffInterp_1 = false;
static uint8_t cutoffFrom_2 = 0;
static uint8_t cutoffTo_2 = 0;
static bool   cutoffInterp_2 = false;
static uint8_t cutoffFrom_d = 0;
static uint8_t cutoffTo_d = 0;
static bool   cutoffInterp_d = false;
static bool   legacyClavMapping = false;

// ============================================================
// Drum voice MIDI note mappings (matching AcidBanger.cpp)
// ============================================================
#define KICK_NOTE  0  // BD
#define SNARE_NOTE 1  // SD
#define CH_NOTE    2  // CH -> slot 003
#define OH_NOTE    3  // OH -> slot 004
#define CLAP_NOTE  4  // CLAP (handclap)
#define LT_NOTE    5  // LT -> slot 006
#define MT_NOTE    6  // MT -> slot 007
#define HT_NOTE    7  // HT -> slot 008
#define CRASH_NOTE 8  // CR -> slot 009
#define RIM_NOTE   9  // RIM -> slot 010
#define PERC_NOTE  10 // MAR -> slot 011

// ============================================================
// Helper: calculate microseconds per 16th note at given BPM
// ============================================================
static inline uint32_t calc_16th_interval_us(float bpm) {
  // 1 beat (quarter note) = 60,000,000 / bpm microseconds
  // 1 sixteenth note = quarter / 4
  if (bpm < 1.0f) bpm = 1.0f;
  return (uint32_t)(15000000.0f / bpm);
}

// ============================================================
// Helper: send note-off for all sequencer voices
// ============================================================
static void sequencer_all_notes_off() {
  // Synth voices — use allNotesOff() to force envelopes to IDLE and clear MVA
  Synth1.allNotesOff();
  Synth2.allNotesOff();

  // Drum voices — stop all active sample players immediately
  Drums.allNotesOff();
  // The sweep voice is live-only, but stopping transport/mode must still
  // silence any synced swell that is currently active.
  Sweep.Stop();

  // Reset last note tracking
  lastNote1 = 0;
  lastNote2 = 0;
}

// ============================================================
// Initialisation
// ============================================================
void sequencer_init() {
  // Clear all patterns
  memset(&globalSeq, 0, sizeof(SequencerState));

  // Set defaults
  globalSeq.bpm       = 130.0f;
  globalSeq.swing     = 0.0f;
  globalSeq.currentStep = 0;
  globalSeq.isPlaying = false;

  // Default 4-on-the-floor kick drum pattern on power-on
  globalSeq.drum.steps[0]  |= (1 << 0);  // BD on step 1
  globalSeq.drum.steps[4]  |= (1 << 0);  // BD on step 5
  globalSeq.drum.steps[8]  |= (1 << 0);  // BD on step 9
  globalSeq.drum.steps[12] |= (1 << 0);  // BD on step 13

  currentMode = MODE_EDIT;
  lastTickUs = 0;
  nextTickIntervalUs = 0;
  lastNote1 = 0;
  lastNote2 = 0;
}

// ============================================================
// Mode switching
// ============================================================
void setMode(PlaybackMode newMode) {
  if (newMode == currentMode) return;

  // Lock the pot when switching playback modes — the parameter values
  // may differ between JUKEBOX and EDIT modes.
  potLock();

  if (newMode == MODE_EDIT) {
    // Transition to EDIT mode: suspend jukebox generation
    // Kill any playing notes
    sequencer_all_notes_off();
    // Reset sequencer step to 0 for clean loop start
    globalSeq.currentStep = 0;
    lastTickUs = micros();
    nextTickIntervalUs = calc_16th_interval_us(globalSeq.bpm);
  } else {
    // Transition to JUKEBOX mode: re-enable jukebox generation
    // Reset all parameters to jukebox defaults so the jukebox algorithm
    // regains full control of all synth/drum parameters that may have
    // been changed during EDIT mode.
    jukebox_reset_parameters();
    // The jukebox will repopulate globalSeq on the next run_tick()
    globalSeq.currentStep = 0;
    lastTickUs = micros();
    nextTickIntervalUs = calc_16th_interval_us(globalSeq.bpm);
  }

  currentMode = newMode;
}

// ============================================================
// Start / Stop / Toggle
// ============================================================
void sequencer_start() {
  if (globalSeq.isPlaying) return;
  globalSeq.isPlaying = true;
  // Set currentStep to 15 so the first advance-before-play in sequencer_tick()
  // lands on step 0 — this keeps the playhead in sync with the audio.
  globalSeq.currentStep = 15;
  lastTickUs = micros();
  nextTickIntervalUs = calc_16th_interval_us(globalSeq.bpm);
  midiClockTransportStart();
}

void sequencer_stop() {
  if (!globalSeq.isPlaying) return;
  globalSeq.isPlaying = false;
  sequencer_all_notes_off();
  midiClockTransportStop();
}

void sequencer_toggle_play() {
  if (currentMode == MODE_JUKEBOX) {
    // In JUKEBOX mode: toggle the legacy jukebox generation engine.
    // This flips midi_playing, which drives run_tick() → sequencer_step()
    // that bridges patterns into globalSeq and plays via legacy MIDI.
    const bool wasPlaying = globalSeq.isPlaying;
    midi_toggle_play();
    // do_midi_start()/do_midi_stop() update globalSeq.isPlaying directly.
    if (wasPlaying) midiClockTransportStop();
    else midiClockTransportStart();
  } else {
    // In EDIT mode: toggle the new microsecond-accurate sequencer engine.
    // The jukebox generation is suspended — only sequencer_service() runs.
    if (globalSeq.isPlaying) {
      sequencer_stop();
    } else {
      sequencer_start();
    }
  }
}

// ============================================================
// Automation recall — called at each step to apply parameter automation
// Non-cutoff parameters are applied immediately.
// Cutoff (lane 0 for synths and drums) sets up interpolation state
// so the value glides smoothly across the step duration.
// ============================================================
void sequencer_apply_automation() {
  // Always apply automation at step boundaries regardless of pot state.
  // The pot's direct handleCC may temporarily override a parameter, but
  // the next step boundary will restore the automation value.
  // Cutoff interpolation (continuous ramping) is gated separately in
  // sequencer_interpolate_cutoff() to avoid fighting with the pot.

  uint8_t step = globalSeq.currentStep;
  uint8_t ccVal;

  // --- Synth 1 automation ---
  for (uint8_t lane = 0; lane < 16; lane++) {
    if (globalSeq.autoSynth1.laneEnabled & (1 << lane)) {
      ccVal = globalSeq.autoSynth1.lanes[lane][step];
      if (lane == 0) {
        // Cutoff lane: set up interpolation from previous value to this step's value
        cutoffFrom_1 = cutoffTo_1;  // previous step's target becomes our starting point
        cutoffTo_1 = ccVal;
        cutoffInterp_1 = true;
        // When the pot is unlocked, interpolation won't run, so send cutoff directly.
        if (!isPotLocked()) {
          handleCC(SYNTH1_MIDI_CHAN, synthEditCC[0], ccVal);
        }
      } else {
        handleCC(SYNTH1_MIDI_CHAN, synthEditCC[lane], ccVal);
      }
    }
  }

  // --- Synth 2 automation ---
  for (uint8_t lane = 0; lane < 16; lane++) {
    if (globalSeq.autoSynth2.laneEnabled & (1 << lane)) {
      ccVal = globalSeq.autoSynth2.lanes[lane][step];
      if (lane == 0) {
        cutoffFrom_2 = cutoffTo_2;
        cutoffTo_2 = ccVal;
        cutoffInterp_2 = true;
        if (!isPotLocked()) {
          handleCC(SYNTH2_MIDI_CHAN, synthEditCC[0], ccVal);
        }
      } else {
        handleCC(SYNTH2_MIDI_CHAN, synthEditCC[lane], ccVal);
      }
    }
  }

  // --- Drum automation ---
  for (uint8_t lane = 0; lane < 8; lane++) {
    if (globalSeq.autoDrum.laneEnabled & (1 << lane)) {
      ccVal = globalSeq.autoDrum.lanes[lane][step];
      if (lane == 0) {
        // Drum cutoff lane
        cutoffFrom_d = cutoffTo_d;
        cutoffTo_d = ccVal;
        cutoffInterp_d = true;
        if (!isPotLocked()) {
          handleCC(DRUM_MIDI_CHAN, drumEditCC[0], ccVal);
        }
      } else {
        handleCC(DRUM_MIDI_CHAN, drumEditCC[lane], ccVal);
      }
    }
  }
}

// ============================================================
// Apply loaded pattern parameters immediately
// ============================================================
void sequencer_apply_loaded_pattern_parameters() {
  // A stopped sequencer starts on step 0 (sequencer_start sets currentStep to
  // 15 before the first tick). While already playing, keep the current step so
  // a pattern load does not jump the live parameter state to an unrelated bar.
  const uint8_t step = globalSeq.isPlaying ? globalSeq.currentStep : 0;

  // Reset cutoff interpolation targets so the next automation boundary starts
  // from the newly loaded pattern instead of ramping from the old pattern.
  cutoffInterp_1 = false;
  cutoffInterp_2 = false;
  cutoffInterp_d = false;

  for (uint8_t lane = 0; lane < 16; lane++) {
    if (globalSeq.autoSynth1.laneEnabled & (1 << lane)) {
      const uint8_t value = globalSeq.autoSynth1.lanes[lane][step];
      handleCC(SYNTH1_MIDI_CHAN, synthEditCC[lane], value);
      if (lane == 0) {
        cutoffFrom_1 = value;
        cutoffTo_1 = value;
      }
    }
    if (globalSeq.autoSynth2.laneEnabled & (1 << lane)) {
      const uint8_t value = globalSeq.autoSynth2.lanes[lane][step];
      handleCC(SYNTH2_MIDI_CHAN, synthEditCC[lane], value);
      if (lane == 0) {
        cutoffFrom_2 = value;
        cutoffTo_2 = value;
      }
    }
  }

  for (uint8_t lane = 0; lane < 8; lane++) {
    if (globalSeq.autoDrum.laneEnabled & (1 << lane)) {
      const uint8_t value = globalSeq.autoDrum.lanes[lane][step];
      handleCC(DRUM_MIDI_CHAN, drumEditCC[lane], value);
      if (lane == 0) {
        cutoffFrom_d = value;
        cutoffTo_d = value;
      }
    }
  }
}

// ============================================================
// The main sequencer tick — called at each 16th-note boundary
// Sends MIDI note-on/off for the current step's events.
// ============================================================
uint32_t sequencer_tick() {
  // --- Advance step FIRST (playhead correction) ---
  // Advance BEFORE playing so that globalSeq.currentStep always points to the
  // step that IS playing, not the next step. This keeps the playhead display
  // synchronised with the audio.
  uint8_t nextStep = globalSeq.currentStep + 1;
  if (nextStep >= 16) {
    nextStep = 0;
  }
  globalSeq.currentStep = nextStep;

  // Notify live-only A-bank sweep presets at the exact same step boundary as
  // the sequenced voices.  No FX pattern data is read or written here.
  Sweep.OnSequencerStep(globalSeq.currentStep);

  // --- Apply automation for THIS step (before playing notes) ---
  sequencer_apply_automation();

  // --- Synth 1 ---
  // Match jukebox instr_noteon_raw behaviour:
  //   - Always note-off before note-on (or note-on then note-off for glide)
  //   - Track lastNote so MVA stack stays clean (mva1.n == 1) for non-slide notes
  {
    SynthStep& s1 = globalSeq.synth1.steps[globalSeq.currentStep];
    if (s1.active && s1.note > 0) {
      uint8_t vel = s1.accent ? 120 : 79;
      if (lastNote1 != 0) {
        if (s1.slide) {
          // Fingered glide: note-on first, then note-off
          midi_send_noteon(SYNTH1_MIDI_CHAN, s1.note, vel);
          handleNoteOn(SYNTH1_MIDI_CHAN, s1.note, vel);
          midi_send_noteoff(SYNTH1_MIDI_CHAN, lastNote1);
          handleNoteOff(SYNTH1_MIDI_CHAN, lastNote1, 0);
        } else {
          // Normal: note-off first, then note-on
          midi_send_noteoff(SYNTH1_MIDI_CHAN, lastNote1);
          handleNoteOff(SYNTH1_MIDI_CHAN, lastNote1, 0);
          midi_send_noteon(SYNTH1_MIDI_CHAN, s1.note, vel);
          handleNoteOn(SYNTH1_MIDI_CHAN, s1.note, vel);
        }
      } else {
        // No previous note playing
        midi_send_noteon(SYNTH1_MIDI_CHAN, s1.note, vel);
        handleNoteOn(SYNTH1_MIDI_CHAN, s1.note, vel);
      }
      lastNote1 = s1.note;
    } else {
      // Inactive step — send note-off if we had a note playing
      if (lastNote1 != 0) {
        midi_send_noteoff(SYNTH1_MIDI_CHAN, lastNote1);
        handleNoteOff(SYNTH1_MIDI_CHAN, lastNote1, 0);
        lastNote1 = 0;
      }
    }
  }

  // --- Synth 2 ---
  {
    SynthStep& s2 = globalSeq.synth2.steps[globalSeq.currentStep];
    if (s2.active && s2.note > 0) {
      uint8_t vel = s2.accent ? 120 : 79;
      if (lastNote2 != 0) {
        if (s2.slide) {
          // Fingered glide: note-on first, then note-off
          midi_send_noteon(SYNTH2_MIDI_CHAN, s2.note, vel);
          handleNoteOn(SYNTH2_MIDI_CHAN, s2.note, vel);
          midi_send_noteoff(SYNTH2_MIDI_CHAN, lastNote2);
          handleNoteOff(SYNTH2_MIDI_CHAN, lastNote2, 0);
        } else {
          // Normal: note-off first, then note-on
          midi_send_noteoff(SYNTH2_MIDI_CHAN, lastNote2);
          handleNoteOff(SYNTH2_MIDI_CHAN, lastNote2, 0);
          midi_send_noteon(SYNTH2_MIDI_CHAN, s2.note, vel);
          handleNoteOn(SYNTH2_MIDI_CHAN, s2.note, vel);
        }
      } else {
        // No previous note playing
        midi_send_noteon(SYNTH2_MIDI_CHAN, s2.note, vel);
        handleNoteOn(SYNTH2_MIDI_CHAN, s2.note, vel);
      }
      lastNote2 = s2.note;
    } else {
      // Inactive step — send note-off if we had a note playing
      if (lastNote2 != 0) {
        midi_send_noteoff(SYNTH2_MIDI_CHAN, lastNote2);
        handleNoteOff(SYNTH2_MIDI_CHAN, lastNote2, 0);
        lastNote2 = 0;
      }
    }
  }

  // --- Drums ---
  uint16_t drumMask = globalSeq.drum.steps[globalSeq.currentStep];
  if (drumMask != 0) {
    // Map each bit to its corresponding drum MIDI note
    struct DrumBitMap {
      uint16_t bit;
      uint8_t  note;
    };
    static const DrumBitMap drumMap[] = {
      { 1 << 0,  KICK_NOTE  },  // BD
      { 1 << 1,  SNARE_NOTE },  // SD
      { 1 << 2,  CH_NOTE    },  // CH
      { 1 << 3,  OH_NOTE    },  // OH
      { 1 << 4,  CLAP_NOTE  },  // CLAP
      { 1 << 5,  LT_NOTE    },  // LT
      { 1 << 6,  MT_NOTE    },  // MT
      { 1 << 7,  HT_NOTE    },  // HT
      { 1 << 8,  CRASH_NOTE },  // CR
      { 1 << 9,  RIM_NOTE   },  // RIM
      { 1 << 10, PERC_NOTE  },  // MAR (maraca/shaker) -> slot 011
      // Legacy patterns used bit 11 as a second CLAP trigger. In the current
      // 16-slot format it is the CLAV lane and must select slot 012 (index 11),
      // not slot 011 (index 10), which is the MAR sample above.
      { 1 << 11, (uint8_t)(legacyClavMapping ? CLAP_NOTE : 11) }, // CLAV
      { 1 << 12, 12          },  // COW - extended slot
      { 1 << 13, 13          },  // CY - extended slot
      { 1 << 14, 14          },  // CONG - extended slot
      { 1 << 15, 15          },  // TIMB - extended slot
    };
    static const size_t numDrumEntries = sizeof(drumMap) / sizeof(drumMap[0]);

    for (size_t i = 0; i < numDrumEntries; i++) {
      if (drumMask & drumMap[i].bit) {
        uint8_t drumNote = drumMap[i].note;
        if (!Drums.IsDirectSlotKit()) {
          // Legacy kits retain the original 12-slot voice positions.
          switch (drumMap[i].bit) {
            case (1 << 2):  drumNote = 6;  break; // CH
            case (1 << 3):  drumNote = 7;  break; // OH
            case (1 << 5):  drumNote = 2;  break; // LT
            case (1 << 6):  drumNote = 3;  break; // MT
            case (1 << 7):  drumNote = 5;  break; // HT
            case (1 << 8):  drumNote = 9;  break; // CR
            case (1 << 9):  drumNote = 8;  break; // RIM
            case (1 << 10): drumNote = 11; break; // PERC
            case (1 << 11): drumNote = 4;  break; // legacy CLAV -> CLAP
            default: break;
          }
        }
        uint8_t midiNote = current_drumkit + drumNote;
        uint8_t vel = 100;
        midi_send_noteon(DRUM_MIDI_CHAN, midiNote, vel);
        handleNoteOn(DRUM_MIDI_CHAN, midiNote, vel);
      }
    }
  }

  // --- Calculate interval until the next tick (with swing) ---
  uint32_t baseInterval = calc_16th_interval_us(globalSeq.bpm);

  // Swing: shift even steps (0-indexed: 1, 3, 5, 7, 9, 11, 13, 15) based on ratio
  // Swing range: 0.0 (straight) → 100.0 (maximum swing)
  // At 0%: no shift.
  // At 100%: offbeat 16ths are delayed by ~50% of the base interval.
  // Interval pattern: [T+d, T-d, T+d, T-d, ...]
  //   where d = baseInterval * swing / 200
  //   Even→odd step (this step = even index): interval = T + d
  //   Odd→even step (this step = odd index):  interval = T - d
  float swingRatio = globalSeq.swing / 200.0f;
  if (globalSeq.currentStep & 1) {
    // This is an odd-indexed step. Since we advance before playing,
    // currentStep is the step we just advanced to. If currentStep is
    // odd (1, 3, 5...), then the previous step (prevStep) was even (0, 2, 4...).
    // The even→odd interval is T + d — this is the interval for the step
    // we just advanced to (currentStep).
    // After advance, currentStep is the step being played. The returned
    // interval is the duration this step will play before advancing to the next.
    // Step = odd: we arrived here from an even step (even→odd = T+d).
    // Now we need the odd→even interval = T - d for this odd step's duration.
    nextTickIntervalUs = baseInterval - (uint32_t)((float)baseInterval * swingRatio);
  } else {
    // This is an even-indexed step. currentStep is even (0, 2, 4...).
    // The previous step was odd (15, 1, 3...) — the odd→even interval is T - d.
    // Now we need the even→odd interval = T + d for this even step's duration.
    nextTickIntervalUs = baseInterval + (uint32_t)((float)baseInterval * swingRatio);
  }

  return nextTickIntervalUs;
}

// ============================================================
// Cutoff interpolation — called from sequencer_service() on every poll
// Smoothly ramps cutoff from the previous step's value to the current step's value.
// ============================================================
static void sequencer_interpolate_cutoff() {
  // Only interpolate in EDIT mode
  if (currentMode != MODE_EDIT) return;

  // When the pot is unlocked, the user is actively turning it —
  // skip interpolation and let the pot's direct handleCC take precedence.
  if (!isPotLocked()) return;

  uint32_t elapsed = micros() - lastTickUs;
  if (nextTickIntervalUs == 0) return;

  // Calculate progress through the current step (0.0 to 1.0)
  float t = (float)elapsed / (float)nextTickIntervalUs;
  if (t > 1.0f) t = 1.0f;
  if (t < 0.0f) t = 0.0f;

  // --- Synth 1 cutoff ---
  if (cutoffInterp_1) {
    int16_t diff = (int16_t)cutoffTo_1 - (int16_t)cutoffFrom_1;
    uint8_t val = cutoffFrom_1 + (uint8_t)((float)diff * t);
    handleCC(SYNTH1_MIDI_CHAN, synthEditCC[0], val);
    if (t >= 1.0f) cutoffInterp_1 = false;
  }

  // --- Synth 2 cutoff ---
  if (cutoffInterp_2) {
    int16_t diff = (int16_t)cutoffTo_2 - (int16_t)cutoffFrom_2;
    uint8_t val = cutoffFrom_2 + (uint8_t)((float)diff * t);
    handleCC(SYNTH2_MIDI_CHAN, synthEditCC[0], val);
    if (t >= 1.0f) cutoffInterp_2 = false;
  }

  // --- Drum cutoff ---
  if (cutoffInterp_d) {
    int16_t diff = (int16_t)cutoffTo_d - (int16_t)cutoffFrom_d;
    uint8_t val = cutoffFrom_d + (uint8_t)((float)diff * t);
    handleCC(DRUM_MIDI_CHAN, drumEditCC[0], val);
    if (t >= 1.0f) cutoffInterp_d = false;
  }
}

// ============================================================
// Sequencer service function — call from regular_checks() on Core 1
// Checks micros() and fires sequencer_tick() when the interval expires.
// Also handles cutoff interpolation on every poll.
// ============================================================
void sequencer_service() {
  if (!globalSeq.isPlaying) return;

  // In follower mode the MIDI real-time callback owns the step grid.  Do not
  // let the local micros() scheduler generate a competing tick stream.
  if (midiClockSource() == CLOCK_SRC_MIDI) return;

  // Run cutoff interpolation on every poll (smooth ramping)
  sequencer_interpolate_cutoff();

  uint32_t now = micros();
  uint32_t elapsed = now - lastTickUs;

  if (elapsed >= nextTickIntervalUs) {
    // 1. Capture the interval we actually just waited for before it gets modified
    uint32_t completedInterval = nextTickIntervalUs;

    // Prevent catch-up: if we've overshot by more than one interval, skip
    if (elapsed < completedInterval * 2) {
      // 2. Running this updates nextTickIntervalUs for the NEXT step
      sequencer_tick();
    } else {
      // We've fallen too far behind — reset timing and advance silently
      globalSeq.currentStep = (globalSeq.currentStep + 1) & 0x0F;
      Sweep.OnSequencerStep(globalSeq.currentStep);
      
      // Recalculate nextTickIntervalUs to match the skipped step's swing timing
      uint32_t baseInterval = calc_16th_interval_us(globalSeq.bpm);
      float swingRatio = globalSeq.swing / 200.0f;
      if (globalSeq.currentStep & 1) {
        nextTickIntervalUs = baseInterval - (uint32_t)((float)baseInterval * swingRatio);
      } else {
        nextTickIntervalUs = baseInterval + (uint32_t)((float)baseInterval * swingRatio);
      }
    }

    // 3. Advance the timing anchor by the interval we JUST completed
    lastTickUs += completedInterval;
  }
}

void sequencer_midi_step() {
  if (!globalSeq.isPlaying) return;
  sequencer_tick();
}

// ============================================================
// Load functions: copy legacy AcidBanger patterns into our structures
// ============================================================

void sequencer_load_synth_pattern(SynthPattern* dst,
                                   const uint8_t* notes,
                                   uint16_t accentBits,
                                   uint16_t glideBits) {
  for (int i = 0; i < 16; i++) {
    SynthStep& s = dst->steps[i];
    s.note   = notes[i];
    s.active = (notes[i] > 0);
    s.accent = (accentBits >> i) & 1;
    s.slide  = (glideBits >> i) & 1;
  }
}

void sequencer_load_drum_pattern(DrumPattern* dst,
                                  const uint8_t* kick,
                                  const uint8_t* snare,
                                  const uint8_t* ch,
                                  const uint8_t* oh,
                                  const uint8_t* perc,
                                  const uint8_t* crash) {
  for (int i = 0; i < 16; i++) {
    uint16_t mask = 0;
    if (kick[i]  > 0)  mask |= (1 << 0);
    if (snare[i] > 0)  mask |= (1 << 1);
    if (ch[i]    > 0)  mask |= (1 << 2);
    if (oh[i]    > 0)  mask |= (1 << 3);
    if (perc[i]  > 0)  mask |= (1 << 10);
    if (crash[i] > 0)  mask |= (1 << 8);
    dst->steps[i] = mask;
  }
}

// ============================================================
// Clear all pattern and automation data for a given part
// ============================================================
void sequencer_clear_part(EditType part) {
  switch (part) {
    case Syn1:
      memset(&globalSeq.synth1, 0, sizeof(SynthPattern));
      memset(&globalSeq.autoSynth1, 0, sizeof(SynthAutomation));
      break;
    case Syn2:
      memset(&globalSeq.synth2, 0, sizeof(SynthPattern));
      memset(&globalSeq.autoSynth2, 0, sizeof(SynthAutomation));
      break;
    case Drm:
      memset(&globalSeq.drum, 0, sizeof(DrumPattern));
      memset(&globalSeq.autoDrum, 0, sizeof(DrumAutomation));
      break;
    default:
      break;
  }
  acidBoxSaveLoad.modified = true;
}

// ============================================================
// Automation recording
// ============================================================

// Write a parameter value into a selected voice's automation lane at the
// current step.  The voice is explicit because Sweep can edit the cutoff of a
// synth/drum voice while currentEditType remains Fx.
void sequencer_write_automation_step_for_voice(EditType type, uint8_t lane, uint8_t value) {
  if (globalSeq.currentStep >= 16) return;

  switch (type) {
    case Syn1:
      globalSeq.autoSynth1.lanes[lane][globalSeq.currentStep] = value;
      globalSeq.autoSynth1.laneEnabled |= (1 << lane);
      break;
    case Syn2:
      globalSeq.autoSynth2.lanes[lane][globalSeq.currentStep] = value;
      globalSeq.autoSynth2.laneEnabled |= (1 << lane);
      break;
    case Drm:
      if (lane < 8) {
        globalSeq.autoDrum.lanes[lane][globalSeq.currentStep] = value;
        globalSeq.autoDrum.laneEnabled |= (1 << lane);
      }
      break;
    default:
      break;
  }
  acidBoxSaveLoad.modified = true;
}

// Write a parameter value to ALL 16 steps of a selected voice's automation
// lane. The voice is explicit because Sweep can edit another voice's cutoff.
void sequencer_write_automation_all_steps_for_voice(EditType type, uint8_t lane, uint8_t value) {
  switch (type) {
    case Syn1:
      for (uint8_t i = 0; i < 16; i++) {
        globalSeq.autoSynth1.lanes[lane][i] = value;
      }
      globalSeq.autoSynth1.laneEnabled |= (1 << lane);
      break;
    case Syn2:
      for (uint8_t i = 0; i < 16; i++) {
        globalSeq.autoSynth2.lanes[lane][i] = value;
      }
      globalSeq.autoSynth2.laneEnabled |= (1 << lane);
      break;
    case Drm:
      if (lane < 8) {
        for (uint8_t i = 0; i < 16; i++) {
          globalSeq.autoDrum.lanes[lane][i] = value;
        }
        globalSeq.autoDrum.laneEnabled |= (1 << lane);
      }
      break;
    default:
      break;
  }
  acidBoxSaveLoad.modified = true;
}

// Write the current parameter value into the current voice's automation lane
// at the current step. Used when F1+pot is detected (per-step recording).
void sequencer_write_automation_step(uint8_t lane, uint8_t value) {
  sequencer_write_automation_step_for_voice(currentEditType, lane, value);
}

// Write a parameter value to ALL 16 steps of the current voice's automation
// lane. Used when pot is turned without F1 held (global fill).
void sequencer_write_automation_all_steps(uint8_t lane, uint8_t value) {
  sequencer_write_automation_all_steps_for_voice(currentEditType, lane, value);
}

// ============================================================
// Drum step editing
// ============================================================
void sequencer_toggle_drum_step(uint8_t step, uint16_t laneMask) {
  if (step >= 16) return;
  if (laneMask == 0) return;
  // Toggle the bit for this lane at the given step
  globalSeq.drum.steps[step] ^= laneMask;
  acidBoxSaveLoad.modified = true;
}

void sequencer_set_legacy_drum_mapping(bool legacyMapping) {
  legacyClavMapping = legacyMapping;
}

// ============================================================
// Synth step editing
// ============================================================
void sequencer_toggle_synth_step(uint8_t step, EditType type) {
  if (step >= 16) return;
  
  SynthPattern* pattern;
  uint8_t midiChan;
  if (type == Syn1) {
    pattern = &globalSeq.synth1;
    midiChan = SYNTH1_MIDI_CHAN;
  } else if (type == Syn2) {
    pattern = &globalSeq.synth2;
    midiChan = SYNTH2_MIDI_CHAN;
  } else {
    return;
  }
  
  SynthStep& s = pattern->steps[step];
  
  // Toggle: if active with a note -> deactivate (clear note too)
  // If inactive -> activate with default note
  // In normal mode, always clear slide and accent flags
  if (s.active && s.note > 0) {
    // Always send note-off when toggling a step off
    midi_send_noteoff(midiChan, s.note);
    handleNoteOff(midiChan, s.note, 0);
    // Clear last note tracking if this was the note being played
    if (type == Syn1 && lastNote1 == s.note) lastNote1 = 0;
    if (type == Syn2 && lastNote2 == s.note) lastNote2 = 0;
    s.active = false;
    s.slide = false;
    s.accent = false;
    s.note = 0;
  } else {
    // Activate with a default note (mid-range, quantized to current scale)
    s.active = true;
    s.slide = false;
    s.accent = false;
    s.note = quantizeNoteToScale(60, currentScale); // default to C4 quantized

    // Clear any previous MVA state to prevent accidental slide/legato.
    // When adding a note while the sequencer is playing (or has previously
    // played), the MVA stack may still contain old notes.  If we send a
    // note-on without cleaning up, mva_note_on will stack the new note on
    // top of the old one, causing mva1.n > 1 and thus slide=true in
    // on_midi_noteON.  Using allNotesOff() first ensures n=1 and slide=false.
    uint8_t vel = s.accent ? 120 : 79;

    if (type == Syn1) {
      Synth1.allNotesOff();
      midi_send_noteon(midiChan, s.note, vel);
      handleNoteOn(midiChan, s.note, vel);
      lastNote1 = s.note;
    } else {
      Synth2.allNotesOff();
      midi_send_noteon(midiChan, s.note, vel);
      handleNoteOn(midiChan, s.note, vel);
      lastNote2 = s.note;
    }
  }
  acidBoxSaveLoad.modified = true;
}

// ============================================================
// Synth step slide/accent editing
// ============================================================
void sequencer_toggle_synth_slide_or_accent(uint8_t step, EditType type, bool isSlide) {
  if (step >= 16) return;
  
  SynthPattern* pattern;
  if (type == Syn1) {
    pattern = &globalSeq.synth1;
  } else if (type == Syn2) {
    pattern = &globalSeq.synth2;
  } else {
    return;
  }
  
  SynthStep& s = pattern->steps[step];
  
  if (isSlide) {
    // Slide mode
    if (!s.active || s.note == 0) {
      // Step is off: turn it on with slide flag
      s.active = true;
      s.slide = true;
      if (s.note == 0) {
        s.note = quantizeNoteToScale(60, currentScale);
      }
    } else if (!s.slide) {
      // Step is on but not slide: set slide flag
      s.slide = true;
    } else {
      // Step is already a slide step: remove slide flag but keep step active
      s.slide = false;
    }
  } else {
    // Accent mode
    if (!s.active || s.note == 0) {
      // Step is off: turn it on with accent flag
      s.active = true;
      s.accent = true;
      if (s.note == 0) {
        s.note = quantizeNoteToScale(60, currentScale);
      }
    } else if (!s.accent) {
      // Step is on but not accent: set accent flag
      s.accent = true;
    } else {
      // Step is already an accent step: remove accent flag but keep step active
      s.accent = false;
    }
  }
  acidBoxSaveLoad.modified = true;
}

void sequencer_set_synth_step_note(uint8_t step, uint8_t note, EditType type) {
  if (step >= 16) return;
  
  SynthPattern* pattern;
  uint8_t midiChan;
  if (type == Syn1) {
    pattern = &globalSeq.synth1;
    midiChan = SYNTH1_MIDI_CHAN;
  } else if (type == Syn2) {
    pattern = &globalSeq.synth2;
    midiChan = SYNTH2_MIDI_CHAN;
  } else {
    return;
  }
  
  SynthStep& s = pattern->steps[step];
  s.note = note;
  s.active = true; // Always force the step active when adjusting pitch via pot
  
  // Retrigger the note so we can hear pitch changes
  // Use allNotesOff() to clear the MVA stack entirely, preventing the
  // monophonic voice allocator from setting slide=true (which happens
  // when mva1.n > 1 after note-off + note-on).
  uint8_t vel = s.accent ? 120 : 79;
  
  if (type == Syn1) {
    Synth1.allNotesOff();  // clears MVA stack, sets mva1.n = 0
    midi_send_noteon(midiChan, s.note, vel);
    handleNoteOn(midiChan, s.note, vel);
  } else {
    Synth2.allNotesOff();
    midi_send_noteon(midiChan, s.note, vel);
    handleNoteOn(midiChan, s.note, vel);
  }
  
  // Update last note tracking
  if (type == Syn1) lastNote1 = s.note;
  if (type == Syn2) lastNote2 = s.note;
  acidBoxSaveLoad.modified = true;
}

// ============================================================
// Note quantization to scale
// ============================================================
// Given a MIDI note number and a 12-bit scale mask (bit 0 = C, bit 1 = C#, etc.),
// find the nearest note that is in the scale.
uint8_t quantizeNoteToScale(uint8_t note, uint16_t scaleMask) {
  // If scale is chromatic or all bits set, no quantization needed
  if (scaleMask == SCALE_CHROMATIC) return note;
  
  // Clamp to valid range
  if (note > 127) note = 127;
  
  uint8_t octave = note / 12;
  uint8_t semitone = note % 12;
  
  // If the current semitone is in the scale, return as-is
  if (scaleMask & (1 << semitone)) {
    return note;
  }
  
  // Search upward and downward for the nearest scale note
  for (uint8_t offset = 1; offset <= 6; offset++) {
    // Check upward
    int8_t upSemitone = semitone + offset;
    int8_t upOctave = octave;
    if (upSemitone >= 12) {
      upSemitone -= 12;
      upOctave++;
    }
    if (upOctave <= 10 && (scaleMask & (1 << upSemitone))) {
      uint8_t result = upOctave * 12 + upSemitone;
      if (result <= 127) return result;
    }
    
    // Check downward
    int8_t downSemitone = semitone - offset;
    int8_t downOctave = octave;
    if (downSemitone < 0) {
      downSemitone += 12;
      downOctave--;
    }
    if (downOctave >= 0 && (scaleMask & (1 << downSemitone))) {
      uint8_t result = downOctave * 12 + downSemitone;
      return result;
    }
  }
  
  // Fallback: return the original note
  return note;
}
