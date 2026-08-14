#ifndef SAVE_H
#define SAVE_H

#include <Arduino.h>
#include <stdint.h>
#include "sequencer.h"

// ========================================
// ACIDBOX SAVE/LOAD SYSTEM
// ========================================
//
// File Structure:
// /ACIDBOX/BANKS/BANK_01/ to BANK_16/
//   Each bank folder contains:
//   /SONG_01/ to /SONG_16/
//     Each song folder contains:
//     pattern_01.dat to pattern_16.dat (serialized pattern data)
//
// Folder/file numbers are 1-indexed to match the UI.
// Steps 1-8 (A1-A8) = pattern indices 0-7
// Steps 9-16 (B1-B8) = pattern indices 8-15
//
// ========================================

#define ACIDBOX_SAVE_MAGIC   0x41424F58  // 'ABOX'
#define ACIDBOX_SAVE_VERSION 3

// Binary file format for a single pattern slot
struct __attribute__((packed)) AcidBoxPatternFile {
    uint32_t magic;       // ACIDBOX_SAVE_MAGIC for validation
    uint8_t  version;     // ACIDBOX_SAVE_VERSION

    // Pattern data
    SynthPattern synth1;
    SynthPattern synth2;
    DrumPattern  drum;

    // Automation lanes
    SynthAutomation autoSynth1;
    SynthAutomation autoSynth2;
    DrumAutomation  autoDrum;

    // Per-pattern timing
    float bpm;
    float swing;

    // Drum kit selection (since version 2)
    uint8_t drumKitNumber;      // program number of the drum kit (0 = unset/default)
    char    drumKitName[32];    // folder name of the drum kit, null-terminated

    // Scale and root note (since version 3)
    uint8_t scaleIndex;         // active scale index (0..NUM_SCALES)
    uint8_t rootNote;           // MIDI root note (default 60 = C4)
};

// Save/Load UI state tracking
struct AcidBoxSaveLoadState {
    uint8_t currentBank;         // 0-15
    uint8_t currentSong;         // 0-15
    uint8_t currentPattern;      // 0-15 (which slot is loaded in memory)
    bool modified;               // has current pattern been modified since last save?
    bool patternExistsCache[16]; // cache for pattern existence
    bool bankExistsCache[16];    // cache for bank existence
    bool songExistsCache[16];    // cache for song existence in currentBank
};

extern AcidBoxSaveLoadState acidBoxSaveLoad;

// Deferred drum kit load state (to avoid WDT timeout from calling Init() inside loadAcidBoxPattern)
extern int     deferredKitLoadIndex;   // -1 = use number-based load, >=0 = use index-based
extern uint8_t deferredKitLoadNumber;  // program number for fallback (only used when deferredKitLoadIndex < 0)
void processDeferredKitLoad();         // call periodically from UI task

// ========================================
// API Functions
// ========================================

// Initialize save directories and state
void acidbox_save_init();

// Initialize just the state (for when SD is not available)
void acidbox_save_init_state();

// Path builders
void getAcidBoxBankPath(char* buffer, uint8_t bankNum);
void getAcidBoxSongPath(char* buffer, uint8_t bankNum, uint8_t songNum);
void getAcidBoxPatternPath(char* buffer, uint8_t bankNum, uint8_t songNum, uint8_t patternNum);

// Existence checks
bool doesAcidBoxBankExist(uint8_t bankNum);
bool doesAcidBoxSongExist(uint8_t bankNum, uint8_t songNum);
bool doesAcidBoxPatternExist(uint8_t bankNum, uint8_t songNum, uint8_t patternNum);

// Cache refresh
void refreshAcidBoxPatternCache();
void refreshAcidBoxBankCache();
void refreshAcidBoxSongCache();

// Core save/load
bool saveAcidBoxPattern(uint8_t bankNum, uint8_t songNum, uint8_t patternNum);
bool loadAcidBoxPattern(uint8_t bankNum, uint8_t songNum, uint8_t patternNum);

// Delete saved AcidBox content. Pattern deletion removes one file; song and
// bank deletion remove their complete directory trees.
bool deleteAcidBoxPattern(uint8_t bankNum, uint8_t songNum, uint8_t patternNum);
bool clearAcidBoxSong(uint8_t bankNum, uint8_t songNum);
bool clearAcidBoxBank(uint8_t bankNum);

// Get the current drum kit number from the Sampler
uint8_t getCurrentDrumKitNumber();

// Helper: save the current globalSeq into the specified pattern slot
bool saveCurrentPattern(uint8_t patternNum);

// Helper: load a pattern from the current bank/song into the specified slot in memory
bool loadCurrentPattern(uint8_t patternNum);

#endif // SAVE_H