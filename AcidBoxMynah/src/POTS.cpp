#include <Arduino.h>
#include "config.h"
#include "general.h"
#include "midi_handler.h"
#include "sequencer.h"
#include "SCALES.h"
#include "UI.h"

// Potentiometer state variables
static uint8_t potAvgIndex = 0;
static uint16_t potValAvg[8] = {0, 0, 0, 0, 0, 0, 0, 0};
static uint16_t lastPotVal = 0;
static uint8_t potUnlocked = 0;

// Pot locking (for mode-change protection)
static bool potLocked = false;           // true when pot is locked after a mode change
static uint16_t potLockPos = 0;          // pot position at lock time

// Step pitch edit state
static int8_t lastEditedStep = -1;     // which step we last edited with the pot (-1 = none)
static uint8_t lastEditNote = 0;       // last note we set for hysteresis

// ---------- LOCK POT ----------
// Lock the pot so it ignores small movements after a mode change.
// The pot will only respond again once the user moves it past POT_LOCK_THRESHOLD
// from the position it was at when locked.
void potLock()
{
    potLocked = true;
    potLockPos = lastPotVal;  // use the last known pot value as the lock reference
    potUnlocked = 0;          // also clear the normal unlock timer
}

// ---------- UPDATE POTENTIOMETER ----------
void updatePot()
{
	potValAvg[potAvgIndex] = analogRead(POT_PIN);
	potAvgIndex = (potAvgIndex + 1) % 8;

	// Calculate average
	uint32_t sum = 0;
	for (uint8_t i = 0; i < 8; i++)
	{
		sum += potValAvg[i];
	}
	uint16_t potVal = sum >> 3;

	// If pot is locked (due to a mode change), check if it has moved enough to unlock
	if (potLocked)
	{
		int16_t movement = abs((int)potVal - (int)potLockPos);
		if (movement > POT_LOCK_THRESHOLD)
		{
			// Unlock: the user has deliberately moved the pot past the threshold
			potLocked = false;
			lastPotVal = potVal;
			potUnlocked = 50; // POT_LOCK_TIME equivalent
			ledsDirty = true;
			handlePot(potVal);
		}
		// If still locked, do nothing (ignore the pot entirely)
		return;
	}

	// Normal operation: check if pot moved enough to trigger update
	if (abs((int)potVal - (int)lastPotVal) > POT_THRESHOLD)
	{
		potUnlocked = 50; // POT_LOCK_TIME equivalent
		lastPotVal = potVal;
		ledsDirty = true;
		handlePot(potVal);
	}
	else if (potUnlocked > 0)
	{
		potUnlocked--;
	}
}

// ---------- HANDLE POT VALUE ----------
void handlePot(uint16_t potVal)
{
	// ---- SCALE mode: pot selects the scale ----
	if (currentUiMode == UI_SCALE)
	{
		// Map 0-4095 to 0..NUM_SCALES
		uint8_t newScale = (uint8_t)((float)potVal / 4095.0f * (float)NUM_SCALES + 0.5f);
		if (newScale > NUM_SCALES) newScale = NUM_SCALES;
		if (newScale != scaleIndex)
		{
			setScale(newScale);
			syncSequencerScale();
			refreshOLED = true;
			ledsDirty = true;
		}
		return;
	}

	// ---- ROOT mode: pot selects the root note (C0..B7 = MIDI 12..107) ----
	if (currentUiMode == UI_ROOT)
	{
		uint8_t newRoot = 12 + (uint8_t)((float)potVal / 4095.0f * 95.0f + 0.5f); // 12..107
		if (newRoot > 107) newRoot = 107;
		if (newRoot != rootNote)
		{
			rootNote = newRoot;
			syncSequencerScale();
			refreshOLED = true;
			ledsDirty = true;
		}
		return;
	}

	// Check if we're in EDIT mode with Syn1 or Syn2 and a step button is held
	if (currentMode == MODE_EDIT && (currentEditType == Syn1 || currentEditType == Syn2))
	{
		// Look for which step button is currently held (bits 0-15 of buttonStates)
		int8_t heldStep = -1;
		uint8_t heldCount = 0;
		for (uint8_t i = 0; i < 16; i++)
		{
			if (buttonStates & (1UL << i))
			{
				heldStep = i;
				heldCount++;
			}
		}

		// Only edit pitch if exactly one step button is held
		if (heldCount == 1 && heldStep >= 0)
		{
			// Map pot 0-4095 to note range 36-71 (C2 to G4)
			uint8_t rawNote = 36 + (uint8_t)((float)potVal / 4095.0f * 35.0f); // 36..71
			if (rawNote > 71) rawNote = 71;

			// Quantize to current scale
			uint8_t quantizedNote = quantizeNoteToScale(rawNote, currentScale);

			// Apply hysteresis: only update if note changed or step changed
			if (heldStep != lastEditedStep || quantizedNote != lastEditNote)
			{
				sequencer_set_synth_step_note((uint8_t)heldStep, quantizedNote, currentEditType);
				lastEditedStep = heldStep;
				lastEditNote = quantizedNote;
				stepPotAdjusted = true; // mark that pot was adjusted so button release won't toggle
				refreshOLED = true;
				ledsDirty = true;
			}
			return; // handled as pitch edit
		}
		else
		{
			// No step held or multiple steps — clear edit state
			lastEditedStep = -1;
			lastEditNote = 0;
		}
	}
	else
	{
		// Not in synth step edit mode — clear edit state
		lastEditedStep = -1;
		lastEditNote = 0;
	}

	// Normal param CC handling (when not editing step pitch)
	uint8_t midiCC = 0;
	uint8_t lane = 0;
	switch (currentEditType)
	{
	case Syn1:
	case Syn2:
		midiCC = synthEditCC[currentEditMode];
		lane = (uint8_t)currentEditMode;
		break;
	case Drm:
		midiCC = drumEditCC[currentDrumEditMode];
		lane = (uint8_t)currentDrumEditMode;
		break;
	default:
		return; // No action for other modes
	}

	// Map 0-4095 to 0.0-1.0 for compatibility with AcidBox's param[] system
	float normalizedVal = (float)potVal / 4095.0f;
	if (normalizedVal > 1.0f)
		normalizedVal = 1.0f;

	uint8_t ccVal = (uint8_t)(normalizedVal * 127.0f);

	// Send the CC to the synth/drum engine
	handleCC(midiChn[currentEditType], midiCC, ccVal);

	// Write to automation:
	//   - If F1 is held: record at the current step only (per-step recording)
	//   - If F1 is NOT held: fill all 16 steps (global parameter set)
	if (currentMode == MODE_EDIT)
	{
		if (isButtonPressed(BTN_F1))
		{
			sequencer_write_automation_step(lane, ccVal);
		}
		else
		{
			sequencer_write_automation_all_steps(lane, ccVal);
		}
	}
}