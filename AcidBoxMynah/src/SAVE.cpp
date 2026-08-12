#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include "SAVE.h"
#include "general.h"
#include "sampler.h"
#include "sequencer.h"
#include "SCALES.h"

// ========================================
// ACIDBOX SAVE/LOAD SYSTEM IMPLEMENTATION
// ========================================

#define SAVE_BASE_PATH "/ACIDBOX/BANKS"

// Global save/load state
AcidBoxSaveLoadState acidBoxSaveLoad;

// Deferred drum kit load state
int     deferredKitLoadIndex = -1;   // -1 = no pending load, >=0 = index-based load
uint8_t deferredKitLoadNumber = 0;   // program number fallback (used when index < 0)

void processDeferredKitLoad() {
    if (deferredKitLoadIndex < 0 && deferredKitLoadNumber == 0) return; // nothing pending
    
    // Perform the deferred kit load. This calls Init() which does heavy SD card I/O,
    // but running it here (separate from the audio notification chain) with the UI
    // task's natural vTaskDelay(5) yielding pattern prevents WDT timeout.
    if (deferredKitLoadIndex >= 0) {
        Serial.printf("[DeferredKit] Loading kit index %d\n", deferredKitLoadIndex);
        Drums.LoadKitByIndex(deferredKitLoadIndex);
        Drums.SetKitIndex(deferredKitLoadIndex);
    } else if (deferredKitLoadNumber > 0) {
        Serial.printf("[DeferredKit] Loading kit program %d\n", deferredKitLoadNumber);
        Drums.SetProgram(deferredKitLoadNumber);
    }
    
    // Clear pending
    deferredKitLoadIndex = -1;
    deferredKitLoadNumber = 0;
}

// ========================================
// State initialization
// ========================================
void acidbox_save_init_state() {
    acidBoxSaveLoad.currentBank = 0;
    acidBoxSaveLoad.currentSong = 0;
    acidBoxSaveLoad.currentPattern = 0;
    acidBoxSaveLoad.modified = false;

    for (int i = 0; i < 16; i++) {
        acidBoxSaveLoad.patternExistsCache[i] = false;
        acidBoxSaveLoad.bankExistsCache[i] = false;
        acidBoxSaveLoad.songExistsCache[i] = false;
    }
}

// ========================================
// Directory initialization
// ========================================
void acidbox_save_init() {
    acidbox_save_init_state();

    if (!sdCardAvailable) {
        Serial.println("⚠️  acidbox_save_init: SD not available");
        return;
    }

    // Create /ACIDBOX directory
    if (!SD_MMC.exists("/ACIDBOX")) {
        if (SD_MMC.mkdir("/ACIDBOX")) {
            Serial.println("✓ Created /ACIDBOX directory");
        } else {
            Serial.println("❌ Failed to create /ACIDBOX directory");
            return;
        }
    }

    // Create /ACIDBOX/BANKS directory
    if (!SD_MMC.exists(SAVE_BASE_PATH)) {
        if (SD_MMC.mkdir(SAVE_BASE_PATH)) {
            Serial.println("✓ Created /ACIDBOX/BANKS directory");
        } else {
            Serial.println("❌ Failed to create /ACIDBOX/BANKS directory");
            return;
        }
    }

    Serial.println("✓ AcidBox save directory structure ready");

    // Refresh caches
    refreshAcidBoxBankCache();
    refreshAcidBoxSongCache();
    refreshAcidBoxPatternCache();
}

// ========================================
// Path builders
// ========================================
void getAcidBoxBankPath(char* buffer, uint8_t bankNum) {
    snprintf(buffer, 64, "%s/BANK_%02d", SAVE_BASE_PATH, bankNum + 1);
}

void getAcidBoxSongPath(char* buffer, uint8_t bankNum, uint8_t songNum) {
    snprintf(buffer, 64, "%s/BANK_%02d/SONG_%02d", SAVE_BASE_PATH, bankNum + 1, songNum + 1);
}

void getAcidBoxPatternPath(char* buffer, uint8_t bankNum, uint8_t songNum, uint8_t patternNum) {
    snprintf(buffer, 80, "%s/BANK_%02d/SONG_%02d/pattern_%02d.dat",
             SAVE_BASE_PATH, bankNum + 1, songNum + 1, patternNum + 1);
}

