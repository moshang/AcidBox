#pragma once
#ifndef MIDI_HANDLER_H
#define MIDI_HANDLER_H

#include <Arduino.h>

// Initialization & I/O Functions
void MidiInit();
void midi_read();
void midi_send_noteon(uint8_t chan, uint8_t note, uint8_t vol);
void midi_send_noteoff(uint8_t chan, uint8_t note);

// MIDI Event Handler Callbacks
void handleNoteOn(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity);
void handleNoteOff(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity);
void handleCC(uint8_t inChannel, uint8_t cc_number, uint8_t cc_value);
void handlePitchBend(uint8_t inChannel, int number);
void handleProgramChange(uint8_t inChannel, uint8_t number);

#endif // MIDI_HANDLER_H