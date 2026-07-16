#include <Arduino.h>
#include "SCALES.h"
#include "sequencer.h"

// ============================================================
// SCALES.cpp — Scale system for AcidBoxMynah
//
// Scale arrays: 1-based semitone offsets from the root.
//   scalePointer[i] == semitone_offset + 1
//   e.g. major[0] = 1  → 0 semitones (root)
//        major[1] = 3  → 2 semitones (major 2nd)
//        major[6] = 12 → 11 semitones (major 7th)
// ============================================================

// ----------------------------------------
// Scale interval arrays (1-based, RAM)
// ----------------------------------------
static const uint8_t scale_major[7]        = { 1, 3, 5, 6, 8, 10, 12 };
static const uint8_t scale_minorMel[7]     = { 1, 3, 4, 6, 8, 10, 12 };
static const uint8_t scale_minorHarm[7]    = { 1, 3, 4, 6, 8,  9, 12 };
static const uint8_t scale_pentaMaj[5]     = { 1, 3, 5, 8, 10 };
static const uint8_t scale_pentaMin[5]     = { 1, 4, 6, 8, 11 };
static const uint8_t scale_bluesHex[6]     = { 1, 4, 6, 7,  8, 11 };
static const uint8_t scale_bluesHept[7]    = { 1, 3, 4, 6,  7, 10, 11 };
static const uint8_t scale_triadMaj[3]     = { 1, 5, 8 };
static const uint8_t scale_triadMin[3]     = { 1, 4, 8 };
static const uint8_t scale_maj7[4]         = { 1, 5, 8, 12 };
static const uint8_t scale_min7[4]         = { 1, 4, 8, 11 };
static const uint8_t scale_ragaBhairav[7]  = { 1, 2, 5, 6, 8,  9, 12 };
static const uint8_t scale_spanish[8]      = { 1, 2, 4, 5, 6,  8,  9, 11 };
static const uint8_t scale_romani[7]       = { 1, 3, 4, 7, 8,  9, 12 };
static const uint8_t scale_arabian[7]      = { 1, 3, 5, 6, 7,  9, 11 };
static const uint8_t scale_egyptian[5]     = { 1, 3, 6, 8, 11 };
static const uint8_t scale_hawaiian[5]     = { 1, 3, 4, 8, 10 };
static const uint8_t scale_baliPelog[5]    = { 1, 2, 4, 8,  9 };
static const uint8_t scale_miyakobushi[5]  = { 1, 2, 6, 8,  9 };
static const uint8_t scale_ryukyu[5]       = { 1, 5, 6, 8, 12 };
static const uint8_t scale_wholetone[6]    = { 1, 3, 5, 7,  9, 11 };
// Chromatic: all 12 semitones (used for SCALE_INDEX_CHROMATIC index)
static const uint8_t scale_chromatic[12]   = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

// ----------------------------------------
// Scale lookup table (index 0..NUM_SCALES)
// ----------------------------------------
struct ScaleInfo {
    const uint8_t* intervals;
    uint8_t        size;
    const char*    name;
};

static const ScaleInfo scaleTable[NUM_SCALES + 1] = {
    { scale_major,       7,  "Major"        },  //  0
    { scale_minorMel,    7,  "Minor"        },  //  1
    { scale_minorHarm,   7,  "Min Harm"     },  //  2
    { scale_pentaMaj,    5,  "Penta Maj"    },  //  3
    { scale_pentaMin,    5,  "Penta Min"    },  //  4
    { scale_bluesHex,    6,  "Blues Hex"    },  //  5
    { scale_bluesHept,   7,  "Blues Hept"   },  //  6
    { scale_triadMaj,    3,  "Maj Triad"    },  //  7
    { scale_triadMin,    3,  "Min Triad"    },  //  8
    { scale_maj7,        4,  "Major 7th"    },  //  9
    { scale_min7,        4,  "Minor 7th"    },  // 10
    { scale_ragaBhairav, 7,  "Raga Bhairav" },  // 11
    { scale_spanish,     8,  "Spanish"      },  // 12
    { scale_romani,      7,  "Romani"       },  // 13
    { scale_arabian,     7,  "Arabian"      },  // 14
    { scale_egyptian,    5,  "Egyptian"     },  // 15
    { scale_hawaiian,    5,  "Hawaiian"     },  // 16
    { scale_baliPelog,   5,  "Bali Pelog"   },  // 17
    { scale_miyakobushi, 5,  "Miyakobushi"  },  // 18
    { scale_ryukyu,      5,  "Ryukyu"       },  // 19
    { scale_wholetone,   6,  "Wholetone"    },  // 20
    { scale_chromatic,   12, "Chromatic"    },  // 21 = SCALE_INDEX_CHROMATIC
};

