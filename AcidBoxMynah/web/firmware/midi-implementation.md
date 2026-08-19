# AcidBoxMynah MIDI Implementation

This document describes the MIDI implementation currently present in the
`AcidBoxMynah` firmware. It is intended as a reference for MIDI controllers,
sequencers, DAWs, and other AcidBox/Mynah devices.

## Connection and MIDI configuration

- **MIDI transport:** standard hardware MIDI UART
- **Baud rate:** `31250`
- **Format:** `8N1`
- **MIDI input:** GPIO 36 (`MIDIRX_PIN`)
- **MIDI output:** GPIO 35 (`MIDITX_PIN`)
- **Receive mode:** all MIDI channels (`MIDI_CHANNEL_OMNI`)
- **MIDI Thru:** disabled in the MIDI library; explicit clock-thru and pattern
  synchronization transmission are handled by the firmware
- **Parser:** FortySevenEffects MIDI Library, serviced while bytes are present
  in the UART buffer so real-time messages are not stranded

MIDI input is serviced from the regular firmware checks. Incoming note, CC,
pitch-bend, program-change, and transport callbacks are handled immediately.
MIDI-clock step dispatch is deferred until the main service loop when an audio
offset or swing delay is configured.

## MIDI channel summary

MIDI channels in this table use the normal 1-based MIDI channel numbering.

| Channel | Function | Note input | Note output | CC / other input |
|---:|---|---|---|---|
| 1 | Synth 1 | Monophonic synth notes | Synth 1 sequencer/jukebox notes | Synth 1 CCs, pitch bend, global reset commands |
| 2 | Synth 2 | Monophonic synth notes | Synth 2 sequencer/jukebox notes | Synth 2 CCs, pitch bend |
| 3 | Sweep | Notes 1–16 trigger presets A1–B8 | None | No Sweep CC mapping |
| 10 | Drums | Sample/instrument notes | Drum sequencer/jukebox notes | Drum CCs, program change |
| 16 | Pattern synchronization | Bank/song/pattern selection protocol when configured as follower | Bank/song/pattern messages when configured as leader; follower echoes received messages | Not a musical voice channel |

Other channels are ignored by the musical voice handlers. Global CCs are
accepted on any channel, subject to the reset-command restriction described
below.

## Note messages

### Synth 1 and Synth 2

Note On and Note Off messages on channels 1 and 2 are routed to the respective
303-style monophonic synth voice.

- External controllers should send an explicit Note Off. The current callback
  does not explicitly convert a velocity-zero Note On into Note Off before it
  reaches the synth voice.
- Velocity `>= 80` is treated as accented.
- Each synth has an eight-entry newest-note-priority stack. Releasing the
  current note can return to a previously held note with slide behavior.
- Synth Note Off releases the envelope when no held notes remain.

### Drums

Note On and Note Off messages on channel 10 are routed to the `Sampler`.
The note selects a sample/instrument according to the currently loaded drum
kit, and velocity controls the triggered sample level.

The sampler's current Note Off implementation does not stop an already
playing sample; samples continue according to their configured playback and
decay behavior. A velocity-zero Note On is likewise not explicitly converted
to Note Off by the AcidBox callback.

### Sweep presets

Sweep is a live-only procedural FX voice. It is trigger-based rather than a
held-note instrument.

| MIDI note | Preset bank/button | Internal preset index |
|---:|---|---:|
| 1 | A1 | 0 |
| 2 | A2 | 1 |
| 3 | A3 | 2 |
| 4 | A4 | 3 |
| 5 | A5 | 4 |
| 6 | A6 | 5 |
| 7 | A7 | 6 |
| 8 | A8 | 7 |
| 9 | B1 | 8 |
| 10 | B2 | 9 |
| 11 | B3 | 10 |
| 12 | B4 | 11 |
| 13 | B5 | 12 |
| 14 | B6 | 13 |
| 15 | B7 | 14 |
| 16 | B8 | 15 |

Any nonzero velocity triggers the selected preset. Velocity does not currently
change Sweep level or timbre. Velocity-zero Note On messages and all Note Off
messages on channel 3 are ignored. The first eight presets are synchronized
to the next sequencer step boundary for their release behavior; the second
bank is free-running.

### Channel 16 pattern synchronization notes

Pattern synchronization is enabled through the Pattern Sync setting. When the
device is a **follower**, channel-16 Note On messages are echoed immediately
as Note On/Note Off pairs for downstream devices, then interpreted locally.
The resulting pattern load is performed later from the UI/core service so SD
card access does not occur inside the MIDI callback.

| Note range | Meaning | Value calculation |
|---:|---|---|
| 1–16 | Bank 1–16 | `bank = note - 1` |
| 21–36 | Song 1–16 | `song = note - 21` |
| 41–56 | Pattern 1–16 | `pattern = note - 41` |

The pattern message sequence is bank, song, then pattern. A follower loads the
requested bank/song/pattern only when the referenced pattern exists. A leader
sends the three messages as Note On/Note Off pairs whenever the local pattern,
song, or bank selection changes.

## Control Change messages

