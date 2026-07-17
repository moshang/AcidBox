#include <Arduino.h>
#include "sequencer.h"
#include "general.h"
#include "config.h"
#include "midi_config.h"
#include "synthvoice.h"
#include "sampler.h"

// ============================================================
// Global instances
// ============================================================
SequencerState globalSeq;
PlaybackMode    currentMode = MODE_JUKEBOX;
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
// Drum voice MIDI note mappings (matching AcidBanger.cpp)
// ============================================================
#define KICK_NOTE  0  // BD
#define SNARE_NOTE 1  // SD
#define CH_NOTE    6  // CH (closed hat)
#define OH_NOTE    7  // OH (open hat)
#define CLAP_NOTE  4  // CLAP (handclap)
#define LT_NOTE    2  // low tom
#define MT_NOTE    3  // mid tom
#define HT_NOTE    5  // high tom
#define CRASH_NOTE 9  // CR
#define RIM_NOTE   8  // rimshot
#define PERC_NOTE  11 // percussion / maraca

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
  globalSeq.swing     = 60.0f;
  globalSeq.currentStep = 0;
  globalSeq.isPlaying = false;

  currentMode = MODE_JUKEBOX;
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
}

void sequencer_stop() {
  if (!globalSeq.isPlaying) return;
  globalSeq.isPlaying = false;
  sequencer_all_notes_off();
}

void sequencer_toggle_play() {
  if (currentMode == MODE_JUKEBOX) {
    // In JUKEBOX mode: toggle the legacy jukebox generation engine.
    // This flips midi_playing, which drives run_tick() → sequencer_step()
    // that bridges patterns into globalSeq and plays via legacy MIDI.
    midi_toggle_play();
    // Keep globalSeq.isPlaying in sync with the jukebox
    // (midi_toggle_play flips midi_playing inside AcidBanger.cpp)
    globalSeq.isPlaying = !globalSeq.isPlaying;
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
// ============================================================
void sequencer_apply_automation() {
  uint8_t step = globalSeq.currentStep;
  uint8_t ccVal;

  // --- Synth 1 automation ---
  for (uint8_t lane = 0; lane < 16; lane++) {
    if (globalSeq.autoSynth1.laneEnabled & (1 << lane)) {
      ccVal = globalSeq.autoSynth1.lanes[lane][step];
      handleCC(SYNTH1_MIDI_CHAN, synthEditCC[lane], ccVal);
    }
  }

  // --- Synth 2 automation ---
  for (uint8_t lane = 0; lane < 16; lane++) {
    if (globalSeq.autoSynth2.laneEnabled & (1 << lane)) {
      ccVal = globalSeq.autoSynth2.lanes[lane][step];
      handleCC(SYNTH2_MIDI_CHAN, synthEditCC[lane], ccVal);
    }
  }

  // --- Drum automation ---
  for (uint8_t lane = 0; lane < 8; lane++) {
    if (globalSeq.autoDrum.laneEnabled & (1 << lane)) {
      ccVal = globalSeq.autoDrum.lanes[lane][step];
      handleCC(DRUM_MIDI_CHAN, drumEditCC[lane], ccVal);
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
      { 1 << 10, PERC_NOTE  },  // MAR (maraca/shaker)
      { 1 << 11, CLAP_NOTE  },  // CLAV (claves → reuse clap note for simplicity)
    };
    static const size_t numDrumEntries = sizeof(drumMap) / sizeof(drumMap[0]);

    for (size_t i = 0; i < numDrumEntries; i++) {
      if (drumMask & drumMap[i].bit) {
        uint8_t midiNote = current_drumkit + drumMap[i].note;
        uint8_t vel = 100;
        midi_send_noteon(DRUM_MIDI_CHAN, midiNote, vel);
        handleNoteOn(DRUM_MIDI_CHAN, midiNote, vel);
      }
    }
  }

  // --- Calculate interval until the next tick (with swing) ---
  uint32_t baseInterval = calc_16th_interval_us(globalSeq.bpm);

  // Swing: shift even steps (0-indexed: 1, 3, 5, 7, 9, 11, 13, 15) based on ratio
  // Swing range: 50.0 (straight) → 75.0 (maximum swing)
  // At 50%: no shift.
  // At 75%: offbeat 16ths are delayed by ~50% of the base interval.
  // Interval pattern: [T+d, T-d, T+d, T-d, ...]
  //   where d = baseInterval * (swing - 50) / 50
  //   Even→odd step (this step = even index): interval = T + d
  //   Odd→even step (this step = odd index):  interval = T - d
  float swingRatio = (globalSeq.swing - 50.0f) / 50.0f;
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
// Sequencer service function — call from regular_checks() on Core 1
// Checks micros() and fires sequencer_tick() when the interval expires.
// ============================================================
void sequencer_service() {
  if (!globalSeq.isPlaying) return;

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
      
      // Recalculate nextTickIntervalUs to match the skipped step's swing timing
      uint32_t baseInterval = calc_16th_interval_us(globalSeq.bpm);
      float swingRatio = (globalSeq.swing - 50.0f) / 50.0f;
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
// Automation recording
// ============================================================

// Write the current parameter value into the automation lane at the current step.
// Used when F1+pot is detected (per-step recording).
void sequencer_write_automation_step(uint8_t lane, uint8_t value) {
  if (globalSeq.currentStep >= 16) return;

  switch (currentEditType) {
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
}

// Write the current parameter value to ALL 16 steps of the automation lane.
// Used when pot is turned without F1 held (global fill).
void sequencer_write_automation_all_steps(uint8_t lane, uint8_t value) {
  switch (currentEditType) {
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
}

// ============================================================
// Drum step editing
// ============================================================
void sequencer_toggle_drum_step(uint8_t step, uint16_t laneMask) {
  if (step >= 16) return;
  if (laneMask == 0) return;
  // Toggle the bit for this lane at the given step
  globalSeq.drum.steps[step] ^= laneMask;
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
      // Step is already a slide step: toggle it off
      s.active = false;
      s.slide = false;
      s.note = 0;
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
      // Step is already an accent step: toggle it off
      s.active = false;
      s.accent = false;
      s.note = 0;
    }
  }
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
  uint8_t oldNote = s.note;
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
