#pragma once

#include <Arduino.h>
#include "config.h"

// A small, monophonic, live-triggered procedural FX voice.  The first eight
// presets are bar-synchronised: they remain in their sustain phase until the
// sequencer reaches step 1 (index 0).  The second bank is ordinary one-shots.
class NoiseFxVoice {
public:
    enum Oscillator : uint8_t {
        OSC_NONE = 0,
        OSC_SINE,
        OSC_TRIANGLE,
        OSC_SQUARE
    };

    enum LfoWave : uint8_t {
        LFO_OFF = 0,
        LFO_SINE,
        LFO_TRIANGLE,
        LFO_SAMPLE_HOLD
    };

    enum LfoTarget : uint8_t {
        LFO_TARGET_NONE = 0,
        LFO_TARGET_FILTER,
        LFO_TARGET_AMPLITUDE,
        LFO_TARGET_PITCH,
        LFO_TARGET_PAN,
        LFO_TARGET_MIX
    };

    enum FilterColor : uint8_t {
        COLOR_LOW = 0,
        COLOR_BAND,
        COLOR_HIGH
    };

    struct Preset {
        const char* name;
        float startCutoff;
        float endCutoff;
        float attackMs;
        float holdMs;
        float releaseMs;
        float gain;
        float width;
        float pan;
        FilterColor color;
        bool releaseAtStepOne;
        Oscillator oscillator;
        float oscStartHz;
        float oscEndHz;
        float oscGain;
        LfoWave lfoWave;
        float lfoBeats;
        float lfoDepth;
        LfoTarget lfoTarget;
        bool lfoResetAtStepOne;
    };

    NoiseFxVoice() = default;

    void Init();
    void Trigger(uint8_t presetIndex);
    void Stop();
    void Process(float* left, float* right);

    // Called by the sequencer at a 16th-note boundary.  This is only a timing
    // notification; the FX voice has no pattern data or saved state.
    void OnSequencerStep(uint8_t stepIndex);

    void SetVolume(float value);
    void SetDelaySend(float value);
    void SetReverbSend(float value);

    float GetVolume() const { return _volume; }
    float GetDelaySend() const { return _sendDelay; }
    float GetReverbSend() const { return _sendReverb; }
    uint8_t GetPresetIndex() const { return _presetIndex; }
    const char* GetPresetName() const;
    bool IsActive() const { return _active; }

    float _sendDelay = 0.35f;
    float _sendReverb = 0.55f;

private:
    enum Stage : uint8_t { IDLE, ATTACK, SUSTAIN, RELEASE };

    static const uint8_t PRESET_COUNT = 16;
    static const Preset _presets[PRESET_COUNT];

    static uint32_t NextRandom(uint32_t& state);
    static float NoiseSample(uint32_t& state);
    static float FilterAlpha(float cutoff);
    static float OscillatorSample(Oscillator oscillator, float phase);
    static float LfoSample(LfoWave wave, float phase, float sampleHold);
    static float SineTableSample(float phase);

    void BeginRelease();
    float ProcessChannel(float input, float& fastState, float& slowState,
                         float fastAlpha, float slowAlpha, FilterColor color) const;

    const Preset* _preset = nullptr;
    uint8_t _presetIndex = 0;
    Stage _stage = IDLE;
    bool _active = false;
    uint32_t _stageSamples = 0;
    uint32_t _ageSamples = 0;
    uint32_t _sweepSamples = 1;
    uint32_t _fallbackSamples = 1;
    float _oscPhase = 0.0f;
    float _lfoPhase = 0.0f;
    float _lfoControl = 0.0f;
    float _lfoBpm = 130.0f;
    float _lfoSampleHold = 0.0f;
    volatile bool _lfoResetRequested = false;
    uint8_t _filterUpdateCounter = 0;
    float _fastAlpha = 0.001f;
    float _slowAlpha = 0.001f;
    float _volume = 0.8f;
    float _env = 0.0f;
    float _attackStep = 1.0f;
    float _releaseStep = 1.0f;
    float _fastL = 0.0f;
    float _slowL = 0.0f;
    float _fastR = 0.0f;
    float _slowR = 0.0f;
    uint32_t _rngL = 0x13579BDFu;
    uint32_t _rngR = 0x2468ACE1u;
};