CC values are MIDI values from `0` to `127`. Unless otherwise noted, the
firmware normalizes the value by dividing by `127`.

### Global CCs

Global CCs are recognized on any MIDI channel.

| CC | Name | Behavior |
|---:|---|---|
| 84 | Delay time | Sets the global delay length from 0.0 to 1.0 normalized |
| 85 | Delay feedback | Sets global delay feedback |
| 86 | Delay level | Sets global delay level |
| 87 | Reverb time | Sets global reverb time; available when PSRAM/reverb is enabled |
| 88 | Reverb level | Sets global reverb level; available when PSRAM/reverb is enabled |
| 93 | Compressor ratio | Maps to approximately `3.0` through `42.0` |
| 120 | All sound off | Uses the reset-command path described below |
| 121 | Reset all controllers | Uses the reset-command path described below |
| 123 | All notes off | Uses the reset-command path described below |

CCs 120, 121, and 123 are acted on only when received on channel 1 and at
least one second has elapsed since the previous reset. In the current
firmware, the reset path stops the Jukebox when enabled and calls
`allNotesOff()` for Synth 1 and Synth 2. It does not independently stop the
Sweep voice or sampler; transport stop/sequencer stop does stop Sweep.

### Synth CCs — channels 1 and 2

The following mappings are available on both synth channels.

| CC | Parameter | Behavior |
|---:|---|---|
| 5 | Portamento time | Sets slide time in milliseconds using the raw CC value |
| 7 | Volume | Synth voice volume |
| 10 | Pan | Stereo pan |
| 65 | Portamento | Enabled when value is `>= 64` |
| 70 | Waveform | Blends the available waveform tables |
| 71 | Resonance | Filter resonance |
| 72 | Decay | Maps to filter and amp decay times |
| 73 | Attack | Maps to filter and amp attack times |
| 74 | Cutoff | Filter cutoff |
| 75 | Env mod level | Filter-envelope modulation amount |
| 76 | Accent level | Accent amount |
| 91 | Reverb send | Synth reverb send |
| 92 | Delay send | Synth delay send |
| 94 | Distortion | Distortion amount |
| 95 | Overdrive | Overdrive amount |
| 104 | Tuning | Selects a tuning value from the firmware tuning table |

The firmware also defines **CC 128** as the saturator control. CC 128 is not a
valid standard 7-bit MIDI CC number and therefore cannot arrive as a normal
MIDI Control Change message. The internal parameter/editing code can still
refer to this constant.

### Drum CCs — channel 10

| CC | Parameter | Behavior |
|---:|---|---|
| 7 | Volume | Overall sampler volume |
| 8 | Note pan | Pan for the currently selected sample/note |
| 21 | Bass drum tone | Selects BD and changes its pitch/tone |
| 23 | Bass drum decay | Selects BD and changes its decay |
| 24 | Bass drum level | Selects BD and changes its level |
| 25 | Snare drum tone | Selects SD and changes its pitch/tone |
| 26 | Snare snap | Selects SD and changes its decay/snap |
| 29 | Snare drum level | Selects SD and changes its level |
| 61 | Closed-hat tune | Selects CH and changes its pitch |
| 63 | Closed-hat level | Selects CH and changes its level |
| 72 | Note decay | Changes the currently selected sample decay |
| 73 | Note attack/offset | Changes the currently selected sample start offset |
| 74 | Filter cutoff | Sampler filter cutoff |
| 71 | Filter resonance | Sampler filter resonance |
| 80 | Open-hat tune | Selects OH and changes its pitch |
| 81 | Open-hat decay | Selects OH and changes its decay |
| 82 | Open-hat level | Selects OH and changes its level |
| 89 | Pitch | Currently selected sample pitch |
| 90 | Note select | Selects the sample/instrument to edit |
| 91 | Reverb send | Sampler reverb send |
| 92 | Delay send | Sampler delay send |
| 94 | Distortion | Sampler bit-crusher amount; zero resets it to clean |

CC 10 is defined in the MIDI configuration as drum pan, but the current
sampler CC parser does not implement a handler for it. Per-note pan is handled
by CC 8.

## Program Change

Program Change is implemented on channel 10 only:

- Program number selects the drum kit.
- The sampler reloads the selected kit through `Sampler::SetProgram()`.
- Kit numbering follows the firmware's available kit folders and embedded
  fallback content; the default configured kit is `DEFAULT_DRUMKIT`.

Program Change on other channels is ignored.

## Drum-kit sample slots

The current firmware exposes 16 drum lanes and loads a kit from:

```text
/ACIDBOX/KITS/<program-number>/
```

When a kit contains files whose names begin with `001` through `016`, those
numeric prefixes define the slot directly. For example, `001_BD.wav` loads
into slot 0 and `016_TIMB.wav` loads into slot 15. Missing numbered files are
filled from the embedded fallback samples. The original ten fallback entries
are preserved; slots 10 and 11 repeat the first two fallback entries, and
slots 12 through 15 repeat fallback slots 0 through 3.

