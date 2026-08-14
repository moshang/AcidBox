#include <Arduino.h>
#include "config.h"
#include "general.h"
#include "sampler.h"
#include "noise_fx_voice.h"
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
	delayMicroseconds(1);
	digitalWrite(SR_LATCH_PIN, HIGH);
	delayMicroseconds(1);

	// Read 24 bits - IC3 comes out first (MSB), then IC2, then IC1 (LSB)
	// 74HC165 minimum clock period is ~20ns; 1µs per edge is extremely conservative
	for (int i = 23; i >= 0; i--)
	{
		int bit = digitalRead(SR_DATA_PIN);
		if (bit)
		{
			buttonData |= (1UL << i);
		}
		digitalWrite(SR_CLOCK_PIN, HIGH);
		delayMicroseconds(1);
		digitalWrite(SR_CLOCK_PIN, LOW);
		delayMicroseconds(1);
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

	// Fast scan: read every 2ms (~500Hz) for responsive input
	// Physical debounce is inherent — mechanical switches settle in ~1-5ms,
	// and the shift register's 24-bit parallel read naturally filters glitches.
	if (now - lastReadTime < 2)
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
// Tracks the last release time for voice-select buttons to detect double-clicks.
static uint32_t lastF2Release = 0;
static uint32_t lastF3Release = 0;
static uint32_t lastF4Release = 0;
static uint32_t lastF7Release = 0;
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
        case BTN_F7: lastRelease = &lastF7Release; break;
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
        Delay.SetBPM(newBpm);
        midiClockBpmChanged(newBpm);
        refreshOLED = true;
        ledsDirty = true;
    }
    else if (currentUiMode == UI_SWING)
    {
        // Nudge swing by ±1
        float newSwing = globalSeq.swing + (float)direction;
        if (newSwing < 0.0f) newSwing = 0.0f;
        if (newSwing > 100.0f) newSwing = 100.0f;
        globalSeq.swing = newSwing;
        refreshOLED = true;
        ledsDirty = true;
    }
    else if (currentUiMode == UI_KITS)
    {
        // Nudge kit selection index
        int newKit = Drums.GetKitIndex() + direction;
        if (newKit < 0) newKit = Drums.GetKitCount() - 1;
        if (newKit >= Drums.GetKitCount()) newKit = 0;
        Drums.SetKitIndex(newKit);
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
	else if (currentUiMode == UI_CLOCK_SRC)
	{
		midiClockSetSource(midiClockSource() == CLOCK_SRC_INT ? CLOCK_SRC_MIDI : CLOCK_SRC_INT);
		refreshOLED = true;
		ledsDirty = true;
	}
	else if (currentUiMode == UI_CLOCK_OUT)
	{
		midiClockSetOutput(midiClockOutput() ? 0 : 1);
		refreshOLED = true;
		ledsDirty = true;
	}
	else if (currentUiMode == UI_CLOCK_OFFSET)
	{
		int newOffset = (int)midiClockOffset() + direction;
		if (newOffset < 0) newOffset = 20;
		if (newOffset > 20) newOffset = 0;
		midiClockSetOffset((uint8_t)newOffset);
		refreshOLED = true;
		ledsDirty = true;
	}
	else if (currentUiMode == UI_PATTERN_SYNC)
	{
		int role = (int)midiPatternSyncRole() + direction;
		if (role < PATTERN_SYNC_OFF) role = PATTERN_SYNC_FOLLOWER;
		if (role > PATTERN_SYNC_FOLLOWER) role = PATTERN_SYNC_OFF;
		midiPatternSyncSetRole((uint8_t)role);
		refreshOLED = true;
		ledsDirty = true;
	}
}

// ---------- FORWARD DECLARATIONS ----------
static bool handleStandaloneDrumSteps();
static bool handleStandaloneSynthSteps();
static bool handleF7SweepSteps();
static bool handleF1Combos();
static bool handleF4DrumLaneCombos();
static bool handleKitStepCombos();
static bool handleF8ScaleRootCombos();
static bool handleF5PartGen();
static bool handleF6PatternGen();
static bool handleAcidBoxSelectModes();
static void handleFunctionButtons();

