#include <Arduino.h>
#include "config.h"
#include "general.h"
#include "sampler.h"
#include "midi_handler.h"
#include "sequencer.h"
#include "SCALES.h"
#include "UI.h"
#include "SAVE.h"

// Potentiometer state variables
static uint8_t potAvgIndex = 0;
static uint16_t potValAvg[8] = {0, 0, 0, 0, 0, 0, 0, 0};
static uint16_t lastPotVal = 0;
static uint8_t potUnlocked = 0;

// Pot locking (for mode-change protection)
static bool potLocked = false;           // true when pot is locked after a mode change
bool isPotLocked() {
    return potLocked;
}
static uint16_t potLockPos = 0;          // pot position at lock time

// Step pitch edit state
static int8_t lastEditedStep = -1;     // which step we last edited with the pot (-1 = none)
static uint8_t lastEditNote = 0;       // last note we set for hysteresis

// Temp master volume override (F8+Pot shortcut)
// When F8 is held and the pot is turned, we temporarily enter UI_MASTERVOL mode.
// When the pot settles (potUnlocked reaches 0) or F8 is released, we restore the previous mode.
static UiMode savedUiMode = UI_NORMAL;
static bool tempMasterVolOverride = false;

// Flags to suppress F-key release actions when the pot was used with that F-key.
// Set in handlePot() when F2/F3/F4/F8 + Pot combos are used.
// Checked and cleared in processButtons() / handleFunctionButtons().
bool f2PotUsed = false;
bool f3PotUsed = false;
bool f4PotUsed = false;
bool f8PotUsed = false;

// ---------- LOCK POT ----------
// Lock the pot so it ignores small movements after a mode change.
// The pot will only respond again once the user moves it past POT_LOCK_THRESHOLD
// from the position it was at when locked.
void potLock()
{
    potLocked = true;
    // Use the current pot value as the lock reference to prevent jumps
    uint32_t sum = 0;
    for (uint8_t i = 0; i < 8; i++)
    {
        sum += potValAvg[i];
    }
    uint16_t currentPotVal = sum >> 3;
    potLockPos = currentPotVal;
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

	// ---- Temp master volume override: restore previous mode when pot settles ----
	// When the pot has been still for long enough (potUnlocked reaches 0) or F8 is released,
	// restore the saved mode and lock the pot to prevent jumping.
	if (tempMasterVolOverride)
	{
		bool shouldExit = false;
		if (potUnlocked == 0)
		{
			// Pot has settled — restore previous mode
			shouldExit = true;
		}
		if (!isButtonPressed(BTN_F8))
		{
			// F8 was released — restore previous mode immediately
			shouldExit = true;
		}
		if (shouldExit)
		{
			tempMasterVolOverride = false;
			currentUiMode = savedUiMode;
			potLock();
			refreshOLED = true;
			ledsDirty = true;
		}
	}
}