If a kit has no `001`–`016` prefixes, the legacy directory-order loading path
is used for compatibility. The first 16 files become slots 0 through 15 and
any missing slots use the fallback samples.

Each numbered sample slot maps directly to the same numbered drum lane: 001=BD, 002=SD, 003=CH, 004=OH, 005=CLAP, 006=LT, 007=MT, 008=HT, 009=CR, 010=RIM, 011=MAR, 012=CLAV, 013=COW, 014=CY, 015=CONG, and 016=TIMB. The sequencer and jukebox use these direct offsets.

The sample cache remains 1,572,864 bytes (1.5 MiB) of PSRAM and stores the
uncompressed PCM payloads. All 16 slot payloads, including fallback samples,
must fit in that shared cache.

Saved pattern files retain their existing binary layout. Files written by
save versions 1–3 keep the historical CLAV-as-CLAP mapping; newly written
version-4 files use CLAV for slot 10 so all 16 slots are addressable.

## Pitch Bend

Pitch Bend is implemented on synth channels 1 and 2.

- The normal MIDI bend range is interpreted as approximately **±12
  semitones**.
- The bend is converted to a playback step multiplier and immediately applied
  to the current synth target.
- Drum pitch bend is currently accepted by the callback path but has no
  effective sampler implementation.
- Sweep pitch bend is ignored.

## MIDI clock and transport

The firmware supports internal-clock operation, MIDI-clock following, optional
clock output, and MIDI-clock thru while following.

### Clock source

The clock source can be:

- **Internal:** the AcidBox sequencer uses its local BPM/timing engine.
- **MIDI:** incoming MIDI Clock (`0xF8`) drives the sequencer timing.

MIDI Clock uses the standard **24 pulses per quarter note**. AcidBox advances
one 16th-note step every six clock pulses. In MIDI-clock mode it measures the
incoming clock period over 24-pulse windows and updates the sequencer BPM when
the detected value is between 20 and 300 BPM.

### Transport messages

| Message | Hex | Behavior in MIDI-clock source mode |
|---|---:|---|
| MIDI Clock | `F8` | Measures timing and queues 16th-note step boundaries |
| MIDI Start | `FA` | Enables sync, starts playback, resets the playhead before the first step |
| MIDI Continue | `FB` | Enables sync and resumes playback |
| MIDI Stop | `FC` | Disables sync and stops the sequencer/active voices |

Start, Continue, and Stop are ignored as transport commands while the clock
source is Internal. MIDI Clock is also ignored unless MIDI clock source and
sync are active.

### Swing and slave offset

- AcidBox swing is applied to queued odd 16th-note boundaries while following
  MIDI Clock.
- The configurable slave audio offset is `0..20 ms`.
- Clock counting and tempo measurement remain tied to the incoming UART clock;
  the offset delays only local voice/step dispatch.
- A pending-step queue is used to catch up if the regular service loop is
  briefly busy.

### Clock output

When clock output is enabled while using the Internal clock, the firmware:

1. Sends MIDI Start when transport begins.
2. Sends MIDI Clock at an absolute-deadline 24 PPQN rate.
3. Sends MIDI Stop when transport stops or clock output is disabled.

When clock output is enabled while following MIDI Clock, incoming Clock bytes
are echoed to the MIDI output as clock-thru. The output timer is not used in
follower mode.

Clock source, clock output, slave offset, and pattern-sync role are stored in
ESP32 `Preferences` under the `acidbox` namespace and survive reboot.

## MIDI output

The firmware's sequencer and Jukebox paths use the MIDI helper functions to
send voice notes using the same channel layout as MIDI input:

- Synth 1 output: channel 1
- Synth 2 output: channel 2
- Drums output: channel 10

The output helpers send Note On and Note Off messages through the hardware MIDI
UART. Sweep is intentionally local-only and does not generate MIDI notes.

Pattern-sync leader output uses channel 16 and sends each bank/song/pattern
selection as an immediate Note On followed by Note Off.

## Current limitations and implementation notes

- Sweep has no MIDI CC or program-change interface; its 16 presets are selected
  by channel-3 notes only.
- Sweep velocity is currently a gate, not a level or timbre control.
- Sampler Note Off and sampler Pitch Bend are currently no-ops.
- Drum CC 10 is defined but not handled by the sampler parser.
- The saturator definition uses CC 128, outside the standard 0–127 MIDI CC
  range.
- MIDI Thru is not a general byte-for-byte MIDI Thru. Only configured clock
  thru and channel-16 pattern-sync forwarding are transmitted.

## Source references

The primary implementation is in:

- `src/midi_handler.cpp` — UART setup, parser callbacks, voice routing, clock,
  transport, and pattern synchronization
- `src/midi_config.h` — CC assignments
- `src/config.h` — MIDI pins, voice channels, and MIDI constants
- `src/synthvoice.cpp` — synth Note On/Off, CC, and pitch-bend behavior
- `src/sampler.cpp` — drum Note On/Off, CC, and program-change behavior
- `src/noise_fx_voice.cpp` — Sweep preset definitions and trigger behavior
- `src/sequencer.cpp` — transport stop, MIDI output, and clock-driven steps