// ========================================
// Existence checks
// ========================================
bool doesAcidBoxBankExist(uint8_t bankNum) {
    if (!sdCardAvailable) return false;
    char path[64];
    getAcidBoxBankPath(path, bankNum);
    return SD_MMC.exists(path);
}

bool doesAcidBoxSongExist(uint8_t bankNum, uint8_t songNum) {
    if (!sdCardAvailable) return false;
    char path[64];
    getAcidBoxSongPath(path, bankNum, songNum);
    return SD_MMC.exists(path);
}

bool doesAcidBoxPatternExist(uint8_t bankNum, uint8_t songNum, uint8_t patternNum) {
    if (!sdCardAvailable) return false;
    char path[80];
    getAcidBoxPatternPath(path, bankNum, songNum, patternNum);
    return SD_MMC.exists(path);
}

// ========================================
// Cache refresh
// ========================================
void refreshAcidBoxPatternCache() {
    if (!sdCardAvailable) return;
    for (int i = 0; i < 16; i++) {
        acidBoxSaveLoad.patternExistsCache[i] = doesAcidBoxPatternExist(
            acidBoxSaveLoad.currentBank, acidBoxSaveLoad.currentSong, i);
    }
}

void refreshAcidBoxBankCache() {
    if (!sdCardAvailable) return;
    for (int i = 0; i < 16; i++) {
        acidBoxSaveLoad.bankExistsCache[i] = doesAcidBoxBankExist(i);
    }
}

void refreshAcidBoxSongCache() {
    if (!sdCardAvailable) return;
    for (int i = 0; i < 16; i++) {
        acidBoxSaveLoad.songExistsCache[i] = doesAcidBoxSongExist(
            acidBoxSaveLoad.currentBank, i);
    }
}

// ========================================
// Helpers for drum kit number extraction
// ========================================
uint8_t getCurrentDrumKitNumber() {
    // Try to extract numeric part from the current kit folder name
    const char* name = Drums.GetCurrentKitName();
    if (name == nullptr || strlen(name) == 0 || strcmp(name, "---") == 0) {
        return 0; // default/unset
    }
    const char* p = name;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            return (uint8_t)atoi(p);
        }
        p++;
    }
    return 0; // no number found
}

// ========================================
// Core save function
// ========================================
bool saveAcidBoxPattern(uint8_t bankNum, uint8_t songNum, uint8_t patternNum) {
    if (!sdCardAvailable) {
        Serial.println("❌ saveAcidBoxPattern: SD not available");
        return false;
    }

    if (bankNum > 15 || songNum > 15 || patternNum > 15) {
        Serial.println("❌ saveAcidBoxPattern: Invalid bank/song/pattern number");
        return false;
    }

    char path[80];
    char songPath[64];
    char bankPath[64];

    // Ensure bank directory exists
    getAcidBoxBankPath(bankPath, bankNum);
    if (!SD_MMC.exists(bankPath)) {
        if (!SD_MMC.mkdir(bankPath)) {
            Serial.printf("❌ Failed to create bank directory: %s\n", bankPath);
            return false;
        }
    }

    // Ensure song directory exists
    getAcidBoxSongPath(songPath, bankNum, songNum);
    if (!SD_MMC.exists(songPath)) {
        if (!SD_MMC.mkdir(songPath)) {
            Serial.printf("❌ Failed to create song directory: %s\n", songPath);
            return false;
        }
    }

    // Build the pattern file
    AcidBoxPatternFile pf;
    memset(&pf, 0, sizeof(pf));
    pf.magic = ACIDBOX_SAVE_MAGIC;
    pf.version = ACIDBOX_SAVE_VERSION;

    // Copy pattern data from globalSeq
    memcpy(&pf.synth1, &globalSeq.synth1, sizeof(SynthPattern));
    memcpy(&pf.synth2, &globalSeq.synth2, sizeof(SynthPattern));
    memcpy(&pf.drum, &globalSeq.drum, sizeof(DrumPattern));
    memcpy(&pf.autoSynth1, &globalSeq.autoSynth1, sizeof(SynthAutomation));
    memcpy(&pf.autoSynth2, &globalSeq.autoSynth2, sizeof(SynthAutomation));
    memcpy(&pf.autoDrum, &globalSeq.autoDrum, sizeof(DrumAutomation));
    pf.bpm = globalSeq.bpm;
    pf.swing = globalSeq.swing;

    // Save drum kit info (version 2+)
    pf.drumKitNumber = getCurrentDrumKitNumber();
    const char* kitName = Drums.GetCurrentKitName();
    if (kitName != nullptr) {
        strncpy(pf.drumKitName, kitName, sizeof(pf.drumKitName) - 1);
        pf.drumKitName[sizeof(pf.drumKitName) - 1] = '\0';
    } else {
        pf.drumKitName[0] = '\0';
    }

    // Save scale and root note (version 3+)
    pf.scaleIndex = scaleIndex;
    pf.rootNote   = rootNote;

    // Write to file
    getAcidBoxPatternPath(path, bankNum, songNum, patternNum);
    File file = SD_MMC.open(path, FILE_WRITE);
    if (!file) {
        Serial.printf("❌ Failed to open pattern file: %s\n", path);
        return false;
    }

    size_t bytesWritten = file.write((uint8_t*)&pf, sizeof(AcidBoxPatternFile));
    file.close();

    if (bytesWritten != sizeof(AcidBoxPatternFile)) {
        Serial.printf("❌ Short write (%d/%d bytes)\n", bytesWritten, sizeof(AcidBoxPatternFile));
        return false;
    }

    Serial.printf("✓ Saved pattern B%02d/S%02d/P%02d (%d bytes, BPM=%.0f, Swing=%.0f%%, Scale=%d, Root=%d)\n",
                  bankNum + 1, songNum + 1, patternNum + 1, bytesWritten, pf.bpm, pf.swing,
                  pf.scaleIndex, pf.rootNote);

    acidBoxSaveLoad.modified = false;
    return true;
}

