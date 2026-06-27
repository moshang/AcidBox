#include <Arduino.h>
#include "config.h"
#include "general.h"
#include "midi_handler.h"
// Potentiometer state variables
static uint8_t potAvgIndex = 0;
static uint16_t potValAvg[8] = {0, 0, 0, 0, 0, 0, 0, 0};
static uint16_t lastPotVal = 0;
static uint8_t potUnlocked = 0;

// ---------- UPDATE POTENTIOMETER ----------
void updatePot() {
	potValAvg[potAvgIndex] = analogRead(POT_PIN);
	potAvgIndex = (potAvgIndex + 1) % 8;

	// Calculate average
	uint32_t sum = 0;
	for (uint8_t i = 0; i < 8; i++) {
		sum += potValAvg[i];
	}
	uint16_t potVal = sum >> 3;

	// Check if pot moved enough to trigger unlock
	if (abs((int)potVal - (int)lastPotVal) > POT_THRESHOLD) {
		potUnlocked = 50;  // POT_LOCK_TIME equivalent
		lastPotVal = potVal;
		ledsDirty = true;
		handlePot(potVal);
	} else if (potUnlocked > 0) {
		potUnlocked--;
	}
}

// ---------- HANDLE POT VALUE ----------
void handlePot(uint16_t potVal) {
	// Map 0-511 to 0.0-1.0 for compatibility with AcidBox's param[] system
	float normalizedVal = (float)potVal / 4095.0f;
	if (normalizedVal > 1.0f) normalizedVal = 1.0f;

	handleCC(midiChn[currentEditType], 7, (uint8_t)(normalizedVal * 127.0f));
	// // For now, just log pot activity
	// // This will be expanded to control various parameters
	// static uint32_t lastPrint = 0;
	// if (millis() - lastPrint > 500) {c
	// 	lastPrint = millis();
	// 	Serial.printf("Pot: %d (%.3f)\n", potVal, normalizedVal);
	//}
}