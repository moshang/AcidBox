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

void setEditType(EditType editType);
void setSynthEditMode(SynthEditMode mode);

#endif