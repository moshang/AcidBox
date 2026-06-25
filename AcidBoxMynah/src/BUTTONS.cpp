#include <Arduino.h>
#include "config.h"
#include "general.h"

// ---------- INITIALIZE SHIFT REGISTER ----------
void initShiftRegister() {
	pinMode(SR_DATA_PIN, INPUT);
	pinMode(SR_CLOCK_PIN, OUTPUT);
	pinMode(SR_LATCH_PIN, OUTPUT);

	// Initial states
	digitalWrite(SR_CLOCK_PIN, LOW);
	digitalWrite(SR_LATCH_PIN, HIGH);
}

// ---------- READ ALL 24 BUTTONS ----------
uint32_t readShiftRegister() {
	uint32_t buttonData = 0;

	// Pulse latch LOW to load button states into shift register
	digitalWrite(SR_LATCH_PIN, LOW);
	delayMicroseconds(5);
	digitalWrite(SR_LATCH_PIN, HIGH);
	delayMicroseconds(5);

	// Read 24 bits - IC3 comes out first (MSB), then IC2, then IC1 (LSB)
	for (int i = 23; i >= 0; i--) {
		int bit = digitalRead(SR_DATA_PIN);
		if (bit) {
			buttonData |= (1UL << i);
		}
		digitalWrite(SR_CLOCK_PIN, HIGH);
		delayMicroseconds(5);
		digitalWrite(SR_CLOCK_PIN, LOW);
		delayMicroseconds(5);
	}

	// With resistor networks: unpressed = HIGH (1), pressed = LOW (0)
	// Invert so unpressed = 0, pressed = 1
	buttonData = ~buttonData;

	return buttonData & 0xFFFFFF;
}

// ---------- CHECK IF BUTTON IS CURRENTLY PRESSED ----------
bool isButtonPressed(uint8_t buttonNum) {
	if (buttonNum >= NUM_BUTTONS) return false;
	return (buttonStates >> buttonNum) & 0x01;
}

// ---------- CHECK IF BUTTON WAS JUST PRESSED (RISING EDGE) ----------
bool isButtonJustPressed(uint8_t buttonNum) {
	if (buttonNum >= NUM_BUTTONS) return false;
	bool currentState = (buttonStates >> buttonNum) & 0x01;
	bool lastState = (lastButtonStates >> buttonNum) & 0x01;
	return currentState && !lastState;
}

// ---------- CHECK IF BUTTON WAS JUST RELEASED (FALLING EDGE) ----------
bool isButtonJustReleased(uint8_t buttonNum) {
	if (buttonNum >= NUM_BUTTONS) return false;
	bool currentState = (buttonStates >> buttonNum) & 0x01;
	bool lastState = (lastButtonStates >> buttonNum) & 0x01;
	return !currentState && lastState;
}

// ---------- UPDATE BUTTON STATES ----------
void updateButtons() {
	static uint32_t lastReadTime = 0;
	uint32_t now = millis();

	// Simple debounce: read every 5ms
	if (now - lastReadTime < 5) {
		return;
	}
	lastReadTime = now;

	// Always update states for reliable edge detection
	lastButtonStates = buttonStates;
	buttonStates = readShiftRegister();

	// Update global any-step flag: bits 0..15 correspond to BTN_STEP_1..BTN_STEP_16
	anyStepButtonHeld = ( (buttonStates & 0xFFFF) != 0 );

	// Signal that LEDs need updating
	ledsDirty = true;
}

// ---------- PROCESS BUTTON EVENTS ----------
void processButtons() {
	// F8 release: toggle sequencer start/stop
	if (isButtonJustReleased(BTN_F8)) {
		midi_toggle_play();
	}
}

// ---------- DEBUG: PRINT BUTTON STATES ----------
void printButtonStates() {
	static uint32_t lastPrint = 0;
	if (millis() - lastPrint < 500) return;
	lastPrint = millis();

	Serial.print("Buttons: ");
	for (int i = NUM_BUTTONS - 1; i >= 0; i--) {
		Serial.print(isButtonPressed(i) ? "1" : "0");
		if (i % 4 == 0) Serial.print(" ");
	}
	Serial.println();
}