// ---------- HANDLE POT VALUE ----------
void handlePot(uint16_t potVal)
{
	// ---- F8+Pot shortcut: adjust master volume while F8 is held ----
	// Temporarily enter UI_MASTERVOL mode. When the pot settles (potUnlocked reaches 0)
	// or F8 is released, updatePot() will restore the previous mode.
	if (isButtonPressed(BTN_F8) && currentUiMode == UI_NORMAL)
	{
		if (!tempMasterVolOverride)
		{
			savedUiMode = currentUiMode;
			currentUiMode = UI_MASTERVOL;
			tempMasterVolOverride = true;
		}
		// Handle master volume
		float newVol = (float)potVal / 4095.0f; // 0.0..1.0
		if (newVol > 1.0f) newVol = 1.0f;
		if (newVol < 0.0f) newVol = 0.0f;
		if (fabs(newVol - masterVolume) > 0.005f)
		{
			masterVolume = newVol;
			refreshOLED = true;
			ledsDirty = true;
		}
		// Mark that F8 was used with the pot — suppress F8 release toggle
		f8PotUsed = true;
		return;
	}

	// ---- F2+Pot shortcut: adjust Synth1 volume while F2 is held ----
	if (isButtonPressed(BTN_F2) && currentUiMode == UI_NORMAL)
	{
		uint8_t newVol = (uint8_t)((float)potVal / 4095.0f * 127.0f);
		handleCC(SYNTH1_MIDI_CHAN, CC_303_VOLUME, newVol);
		f2PotUsed = true;
		refreshOLED = true;
		ledsDirty = true;
		return;
	}

	// ---- F3+Pot shortcut: adjust Synth2 volume while F3 is held ----
	if (isButtonPressed(BTN_F3) && currentUiMode == UI_NORMAL)
	{
		uint8_t newVol = (uint8_t)((float)potVal / 4095.0f * 127.0f);
		handleCC(SYNTH2_MIDI_CHAN, CC_303_VOLUME, newVol);
		f3PotUsed = true;
		refreshOLED = true;
		ledsDirty = true;
		return;
	}

	// ---- F4+Pot shortcut: adjust Drums volume while F4 is held ----
	if (isButtonPressed(BTN_F4) && currentUiMode == UI_NORMAL)
	{
		uint8_t newVol = (uint8_t)((float)potVal / 4095.0f * 127.0f);
		handleCC(DRUM_MIDI_CHAN, CC_808_VOLUME, newVol);
		f4PotUsed = true;
		refreshOLED = true;
		ledsDirty = true;
		return;
	}

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

	// ---- BPM mode: pot sets BPM (20..300) ----
	if (currentUiMode == UI_BPM)
	{
		float newBpm = 20.0f + (float)potVal / 4095.0f * 280.0f; // 20..300
		if (newBpm > 300.0f) newBpm = 300.0f;
		if (newBpm < 20.0f) newBpm = 20.0f;
		if (fabs(newBpm - globalSeq.bpm) > 0.5f)
		{
			globalSeq.bpm = newBpm;
			bpm = newBpm;
			Delay.SetBPM(newBpm);
			midiClockBpmChanged(newBpm);
			acidBoxSaveLoad.modified = true;
			refreshOLED = true;
			ledsDirty = true;
		}
		return;
	}

	// ---- MIDI clock source: Internal / MIDI ----
	if (currentUiMode == UI_CLOCK_SRC)
	{
		uint8_t source = (potVal >= 2048) ? CLOCK_SRC_MIDI : CLOCK_SRC_INT;
		if (source != midiClockSource())
		{
			midiClockSetSource(source);
			refreshOLED = true;
			ledsDirty = true;
		}
		return;
	}

	// ---- MIDI clock output: OFF / ON ----
	if (currentUiMode == UI_CLOCK_OUT)
	{
		uint8_t output = (potVal >= 2048) ? 1 : 0;
		if (output != midiClockOutput())
		{
			midiClockSetOutput(output);
			refreshOLED = true;
			ledsDirty = true;
		}
		return;
	}

	// ---- MIDI slave audio offset: 0..20 ms ----
	if (currentUiMode == UI_CLOCK_OFFSET)
	{
		uint8_t newOffset = (uint8_t)(((uint32_t)potVal * 20UL + 2047UL) / 4095UL);
		if (newOffset > 20) newOffset = 20;
		if (newOffset != midiClockOffset())
		{
			midiClockSetOffset(newOffset);
			refreshOLED = true;
			ledsDirty = true;
		}
		return;
	}

	// ---- PATTERN SYNC: OFF / LEADER / FOLLOWER ----
	if (currentUiMode == UI_PATTERN_SYNC)
	{
		uint8_t role = (uint8_t)(((uint32_t)potVal * 3UL) / 4096UL);
		if (role > PATTERN_SYNC_FOLLOWER) role = PATTERN_SYNC_FOLLOWER;
		if (role != midiPatternSyncRole())
		{
			midiPatternSyncSetRole(role);
			refreshOLED = true;
			ledsDirty = true;
		}
		return;
	}

	// ---- SWING mode: pot sets swing (0..100) ----
	if (currentUiMode == UI_SWING)
	{
		float newSwing = (float)potVal / 4095.0f * 100.0f; // 0..100
		if (newSwing > 100.0f) newSwing = 100.0f;
		if (newSwing < 0.0f) newSwing = 0.0f;
		if (fabs(newSwing - globalSeq.swing) > 0.5f)
		{
			globalSeq.swing = newSwing;
			acidBoxSaveLoad.modified = true;
			refreshOLED = true;
			ledsDirty = true;
		}
		return;
	}

	// ---- KITS mode: pot scrolls through available kits ----
	if (currentUiMode == UI_KITS)
	{
		int kitCount = Drums.GetKitCount();
		if (kitCount > 0)
		{
			int newIndex = (int)((float)potVal / 4095.0f * (float)kitCount);
			if (newIndex >= kitCount) newIndex = kitCount - 1;
			if (newIndex < 0) newIndex = 0;
			if (newIndex != Drums.GetKitIndex())
			{
				Drums.SetKitIndex(newIndex);
				refreshOLED = true;
				ledsDirty = true;
			}
		}
		return;
	}

	// ---- MASTERVOL mode: pot sets master volume (0..100%) ----
	if (currentUiMode == UI_MASTERVOL)
	{
		float newVol = (float)potVal / 4095.0f; // 0.0..1.0
		if (newVol > 1.0f) newVol = 1.0f;
		if (newVol < 0.0f) newVol = 0.0f;
		if (fabs(newVol - masterVolume) > 0.005f)
		{
			masterVolume = newVol;
			acidBoxSaveLoad.modified = true;
			refreshOLED = true;
			ledsDirty = true;
		}
		return;
	}

	// ---- PATTERN_SELECT / SONG_SELECT / BANK_SELECT modes: pot has no functionality ----
	// The white highlight is strictly controlled by step button presses.
	// The pot should be ignored in these modes.
	if (currentUiMode == UI_PATTERN_SELECT || currentUiMode == UI_SONG_SELECT || currentUiMode == UI_BANK_SELECT)
	{
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