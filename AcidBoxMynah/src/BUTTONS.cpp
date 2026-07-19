#include <Arduino.h>
#include "config.h"
#include "general.h"
#include "sequencer.h"
#include "SCALES.h"
#include "UI.h"

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

// ---------- NUDGE FLAGS ----------
// These flags suppress the normal edit-type switch (and sub-mode exit) when
// F2/F3 are released after being used in an F1+F2 / F1+F3 nudge combo.
static bool suppressF2Release = false;
static bool suppressF3Release = false;

// ---------- NUDGE FUNCTION ----------
// Nudges the currently active parameter by ±1.
// Called from the F1 combo handler when F1+F2 (direction=-1) or F1+F3 (direction=1) is detected.
static void nudgeParam(int8_t direction)
{
    if (currentUiMode == UI_SCALE)
    {
        // Nudge scale index
        int newScale = (int)scaleIndex + direction;
        if (newScale < 0) newScale = NUM_SCALES;
        if (newScale > NUM_SCALES) newScale = 0;
        setScale((uint8_t)newScale);
        syncSequencerScale();
        refreshOLED = true;
        ledsDirty = true;
    }
    else if (currentUiMode == UI_ROOT)
    {
        // Nudge root note
        int newRoot = (int)rootNote + direction;
        if (newRoot < 0) newRoot = 127;
        if (newRoot > 127) newRoot = 0;
        rootNote = (uint8_t)newRoot;
        syncSequencerScale();
        refreshOLED = true;
        ledsDirty = true;
    }
    else if (currentUiMode == UI_BPM)
    {
        // Nudge BPM by ±1
        float newBpm = globalSeq.bpm + (float)direction;
        if (newBpm < 20.0f) newBpm = 20.0f;
        if (newBpm > 300.0f) newBpm = 300.0f;
        globalSeq.bpm = newBpm;
        bpm = newBpm;
        refreshOLED = true;
        ledsDirty = true;
    }
    else if (currentUiMode == UI_SWING)
    {
        // Nudge swing by ±1
        float newSwing = globalSeq.swing + (float)direction;
        if (newSwing < 50.0f) newSwing = 50.0f;
        if (newSwing > 75.0f) newSwing = 75.0f;
        globalSeq.swing = newSwing;
        refreshOLED = true;
        ledsDirty = true;
    }
    else if (currentUiMode == UI_MASTERVOL)
    {
        // Nudge master volume by ±0.05
        float newVol = masterVolume + (float)direction * 0.05f;
        if (newVol < 0.0f) newVol = 0.0f;
        if (newVol > 1.0f) newVol = 1.0f;
        masterVolume = newVol;
        refreshOLED = true;
        ledsDirty = true;
    }
}

// ---------- FORWARD DECLARATIONS ----------
static bool handleStandaloneDrumSteps();
static bool handleStandaloneSynthSteps();
static bool handleF1Combos();
static bool handleF4DrumLaneCombos();
static bool handleF8ScaleRootCombos();
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

	// Standalone step press in synths mode: toggle step active/inactive
	if (handleStandaloneSynthSteps())
	{
		return;
	}

	// Standalone step press in drums mode: toggle step active/inactive
	if (handleStandaloneDrumSteps())
	{
		return;
	}

	// F8+Step combos for SCALE/ROOT mode — must come before F8 release handler
	if (handleF8ScaleRootCombos())
	{
		return; // already handled (includes suppressing F8 release when F8 active)
	}

	// F8 release: toggle sequencer start/stop
	if (isButtonJustReleased(BTN_F8))
	{
		sequencer_toggle_play();
	}

	handleFunctionButtons();
}

// Track the currently held step for synth editing (-1 = none)
static int8_t heldSynthStep = -1;