// ----------------------------------------
// Global state
// ----------------------------------------
const uint8_t* scalePointer = scale_major;
uint8_t        scaleSize    = 7;
uint8_t        rootNote     = 60;   // Middle C (C4)
uint8_t        scaleIndex = 0;
const char*    scaleName    = scaleTable[0].name;

// ----------------------------------------
// setScale()
// ----------------------------------------
void setScale(uint8_t index) {
    if (index > NUM_SCALES)
        index = SCALE_INDEX_CHROMATIC;

    scaleIndex = index;
    scalePointer = scaleTable[index].intervals;
    scaleSize    = scaleTable[index].size;
    scaleName    = scaleTable[index].name;
}

// ----------------------------------------
// rawNoteToMidi()
//
// Maps a sequential scale-degree index to a MIDI note number.
//
//   rawNote 0           → rootNote (first degree, octave 0)
//   rawNote scaleSize   → rootNote + 12 (first degree, octave 1)
//
// Formula:
//   notenum = rootNote
//             + scalePointer[rawNote % scaleSize] - 1   (0-based semitone offset)
//             + (rawNote / scaleSize) * 12               (octave shift)
// ----------------------------------------
uint8_t rawNoteToMidi(int rawNote) {
    if (scaleSize == 0)
        return rootNote;

    // Handle negative rawNote values (e.g. pitches below root)
    int octaveShift = rawNote / (int)scaleSize;
    int degree      = rawNote % (int)scaleSize;
    if (degree < 0) {
        degree      += (int)scaleSize;
        octaveShift -= 1;
    }

    int notenum = (int)rootNote
                  + (int)scalePointer[degree] - 1
                  + octaveShift * 12;

    // Clamp to valid MIDI range
    if (notenum < 0)   notenum = 0;
    if (notenum > 127) notenum = 127;

    return (uint8_t)notenum;
}

// ----------------------------------------
// isNoteInScale()
//
// Returns true if checkNotenum belongs to the active scale
// (pitch-class check — octave-agnostic).
//
// Algorithm:
//   1. Compute semitone distance from rootNote, mod 12 → 0..11
//   2. Convert to 1-based interval (+ 1)
//   3. Search for that interval in scalePointer[]
// ----------------------------------------
bool isNoteInScale(uint8_t notenum) {
    int diff           = (int)notenum - (int)rootNote;
    int semitoneOffset = ((diff % 12) + 12) % 12;   // 0..11, always positive
    int scaleInterval  = semitoneOffset + 1;          // 1-based

    for (int i = 0; i < (int)scaleSize; ++i) {
        if (scalePointer[i] == (uint8_t)scaleInterval)
            return true;
    }
    return false;
}

// ----------------------------------------
// snapToScale()
//
// Finds the nearest MIDI note in the active scale to notenum.
// Searches outward (±1, ±2, ...) until a scale note is found.
// ----------------------------------------
uint8_t snapToScale(uint8_t notenum) {
    if (isNoteInScale(notenum))
        return notenum;

    for (int delta = 1; delta <= 6; ++delta) {
        int upper = (int)notenum + delta;
        int lower = (int)notenum - delta;

        if (upper <= 127 && isNoteInScale((uint8_t)upper))
            return (uint8_t)upper;
        if (lower >= 0   && isNoteInScale((uint8_t)lower))
            return (uint8_t)lower;
    }

    // Fallback: return rootNote pitch class at same octave
    return notenum;
}

// ----------------------------------------
// getRootNoteName()
//
// Writes a note-name string (e.g. "C4", "F#3") to buf.
// Convention: MIDI note 60 = C4 (octave = notenum/12 - 1).
// ----------------------------------------
void getRootNoteName(char* buf, uint8_t bufSize) {
    static const char* const kNoteNames[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    int octave = (int)(rootNote / 12) - 1;
    const char* note = kNoteNames[rootNote % 12];
    snprintf(buf, bufSize, "%s%d", note, octave);
}

// ----------------------------------------
// syncSequencerScale()
//
// Rebuild the sequencer's currentScale bitmask (12-bit) from the
// active scale intervals.  This keeps note-quantization in sequencer.cpp
// in sync with whatever scale the user selects via SCALE mode.
// ----------------------------------------
void syncSequencerScale() {
    uint16_t mask = 0;
    for (uint8_t i = 0; i < scaleSize; i++) {
        // scalePointer[i] is 1-based semitone offset (1..12)
        // bit position = offset - 1
        uint8_t semitone = scalePointer[i] - 1;
        if (semitone < 12) {
            mask |= (1 << semitone);
        }
    }
    // Rotate the bitmask so bit 0 = rootNote pitch class
    uint8_t rootPc = rootNote % 12;
    if (rootPc != 0) {
        // Rotate left by rootPc positions
        mask = ((mask << rootPc) | (mask >> (12 - rootPc))) & 0x0FFF;
    }
    currentScale = mask;
}
