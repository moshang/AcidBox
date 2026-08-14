#include <Arduino.h>
#include "general.h"
#include "UI.h"

const char* editTypeNames[4] = {
    "SYNTH1", 
    "SYNTH2", 
    "DRUMS",
    "GLOBAL"
};

const char* synthEditModeNames[16] = {
    "CUTOFF",        // F1+A1
    "RESO",          // F1+A2
    "WAVE",          // F1+A3
    "ENV MOD",       // F1+A4
    "PAN",           // F1+A5
    "DELAY",         // F1+A6
    "REVERB",        // F1+A7
    "VOLUME",        // F1+A8
    "SLIDE",         // F1+B1
    "ACCENT",        // F1+B2
    "ATTACK",        // F1+B3
    "DECAY",         // F1+B4
    "DISTORTION",    // F1+B5
    "OVERDRIVE",     // F1+B6
    "SATURATOR",     // F1+B7
    "TUNING"         // F1+B8
};

const char* drumEditModeNames[8] = {
    "CUTOFF",    // DrumCutoffEdit
    "RESO",      // DrumResoEdit
    "SN TONE",   // DrumSnToneEdit
    "BD DECAY",  // DrumBDDecayEdit
    "BD TONE",   // DrumBDToneEdit
    "DELAY",     // DrumDelayEdit
    "REVERB",    // DrumReverbEdit
    "VOLUME"     // DrumVolumeEdit
};

const uint8_t drumEditCC[8] = {
    CC_808_CUTOFF,      // DrumCutoffEdit: CC 74
    CC_808_RESO,        // DrumResoEdit: CC 71
    CC_808_SD_TONE,     // DrumSnToneEdit: CC 25
    CC_808_BD_DECAY,    // DrumBDDecayEdit: CC 23
    CC_808_BD_TONE,     // DrumBDToneEdit: CC 21
    CC_808_DELAY_SEND,  // DrumDelayEdit: CC 92
    CC_808_REVERB_SEND, // DrumReverbEdit: CC 91
    CC_808_VOLUME       // DrumVolumeEdit: CC 7
};

// Drum lane names (matching bit positions from sequencer.h)
const char* drumLaneNames[16] = {
    "BD",       // 1 << 0  (kick)
    "SD",       // 1 << 1  (snare)
    "CH",       // 1 << 2  (closed hi-hat)
    "OH",       // 1 << 3  (open hi-hat)
    "CLAP",     // 1 << 4  (clap)
    "LT",       // 1 << 5  (low tom)
    "MT",       // 1 << 6  (mid tom)
    "HT",       // 1 << 7  (high tom)
    "CR",       // 1 << 8  (crash)
    "RIM",      // 1 << 9  (rimshot)
    "MAR",      // 1 << 10 (maraca/shaker)
    "CLAV",     // 1 << 11 (claves)
    "COW",      // 1 << 12 (cowbell)
    "CY",       // 1 << 13 (cymbal)
    "CONG",     // 1 << 14 (conga)
    "TIMB"      // 1 << 15 (timbale)
};

uint16_t currentDrumLane = 1 << 0;      // default: BD
uint8_t  currentDrumLaneIndex = 0;      // default: 0 = BD

const uint8_t midiChn[4]
{
    SYNTH1_MIDI_CHAN,
    SYNTH2_MIDI_CHAN,
    DRUM_MIDI_CHAN,
    99
};

EditType currentEditType = Syn1;
SynthEditMode currentEditMode = WaveEdit;
DrumEditMode currentDrumEditMode = DrumCutoffEdit;
UiMode currentUiMode = UI_NORMAL;
AcidBoxClearConfirmType acidBoxClearConfirmType = ACIDBOX_CLEAR_NONE;
uint8_t acidBoxClearConfirmTarget = 0;
AcidBoxPatternConfirmAction acidBoxPatternConfirmAction = ACIDBOX_PATTERN_REPLACE;

void setEditType(EditType voice)
{
    // Lock the pot whenever the edit type changes
    // This prevents the parameter value from jumping when the pot
    // position doesn't match the new mode's current parameter value.
    potLock();

    currentEditType = voice;
    refreshOLED = true;
}

void setSynthEditMode(SynthEditMode mode )
{
    // Lock the pot whenever the synth edit mode changes
    potLock();

    currentEditMode = mode;
    refreshOLED = true;
}

void setDrumEditMode(DrumEditMode mode)
{
    // Lock the pot whenever the drum edit mode changes
    potLock();

    currentDrumEditMode = mode;
    refreshOLED = true;
}

void setDrumLane(uint8_t laneIndex)
{
    if (laneIndex >= 16) return;
    currentDrumLaneIndex = laneIndex;
    currentDrumLane = 1 << laneIndex;
    // Update drumViewMask in main.cpp so the neopixel display shows the right lane
    drumViewMask = currentDrumLane;
    refreshOLED = true;
}