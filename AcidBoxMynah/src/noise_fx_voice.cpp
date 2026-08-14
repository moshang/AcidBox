#include "noise_fx_voice.h"

#include <math.h>

extern float sin_tbl[TABLE_SIZE + 1];
extern float bpm;

// Presets 0-7 (A1-A8) wait for the next pattern step 1 before entering their
// release. Presets 8-15 (B1-B8) are free-running one-shots.
const NoiseFxVoice::Preset NoiseFxVoice::_presets[PRESET_COUNT] = {
    // name, noise sweep, envelope, gain/space, filter, sync, oscillator, LFO
    {"RISE",      180.0f,  8500.0f,  650.0f, 4000.0f, 420.0f, 0.56f, 0.85f,  0.00f, COLOR_BAND, true,  OSC_SINE,     110.0f,  880.0f, 0.30f, LFO_OFF,          0.0f, 0.00f, LFO_TARGET_NONE,      false},
    {"FALL",    10000.0f,   220.0f,  700.0f, 4000.0f, 500.0f, 0.54f, 0.80f,  0.00f, COLOR_BAND, true,  OSC_SINE,     880.0f,   55.0f, 0.34f, LFO_OFF,          0.0f, 0.00f, LFO_TARGET_NONE,      false},
    {"WIND UP",    90.0f, 12000.0f, 1100.0f, 4000.0f, 650.0f, 0.48f, 1.00f, -0.15f, COLOR_LOW,  true,  OSC_TRIANGLE,  70.0f,  700.0f, 0.26f, LFO_TRIANGLE,    0.5f, 0.40f, LFO_TARGET_FILTER,     false},
    {"WIND DN", 12000.0f,    90.0f, 1200.0f, 4000.0f, 700.0f, 0.46f, 1.00f,  0.15f, COLOR_LOW,  true,  OSC_TRIANGLE, 700.0f,   55.0f, 0.26f, LFO_TRIANGLE,    0.5f, 0.40f, LFO_TARGET_FILTER,     false},
    {"AIR RISE",   450.0f,  5500.0f,  450.0f, 4000.0f, 300.0f, 0.50f, 0.95f, -0.25f, COLOR_HIGH, true,  OSC_SINE,     500.0f, 3200.0f, 0.20f, LFO_SINE,        0.25f, 0.25f, LFO_TARGET_MIX,        false},
    {"AIR FALL",  6500.0f,   250.0f,  500.0f, 4000.0f, 340.0f, 0.50f, 0.95f,  0.25f, COLOR_HIGH, true,  OSC_SINE,    3200.0f,  220.0f, 0.20f, LFO_SINE,        0.25f, 0.25f, LFO_TARGET_MIX,        false},
    {"SWELL",      120.0f,  2400.0f, 1800.0f, 4000.0f, 900.0f, 0.58f, 0.70f,  0.00f, COLOR_LOW,  true,  OSC_SINE,      55.0f,  110.0f, 0.38f, LFO_SINE,        4.0f, 0.35f, LFO_TARGET_AMPLITUDE,  true },
    {"DISSOLVE",  7000.0f,   300.0f, 1700.0f, 4000.0f, 950.0f, 0.54f, 0.75f,  0.00f, COLOR_BAND, true,  OSC_TRIANGLE,1600.0f,  120.0f, 0.25f, LFO_SINE,        0.5f, 0.50f, LFO_TARGET_FILTER,     true },
    {"BURST",      700.0f,  9000.0f,    3.0f,   90.0f, 120.0f, 0.64f, 0.55f, -0.30f, COLOR_HIGH, false, OSC_SQUARE,   180.0f, 1800.0f, 0.28f, LFO_OFF,          0.0f, 0.00f, LFO_TARGET_NONE,      false},
    {"WHOOSH",     180.0f,  7500.0f,  120.0f,  650.0f, 420.0f, 0.56f, 0.90f,  0.20f, COLOR_BAND, false, OSC_SINE,       90.0f, 1400.0f, 0.24f, LFO_SINE,        2.0f, 0.45f, LFO_TARGET_PAN,        false},
    {"DROP",     10000.0f,   180.0f,  100.0f,  700.0f, 350.0f, 0.58f, 0.90f, -0.20f, COLOR_LOW,  false, OSC_SINE,      120.0f,   35.0f, 0.52f, LFO_OFF,          0.0f, 0.00f, LFO_TARGET_NONE,      false},
    {"HISS",      5000.0f,  7000.0f,    2.0f,  900.0f, 180.0f, 0.42f, 1.00f,  0.00f, COLOR_HIGH, false, OSC_NONE,        0.0f,    0.0f, 0.00f, LFO_TRIANGLE,    0.25f, 0.45f, LFO_TARGET_AMPLITUDE, false},
    {"CRASH AIR",  250.0f, 11000.0f,   20.0f,  450.0f, 700.0f, 0.52f, 1.00f,  0.00f, COLOR_HIGH, false, OSC_TRIANGLE,  700.0f, 2200.0f, 0.22f, LFO_SAMPLE_HOLD, 0.25f, 0.35f, LFO_TARGET_PITCH,     false},
    {"TENSION",    300.0f,  3200.0f,  900.0f,  850.0f, 500.0f, 0.50f, 0.80f, -0.35f, COLOR_BAND, false, OSC_SINE,      120.0f,  660.0f, 0.26f, LFO_TRIANGLE,    0.25f, 0.45f, LFO_TARGET_FILTER,     false},
    {"REVERSE",   9000.0f,   300.0f,  850.0f,  850.0f, 500.0f, 0.50f, 0.80f,  0.35f, COLOR_BAND, false, OSC_SINE,      900.0f,  100.0f, 0.28f, LFO_SINE,        1.0f, 0.55f, LFO_TARGET_PITCH,      false},
    {"NOISE HIT",  900.0f,  4500.0f,    5.0f,  150.0f, 220.0f, 0.62f, 0.65f,  0.00f, COLOR_BAND, false, OSC_SQUARE,    220.0f,  440.0f, 0.34f, LFO_SAMPLE_HOLD, 0.125f,0.50f, LFO_TARGET_AMPLITUDE, false}
};

