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
// Enum order matches button positions (F1+A1=0, F1+A2=1, ... F1+B1=8, F1+B2=9, ...)
enum SynthEditMode {
  CutoffEdit,       // F1+A1 (was F1+B2)
  ResoEdit,         // F1+A2 (was F1+B1)
  WaveEdit,         // F1+A3 (was F1+A1)
  EnvModLvlEdit,    // F1+A4 (unchanged)
  PanEdit,          // F1+A5 (unchanged)
  DelayEdit,        // F1+A6 (unchanged)
  ReverbEdit,       // F1+A7 (unchanged)
  VolumeEdit,       // F1+A8 (unchanged)
  SlideEdit,        // F1+B1 (was F1+A2)
  AccentEdit,       // F1+B2 (was F1+A3)
  AttackEdit,       // F1+B3 (unchanged)
  DecayEdit,        // F1+B4 (unchanged)
  DistortionEdit,   // F1+B5 (unchanged)
  OverdriveEdit,    // F1+B6 (unchanged)
  SaturatorEdit,    // F1+B7 (unchanged)
  TuningEdit,       // F1+B8 (unchanged)
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

// Drum lane switching (F4+STEP in sequencer mode)
extern const char* drumLaneNames[16];
extern uint16_t currentDrumLane;      // bitmask for the current drum lane (e.g. 1<<0 = BD)
extern uint8_t currentDrumLaneIndex;  // 0-15 index into drumLaneNames

// ---- UI MODE ----
// SUB-mode for scale/root/global editing: entered via F8+Step combos
enum UiMode {
  UI_NORMAL,      // default: edit types work as usual
  UI_SCALE,       // F8+Step9: pot selects the scale
  UI_ROOT,        // F8+Step10: pot selects the root note
  UI_BPM,         // F8+Step8: pot sets BPM
  UI_SWING,       // F8+Step7: pot sets swing
  UI_MASTERVOL,   // F8+Step16: pot sets master volume
  UI_KITS,        // F1+Step9 (Drums mode): browse and load drum kits from SD
  UI_PATTERN_SELECT,  // F8+Step9: pattern browse/select mode
  UI_SONG_SELECT,     // F8+Step10: song browse/select mode
  UI_BANK_SELECT,         // F8+Step11: bank browse/select mode
  UI_PARTGEN,             // F5: press once for "PART" display, press again to generate current part
  UI_PATTERNGEN,          // F6: press once for "PATTERN" display, press again to generate all parts
  UI_CLEARPART,           // F8+F5: press once for "PART / CLEAR [F5]" display, press F5 again to clear current part
  UI_CLEARPATTERN,        // F8+F6: press once for "PATTERN / CLEAR [F6]" display, press F6 again to clear all pattern data
  UI_CLOCK_SRC,           // F8+V2 [1]: clock source (Internal / MIDI)
  UI_CLOCK_OUT,           // F8+V2 [2]: MIDI clock output (OFF / ON)
  UI_CLOCK_OFFSET         // F8+V2 [3]: slave audio offset (0..20 ms)
};
extern UiMode currentUiMode;

void setEditType(EditType editType);
void setSynthEditMode(SynthEditMode mode);
void setDrumEditMode(DrumEditMode mode);
void setDrumLane(uint8_t laneIndex);  // 0-15, maps to 1<<laneIndex

#endif // UI_H
