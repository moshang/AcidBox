
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
    "ENV_MOD_LVL",
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

const uint8_t midiChn[4]
{
    SYNTH1_MIDI_CHAN,
    SYNTH2_MIDI_CHAN,
    DRUM_MIDI_CHAN,
    99
};

EditType currentEditType = Syn1;
SynthEditMode currentEditMode = WaveEdit;

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


