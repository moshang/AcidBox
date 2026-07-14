#pragma once
#ifndef UI_H   
#define UI_H   

// what are we editing? (synth1, synth2, drums, global)
enum EditType {
  Syn1,
  Syn2,
  Drm,
  Global
};

extern const char* editTypeNames[4];

// Synth edit modes
enum SynthEditMode {
  WaveEdit,
  SlideEdit, // portamento - on/off on the buttons and portatime on the pot
  AccentEdit, // accent = on/off on the buttons and accent level on the pot
  EnvModLvlEdit,
  PanEdit,
  DelayEdit,
  ReverbEdit,
  VolumeEdit,
  ResoEdit,
  CutoffEdit,
  AttackEdit,
  DecayEdit,
  DistortionEdit,
  OverdriveEdit,
  SaturatorEdit,
  TuningEdit,
};

extern const char* synthEditModeNames[16];

// Drum edit modes (F1+A1-A8 when editing drums)
enum DrumEditMode {
  DrumCutoffEdit,   // F1+A1: CC 74 cutoff
  DrumResoEdit,     // F1+A2: CC 71 resonance
  DrumSnToneEdit,   // F1+A3: CC 25 snare tone
  DrumBDDecayEdit,  // F1+A4: CC 23 BD decay
  DrumBDToneEdit,   // F1+A5: CC 21 BD tone
  DrumDelayEdit,    // F1+A6: CC 92 delay send
  DrumReverbEdit,   // F1+A7: CC 91 reverb send
  DrumVolumeEdit,   // F1+A8: CC 7 volume
};

extern const char* drumEditModeNames[8];
extern const uint8_t drumEditCC[8];

extern DrumEditMode currentDrumEditMode;

void setEditType(EditType editType);
void setSynthEditMode(SynthEditMode mode);
void setDrumEditMode(DrumEditMode mode);

#endif