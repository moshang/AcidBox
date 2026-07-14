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
    "WAVE", 
    "SLIDE", 
    "ACCENT",
    "ENV MOD LVL",
    "PAN",
    "DELAY",
    "REVERB",
    "VOLUME",
    "RESO",
    "CUTOFF",
    "ATTACK",
    "DECAY",
    "DISTORTION",
    "OVERDRIVE",
    "SATURATOR",
    "TUNING"
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