// ==================== STANDALONE SYNTH STEP HANDLER ====================
// Handles STEP_1..STEP_16 pressed alone (no function button held) in synth edit modes (Syn1 or Syn2).
// On press: sets heldSynthStep so the pot can edit pitch.
// On release: toggles the step on/off ONLY if the pot was NOT adjusted during the hold.
//   - In normal edit modes (not SlideEdit/AccentEdit): toggles step active/inactive.
//   - In SlideEdit mode: toggles slide flag (off→on+slide, on→slide, slide→off).
//   - In AccentEdit mode: toggles accent flag (off→on+accent, on→accent, accent→off).
// Must be called before standalone drum step handler and after F1/F4 combo handlers
// have consumed their combo events.
static bool handleStandaloneSynthSteps()
{
	// Only in synth edit types + EDIT mode
	if (currentMode != MODE_EDIT || (currentEditType != Syn1 && currentEditType != Syn2))
	{
		heldSynthStep = -1;
		return false;
	}

	// Skip if any function button (F1-F8) is held — combos take priority
	if (isButtonPressed(BTN_F1) || isButtonPressed(BTN_F2) ||
		isButtonPressed(BTN_F3) || isButtonPressed(BTN_F4) ||
		isButtonPressed(BTN_F5) || isButtonPressed(BTN_F6) ||
		isButtonPressed(BTN_F7) || isButtonPressed(BTN_F8))
	{
		heldSynthStep = -1;
		return false;
	}

	// Check for a step button just pressed — track it for pot editing
	for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + 16; i++)
	{
		if (isButtonJustPressed(i))
		{
			uint8_t step = i - BTN_STEP_1; // 0-15
			heldSynthStep = step;
			stepPotAdjusted = false; // reset pot adjustment flag
			refreshOLED = true;
			ledsDirty = true;
			return true;
		}
	}

	// Check for a step button just released — toggle only if pot wasn't adjusted
	for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + 16; i++)
	{
		if (isButtonJustReleased(i))
		{
			uint8_t step = i - BTN_STEP_1; // 0-15
			if (!stepPotAdjusted)
			{
				// Pot was not adjusted — toggle based on current edit mode
				if (currentEditMode == SlideEdit)
				{
					// Slide mode: toggle slide flag on the step
					sequencer_toggle_synth_slide_or_accent(step, currentEditType, true);
				}
				else if (currentEditMode == AccentEdit)
				{
					// Accent mode: toggle accent flag on the step
					sequencer_toggle_synth_slide_or_accent(step, currentEditType, false);
				}
				else
				{
					// Normal mode: toggle step on/off
					sequencer_toggle_synth_step(step, currentEditType);
				}
			}
			else
			{
				// Pot was adjusted during the step hold — lock the pot
				// so it doesn't jump to the current parameter value
				// now that the step edit context is gone.
				potLock();
			}
			heldSynthStep = -1;
			stepPotAdjusted = false;
			refreshOLED = true;
			ledsDirty = true;
			return true;
		}
	}

	return false;
}

