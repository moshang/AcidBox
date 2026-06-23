#include <Arduino.h>
#include "general.h"
#include "synthvoice.h"
#include "sampler.h"
#include "fx_delay.h"
#ifndef NO_PSRAM
#include "fx_reverb.h"
#endif
#include "compressor.h"

#if defined MIDI_VIA_SERIAL2 || defined MIDI_VIA_SERIAL
#include <MIDI.h>
#endif

#ifdef MIDI_VIA_SERIAL
  struct CustomBaudRateSettings : public MIDI_NAMESPACE::DefaultSettings {
    static const long BaudRate = 115200;
    static const bool Use1ByteParsing = false;
  };

  MIDI_NAMESPACE::SerialMIDI<MIDI_PORT_TYPE, CustomBaudRateSettings> serialMIDI(MIDI_PORT);
  MIDI_NAMESPACE::MidiInterface<MIDI_NAMESPACE::SerialMIDI<MIDI_PORT_TYPE, CustomBaudRateSettings>> MIDI(serialMIDI);
#endif

#ifdef MIDI_VIA_SERIAL2
// MIDI port on UART2, pins 16 (RX) and 17 (TX) prohibited on ESP32, as they are used for PSRAM
struct Serial2MIDISettings : public midi::DefaultSettings {
  static const long BaudRate = 31250;
  static const int8_t RxPin  = MIDIRX_PIN;
  static const int8_t TxPin  = MIDITX_PIN;
  static const bool Use1ByteParsing = false;
};
MIDI_NAMESPACE::SerialMIDI<HardwareSerial> Serial2MIDI2(Serial2);
MIDI_NAMESPACE::MidiInterface<MIDI_NAMESPACE::SerialMIDI<HardwareSerial, Serial2MIDISettings>> MIDI2((MIDI_NAMESPACE::SerialMIDI<HardwareSerial, Serial2MIDISettings>&)Serial2MIDI2);
#endif

// Forward declaration of do_midi_stop defined in AcidBanger.cpp
void do_midi_stop();

void handleNoteOn(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity);
void handleNoteOff(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity);
void handleCC(uint8_t inChannel, uint8_t cc_number, uint8_t cc_value);
void handlePitchBend(uint8_t inChannel, int number);
void handleProgramChange(uint8_t inChannel, uint8_t number);

void MidiInit() {
  
#ifdef MIDI_VIA_SERIAL2
  pinMode( MIDIRX_PIN , INPUT_PULLDOWN);
  pinMode( MIDITX_PIN , OUTPUT);
  Serial2.begin( 31250, SERIAL_8N1, MIDIRX_PIN, MIDITX_PIN ); // midi port
#endif

#ifdef MIDI_VIA_SERIAL
  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.setHandleNoteOff(handleNoteOff);
  MIDI.setHandleControlChange(handleCC);
  MIDI.setHandlePitchBend(handlePitchBend);
  MIDI.setHandleProgramChange(handleProgramChange);
  MIDI.begin(MIDI_CHANNEL_OMNI);
#endif
#ifdef MIDI_VIA_SERIAL2
  MIDI2.setHandleNoteOn(handleNoteOn);
  MIDI2.setHandleNoteOff(handleNoteOff);
  MIDI2.setHandleControlChange(handleCC);
  MIDI2.setHandlePitchBend(handlePitchBend);
  MIDI2.setHandleProgramChange(handleProgramChange);
  MIDI2.begin(MIDI_CHANNEL_OMNI);
#endif

}

void midi_read() {
#ifdef MIDI_VIA_SERIAL
  MIDI.read();
#endif
#ifdef MIDI_VIA_SERIAL2
  MIDI2.read();
#endif
}

void midi_send_noteon(uint8_t chan, uint8_t note, uint8_t vol) {
#ifdef MIDI_VIA_SERIAL
  MIDI.sendNoteOn(note, vol, chan);
#endif
#ifdef MIDI_VIA_SERIAL2
  MIDI2.sendNoteOn(note, vol, chan);
#endif
}

void midi_send_noteoff(uint8_t chan, uint8_t note) {
#ifdef MIDI_VIA_SERIAL
  MIDI.sendNoteOn(note, 0, chan);
#endif
#ifdef MIDI_VIA_SERIAL2
  MIDI2.sendNoteOn(note, 0, chan);
#endif
}

void handleNoteOn(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity) {
#ifdef DEBUG_MIDI
  DEB("MIDI note on ");
  DEBUG(inNote);
#endif
  if (inChannel == DRUM_MIDI_CHAN )         {Drums.NoteOn(inNote, inVelocity);}
  else if (inChannel == SYNTH1_MIDI_CHAN )  {Synth1.on_midi_noteON(inNote, inVelocity);}
  else if (inChannel == SYNTH2_MIDI_CHAN )  {Synth2.on_midi_noteON(inNote, inVelocity);}
}

void handleNoteOff(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity) {
  if (inChannel == DRUM_MIDI_CHAN )         {Drums.NoteOff(inNote);}
  else if (inChannel == SYNTH1_MIDI_CHAN )  {Synth1.on_midi_noteOFF(inNote, inVelocity);}
  else if (inChannel == SYNTH2_MIDI_CHAN )  {Synth2.on_midi_noteOFF(inNote, inVelocity);}

}

void handleCC(uint8_t inChannel, uint8_t cc_number, uint8_t cc_value) {
  switch (cc_number) { // global parameters yet set via ANY channel CCs
    case CC_ANY_COMPRESSOR:
      Comp.SetRatio(3.0f + cc_value * 0.307081f);
      DEBF("Set Comp Ratio %d\r\n", cc_value);
      break;
    case CC_ANY_DELAY_TIME:
      Delay.SetLength(cc_value * MIDI_NORM);
      break;
    case CC_ANY_DELAY_FB:
      Delay.SetFeedback(cc_value * MIDI_NORM);
      break;
    case CC_ANY_DELAY_LVL:
      Delay.SetLevel(cc_value * MIDI_NORM);
      break;
    case CC_ANY_RESET_CCS:
    case CC_ANY_NOTES_OFF:
    case CC_ANY_SOUND_OFF:
        if (inChannel == SYNTH1_MIDI_CHAN && millis()-last_reset>1000 ) {
#ifdef JUKEBOX
          do_midi_stop();
#endif
          Synth1.allNotesOff();
          Synth2.allNotesOff();
          last_reset = millis();
        }
      break;
#ifndef NO_PSRAM
    case CC_ANY_REVERB_TIME:
      Reverb.SetTime(cc_value * MIDI_NORM);
      break;
    case CC_ANY_REVERB_LVL:
      Reverb.SetLevel(cc_value * MIDI_NORM);
      break;
#endif
    default:
      if (inChannel == DRUM_MIDI_CHAN )         {Drums.ParseCC(cc_number, cc_value);}
      else if (inChannel == SYNTH1_MIDI_CHAN )  {Synth1.ParseCC(cc_number, cc_value);}
      else if (inChannel == SYNTH2_MIDI_CHAN )  {Synth2.ParseCC(cc_number, cc_value);}
  }
}

void handleProgramChange(uint8_t inChannel, uint8_t number) {
  if (inChannel == DRUM_MIDI_CHAN) {     Drums.SetProgram(number);  }
}

void handlePitchBend(uint8_t inChannel, int number) {
  if (inChannel == DRUM_MIDI_CHAN )         {Drums.PitchBend(number);}
  else if (inChannel == SYNTH1_MIDI_CHAN )  {Synth1.PitchBend(number);}
  else if (inChannel == SYNTH2_MIDI_CHAN )  {Synth2.PitchBend(number);}
}
