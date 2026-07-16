#pragma once
#include <stdint.h>

// ============================================================
// SCALES.h — Scale system for AcidBoxMynah
//
// Scale arrays use 1-based semitone intervals from the root:
//   value 1  = root (0 semitones above root)
//   value 2  = 1 semitone above root
//   ...
//   value 12 = 11 semitones above root (major 7th)
//
// rawNoteToMidi() maps a sequential scale-degree index (rawNote)
// to a MIDI note number using:
//   notenum = rootNote + scalePointer[rawNote % scaleSize] - 1
//             + (rawNote / scaleSize) * 12
//
// isNoteInScale() checks whether any MIDI note number belongs to
// the active scale, regardless of octave.
// ============================================================

// ----------------------------------------
// Constants
// ----------------------------------------
#define NUM_SCALES      21        // Number of named scales (0-20); 21 = chromatic
#define SCALE_INDEX_CHROMATIC 21  // Index for the chromatic (all-notes) pseudo-scale
#define MAX_SCALE_SIZE  12   // Maximum notes in any scale array

// ----------------------------------------
// Global scale state (defined in SCALES.cpp)
// ----------------------------------------
extern const uint8_t* scalePointer; // Points to the active scale's interval array
extern uint8_t        scaleSize;    // Number of notes in the active scale
extern uint8_t        rootNote;     // MIDI root note (default 60 = C4)
extern uint8_t        scaleIndex;   // Index of the active scale (0..NUM_SCALES)
extern const char*    scaleName;    // Display name of the active scale

// ----------------------------------------
// Function declarations
// ----------------------------------------

/** Set the active scale by index (0..NUM_SCALES).
 *  SCALE_INDEX_CHROMATIC (21) selects the full chromatic scale. */
void setScale(uint8_t index);

/** Convert a sequential scale-degree index to a MIDI note number.
 *  rawNote 0 = root, rawNote 1 = second scale degree, etc.
 *  Wraps across octaves automatically. */
uint8_t rawNoteToMidi(int rawNote);

/** Returns true if the given MIDI note number belongs to the
 *  active scale (any octave). */
bool isNoteInScale(uint8_t notenum);

/** Snap a MIDI note number to the nearest note in the active scale.
 *  Useful for quantising incoming pitches. Returns the snapped note. */
uint8_t snapToScale(uint8_t notenum);

/** Write a human-readable root note name (e.g. "C4", "F#3") into buf.
 *  buf must be at least 5 bytes.  Based on MIDI convention: note 60 = C4. */
void getRootNoteName(char* buf, uint8_t bufSize);

/** Rebuild sequencer.currentScale bitmask from the active scale intervals + rootNote.
 *  Call this after setScale() or rootNote changes so note quantization stays in sync. */
void syncSequencerScale();