// ========================================
// Core load function
// ========================================
bool loadAcidBoxPattern(uint8_t bankNum, uint8_t songNum, uint8_t patternNum) {
    if (!sdCardAvailable) {
        Serial.println("❌ loadAcidBoxPattern: SD not available");
        return false;
    }

    if (bankNum > 15 || songNum > 15 || patternNum > 15) {
        Serial.println("❌ loadAcidBoxPattern: Invalid bank/song/pattern number");
        return false;
    }

    char path[80];
    getAcidBoxPatternPath(path, bankNum, songNum, patternNum);

    if (!SD_MMC.exists(path)) {
        Serial.printf("⚠️ Pattern file not found: %s\n", path);
        return false;
    }

    File file = SD_MMC.open(path, FILE_READ);
    if (!file) {
        Serial.printf("❌ Failed to open pattern file: %s\n", path);
        return false;
    }

    AcidBoxPatternFile pf;
    memset(&pf, 0, sizeof(pf));
    size_t bytesRead = file.read((uint8_t*)&pf, sizeof(AcidBoxPatternFile));
    file.close();
    pf.drumKitName[sizeof(pf.drumKitName) - 1] = '\0';

    if (bytesRead < sizeof(AcidBoxPatternFile)) {
        Serial.printf("⚠️ Short read (%d/%d bytes)\n", bytesRead, sizeof(AcidBoxPatternFile));
        if (bytesRead < 8) return false; // Not even magic/version
    }

    // Validate magic
    if (pf.magic != ACIDBOX_SAVE_MAGIC) {
        Serial.printf("❌ Invalid magic: 0x%08X (expected 0x%08X)\n", pf.magic, ACIDBOX_SAVE_MAGIC);
        return false;
    }

    // Copy pattern data into globalSeq
    memcpy(&globalSeq.synth1, &pf.synth1, sizeof(SynthPattern));
    memcpy(&globalSeq.synth2, &pf.synth2, sizeof(SynthPattern));
    memcpy(&globalSeq.drum, &pf.drum, sizeof(DrumPattern));
    memcpy(&globalSeq.autoSynth1, &pf.autoSynth1, sizeof(SynthAutomation));
    memcpy(&globalSeq.autoSynth2, &pf.autoSynth2, sizeof(SynthAutomation));
    memcpy(&globalSeq.autoDrum, &pf.autoDrum, sizeof(DrumAutomation));
    globalSeq.bpm = pf.bpm;
    globalSeq.swing = pf.swing;

    // Loading a pattern updates the automation data, but does not itself
    // cross a sequencer step boundary. Apply its saved synth/drum parameter
    // state now so Synth1/Synth2 filters (especially cutoff) do not retain the
    // previous pattern's live values until the next tick.
    sequencer_apply_loaded_pattern_parameters();

    // Update the global bpm variable used by the jukebox
    bpm = pf.bpm;
    Delay.SetBPM(bpm);
    midiClockBpmChanged(bpm);

    // Restore scale and root note (version 3+)
    if (pf.version >= 3) {
        setScale(pf.scaleIndex);     // restores scalePointer, scaleSize, scaleName
        rootNote = pf.rootNote;
        syncSequencerScale();        // rebuilds sequencer's currentScale bitmask
        Serial.printf("  → Restored scale=%d, root=%d\n", pf.scaleIndex, pf.rootNote);
    }

    Serial.printf("✓ Loaded pattern B%02d/S%02d/P%02d (BPM=%.0f, Swing=%.0f%%, Scale=%d, Root=%d)\n",
                  bankNum + 1, songNum + 1, patternNum + 1, pf.bpm, pf.swing,
                  pf.scaleIndex, pf.rootNote);

    acidBoxSaveLoad.currentBank = bankNum;
    acidBoxSaveLoad.currentSong = songNum;
    acidBoxSaveLoad.currentPattern = patternNum;
    acidBoxSaveLoad.modified = false;

    // Deferred drum kit load (version 2+ only)
    // Instead of calling LoadKitByIndex()/SetProgram() here (which triggers a lengthy
    // Init() that can starve the audio notification chain and trigger WDT reset),
    // we store the desired kit and let the UI task apply it on the next cycle.
    if (pf.version >= 2 && pf.drumKitName[0] != '\0') {
        // Check if the pattern's kit already matches the currently loaded kit — skip if so
        const char* currentKitName = Drums.GetCurrentKitName();
        bool alreadyLoaded = (currentKitName != nullptr && strcmp(currentKitName, pf.drumKitName) == 0);
        
        if (!alreadyLoaded) {
            // Try to find the kit by name first
            int foundKitIndex = -1;
            int kitCount = Drums.GetKitCount();
            if (kitCount > 0) {
                for (int i = 0; i < kitCount; i++) {
                    const char* candidate = Drums.GetKitName(i);
                    if (candidate != nullptr && strcmp(candidate, pf.drumKitName) == 0) {
                        foundKitIndex = i;
                        break;
                    }
                }
            }
            // Store what we found (or -1 for number-based fallback)
            if (foundKitIndex >= 0) {
                // Schedule a deferred kit load — will be picked up by UI task
                deferredKitLoadIndex = foundKitIndex;
                deferredKitLoadNumber = 0; // use index-based load
                Serial.printf("  → Deferred drum kit load: '%s' (index %d)\n", pf.drumKitName, foundKitIndex);
            } else if (pf.drumKitNumber > 0) {
                deferredKitLoadIndex = -1;
                deferredKitLoadNumber = pf.drumKitNumber;
                Serial.printf("  → Deferred drum kit load: program %d (name '%s' not found)\n",
                              pf.drumKitNumber, pf.drumKitName);
            } else {
                Serial.printf("  → Drum kit '%s' not found; keeping current kit\n", pf.drumKitName);
            }
        } else {
            Serial.printf("  → Drum kit '%s' already loaded; skipping\n", pf.drumKitName);
        }
    }

    return true;
}

// ========================================
// Helper: save current pattern to a slot in the current bank/song
// ========================================
bool saveCurrentPattern(uint8_t patternNum) {
    if (patternNum > 15) return false;
    bool result = saveAcidBoxPattern(acidBoxSaveLoad.currentBank, acidBoxSaveLoad.currentSong, patternNum);
    if (result) {
        acidBoxSaveLoad.currentPattern = patternNum;
        acidBoxSaveLoad.modified = false;
        refreshAcidBoxPatternCache();
    }
    return result;
}

// ========================================
// Helper: load a pattern from the current bank/song
// ========================================
bool loadCurrentPattern(uint8_t patternNum) {
    if (patternNum > 15) return false;
    bool result = loadAcidBoxPattern(acidBoxSaveLoad.currentBank, acidBoxSaveLoad.currentSong, patternNum);
    if (result) {
        refreshAcidBoxPatternCache();
    }
    return result;
}