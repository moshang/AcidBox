#ifndef SEQUENCER_H
#define SEQUENCER_H

#include <Arduino.h>
#include <stdint.h>
#include "UI.h"

// ============================================================
// Data Structures for the 16-Step Sequencer
// ============================================================

// A single step for a synth voice
struct SynthStep {
  uint8_t note;    // MIDI note number (0 = rest/no note)
  bool active;     // step is active (plays when sequenced)
  bool accent;     // accent flag (louder)
  bool slide;      // glide/portamento flag
};

// A 16-step pattern for a synth voice
struct SynthPattern {
  SynthStep steps[16];
};

// A 16-step drum pattern using bitmasks for up to 16 voices
// Bit assignments:
//   BD  = 1 << 0  (kick)
//   SD  = 1 << 1  (snare)
//   CH  = 1 << 2  (closed hi-hat)
//   OH  = 1 << 3  (open hi-hat)
//   CLAP= 1 << 4  (clap)
//   LT  = 1 << 5  (low tom)
//   MT  = 1 << 6  (mid tom)
//   HT  = 1 << 7  (high tom)
//   CR  = 1 << 8  (crash)
//   RIM = 1 << 9  (rimshot)
//   MAR = 1 << 10 (maraca/shaker)
//   CLAV= 1 << 11 (claves)
//   COW = 1 << 12 (cowbell)
//   CY  = 1 << 13 (cymbal)
//   CONG= 1 << 14 (conga)
//   TIMB= 1 << 15 (timbale)
struct DrumPattern {
  uint16_t steps[16];  // bitmask per step
};

// Playback modes
enum PlaybackMode {
  MODE_JUKEBOX = 0,  // Jukebox algorithm fills and drives globalSeq
  MODE_EDIT    = 1   // User edits patterns; playback loops globalSeq
};

// Top-level sequencer state
struct SequencerState {
  SynthPattern synth1;
  SynthPattern synth2;
  DrumPattern  drum;

  float        bpm;          // beats per minute (default 120.0)
  float        swing;        // swing percentage (50.0 = straight, 50..75)
  uint8_t      currentStep;  // current step index [0..15]
  bool         isPlaying;    // whether the sequencer is actively playing
};

// ============================================================
// Scale definitions for note quantization
// ============================================================
// Bitmask of 12 semitones (bit 0 = C, bit 1 = C#, ..., bit 11 = B)
#define SCALE_CHROMATIC  0x0FFF  // All 12 semitones
#define SCALE_MAJOR      0x0AB5  // C D E F G A B
#define SCALE_MINOR      0x08AE  // A B C D E F G (natural minor)
#define SCALE_PENTATONIC 0x0296  // C D E G A
#define SCALE_BLUES      0x02B6  // C Eb F F# G Bb

// ============================================================
// Global instances
// ============================================================
extern SequencerState globalSeq;
extern PlaybackMode    currentMode;
extern uint16_t        currentScale;  // active scale bitmask (default SCALE_CHROMATIC)

// ============================================================
// Sequencer API
// ============================================================

// Initialise globalSeq with default values
void sequencer_init();

// Set the playback mode (JUKEBOX or EDIT) — safely suspends/resumes generation
void setMode(PlaybackMode newMode);

// Called each time a 16th-note boundary is reached.
// Triggers note-ons/offs for the current step via MIDI.
// The caller is responsible for calling this at the correct microsecond interval.
// Returns the number of microseconds until the next 16th-note tick (accounting for swing).
uint32_t sequencer_tick();

// Start / stop the sequencer transport
void sequencer_start();
void sequencer_stop();

// Toggle play state
void sequencer_toggle_play();

// Service function: call from the main loop or regular_checks()
// Checks micros() and fires sequencer_tick() at the correct interval.
void sequencer_service();

// Toggle a drum step on/off for a specific lane (bitmask)
void sequencer_toggle_drum_step(uint8_t step, uint16_t laneMask);

// Toggle a synth step on/off for Syn1 or Syn2
void sequencer_toggle_synth_step(uint8_t step, EditType type);

// Toggle slide or accent flag on a synth step for Syn1 or Syn2
// In slide mode (isSlide=true): off→on+slide, on→slide, slide→off
// In accent mode (isSlide=false): off→on+accent, on→accent, accent→off
void sequencer_toggle_synth_slide_or_accent(uint8_t step, EditType type, bool isSlide);

// Set the note for a synth step
void sequencer_set_synth_step_note(uint8_t step, uint8_t note, EditType type);

// Quantize a MIDI note to the nearest note in the given scale bitmask
uint8_t quantizeNoteToScale(uint8_t note, uint16_t scaleMask);

// Fill a SynthPattern from a legacy AcidBanger Pattern (notes, accent, glide bitfields)
void sequencer_load_synth_pattern(SynthPattern* dst, const uint8_t* notes, uint16_t accentBits, uint16_t glideBits);

// Fill a DrumPattern from legacy drum byte arrays (one per instrument)
void sequencer_load_drum_pattern(DrumPattern* dst, const uint8_t* kick, const uint8_t* snare,
                                  const uint8_t* ch, const uint8_t* oh,
                                  const uint8_t* perc, const uint8_t* crash);

#endif // SEQUENCER_H