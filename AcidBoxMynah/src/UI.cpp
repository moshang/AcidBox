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
    "ENV MOD LVL",   // F1+A4
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

void setEditType(EditType voice)
{
    currentEditType = voice;
    refreshOLED = true;
}

void setSynthEditMode(SynthEditMode mode )
{
    currentEditMode = mode;
    refreshOLED = true;
}

void setDrumEditMode(DrumEditMode mode)
{
    currentDrumEditMode = mode;
    refreshOLED = true;
}