// ==================== STANDALONE DRUM STEP HANDLER ====================
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
// Handles F1+STEP (set synth edit mode), F1+F8 (JUKEBOX/EDIT toggle),
// and F1+F2/F1+F3 (parameter nudge in SCALE/ROOT mode).
// Returns true if the event was consumed (which suppresses the subsequent
// F8 release handler for F1+F8 combos).
static bool handleF1Combos()
{
	// F1 just released: lock the pot to prevent accidental overwrite of automation
	if (isButtonJustReleased(BTN_F1))
	{
		// In EDIT mode, lock the pot so the next turn doesn't immediately overwrite
		// the automation we just recorded with F1 held.
		if (currentMode == MODE_EDIT)
		{
			potLock();
		}
	}

	// F1 not held: nothing to do
	if (!isButtonPressed(BTN_F1))
	{
		// Clear nudge suppression flags if F1 is released — but only if F2/F3
		// are also no longer held (to avoid leaking suppression beyond the combo)
		if (!isButtonPressed(BTN_F2)) suppressF2Release = false;
		if (!isButtonPressed(BTN_F3)) suppressF3Release = false;
		return false;
	}

	// ---- F1+F2: Nudge parameter down ----
	if (isButtonJustPressed(BTN_F2))
	{
		nudgeParam(-1);
		suppressF2Release = true;  // prevent F2 release from switching edit type
		ledsDirty = true;
		return false; // don't block other handlers — F2 release will be suppressed later
	}

	// ---- F1+F3: Nudge parameter up ----
	if (isButtonJustPressed(BTN_F3))
	{
		nudgeParam(1);
		suppressF3Release = true;  // prevent F3 release from switching edit type
		ledsDirty = true;
		return false; // don't block other handlers
	}

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
// Nudge suppression flags prevent edit-type switches when F2/F3 were used
// in F1+F2 / F1+F3 nudge combos.
static void handleFunctionButtons()
{
	if (isButtonJustReleased(BTN_F2))
	{
		if (suppressF2Release)
		{
			suppressF2Release = false; // consume the suppression
		}
		else if (isDoubleClick(BTN_F2))
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
		if (suppressF3Release)
		{
			suppressF3Release = false; // consume the suppression
		}
		else if (isDoubleClick(BTN_F3))
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

// ==================== F8 + STEP combos (SCALE / ROOT / BPM / SWING / MASTERVOL) ====================
// F8+Step9  (BTN_STEP_9)  enters UI_SCALE mode — pot selects the scale.
// F8+Step10 (BTN_STEP_10) enters UI_ROOT mode — pot selects the root note.
// F8+Step8  (BTN_STEP_8)  enters UI_BPM mode — pot sets BPM.
// F8+Step7  (BTN_STEP_7)  enters UI_SWING mode — pot sets swing.
// F8+Step16 (BTN_STEP_16) enters UI_MASTERVOL mode — pot sets master volume.
//
// Once entered, sub-mode persists until the user explicitly selects
// another mode via F1+Step, F2, F3, F4, or F1+F8 — the F8 release is suppressed
// so the sequencer does NOT start/stop.
//
// While in a sub-mode, pressing F8+Step again switches between sub-modes.
//
// F1+F2 / F1+F3 nudge combos are handled in handleF1Combos and do NOT exit
// the sub-mode — they just nudge the current parameter by ±1.
static bool handleF8ScaleRootCombos()
{
	// If we're in a sub-mode, stay in sub-mode (blocking normal handlers)
	// until the user explicitly selects another mode.
	if (currentUiMode != UI_NORMAL)
	{
		// --- Exit conditions: user explicitly selects another mode ---

		// F1+Step (any step): user is selecting a synth/drum edit mode → exit
		if (isButtonPressed(BTN_F1))
		{
			for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + 16; i++)
			{
				if (isButtonJustPressed(i))
				{
					currentUiMode = UI_NORMAL;
					refreshOLED = true;
					ledsDirty = true;
					return false; // let processButtons continue to handleF1Combos
				}
			}
		}

		// F1+F8: toggle JUKEBOX/EDIT → exit
		if (isButtonJustPressed(BTN_F8) && isButtonPressed(BTN_F1))
		{
			currentUiMode = UI_NORMAL;
			refreshOLED = true;
			ledsDirty = true;
			return false; // let processButtons continue to handleF1Combos
		}

		// F2/F3/F4 release (single click): switch edit type → exit
		// BUT: skip if the release is suppressed by a nudge combo
		if (isButtonJustReleased(BTN_F2) && !suppressF2Release)
		{
			currentUiMode = UI_NORMAL;
			refreshOLED = true;
			ledsDirty = true;
			return false; // let processButtons continue to handleFunctionButtons
		}
		if (isButtonJustReleased(BTN_F3) && !suppressF3Release)
		{
			currentUiMode = UI_NORMAL;
			refreshOLED = true;
			ledsDirty = true;
			return false; // let processButtons continue to handleFunctionButtons
		}
		if (isButtonJustReleased(BTN_F4))
		{
			currentUiMode = UI_NORMAL;
			refreshOLED = true;
			ledsDirty = true;
			return false; // let processButtons continue to handleFunctionButtons
		}

		// Allow switching between sub-modes while F8 is held
		if (isButtonPressed(BTN_F8))
		{
			if (isButtonJustPressed(BTN_STEP_9) && currentUiMode != UI_SCALE)
			{
				currentUiMode = UI_SCALE;
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
			if (isButtonJustPressed(BTN_STEP_10) && currentUiMode != UI_ROOT)
			{
				currentUiMode = UI_ROOT;
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
			if (isButtonJustPressed(BTN_STEP_8) && currentUiMode != UI_BPM)
			{
				currentUiMode = UI_BPM;
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
			if (isButtonJustPressed(BTN_STEP_7) && currentUiMode != UI_SWING)
			{
				currentUiMode = UI_SWING;
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
			if (isButtonJustPressed(BTN_STEP_16) && currentUiMode != UI_MASTERVOL)
			{
				currentUiMode = UI_MASTERVOL;
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
		}

		// Block all other handlers while in sub-mode
		return true;
	}

	// Only enter sub-modes when F8 is held
	if (!isButtonPressed(BTN_F8))
		return false;

	// F8+Step9: enter SCALE mode
	if (isButtonJustPressed(BTN_STEP_9))
	{
		currentUiMode = UI_SCALE;
		refreshOLED = true;
		ledsDirty = true;
		return true; // suppresses F8 release toggle
	}

	// F8+Step10: enter ROOT mode
	if (isButtonJustPressed(BTN_STEP_10))
	{
		currentUiMode = UI_ROOT;
		refreshOLED = true;
		ledsDirty = true;
		return true; // suppresses F8 release toggle
	}

	// F8+Step8: enter BPM mode
	if (isButtonJustPressed(BTN_STEP_8))
	{
		currentUiMode = UI_BPM;
		refreshOLED = true;
		ledsDirty = true;
		return true; // suppresses F8 release toggle
	}

	// F8+Step7: enter SWING mode
	if (isButtonJustPressed(BTN_STEP_7))
	{
		currentUiMode = UI_SWING;
		refreshOLED = true;
		ledsDirty = true;
		return true; // suppresses F8 release toggle
	}

	// F8+Step16: enter MASTERVOL mode
	if (isButtonJustPressed(BTN_STEP_16))
	{
		currentUiMode = UI_MASTERVOL;
		refreshOLED = true;
		ledsDirty = true;
		return true; // suppresses F8 release toggle
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