void NoiseFxVoice::Init() {
    _preset = &_presets[0];
    _presetIndex = 0;
    _stage = IDLE;
    _active = false;
    _volume = 0.8f;
    _sendDelay = 0.35f;
    _sendReverb = 0.55f;
    _fastL = _slowL = _fastR = _slowR = 0.0f;
    _oscPhase = 0.0f;
    _lfoPhase = 0.0f;
    _lfoControl = 0.0f;
    _lfoBpm = fmaxf(1.0f, bpm);
    _lfoSampleHold = 0.0f;
    _lfoResetRequested = false;
    _filterUpdateCounter = 0;
    _fastAlpha = _slowAlpha = 0.001f;
}

uint32_t NoiseFxVoice::NextRandom(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float NoiseFxVoice::NoiseSample(uint32_t& state) {
    return ((float)((NextRandom(state) >> 8) & 0x00FFFFFFu) * (1.0f / 8388608.0f)) - 1.0f;
}

float NoiseFxVoice::FilterAlpha(float cutoff) {
    float alpha = (TWOPI * cutoff) * DIV_SAMPLE_RATE;
    if (alpha < 0.001f) alpha = 0.001f;
    if (alpha > 0.92f) alpha = 0.92f;
    return alpha;
}

float NoiseFxVoice::OscillatorSample(Oscillator oscillator, float phase) {
    // The oscillator phase is always non-negative and advances by less than
    // two cycles per sample. Avoid floorf() in this audio-rate helper.
    if (phase >= 1.0f) phase -= (float)(uint32_t)phase;
    switch (oscillator) {
    case OSC_SINE:
        return SineTableSample(phase);
    case OSC_TRIANGLE:
        return 1.0f - 4.0f * fabsf(phase - 0.5f);
    case OSC_SQUARE:
        return phase < 0.5f ? 1.0f : -1.0f;
    default:
        return 0.0f;
    }
}

float NoiseFxVoice::LfoSample(LfoWave wave, float phase, float sampleHold) {
    switch (wave) {
    case LFO_SINE:
        return SineTableSample(phase);
    case LFO_TRIANGLE:
        return 1.0f - 4.0f * fabsf(phase - 0.5f);
    case LFO_SAMPLE_HOLD:
        return sampleHold;
    default:
        return 0.0f;
    }
}

float NoiseFxVoice::SineTableSample(float phase) {
    if (phase >= 1.0f) phase -= (float)(uint32_t)phase;
    const float tablePosition = phase * (float)TABLE_SIZE;
    const uint16_t index = (uint16_t)tablePosition;
    const float fraction = tablePosition - (float)index;
    return sin_tbl[index] + fraction * (sin_tbl[index + 1] - sin_tbl[index]);
}

void NoiseFxVoice::Trigger(uint8_t presetIndex) {
    if (presetIndex >= PRESET_COUNT) presetIndex = PRESET_COUNT - 1;

    _presetIndex = presetIndex;
    _preset = &_presets[presetIndex];
    _stage = ATTACK;
    _active = true;
    _stageSamples = 0;
    _ageSamples = 0;
    _env = 0.0f;
    _fastL = _slowL = _fastR = _slowR = 0.0f;
    _oscPhase = 0.0f;
    _lfoPhase = 0.0f;
    _lfoControl = 0.0f;
    _lfoBpm = fmaxf(1.0f, bpm);
    _lfoSampleHold = NoiseSample(_rngL);
    _filterUpdateCounter = 0;

    _sweepSamples = (uint32_t)(fmaxf(1.0f, _preset->attackMs + _preset->holdMs)
                               * 0.001f * (float)SAMPLE_RATE);
    _fallbackSamples = (uint32_t)(fmaxf(1000.0f, _preset->holdMs)
                                  * 0.001f * (float)SAMPLE_RATE);
    _attackStep = 1.0f / fmaxf(1.0f, _preset->attackMs * 0.001f * (float)SAMPLE_RATE);
    _releaseStep = 1.0f / fmaxf(1.0f, _preset->releaseMs * 0.001f * (float)SAMPLE_RATE);

    _rngL ^= 0x9E3779B9u + (uint32_t)presetIndex * 0x10001u;
    _rngR ^= 0x7F4A7C15u + (uint32_t)presetIndex * 0x10003u;
}

void NoiseFxVoice::BeginRelease() {
    if (!_active || _stage == RELEASE || _stage == IDLE) return;
    _stage = RELEASE;
    _stageSamples = 0;
}

void NoiseFxVoice::Stop() {
    _active = false;
    _stage = IDLE;
    _env = 0.0f;
}

void NoiseFxVoice::OnSequencerStep(uint8_t stepIndex) {
    if (_active && _preset != nullptr && _preset->releaseAtStepOne && stepIndex == 0) {
        BeginRelease();
    }
    if (_active && _preset != nullptr && _preset->lfoResetAtStepOne && stepIndex == 0) {
        // The sequencer runs outside the audio task (including MIDI-clock
        // mode). Request the reset and let Process() apply it at an audio-safe
        // boundary instead of racing the phase accumulator.
        _lfoResetRequested = true;
    }
}

void NoiseFxVoice::SetVolume(float value) {
    _volume = fminf(1.0f, fmaxf(0.0f, value));
}

void NoiseFxVoice::SetDelaySend(float value) {
    _sendDelay = fminf(1.0f, fmaxf(0.0f, value));
}

void NoiseFxVoice::SetReverbSend(float value) {
    _sendReverb = fminf(1.0f, fmaxf(0.0f, value));
}

const char* NoiseFxVoice::GetPresetName() const {
    return _preset != nullptr ? _preset->name : "RISE";
}

float NoiseFxVoice::ProcessChannel(float input, float& fastState, float& slowState,
                                   float fastAlpha, float slowAlpha,
                                   FilterColor color) const {
    fastState += fastAlpha * (input - fastState);
    slowState += slowAlpha * (input - slowState);
    if (color == COLOR_HIGH) return input - fastState;
    if (color == COLOR_BAND) return fastState - slowState;
    return fastState;
}

void NoiseFxVoice::Process(float* left, float* right) {
    if (!_active || _preset == nullptr) {
        *left = 0.0f;
        *right = 0.0f;
        return;
    }

    if (_lfoResetRequested) {
        _lfoResetRequested = false;
        _lfoPhase = 0.0f;
        _lfoControl = 0.0f;
    }

    const float sweepT = fminf(1.0f, (float)_ageSamples / (float)_sweepSamples);
    float lfoAmount = 0.0f;
    if (_preset->lfoWave != LFO_OFF && _preset->lfoBeats > 0.0f) {
        const float lfoTarget = LfoSample(_preset->lfoWave, _lfoPhase, _lfoSampleHold);
        // A short audio-rate slew removes wavetable index steps and makes the
        // deliberately stepped sample-and-hold modulation musical rather than
        // a source of clicks/zipper noise.
        _lfoControl += 0.01f * (lfoTarget - _lfoControl);
        lfoAmount = _lfoControl * _preset->lfoDepth;
    }

    // The cutoff envelope and LFO are slow compared with the 44.1 kHz audio
    // stream. Recalculate the one-pole coefficients every four samples; the
    // filter state still advances on every sample, so this is inaudible but
    // removes a substantial amount of float work from the tight audio loop.
    if (_filterUpdateCounter == 0) {
        float cutoff = _preset->startCutoff +
                       (_preset->endCutoff - _preset->startCutoff) * sweepT;
        if (_preset->lfoTarget == LFO_TARGET_FILTER) cutoff *= 1.0f + lfoAmount;
        cutoff = fminf(18000.0f, fmaxf(20.0f, cutoff));
        _fastAlpha = FilterAlpha(cutoff);
        _slowAlpha = FilterAlpha(fmaxf(20.0f, cutoff * 0.22f));
    }
    _filterUpdateCounter = (_filterUpdateCounter + 1u) & 0x03u;

    const float nL = NoiseSample(_rngL);
    const float nR = NoiseSample(_rngR);
    const float filteredL = ProcessChannel(nL, _fastL, _slowL, _fastAlpha, _slowAlpha, _preset->color);
    const float filteredR = ProcessChannel(nR, _fastR, _slowR, _fastAlpha, _slowAlpha, _preset->color);

    if (_stage == ATTACK) {
        _env += _attackStep;
        if (_env >= 1.0f) {
            _env = 1.0f;
            _stage = SUSTAIN;
            _stageSamples = 0;
        }
    } else if (_stage == SUSTAIN) {
        _env = 1.0f;
        _stageSamples++;
        if (!_preset->releaseAtStepOne &&
            _stageSamples >= (uint32_t)(_preset->holdMs * 0.001f * (float)SAMPLE_RATE)) {
            BeginRelease();
        } else if (_preset->releaseAtStepOne && _stageSamples >= _fallbackSamples) {
            BeginRelease();
        }
    } else if (_stage == RELEASE) {
        _env -= _releaseStep;
        if (_env <= 0.0f) {
            Stop();
            *left = 0.0f;
            *right = 0.0f;
            return;
        }
    }

    float oscHz = _preset->oscStartHz +
                  (_preset->oscEndHz - _preset->oscStartHz) * sweepT;
    if (_preset->lfoTarget == LFO_TARGET_PITCH) oscHz *= 1.0f + lfoAmount;
    oscHz = fmaxf(0.0f, oscHz);
    const float oscL = OscillatorSample(_preset->oscillator, _oscPhase);
    const float oscR = OscillatorSample(_preset->oscillator, _oscPhase + 0.173f);
    float oscGain = _preset->oscGain;
    if (_preset->lfoTarget == LFO_TARGET_MIX) oscGain *= 1.0f + lfoAmount;
    oscGain = fminf(1.0f, fmaxf(0.0f, oscGain));

    float amplitude = _env;
    if (_preset->lfoTarget == LFO_TARGET_AMPLITUDE) {
        amplitude *= fmaxf(0.0f, 1.0f - 0.5f * _preset->lfoDepth + 0.5f * lfoAmount);
    }

    float pan = _preset->pan;
    if (_preset->lfoTarget == LFO_TARGET_PAN) pan += 0.8f * lfoAmount;
    pan = fminf(1.0f, fmaxf(-1.0f, pan));
    const float panL = pan > 0.0f ? 1.0f - pan : 1.0f;
    const float panR = pan < 0.0f ? 1.0f + pan : 1.0f;
    const float gain = amplitude * _volume * _preset->gain;

    const float noiseMix = 1.0f - 0.20f * _preset->width;
    const float crossMix = 0.20f * _preset->width;
    const float outL = (filteredL * noiseMix + filteredR * crossMix) + oscL * oscGain;
    const float outR = (filteredR * noiseMix + filteredL * crossMix) + oscR * oscGain;
    *left = outL * panL * gain;
    *right = outR * panR * gain;

    if (_preset->lfoWave != LFO_OFF && _preset->lfoBeats > 0.0f) {
        const float targetBpm = fmaxf(1.0f, bpm);
        _lfoBpm += 0.001f * (targetBpm - _lfoBpm);
        const float lfoIncrement = _lfoBpm /
            (60.0f * _preset->lfoBeats * (float)SAMPLE_RATE);
        _lfoPhase += lfoIncrement;
        if (_lfoPhase >= 1.0f) {
            _lfoPhase -= 1.0f;
            if (_preset->lfoWave == LFO_SAMPLE_HOLD) _lfoSampleHold = NoiseSample(_rngL);
        }
    }
    if (_preset->oscillator != OSC_NONE && oscHz > 0.0f) {
        _oscPhase += oscHz / (float)SAMPLE_RATE;
        if (_oscPhase >= 1.0f) _oscPhase -= 1.0f;
    }
    _ageSamples++;
}