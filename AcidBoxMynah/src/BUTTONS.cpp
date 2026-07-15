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

// ---------- DOUBLE-CLICK DETECTION ----------
// Tracks the last release time for F2/F3/F4 to detect double-clicks.
static uint32_t lastF2Release = 0;
static uint32_t lastF3Release = 0;
static uint32_t lastF4Release = 0;
static const uint32_t DOUBLE_CLICK_MS = 300; // max ms between clicks to count as double-click

// Returns true if a double-click is detected for the given button.
// Must be called on button release. Updates the last-release timestamp.
static bool isDoubleClick(uint8_t buttonNum) {
    uint32_t now = millis();
    uint32_t* lastRelease;
    switch (buttonNum) {
        case BTN_F2: lastRelease = &lastF2Release; break;
        case BTN_F3: lastRelease = &lastF3Release; break;
        case BTN_F4: lastRelease = &lastF4Release; break;
        default: return false;
    }
    uint32_t elapsed = now - *lastRelease;
    *lastRelease = now;
    return (elapsed < DOUBLE_CLICK_MS);
}

// ---------- FORWARD DECLARATIONS ----------
static bool handleStandaloneDrumSteps();
static bool handleF1Combos();
static bool handleF4DrumLaneCombos();
static void handleFunctionButtons();

// ---------- PROCESS BUTTON EVENTS ----------
void processButtons()
{
	if (handleF1Combos())
	{
		// F1+F8 combo was handled — skip the normal F8 release handler this cycle
		return;
	}

	// F4+STEP drum lane switching (sequencer mode, drums edit type)
	if (handleF4DrumLaneCombos())
	{
		return;
	}

	// Standalone step press in drums mode: toggle step active/inactive
	if (handleStandaloneDrumSteps())
	{
		return;
	}

	// F8 release: toggle sequencer start/stop
	if (isButtonJustReleased(BTN_F8))
	{
		sequencer_toggle_play();
	}

	handleFunctionButtons();
}

// ==================== STANDALONE STEP BUTTON HANDLER ====================
// Handles STEP_1..STEP_16 pressed alone (no function button held) in drums mode.
// Toggles the step on/off for the current drum lane.
// Must be called before F8 release handler and after F1/F4 combo handlers
// have consumed their combo events.
static bool handleStandaloneDrumSteps()
{
	// Only in drums edit type + EDIT mode
	if (currentMode != MODE_EDIT || currentEditType != Drm)
		return false;

	// Skip if any function button (F1-F8) is held — combos take priority
	if (isButtonPressed(BTN_F1) || isButtonPressed(BTN_F2) ||
		isButtonPressed(BTN_F3) || isButtonPressed(BTN_F4) ||
		isButtonPressed(BTN_F5) || isButtonPressed(BTN_F6) ||
		isButtonPressed(BTN_F7) || isButtonPressed(BTN_F8))
		return false;

	// Check for a step button just pressed
	for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + 16; i++)
	{
		if (isButtonJustPressed(i))
		{
			uint8_t step = i - BTN_STEP_1; // 0-15
			sequencer_toggle_drum_step(step, currentDrumLane);
			refreshOLED = true;
			ledsDirty = true;
			return true;
		}
	}

	return false;
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

	// F1+STEP_1 through F1+STEP_5: set edit mode (synth or drum depending on edit type)
	const uint8_t F1_STEP_COUNT = 16;
	for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + F1_STEP_COUNT; i++)
	{
		if (isButtonJustPressed(i))
		{
			if (currentEditType < 2)
			{
				// Syn1 or Syn2: set synth edit mode
				setSynthEditMode((SynthEditMode)i);
			}
			else if (currentEditType == Drm && i <= BTN_STEP_8)
			{
				// Drums: F1+A1-A8 sets drum edit mode
				setDrumEditMode((DrumEditMode)(i - BTN_STEP_1));
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
// Double-click toggles mute for the corresponding voice.
static void handleFunctionButtons()
{
	if (isButtonJustReleased(BTN_F2))
	{
		if (isDoubleClick(BTN_F2))
		{
			muteSynth1 = !muteSynth1;
			refreshOLED = true;
		}
		else
		{
			setEditType(Syn1);
		}
	}

	if (isButtonJustReleased(BTN_F3))
	{
		if (isDoubleClick(BTN_F3))
		{
			muteSynth2 = !muteSynth2;
			refreshOLED = true;
		}
		else
		{
			setEditType(Syn2);
		}
	}

	if (isButtonJustReleased(BTN_F4))
	{
		if (isDoubleClick(BTN_F4))
		{
			muteDrums = !muteDrums;
			refreshOLED = true;
		}
		else
		{
			setEditType(Drm);
		}
	}
}

// ==================== F4 DRUM LANE SWITCHING ====================
// Handles F4+STEP_1..STEP_16: switch drum lane when in sequencer mode (MODE_EDIT)
// and drums (Drm) edit type is selected.
// Returns true if the event was consumed.
static bool handleF4DrumLaneCombos()
{
	// F4 not held: nothing to do
	if (!isButtonPressed(BTN_F4))
		return false;

	// Only works in sequencer (EDIT) mode when drums are the current edit type
	if (currentMode != MODE_EDIT || currentEditType != Drm)
		return false;

	// F4+STEP_1 through F4+STEP_16: switch drum lane
	for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + 16; i++)
	{
		if (isButtonJustPressed(i))
		{
			uint8_t laneIndex = i - BTN_STEP_1; // 0-15
			setDrumLane(laneIndex);
			return true;
		}
	}

	return false;
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