// ---------- PROCESS BUTTON EVENTS ----------
void processButtons()
{
	// Handle AcidBox save/select modes FIRST (before F1 combo)
	// These modes block all other handlers.
	if (handleAcidBoxSelectModes())
	{
		return;
	}

	if (handleF1Combos())
	{
		// F1+F8 combo was handled — skip the normal F8 release handler this cycle
		return;
	}

	// F5 part generation (current part only)
	if (handleF5PartGen())
	{
		return;
	}

	// F6 full pattern generation (all parts)
	if (handleF6PatternGen())
	{
		return;
	}

	// F7+STEP is the live-only sweep trigger bank. It must run before the
	// standalone sequencer step handlers so no pattern data is changed.
	if (handleF7SweepSteps())
	{
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

	// Kit browser F4 load (when in UI_KITS mode)
	if (handleKitStepCombos())
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
	// BUT: if the pot was used while F8 was held (F8+Pot volume shortcut),
	// suppress the toggle — the user was adjusting volume, not toggling play.
	if (isButtonJustReleased(BTN_F8))
	{
		if (f8PotUsed)
		{
			f8PotUsed = false; // consume the flag
		}
		else
		{
			sequencer_toggle_play();
		}
	}

	handleFunctionButtons();
}

// ==================== LIVE SWEEP FX HANDLER ====================
// F7+A1..B8 triggers one of the 16 procedural FX presets.  No sequencer or
// save data is touched.  Releasing F7 selects the SWEEP edit page.
static bool f7StepUsed = false;

static bool handleF7SweepSteps()
{
	if (isButtonPressed(BTN_F7))
	{
		for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + 16; i++)
		{
			if (isButtonJustPressed(i))
			{
				Sweep.Trigger((uint8_t)(i - BTN_STEP_1));
				setEditType(Fx);
				f7StepUsed = true;
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
		}
	}

	// Once the SWEEP page is selected, the 16 step buttons are the soundboard
	// triggers by themselves.  Keep this separate from the F7-held shortcut so
	// entering the page does not require the user to keep holding F7.
	if (currentEditType == Fx &&
		!isButtonPressed(BTN_F1) && !isButtonPressed(BTN_F2) &&
		!isButtonPressed(BTN_F3) && !isButtonPressed(BTN_F4) &&
		!isButtonPressed(BTN_F5) && !isButtonPressed(BTN_F6) &&
		!isButtonPressed(BTN_F7) && !isButtonPressed(BTN_F8))
	{
		for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + 16; i++)
		{
			if (isButtonJustPressed(i))
			{
				Sweep.Trigger((uint8_t)(i - BTN_STEP_1));
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
		}
	}

	if (isButtonJustReleased(BTN_F7))
	{
		if (f7StepUsed)
		{
			f7StepUsed = false;
		}
		else if (isDoubleClick(BTN_F7))
		{
			muteSweep = !muteSweep;
			refreshOLED = true;
		}
		else
		{
			setEditType(Fx);
		}
		return true;
	}

	return false;
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
		if (!isButtonPressed(BTN_F3) &&
		    currentUiMode != UI_CLOCK_SRC && currentUiMode != UI_CLOCK_OUT &&
		    currentUiMode != UI_CLOCK_OFFSET && currentUiMode != UI_PATTERN_SYNC) {
			suppressF3Release = false;
		}
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

	// ---- F1+F4 (Drums mode only): enter KITS sub-mode ----
	// Kits are scanned at startup — no SD access here, instant entry.
	// Loading the kit happens on standalone F4 press (see handleKitStepCombos).
	if (currentEditType == Drm && isButtonJustPressed(BTN_F4) && currentUiMode != UI_KITS)
	{
		// Enter KITS browser mode
		currentUiMode = UI_KITS;
		potLock();
		refreshOLED = true;
		ledsDirty = true;
		return true; // Mark as consumed so no other handlers see BTN_F4
	}

	// F1+STEP_1 through F1+STEP_5: set synth edit mode
	// F1+STEP_1 through F1+STEP_8: set drum edit mode
	for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + 16; i++)
	{
		if (isButtonJustPressed(i))
		{
			if (currentEditType < 2)
			{
				// Syn1 or Syn2: set synth edit mode
				setSynthEditMode((SynthEditMode)(i - BTN_STEP_1));
			}
			else if (currentEditType == Drm && i <= BTN_STEP_8)
			{
				// Drums: F1+A1-A8 sets drum edit mode
				setDrumEditMode((DrumEditMode)(i - BTN_STEP_1));
			}
			else if (currentEditType == Fx)
			{
				// Sweep: A6-A8 control its implemented sends/volume.  The other
				// parameter buttons are still selectable so their unsupported
				// state is shown as a blank second OLED line.
				setSynthEditMode((SynthEditMode)(i - BTN_STEP_1));
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
// Handles F2/F3/F4/F7 releases: switch edit type.
// Double-click toggles mute for the corresponding voice.
// Nudge suppression flags prevent edit-type switches when F2/F3 were used
// in F1+F2 / F1+F3 nudge combos.
// Pot-used flags prevent edit-type switches when F2/F3/F4 were used with
// the pot for volume shortcuts (F2+Pot, F3+Pot, F4+Pot).
static void handleFunctionButtons()
{
	if (isButtonJustReleased(BTN_F2))
	{
		if (suppressF2Release)
		{
			suppressF2Release = false; // consume the suppression
		}
		else if (f2PotUsed)
		{
			f2PotUsed = false; // consume the flag — pot was used, don't switch edit type
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
		else if (f3PotUsed)
		{
			f3PotUsed = false; // consume the flag — pot was used, don't switch edit type
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
		if (f4PotUsed)
		{
			f4PotUsed = false; // consume the flag — pot was used, don't switch edit type
		}
		else if (isDoubleClick(BTN_F4))
		{
			muteDrums = !muteDrums;
			refreshOLED = true;
		}
		else
		{
			setEditType(Drm);
		}
	}

	if (isButtonJustReleased(BTN_F7))
	{
		// F7's trigger/release behavior is handled in handleF7SweepSteps().
		// This fallback is intentionally empty to avoid double-processing the
		// release when that handler has already consumed it.
	}
}

// ==================== F5 PART GENERATION (current part only) ====================
// Handles F5 press: two-state toggle.
// First press: enter UI_PARTGEN mode, show "PART" on OLED.
// Second press: clear current part data, generate new pattern via jukebox,
//               and bridge it into globalSeq. Other parts are left untouched.
static bool handleF5PartGen()
{
    // F5 must be pressed alone (no other function buttons)
    if (isButtonPressed(BTN_F1) || isButtonPressed(BTN_F2) ||
        isButtonPressed(BTN_F3) || isButtonPressed(BTN_F4) ||
        isButtonPressed(BTN_F6) || isButtonPressed(BTN_F7) ||
        isButtonPressed(BTN_F8))
        return false;

    // SWEEP is deliberately live-only; it has no pattern part to generate or
    // clear and must never be routed through the save/pattern workflow.
    if (currentEditType == Fx)
        return false;

    if (isButtonJustPressed(BTN_F5))
    {
        // In CLEARPART mode: confirm clear, then transition to PARTGEN create mode
        if (currentUiMode == UI_CLEARPART)
        {
            sequencer_clear_part(currentEditType);
            currentUiMode = UI_PARTGEN;  // transition to create mode after clearing
            refreshOLED = true;
            ledsDirty = true;
            Serial.printf("F5: Cleared %s part\n", editTypeNames[currentEditType]);
            return true;
        }

        if (currentUiMode == UI_PARTGEN)
        {
            // F5 pressed again — generate the current part only
            EditType part = currentEditType;

            // 1. Clear all data for the current part
            sequencer_clear_part(part);

            // 2. Generate new pattern using jukebox engine
            //    (also bridges the pattern into globalSeq)
#ifdef JUKEBOX
            jukebox_generate_part(part);
#endif

            // Exit PARTGEN mode
            currentUiMode = UI_NORMAL;
            refreshOLED = true;
            ledsDirty = true;

            Serial.printf("F5: Generated %s pattern\n", editTypeNames[currentEditType]);
        }
        else
        {
            // First press: enter PARTGEN mode
            currentUiMode = UI_PARTGEN;
            refreshOLED = true;
            ledsDirty = true;
        }
        return true;
    }

    return false;
}

// ==================== F6 FULL PATTERN GENERATION (all parts) ====================
// Handles F6 press: two-state toggle.
// First press: enter UI_PATTERNGEN mode, show "PATTERN" on OLED.
// Second press: clear all parts, generate full pattern via jukebox,
//               and bridge into globalSeq.
static bool handleF6PatternGen()
{
    // F6 must be pressed alone (no other function buttons)
    if (isButtonPressed(BTN_F1) || isButtonPressed(BTN_F2) ||
        isButtonPressed(BTN_F3) || isButtonPressed(BTN_F4) ||
        isButtonPressed(BTN_F5) || isButtonPressed(BTN_F7) ||
        isButtonPressed(BTN_F8))
        return false;

    if (isButtonJustPressed(BTN_F6))
    {
        // In CLEARPATTERN mode: confirm clear, then transition to PATTERNGEN create mode
        if (currentUiMode == UI_CLEARPATTERN)
        {
            sequencer_clear_part(Syn1);
            sequencer_clear_part(Syn2);
            sequencer_clear_part(Drm);
            currentUiMode = UI_PATTERNGEN;  // transition to create mode after clearing
            refreshOLED = true;
            ledsDirty = true;
            Serial.printf("F6: Cleared all pattern data\n");
            return true;
        }

        if (currentUiMode == UI_PATTERNGEN)
        {
            // F6 pressed again — generate all parts
            // 1. Clear all parts
            sequencer_clear_part(Syn1);
            sequencer_clear_part(Syn2);
            sequencer_clear_part(Drm);

            // 2. Generate full pattern using jukebox engine
#ifdef JUKEBOX
            jukebox_generate_all();
#endif

            // Exit PATTERNGEN mode
            currentUiMode = UI_NORMAL;
            refreshOLED = true;
            ledsDirty = true;

            Serial.printf("F6: Generated full pattern\n");
        }
        else
        {
            // First press: enter PATTERNGEN mode
            currentUiMode = UI_PATTERNGEN;
            refreshOLED = true;
            ledsDirty = true;
        }
        return true;
    }

    return false;
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

	// Block drum lane switching while in KITS browser mode
	// Return false so the F4 press can propagate through to handleKitStepCombos()
	if (currentUiMode == UI_KITS)
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

// ==================== KIT BROWSER (UI_KITS) F4 HANDLER ====================
// When in UI_KITS mode, handles a standalone F4 press (single click) to load
// the selected kit and exit KITS mode.
static bool handleKitStepCombos()
{
	if (currentUiMode != UI_KITS)
		return false;

	// Ignore if F1 is held — that's the entry combo, not the load action
	if (isButtonPressed(BTN_F1))
		return false;

	// F4 pressed alone (no F1): load the selected kit and exit KITS mode
	if (isButtonJustPressed(BTN_F4))
	{
		Drums.LoadKitByIndex(Drums.GetKitIndex());
		currentUiMode = UI_NORMAL;
		potLock();
		refreshOLED = true;
		ledsDirty = true;
		return true;
	}

	return false;
}

// ==================== ACIDBOX PATTERN/ SONG / BANK SELECT MODE HANDLING ====================
// Long-press tracking for pattern save
static uint32_t patternStepPressTime[16] = {0};
static bool patternStepLongPressHandled[16] = {false};
static const uint32_t PATTERN_LONG_PRESS_MS = 800;

// Tracks whether the next F8 release should be suppressed (the one that
// immediately follows entering the mode via F8+Step combo).
static bool suppressF8ReleaseInSelectMode = false;

// Handles UI_PATTERN_SELECT, UI_SONG_SELECT, UI_BANK_SELECT modes
// Returns true if the event was consumed.
static bool handleAcidBoxSelectModes()
{
    // Only handle these specific modes
    if (currentUiMode != UI_PATTERN_SELECT && currentUiMode != UI_SONG_SELECT && currentUiMode != UI_BANK_SELECT)
        return false;

    // ---- CLEAR confirmation ----
    // The long-pressed target remains held when this state is entered.  Do not
    // confirm until it is released and pressed again; any other new button
    // press cancels the operation.
    if (acidBoxClearConfirmType != ACIDBOX_CLEAR_NONE)
    {
        uint8_t target = acidBoxClearConfirmTarget;
        uint8_t targetButton = BTN_STEP_1 + target;

        if (isButtonJustPressed(targetButton))
        {
            bool actionSucceeded = false;
            switch (acidBoxClearConfirmType)
            {
            case ACIDBOX_CLEAR_PATTERN:
                if (acidBoxPatternConfirmAction == ACIDBOX_PATTERN_REPLACE)
                {
                    actionSucceeded = saveCurrentPattern(target);
                    Serial.printf("%s replaced pattern slot %d\n",
                                  actionSucceeded ? "✓" : "❌", target + 1);
                }
                else
                {
                    actionSucceeded = deleteAcidBoxPattern(acidBoxSaveLoad.currentBank,
                                                           acidBoxSaveLoad.currentSong,
                                                           target);
                    Serial.printf("%s cleared pattern slot %d\n",
                                  actionSucceeded ? "✓" : "❌", target + 1);
                }
                break;
            case ACIDBOX_CLEAR_SONG:
                actionSucceeded = clearAcidBoxSong(acidBoxSaveLoad.currentBank, target);
                break;
            case ACIDBOX_CLEAR_BANK:
                actionSucceeded = clearAcidBoxBank(target);
                break;
            default:
                break;
            }

            if (acidBoxClearConfirmType != ACIDBOX_CLEAR_PATTERN)
            {
                Serial.printf("%s cleared slot %d (%c%d)\n",
                              actionSucceeded ? "✓" : "❌", target + 1,
                              target < 8 ? 'A' : 'B', (target % 8) + 1);
            }
            acidBoxClearConfirmType = ACIDBOX_CLEAR_NONE;
            acidBoxPatternConfirmAction = ACIDBOX_PATTERN_REPLACE;
            patternStepPressTime[target] = millis();
            patternStepLongPressHandled[target] = true;
            refreshOLED = true;
            ledsDirty = true;
            return true;
        }

        for (uint8_t button = 0; button < NUM_BUTTONS; button++)
        {
            if (button != targetButton && isButtonJustPressed(button))
            {
                acidBoxClearConfirmType = ACIDBOX_CLEAR_NONE;
                acidBoxPatternConfirmAction = ACIDBOX_PATTERN_REPLACE;
                if (button >= BTN_STEP_1 && button < BTN_STEP_1 + 16)
                {
                    uint8_t cancelledStep = button - BTN_STEP_1;
                    patternStepPressTime[cancelledStep] = millis();
                    patternStepLongPressHandled[cancelledStep] = true;
                }
                refreshOLED = true;
                ledsDirty = true;
                Serial.println("Clear cancelled");
                return true;
            }
        }
        return true;
    }

    // ---- Exit conditions ----
    // F1+Step (any step): user is selecting a synth/drum edit mode → exit
    if (isButtonPressed(BTN_F1))
    {
        for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + 16; i++)
        {
            if (isButtonJustPressed(i))
            {
                currentUiMode = UI_NORMAL;
                potLock(); // Lock pot to prevent parameter jumps
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
        potLock(); // Lock pot to prevent parameter jumps
        refreshOLED = true;
        ledsDirty = true;
        return false;
    }

    // F2/F3/F4 release (single click): switch edit type → exit
    if (isButtonJustReleased(BTN_F2))
    {
        currentUiMode = UI_NORMAL;
        potLock(); // Lock pot to prevent parameter jumps
        refreshOLED = true;
        ledsDirty = true;
        return false;
    }
    if (isButtonJustReleased(BTN_F3))
    {
        currentUiMode = UI_NORMAL;
        potLock(); // Lock pot to prevent parameter jumps
        refreshOLED = true;
        ledsDirty = true;
        return false;
    }
    if (isButtonJustReleased(BTN_F4))
    {
        currentUiMode = UI_NORMAL;
        potLock(); // Lock pot to prevent parameter jumps
        refreshOLED = true;
        ledsDirty = true;
        return false;
    }

    // ---- F8 release: toggle sequencer start/stop ----
    // The first F8 release after entering the mode is suppressed (it's the
    // release that follows the F8+Step combo that entered the mode).
    // Subsequent F8 releases toggle the sequencer while staying in the mode.
    if (isButtonJustReleased(BTN_F8))
    {
        if (suppressF8ReleaseInSelectMode)
        {
            suppressF8ReleaseInSelectMode = false; // consume the suppression
            return true; // block this release
        }
        // Let the F8 release through to the sequencer toggle in processButtons()
        return false;
    }

    // ---- F8+Step switching between sub-modes ----
    // When F8 is held, step buttons switch between modes instead of
    // performing pattern/song/bank actions.
    // F8+Step1 (A1) = Pattern Select
    // F8+Step2 (A2) = Song Select
    // F8+Step3 (A3) = Bank Select
    if (isButtonPressed(BTN_F8))
    {
        if (isButtonJustPressed(BTN_STEP_1) && currentUiMode != UI_PATTERN_SELECT)
        {
            currentUiMode = UI_PATTERN_SELECT;
            suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
            refreshAcidBoxBankCache();
            refreshAcidBoxSongCache();
            refreshAcidBoxPatternCache();
            // Mark step 0 as already handled so its release won't trigger a load
            patternStepPressTime[0] = millis();
            patternStepLongPressHandled[0] = true;
            refreshOLED = true;
            ledsDirty = true;
            return true;
        }
        if (isButtonJustPressed(BTN_STEP_2) && currentUiMode != UI_SONG_SELECT)
        {
            currentUiMode = UI_SONG_SELECT;
            suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
            refreshAcidBoxSongCache();
            refreshAcidBoxPatternCache();
            patternStepPressTime[1] = millis();
            patternStepLongPressHandled[1] = true;
            refreshOLED = true;
            ledsDirty = true;
            return true;
        }
        if (isButtonJustPressed(BTN_STEP_3) && currentUiMode != UI_BANK_SELECT)
        {
            currentUiMode = UI_BANK_SELECT;
            suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
            refreshAcidBoxBankCache();
            refreshAcidBoxSongCache();
            refreshAcidBoxPatternCache();
            patternStepPressTime[2] = millis();
            patternStepLongPressHandled[2] = true;
            refreshOLED = true;
            ledsDirty = true;
            return true;
        }
        if (isButtonJustPressed(BTN_STEP_8))
        {
            currentUiMode = UI_BPM;
            suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
            refreshOLED = true;
            ledsDirty = true;
            return true;
        }
        if (isButtonJustPressed(BTN_STEP_7))
        {
            currentUiMode = UI_SWING;
            suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
            refreshOLED = true;
            ledsDirty = true;
            return true;
        }
        if (isButtonJustPressed(BTN_STEP_16))
        {
            currentUiMode = UI_MASTERVOL;
            suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
            refreshOLED = true;
            ledsDirty = true;
            return true;
        }
        // Block all other handlers while F8 is held in select mode
        return true;
    }

    // ---- Mode-specific handling ----
    uint32_t now = millis();

    if (currentUiMode == UI_PATTERN_SELECT)
    {
        // Step button handling for pattern select
        for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + 16; i++)
        {
            uint8_t slot = i - BTN_STEP_1; // 0-15

            if (isButtonJustPressed(i))
            {
                // Record press time for long-press detection
                patternStepPressTime[slot] = now;
                patternStepLongPressHandled[slot] = false;
                refreshOLED = true;
                ledsDirty = true;
                return true;
            }

            // Long-press: clear an existing pattern, or save into an empty slot
            if (isButtonPressed(i) && !patternStepLongPressHandled[slot] &&
                (now - patternStepPressTime[slot] >= PATTERN_LONG_PRESS_MS))
            {
                patternStepLongPressHandled[slot] = true;
                if (acidBoxSaveLoad.patternExistsCache[slot])
                {
                    acidBoxClearConfirmType = ACIDBOX_CLEAR_PATTERN;
                    acidBoxClearConfirmTarget = slot;
                    acidBoxPatternConfirmAction = ACIDBOX_PATTERN_REPLACE;
                    potLock();
                    Serial.printf("Clear confirmation for pattern slot %d\n", slot + 1);
                }
                else if (saveCurrentPattern(slot))
                {
                    Serial.printf("💾 Saved pattern to slot %d (A%d/B%d)\n",
                                  slot + 1, slot < 8 ? slot + 1 : slot - 7,
                                  slot < 8 ? 0 : 1);
                }
                refreshOLED = true;
                ledsDirty = true;
                return true;
            }

            // Release: load pattern from this slot (if it exists)
            if (isButtonJustReleased(i))
            {
                if (!patternStepLongPressHandled[slot] && acidBoxSaveLoad.patternExistsCache[slot])
                {
                    // Load pattern from this slot
                    if (loadCurrentPattern(slot))
                    {
                        Serial.printf("📂 Loaded pattern from slot %d\n", slot + 1);
                        midiPatternSyncSend(acidBoxSaveLoad.currentBank,
                                            acidBoxSaveLoad.currentSong,
                                            acidBoxSaveLoad.currentPattern);
                    }
                }
                patternStepPressTime[slot] = 0;
                patternStepLongPressHandled[slot] = false;
                refreshOLED = true;
                ledsDirty = true;
                return true;
            }
        }
    }
    else if (currentUiMode == UI_SONG_SELECT)
    {
        // Step button: select song, or long-press an existing song to clear it
        for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + 16; i++)
        {
            uint8_t song = i - BTN_STEP_1;

            if (isButtonJustPressed(i))
            {
                patternStepPressTime[song] = now;
                patternStepLongPressHandled[song] = false;
                refreshOLED = true;
                ledsDirty = true;
                return true;
            }

            if (isButtonPressed(i) && !patternStepLongPressHandled[song] &&
                (now - patternStepPressTime[song] >= PATTERN_LONG_PRESS_MS) &&
                acidBoxSaveLoad.songExistsCache[song])
            {
                patternStepLongPressHandled[song] = true;
                acidBoxClearConfirmType = ACIDBOX_CLEAR_SONG;
                acidBoxClearConfirmTarget = song;
                potLock();
                refreshOLED = true;
                ledsDirty = true;
                return true;
            }

            if (isButtonJustReleased(i))
            {
                if (!patternStepLongPressHandled[song])
                {
                    acidBoxSaveLoad.currentSong = song;
                    refreshAcidBoxSongCache();
                    refreshAcidBoxPatternCache();
                    midiPatternSyncSend(acidBoxSaveLoad.currentBank,
                                        acidBoxSaveLoad.currentSong,
                                        acidBoxSaveLoad.currentPattern);
                }
                patternStepPressTime[song] = 0;
                patternStepLongPressHandled[song] = false;
                refreshOLED = true;
                ledsDirty = true;
                return true;
            }
        }
    }
    else if (currentUiMode == UI_BANK_SELECT)
    {
        // Step button: select bank, or long-press an existing bank to clear it
        for (uint8_t i = BTN_STEP_1; i < BTN_STEP_1 + 16; i++)
        {
            uint8_t bank = i - BTN_STEP_1;

            if (isButtonJustPressed(i))
            {
                patternStepPressTime[bank] = now;
                patternStepLongPressHandled[bank] = false;
                refreshOLED = true;
                ledsDirty = true;
                return true;
            }

            if (isButtonPressed(i) && !patternStepLongPressHandled[bank] &&
                (now - patternStepPressTime[bank] >= PATTERN_LONG_PRESS_MS) &&
                acidBoxSaveLoad.bankExistsCache[bank])
            {
                patternStepLongPressHandled[bank] = true;
                acidBoxClearConfirmType = ACIDBOX_CLEAR_BANK;
                acidBoxClearConfirmTarget = bank;
                potLock();
                refreshOLED = true;
                ledsDirty = true;
                return true;
            }

            if (isButtonJustReleased(i))
            {
                if (!patternStepLongPressHandled[bank])
                {
                    acidBoxSaveLoad.currentBank = bank;
                    refreshAcidBoxBankCache();
                    refreshAcidBoxSongCache();
                    refreshAcidBoxPatternCache();
                    midiPatternSyncSend(acidBoxSaveLoad.currentBank,
                                        acidBoxSaveLoad.currentSong,
                                        acidBoxSaveLoad.currentPattern);
                }
                patternStepPressTime[bank] = 0;
                patternStepLongPressHandled[bank] = false;
                refreshOLED = true;
                ledsDirty = true;
                return true;
            }
        }
    }

    // Block all other handlers while in select mode
    return true;
}

// ==================== F8 + STEP combos (SCALE / ROOT / BPM / SWING / MASTERVOL / PATTERN / SONG / BANK) ====================
// F8+Step1  (BTN_STEP_1)  enters UI_PATTERN_SELECT mode — browse/select patterns.
// F8+Step2  (BTN_STEP_2)  enters UI_SONG_SELECT mode — browse/select songs.
// F8+Step3  (BTN_STEP_3)  enters UI_BANK_SELECT mode — browse/select banks.
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
					potLock(); // Lock pot to prevent parameter jumps
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
			potLock(); // Lock pot to prevent parameter jumps
			refreshOLED = true;
			ledsDirty = true;
			return false; // let processButtons continue to handleF1Combos
		}

		// F2/F3/F4 release (single click): switch edit type → exit
		// BUT: skip if the release is suppressed by a nudge combo
		// ALSO: skip F4 release in UI_KITS mode — the F4 release that follows
		// the F1+F4 entry combo should not kick the user out of KITS mode.
		if (isButtonJustReleased(BTN_F2) && !suppressF2Release)
		{
			currentUiMode = UI_NORMAL;
			potLock(); // Lock pot to prevent parameter jumps
			refreshOLED = true;
			ledsDirty = true;
			return false; // let processButtons continue to handleFunctionButtons
		}
		if (isButtonJustReleased(BTN_F3) && !suppressF3Release)
		{
			currentUiMode = UI_NORMAL;
			potLock(); // Lock pot to prevent parameter jumps
			refreshOLED = true;
			ledsDirty = true;
			return false; // let processButtons continue to handleFunctionButtons
		}
		// F3 was used with F8 to enter/cycle the clock pages. Consume its
		// release here so it cannot fall through to Synth2 selection.
		if (isButtonJustReleased(BTN_F3) && suppressF3Release)
		{
			suppressF3Release = false;
			return true;
		}
		if (isButtonJustReleased(BTN_F4) && currentUiMode != UI_KITS)
		{
			currentUiMode = UI_NORMAL;
			potLock(); // Lock pot to prevent parameter jumps
			refreshOLED = true;
			ledsDirty = true;
			return false; // let processButtons continue to handleFunctionButtons
		}

		// F8+V2 (F8+F3) cycles the two MIDI clock pages.  Suppress F3's
		// normal voice-select action when the combo is released.
		if (isButtonPressed(BTN_F8) && isButtonJustPressed(BTN_F3))
		{
			if (currentUiMode == UI_CLOCK_SRC) currentUiMode = UI_CLOCK_OUT;
			else if (currentUiMode == UI_CLOCK_OUT) currentUiMode = UI_CLOCK_OFFSET;
			else if (currentUiMode == UI_CLOCK_OFFSET) currentUiMode = UI_PATTERN_SYNC;
			else currentUiMode = UI_CLOCK_SRC;
			suppressF3Release = true;
			suppressF8ReleaseInSelectMode = true;
			refreshOLED = true;
			ledsDirty = true;
			return true;
		}

	// ---- F8 release: pass through to sequencer toggle in processButtons() ----
	// For UI_PATTERN_SELECT, UI_SONG_SELECT, and UI_BANK_SELECT modes, the
	// F8 release is handled by handleAcidBoxSelectModes(). If it returned false,
	// we need to let it through to the sequencer toggle in processButtons().
	// For UI_BPM, UI_SWING, and UI_MASTERVOL modes, the first F8 release after
	// entering the mode is suppressed (it's the release that follows the F8+Step
	// combo that entered the mode). Subsequent F8 releases toggle the sequencer.
	if (isButtonJustReleased(BTN_F8) &&
	    (currentUiMode == UI_PATTERN_SELECT || currentUiMode == UI_SONG_SELECT ||
	     currentUiMode == UI_BANK_SELECT || currentUiMode == UI_BPM ||
	     currentUiMode == UI_SWING || currentUiMode == UI_MASTERVOL ||
	     currentUiMode == UI_CLOCK_SRC || currentUiMode == UI_CLOCK_OUT ||
	     currentUiMode == UI_CLOCK_OFFSET || currentUiMode == UI_PATTERN_SYNC))
	{
		if (suppressF8ReleaseInSelectMode)
		{
			suppressF8ReleaseInSelectMode = false; // consume the suppression
			return true; // block this release
		}
		return false;
	}

	// Allow switching between sub-modes while F8 is held (and F8+F5/F8+F6 for clear)
		if (isButtonPressed(BTN_F8))
		{
			// F8+F5: enter CLEARPART mode (clear current part)
			if (isButtonJustPressed(BTN_F5) && currentUiMode != UI_CLEARPART)
			{
				currentUiMode = UI_CLEARPART;
				potLock();
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}

			// F8+F6: enter CLEARPATTERN mode (clear all pattern data)
			if (isButtonJustPressed(BTN_F6) && currentUiMode != UI_CLEARPATTERN)
			{
				currentUiMode = UI_CLEARPATTERN;
				potLock();
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}

			if (isButtonJustPressed(BTN_STEP_1) && currentUiMode != UI_PATTERN_SELECT)
			{
				currentUiMode = UI_PATTERN_SELECT;
				suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
				potLock(); // Lock pot to prevent parameter jumps
				refreshAcidBoxBankCache();
				refreshAcidBoxSongCache();
				refreshAcidBoxPatternCache();
				// Mark step 0 as handled so its release won't trigger a load
				patternStepPressTime[0] = millis();
				patternStepLongPressHandled[0] = true;
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
			if (isButtonJustPressed(BTN_STEP_2) && currentUiMode != UI_SONG_SELECT)
			{
				currentUiMode = UI_SONG_SELECT;
				suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
				potLock(); // Lock pot to prevent parameter jumps
				refreshAcidBoxSongCache();
				refreshAcidBoxPatternCache();
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
			if (isButtonJustPressed(BTN_STEP_3) && currentUiMode != UI_BANK_SELECT)
			{
				currentUiMode = UI_BANK_SELECT;
				suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
				potLock(); // Lock pot to prevent parameter jumps
				refreshAcidBoxBankCache();
				refreshAcidBoxSongCache();
				refreshAcidBoxPatternCache();
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
			if (isButtonJustPressed(BTN_STEP_9) && currentUiMode != UI_SCALE)
			{
				currentUiMode = UI_SCALE;
				potLock(); // Lock pot to prevent parameter jumps
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
			if (isButtonJustPressed(BTN_STEP_10) && currentUiMode != UI_ROOT)
			{
				currentUiMode = UI_ROOT;
				potLock(); // Lock pot to prevent parameter jumps
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
			if (isButtonJustPressed(BTN_STEP_8) && currentUiMode != UI_BPM)
			{
				currentUiMode = UI_BPM;
				potLock(); // Lock pot to prevent parameter jumps
				suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
			if (isButtonJustPressed(BTN_STEP_7) && currentUiMode != UI_SWING)
			{
				currentUiMode = UI_SWING;
				potLock(); // Lock pot to prevent parameter jumps
				suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
				refreshOLED = true;
				ledsDirty = true;
				return true;
			}
			if (isButtonJustPressed(BTN_STEP_16) && currentUiMode != UI_MASTERVOL)
			{
				currentUiMode = UI_MASTERVOL;
				potLock(); // Lock pot to prevent parameter jumps
				suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
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

	// F8+V2 (F8+F3): open the MIDI clock and pattern sync settings pages.
	if (isButtonJustPressed(BTN_F3))
	{
		currentUiMode = UI_CLOCK_SRC;
		suppressF3Release = true;
		suppressF8ReleaseInSelectMode = true;
		potLock();
		refreshOLED = true;
		ledsDirty = true;
		return true;
	}

	// F8+F5: enter CLEARPART mode (clear current part)
	if (isButtonJustPressed(BTN_F5) && currentUiMode != UI_CLEARPART)
	{
		currentUiMode = UI_CLEARPART;
		potLock();
		refreshOLED = true;
		ledsDirty = true;
		return true;
	}

	// F8+F6: enter CLEARPATTERN mode (clear all pattern data)
	if (isButtonJustPressed(BTN_F6) && currentUiMode != UI_CLEARPATTERN)
	{
		currentUiMode = UI_CLEARPATTERN;
		potLock();
		refreshOLED = true;
		ledsDirty = true;
		return true;
	}

	// F8+Step1: enter PATTERN SELECT mode
	if (isButtonJustPressed(BTN_STEP_1))
	{
		currentUiMode = UI_PATTERN_SELECT;
		potLock(); // Lock pot to prevent parameter jumps
		refreshAcidBoxBankCache();
		refreshAcidBoxSongCache();
		refreshAcidBoxPatternCache();
		// Mark step 0 as handled so its release won't trigger a load
		patternStepPressTime[0] = millis();
		patternStepLongPressHandled[0] = true;
		suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
		refreshOLED = true;
		ledsDirty = true;
		return true; // suppresses F8 release toggle
	}

	// F8+Step2: enter SONG SELECT mode
	if (isButtonJustPressed(BTN_STEP_2))
	{
		currentUiMode = UI_SONG_SELECT;
		potLock(); // Lock pot to prevent parameter jumps
		refreshAcidBoxSongCache();
		refreshAcidBoxPatternCache();
        patternStepPressTime[1] = millis();
        patternStepLongPressHandled[1] = true;
		suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
		refreshOLED = true;
		ledsDirty = true;
		return true; // suppresses F8 release toggle
	}

	// F8+Step3: enter BANK SELECT mode
	if (isButtonJustPressed(BTN_STEP_3))
	{
		currentUiMode = UI_BANK_SELECT;
		potLock(); // Lock pot to prevent parameter jumps
		refreshAcidBoxBankCache();
		refreshAcidBoxSongCache();
		refreshAcidBoxPatternCache();
        patternStepPressTime[2] = millis();
        patternStepLongPressHandled[2] = true;
		suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
		refreshOLED = true;
		ledsDirty = true;
		return true; // suppresses F8 release toggle
	}

	// F8+Step9: enter SCALE mode
	if (isButtonJustPressed(BTN_STEP_9))
	{
		currentUiMode = UI_SCALE;
		potLock(); // Lock pot to prevent parameter jumps
		refreshOLED = true;
		ledsDirty = true;
		return true; // suppresses F8 release toggle
	}

	// F8+Step10: enter ROOT mode
	if (isButtonJustPressed(BTN_STEP_10))
	{
		currentUiMode = UI_ROOT;
		potLock(); // Lock pot to prevent parameter jumps
		refreshOLED = true;
		ledsDirty = true;
		return true; // suppresses F8 release toggle
	}

	// F8+Step8: enter BPM mode
	if (isButtonJustPressed(BTN_STEP_8))
	{
		currentUiMode = UI_BPM;
		potLock(); // Lock pot to prevent parameter jumps
		suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
		refreshOLED = true;
		ledsDirty = true;
		return true; // suppresses F8 release toggle
	}

	// F8+Step7: enter SWING mode
	if (isButtonJustPressed(BTN_STEP_7))
	{
		currentUiMode = UI_SWING;
		potLock(); // Lock pot to prevent parameter jumps
		suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
		refreshOLED = true;
		ledsDirty = true;
		return true; // suppresses F8 release toggle
	}

	// F8+Step16: enter MASTERVOL mode
	if (isButtonJustPressed(BTN_STEP_16))
	{
		currentUiMode = UI_MASTERVOL;
		potLock(); // Lock pot to prevent parameter jumps
		suppressF8ReleaseInSelectMode = true; // suppress the imminent F8 release
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