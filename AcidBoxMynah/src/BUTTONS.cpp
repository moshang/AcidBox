#include <Arduino.h>
#include "config.h"
#include "general.h"
#include "sequencer.h"

// ---------- INITIALIZE SHIFT REGISTER ----------
void initShiftRegister()
{
	pinMode(SR_DATA_PIN, INPUT);
	pinMode(SR_CLOCK_PIN, OUTPUT);
	pinMode(SR_LATCH_PIN, OUTPUT);

	// Initial states
	digitalWrite(SR_CLOCK_PIN, LOW);
	digitalWrite(SR_LATCH_PIN, HIGH);
}

// ---------- READ ALL 24 BUTTONS ----------
uint32_t readShiftRegister()
{
	uint32_t buttonData = 0;

	// Pulse latch LOW to load button states into shift register
	digitalWrite(SR_LATCH_PIN, LOW);
	delayMicroseconds(5);
	digitalWrite(SR_LATCH_PIN, HIGH);
	delayMicroseconds(5);

	// Read 24 bits - IC3 comes out first (MSB), then IC2, then IC1 (LSB)
	for (int i = 23; i >= 0; i--)
	{
		int bit = digitalRead(SR_DATA_PIN);
		if (bit)
		{
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
bool isButtonPressed(uint8_t buttonNum)
{
	if (buttonNum >= NUM_BUTTONS)
		return false;
	return (buttonStates >> buttonNum) & 0x01;
}

// ---------- CHECK IF BUTTON WAS JUST PRESSED (RISING EDGE) ----------
bool isButtonJustPressed(uint8_t buttonNum)
{
	if (buttonNum >= NUM_BUTTONS)
		return false;
	bool currentState = (buttonStates >> buttonNum) & 0x01;
	bool lastState = (lastButtonStates >> buttonNum) & 0x01;
	return currentState && !lastState;
}

// ---------- CHECK IF BUTTON WAS JUST RELEASED (FALLING EDGE) ----------
bool isButtonJustReleased(uint8_t buttonNum)
{
	if (buttonNum >= NUM_BUTTONS)
		return false;
	bool currentState = (buttonStates >> buttonNum) & 0x01;
	bool lastState = (lastButtonStates >> buttonNum) & 0x01;
	return !currentState && lastState;
}

// ---------- UPDATE BUTTON STATES ----------
void updateButtons()
{
	static uint32_t lastReadTime = 0;
	uint32_t now = millis();

	// Simple debounce: read every 5ms
	if (now - lastReadTime < 5)
	{
		return;
	}
	lastReadTime = now;

	// Always update states for reliable edge detection
	lastButtonStates = buttonStates;
	buttonStates = readShiftRegister();

	// Update global any-step flag: bits 0..15 correspond to BTN_STEP_1..BTN_STEP_16
	anyStepButtonHeld = ((buttonStates & 0xFFFF) != 0);

	// Signal that LEDs need updating
	ledsDirty = true;
}

// ---------- FORWARD DECLARATIONS ----------
static bool handleF1Combos();
static void handleFunctionButtons();

// ---------- PROCESS BUTTON EVENTS ----------
void processButtons()
{
	if (handleF1Combos())
	{
		// F1+F8 combo was handled — skip the normal F8 release handler this cycle
		return;
	}

	// F8 release: toggle sequencer start/stop
	if (isButtonJustReleased(BTN_F8))
	{
		sequencer_toggle_play();
	}

	handleFunctionButtons();
}

// ==================== F1 COMBO HANDLER ====================
// Handles F1+STEP (set synth edit mode) and F1+F8 (JUKEBOX/EDIT toggle).
// Returns true if the event was consumed (which suppresses the subsequent
// F8 release handler for F1+F8 combos).
static bool handleF1Combos()
{
	// F1 not held: nothing to do
	if (!isButtonPressed(BTN_F1))
		return false;

	// F1+STEP_1 through F1+STEP_4: set synth edit mode (only when allowed)
	const uint8_t F1_STEP_COUNT = 16;
	for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + F1_STEP_COUNT; i++)
	{
		if (isButtonJustPressed(i))
		{
			if (currentEditType < 2)
			{
				setSynthEditMode((SynthEditMode)i);
			}
			return false; // don't block F8 release
		}
	}

	// F1+F8: toggle play mode (JUKEBOX ↔ EDIT)
	if (isButtonJustPressed(BTN_F8))
	{
		if (currentMode == MODE_JUKEBOX)
		{
			setMode(MODE_EDIT);
		}
		else
		{
			setMode(MODE_JUKEBOX);
		}
		return true; // suppress the normal F8 release handler
	}

	return false;
}

// ==================== FUNCTION BUTTON HANDLER ====================
// Handles F2/F3/F4 releases: switch to Syn1/Syn2/Drm edit type.
static void handleFunctionButtons()
{
	if (isButtonJustReleased(BTN_F2))
	{
		setEditType(Syn1);
	}

	if (isButtonJustReleased(BTN_F3))
	{
		setEditType(Syn2);
	}

	if (isButtonJustReleased(BTN_F4))
	{
		setEditType(Drm);
	}
}

// ---------- DEBUG: PRINT BUTTON STATES ----------
void printButtonStates()
{
	static uint32_t lastPrint = 0;
	if (millis() - lastPrint < 500)
		return;
	lastPrint = millis();

	Serial.print("Buttons: ");
	for (int i = NUM_BUTTONS - 1; i >= 0; i--)
	{
		Serial.print(isButtonPressed(i) ? "1" : "0");
		if (i % 4 == 0)
			Serial.print(" ");
	}
	Serial.println();
}