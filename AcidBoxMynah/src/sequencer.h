#ifndef SEQUENCER_H
#define SEQUENCER_H

#include <Arduino.h>
#include <stdint.h>

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
// Global instances
// ============================================================
extern SequencerState globalSeq;
extern PlaybackMode    currentMode;

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

// Fill a SynthPattern from a legacy AcidBanger Pattern (notes, accent, glide bitfields)
void sequencer_load_synth_pattern(SynthPattern* dst, const uint8_t* notes, uint16_t accentBits, uint16_t glideBits);

// Fill a DrumPattern from legacy drum byte arrays (one per instrument)
void sequencer_load_drum_pattern(DrumPattern* dst, const uint8_t* kick, const uint8_t* snare,
                                  const uint8_t* ch, const uint8_t* oh,
                                  const uint8_t* perc, const uint8_t* crash);

#endif // SEQUENCER_H