/*
   this file includes the implementation of the sample player
   samples are loaded from SD card (/ACIDBOX/KITS{kit}/) or
   fall back to embedded samples in samples.h

   Author: Marcel Licence

   Modifications:
   2021-04-05 E.Heinemann changed BLOCKSIZE from 2024 to 1024
                , added DEBUG_SAMPLER
                , added sampleRate to the structure of samplePlayerS to optimize the pitch based on lower samplerates
   2021-07-28 E.Heinemann, added pitch-decay and pan
   2021-08-03 E.Heinemann, changed Accent/normal Velocity in the code
   2022-11-27 Copych, made this a class, made it use one big PSRAM buffer for drumkit wav data
   2023-01-20 Copych, changed midi cc handling, now it affects instruments, not the sample players
   2023-03-02 Copych, preload all 3MB of samples from flash to PSRAM to be able of switching kits in realtime
   2026-06-25 Moshang, switched from LittleFS to SD_MMC for sample loading; embedded fallback
   2026-07-20 Fixed: removed `static` from buffPointer in Init() to prevent buffer corruption on kit reload;
            added delay(1) in sample read loop to prevent WDT timeout reboot.
 */
#include <Arduino.h>
#include "general.h"
#include "sampler.h"
#include "samples.h"

//#define DEBUG_SAMPLER

struct EmbeddedSampleRef {
    size_t size;
    const uint8_t* data;
};

// Keep the original fallback order intact. Slots 10 and 11 repeat the first
// two fallback sounds; the four new slots 12..15 repeat fallback slots 0..3.
// This preserves the old sounds in slots 0..9 and gives every UI lane audio.
static const EmbeddedSampleRef kEmbeddedSamples[] = {
    { s01_sz, s01 },  // 001_BD.wav
    { s02_sz, s02 },  // 002_SD.wav
    { s00_sz, s00 },  // 003_.wav (empty)
    { s00_sz, s00 },  // 004_.wav (empty)
    { s05_sz, s05 },  // 005_CB.wav
    { s00_sz, s00 },  // 006_.wav (empty)
    { s07_sz, s07 },  // 007_CH.wav
    { s08_sz, s08 },  // 008_OH.wav
    { s00_sz, s00 },  // 009_.wav (empty)
    { s10_sz, s10 },  // 010_CR.wav
};
static const uint8_t kEmbeddedSampleCount =
    sizeof(kEmbeddedSamples) / sizeof(kEmbeddedSamples[0]);

static uint8_t fallbackSlot(uint8_t slot) {
    if (slot < kEmbeddedSampleCount) return slot;
    if (slot < 12) return slot - 10;
    return slot - 12;
}

bool Sampler::LoadEmbeddedSample(uint8_t slot, size_t &buffPointer, size_t cacheLimit) {
    const EmbeddedSampleRef &embedded = kEmbeddedSamples[fallbackSlot(slot)];
    const size_t headerSize = 44;
    if (embedded.size < headerSize || buffPointer > cacheLimit ||
        embedded.size - headerSize > cacheLimit - buffPointer) {
        return false;
    }

    union wavHeader wav;
    memcpy(wav.wavHdr, embedded.data, sizeof(wav.wavHdr));
    const size_t pcmSize = embedded.size - sizeof(wav.wavHdr);
    samplePlayer[slot].sampleStart = buffPointer;
    memcpy(&RamCache[buffPointer], embedded.data + sizeof(wav.wavHdr), pcmSize);
    buffPointer += pcmSize;
    samplePlayer[slot].sampleRate = wav.sampleRate;
    samplePlayer[slot].sampleSize = min((size_t)wav.dataSize, pcmSize);
    samplePlayer[slot].sampleSeek = 0xFFFFFFFF;
    return samplePlayer[slot].sampleSize > 0;
}

void Sampler::LoadEmbeddedSamples() {
    size_t buffPointer = 0;
#ifndef NO_PSRAM
    const size_t cacheLimit = PSRAM_SAMPLER_CACHE;
#else
    const size_t cacheLimit = RAM_SAMPLER_CACHE;
#endif

    // Allocate PSRAM / RAM buffer if not already done
#ifndef NO_PSRAM
    heap_caps_print_heap_info(MALLOC_CAP_8BIT);
    if (psramFound()) {
        if (RamCache == NULL) {
            psramInit();
            RamCache = (uint8_t*)ps_malloc(PSRAM_SAMPLER_CACHE);
        }
        if (RamCache == NULL) {
            DEBUG("FAILED TO ALLOCATE PSRAM CACHE BUFFER!");
            sampleInfoCount = 0;
            return;
        } else {
            DEBF("PSRAM BUFFER OF %d bytes ALLOCATED! EMBEDDED SAMPLES MODE!\r\n", PSRAM_SAMPLER_CACHE);
            heap_caps_print_heap_info(MALLOC_CAP_8BIT);
        }
    } else {
        DEBUG("STOP! Use #define NO_PSRAM option in config.h");
        while (1) {}
    }
#else
    DEBF("Free heap: %d\r\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    heap_caps_print_heap_info(MALLOC_CAP_8BIT);
    if (RamCache == NULL) {
        RamCache = (uint8_t*)malloc(RAM_SAMPLER_CACHE);
    }
    if (RamCache == NULL) {
        DEBUG("FAILED TO ALLOCATE RAM CACHE BUFFER!");
    } else {
        DEBF("HEAP BUFFER of %d bytes ALLOCATED! EMBEDDED SAMPLES MODE!\r\n", RAM_SAMPLER_CACHE);
    }
#endif

    sampleInfoCount = DRUM_SLOT_COUNT;
    for (uint8_t i = 0; i < DRUM_SLOT_COUNT; i++) {
        if (!LoadEmbeddedSample(i, buffPointer, cacheLimit)) {
            sampleInfoCount = i;
            break;
        }
    }
}

bool Sampler::IsNumericKitFile(const String &path, uint8_t &slot) const {
  int slash = path.lastIndexOf('/');
  String name = slash >= 0 ? path.substring(slash + 1) : path;
  // New direct-slot files use exactly two digits followed by "_":
  // 01_BD.wav through 16_Tamb.wav. Requiring the separator prevents an
  // old filename such as 010_OldKit.wav becoming new-format slot 01.
  if (name.length() < 4 || name.charAt(2) != '_' ||
      name.charAt(0) < '0' || name.charAt(0) > '9' ||
      name.charAt(1) < '0' || name.charAt(1) > '9') {
    return false;
  }
  int number = (name.charAt(0) - '0') * 10 +
               (name.charAt(1) - '0');
  if (number < 1 || number > DRUM_SLOT_COUNT) return false;
  slot = (uint8_t)(number - 1);
  return true;
}
void Sampler::ScanContents(fs::FS &fs, const char *dirname, uint8_t levels) {
  String str;
#ifdef DEBUG_SAMPLER
  DEBF("Listing directory: %s\r\n", dirname);
#endif
  File root = fs.open(dirname);
  if ( !root ) {
    DEBUG("- failed to open directory");
    return;
  }
  if ( !root.isDirectory() ) {
    DEBUG(" - not a directory");
    return;
  }

  File file = root.openNextFile();
  while ( file ) {
    if ( file.isDirectory() ) {
#ifdef DEBUG_SAMPLER
      DEB("  DIR : ");
      DEBUG(file.name());
#endif
      if ( levels ) {
        str = (String)(dirname + (String)"/" + (String)(file.name()));
        ScanContents(fs, str.c_str(), levels - 1);
      }
    } else {
#ifdef DEBUG_SAMPLER
      DEB("  FILE: ");
      DEB(dirname);
      DEB(file.name());
      DEB("\tSIZE: ");
      DEBUG(file.size());
#endif

      str = (String)dirname + "/" + (String)(file.name());
      uint8_t numericSlot = 0;
      if (IsNumericKitFile(str, numericSlot) &&
          (numericKitMask & (1U << numericSlot)) == 0) {
        numericKitFiles[numericSlot] = str;
        numericKitMask |= (1U << numericSlot);
      }
      if (legacyKitFileCount < SAMPLECNT) {
        legacyKitFiles[legacyKitFileCount++] = str;
      }
    }
    delay(1);
    file = root.openNextFile();
  }
}

void Sampler::PrepareKitSampleSlots() {
  memset(filenames, 0, sizeof(filenames));
  sampleInfoCount = 0;
  directSlotKit = (numericKitMask != 0);

  // Numeric prefixes are authoritative whenever at least one 001..016 file
  // exists. Missing numbered slots remain empty and receive fallback audio.
  if (numericKitMask != 0) {
    for (uint8_t slot = 0; slot < DRUM_SLOT_COUNT; slot++) {
      if ((numericKitMask & (1U << slot)) != 0) {
        if (numericKitFiles[slot].length() >= sizeof(filenames[slot])) {
          Serial.printf("[Sampler] Skipping too-long path for slot %u: %s\n",
                        (unsigned)(slot + 1), numericKitFiles[slot].c_str());
          continue;
        }
        strncpy(filenames[slot], numericKitFiles[slot].c_str(), sizeof(filenames[slot]) - 1);
      }
    }
  } else {
    // Compatibility path for old kits that have no 01..16 prefixes.
    for (uint8_t slot = 0; slot < DRUM_SLOT_COUNT && slot < legacyKitFileCount; slot++) {
      if (legacyKitFiles[slot].length() < sizeof(filenames[slot])) {
        strncpy(filenames[slot], legacyKitFiles[slot].c_str(), sizeof(filenames[slot]) - 1);
      }
    }
  }

  for (uint8_t slot = 0; slot < DRUM_SLOT_COUNT; slot++) {
    if (filenames[slot][0] != '\0') {
      sampleInfoCount = DRUM_SLOT_COUNT;
      return;
    }
  }
}

bool Sampler::LoadSdSample(uint8_t slot, size_t &buffPointer, size_t cacheLimit) {
  File f = SD_MMC.open((String)filenames[slot]);
  if (!f) {
    Serial.print("[Sampler] SD open failed: " );
    Serial.println(filenames[slot]);
    return false;
  }

  const size_t fileLength = f.size();
  if (fileLength < 44 || buffPointer > cacheLimit) {
    f.close();
    return false;
  }

  union wavHeader wav;
  if (f.read(wav.wavHdr, sizeof(wav.wavHdr)) != sizeof(wav.wavHdr)) {
    f.close();
    return false;
  }

  const size_t pcmBytes = fileLength - sizeof(wav.wavHdr);
  if (pcmBytes > cacheLimit - buffPointer) {
    Serial.printf("[Sampler] Cache full while loading %s at sample %d\n",
                  filenames[slot], slot);
    f.close();
    return false;
  }

  const size_t start = buffPointer;
  size_t remaining = pcmBytes;
  while (remaining > 0) {
    const size_t toRead = min(remaining, (size_t)(1UL << 15));
    if (f.read(&RamCache[buffPointer], toRead) != toRead) {
      buffPointer = start;
      f.close();
      return false;
    }
    buffPointer += toRead;
    remaining -= toRead;
    delay(1);
  }
  f.close();

  wav.dataSize = min((size_t)wav.dataSize, pcmBytes);
  if (wav.dataSize == 0) {
    buffPointer = start;
    return false;
  }
  samplePlayer[slot].sampleStart = start;
  samplePlayer[slot].sampleRate = wav.sampleRate;
  samplePlayer[slot].sampleSize = wav.dataSize;
  samplePlayer[slot].sampleSeek = 0xFFFFFFFF;
  Serial.print("[Sampler] Loaded SD slot " );
  Serial.print(slot + 1);
  Serial.print(": " );
  Serial.println(filenames[slot]);
  return true;
}


void Sampler::Init() {

  Effects.Init();
  Effects.SetBitCrusher( 0.0f );

  // SHIELD the sampler: zero out count so Process() ignores the buffer during reload
  sampleInfoCount = 0; 
  memset(filenames, 0, sizeof(filenames));
  numericKitMask = 0;
  legacyKitFileCount = 0;
  directSlotKit = true; // default/embedded fallback uses the new 16-slot layout
  for (uint8_t i = 0; i < DRUM_SLOT_COUNT; i++) numericKitFiles[i] = "";
  for (uint8_t i = 0; i < SAMPLECNT; i++) legacyKitFiles[i] = "";
  delay(50); // Give audio task time to see sampleInfoCount=0 (audio task runs at 44.1kHz)
  yield();   // Ensure any pending WDT servicing runs

  Serial.println("  Checking SD card for /ACIDBOX/KITS...");

  // Diagnostic: list SD card root contents (with yield to avoid WDT)
  if (SD_MMC.cardType() != CARD_NONE) {
    Serial.println("  --- SD Card Root Directory ---");
    File root = SD_MMC.open("/");
    if (root && root.isDirectory()) {
      File entry = root.openNextFile();
      while (entry) {
        if (entry.isDirectory()) {
          Serial.printf("    [DIR]  %s\n", entry.name());
        } else {
          Serial.printf("    [FILE] %s  (%u bytes)\n", entry.name(), entry.size());
        }
        delay(1);
        yield();
        entry = root.openNextFile();
      }
      root.close();
    } else {
      Serial.println("  Could not open SD root directory!");
    }
    Serial.println("  -----------------------------");
  }

  // Build the path relative to SD mount: /ACIDBOX/KITS/{progNumber}
  // (SD_MMC.begin("/sdcard") means root is /sdcard, so open("/ACIDBOX/...") resolves correctly)
  // NOTE: Do NOT add a trailing slash to the path! The ESP32 VFS/FAT driver interprets
  // a trailing slash as a missing filename inside the directory, causing open() to fail.
#ifdef NO_PSRAM
  String sdKitPath = "/ACIDBOX/KITS/" + (String)progNumber;
#else
  #ifdef PRELOAD_ALL
    String sdKitPath = "/ACIDBOX/KITS";
  #else
    String sdKitPath = "/ACIDBOX/KITS/" + (String)progNumber;
  #endif
#endif

  // Try SD card first
  if (SD_MMC.cardType() != CARD_NONE) {
    File testDir = SD_MMC.open(sdKitPath);
    if (testDir && testDir.isDirectory()) {
      Serial.printf("  Found SD kit folder: %s\n", sdKitPath.c_str());
      testDir.close();

       ScanContents(SD_MMC, sdKitPath.c_str(), 5);
       PrepareKitSampleSlots();
       Serial.printf("  Found %d candidate sample slots on SD card\n", sampleInfoCount);
    } else {
      Serial.printf("  SD kit folder not found: %s\n", sdKitPath.c_str());
      if (testDir) testDir.close();
    }
  } else {
    Serial.println("  SD card not available");
  }

#ifndef NO_PSRAM
  // Allocate PSRAM buffer if not already done by LoadEmbeddedSamples
  heap_caps_print_heap_info(MALLOC_CAP_8BIT);
  if (psramFound()) {
    if ( RamCache == NULL ) {
      psramInit();
      RamCache = (uint8_t*)ps_malloc(PSRAM_SAMPLER_CACHE);
    }
    if (RamCache == NULL) {
      DEBUG ("FAILED TO ALLOCATE PSRAM CACHE BUFFER!");
    } else {
      DEBF ("PSRAM BUFFER OF %d bytes ALLOCATED! STANDARD CONFIG ENGAGED!\r\n", PSRAM_SAMPLER_CACHE );
      heap_caps_print_heap_info(MALLOC_CAP_8BIT);
    }
  } else {
    DEBUG("STOP! Use #define NO_PSRAM option in config.h");
    while (1) {}
  }
#else
  DEBF("Free heap: %d\r\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));
  heap_caps_print_heap_info(MALLOC_CAP_8BIT);
  if ( RamCache == NULL ) {
    RamCache = (uint8_t*)malloc(RAM_SAMPLER_CACHE);
  }
  if (RamCache == NULL) {
    DEBUG ("FAILED TO ALLOCATE RAM CACHE BUFFER!");
  } else {
    DEBF ("HEAP BUFFER of %d bytes ALLOCATED! MINIMAL CONFIG ENGAGED!\r\n", RAM_SAMPLER_CACHE);
  }
#endif

#ifdef DEBUG_SAMPLER
  DEBUG("---\nList Samples:");
#endif
  // Local buffPointer resets to 0 on every Init() call.
  size_t buffPointer = 0;
#ifndef NO_PSRAM
  const size_t cacheLimit = PSRAM_SAMPLER_CACHE;
#else
  const size_t cacheLimit = RAM_SAMPLER_CACHE;
#endif
  if (RamCache == NULL) {
    sampleInfoCount = 0;
    return;
  }

  // Every kit has a stable 16-slot address space. SD samples replace their
  // numbered/legacy slot; absent or invalid files use the embedded fallback.
  sampleInfoCount = DRUM_SLOT_COUNT;
  repeat = directSlotKit ? DRUM_SLOT_COUNT : 12;
  for (uint8_t i = 0; i < DRUM_SLOT_COUNT; i++) {
    bool loaded = false;
    if (filenames[i][0] != '\0') loaded = LoadSdSample(i, buffPointer, cacheLimit);
    if (!loaded) {
      if (filenames[i][0] != '\0') {
        Serial.printf("[Sampler] Using fallback for slot %u (%s)\n",
                      (unsigned)(i + 1), filenames[i]);
      }
      if (!LoadEmbeddedSample(i, buffPointer, cacheLimit)) {
        Serial.printf("[Sampler] Fallback sample %u does not fit in cache\n", (unsigned)(i + 1));
        sampleInfoCount = i;
        break;
      }
    }
    // Embedded samples already loaded by LoadEmbeddedSamples() — nothing more to do
  }

  for ( int i = 0; i < sampleInfoCount; i++ ) {
    int j = (i % repeat ) + 1 ;
    samplePlayer[i].sampleSeek = 0xFFFFFFFF;
    samplePlayer[i].active = false;

    decay_midi[j] = 100;
    samplePlayer[i].decay_midi = decay_midi[j];
    samplePlayer[i].decay = 1.0f;

    offset_midi[j] = 0;
    samplePlayer[i].offset_midi = offset_midi[j];

    volume_midi[j] = 100;
    samplePlayer[i].volume_midi = volume_midi[j];

    pan_midi[j] = 64;
    samplePlayer[i].pan_midi = pan_midi[j];
    samplePlayer[i].pan = 0.5;

    pitch_midi[j] = 64;
    samplePlayer[i].pitch_midi = pitch_midi[j];
    if ( samplePlayer[i].sampleRate > 0 ) {
      samplePlayer[i].pitch = 1.0f / SAMPLE_RATE * samplePlayer[i].sampleRate;
    }
  };
}

inline void Sampler::SetNoteVolume_Midi( uint8_t data1) {
  volume_midi[ selectedNote + 1 ] = data1;
#ifdef DEBUG_MIDI
  DEBF("Sampler - Note[%d].midi_vol: %d\n",  selectedNote, data1 );
#endif
}

inline void Sampler::SetNotePan_Midi( uint8_t data1) {
  /*
    samplePlayer[ selectedNote ].pan_midi = data1;
    float value = MIDI_NORM * (float)data1;
    samplePlayer[ selectedNote ].pan =  value;
    #ifdef DEBUG_SAMPLER
    DEBF("Sampler - Note[%d].pan: %0.2f\n",  selectedNote, samplePlayer[ selectedNote ].pan );
    #endif
  */
  pan_midi[ selectedNote + 1 ] = data1;
#ifdef DEBUG_MIDI
  DEBF("Sampler - Note[%d].midi_pan: %d\n",  selectedNote, data1 );
#endif
}


inline void Sampler::SetNoteDecay_Midi( uint8_t data1) {
  /*
    samplePlayer[ selectedNote ].decay_midi = data1;
    float value = MIDI_NORM * (float)data1;
    // samplePlayer[ selectedNote ].decay = 1.0f - (0.000005f * pow( 5000.0f, 1.0f - value) );
    samplePlayer[ selectedNote ].decay = 1.0f -  value * 0.05 ;
    #ifdef DEBUG_SAMPLER
    DEBF("Sampler - Note[%d].decay: %0.2f\n",  selectedNote, samplePlayer[ selectedNote ].decay);
    #endif
  */

#ifdef DEBUG_MIDI
  DEBF("Sampler - Note[%d].decay_midi: %d\n",  selectedNote, data1);
#endif
  decay_midi[ selectedNote + 1 ] = data1;
}


inline void Sampler::SetNoteOffset_Midi( uint8_t data1) {
  /*
    samplePlayer[ selectedNote ].offset_midi = data1;
    #ifdef DEBUG_SAMPLER
    DEBF("Sampler - Note[%d].offset: %0.2f\n",  selectedNote, samplePlayer[ selectedNote ].offset_midi);
    #endif
  */

#ifdef DEBUG_MIDI
  DEBF("Sampler - Note[%d].offset_midi: %d\n",  selectedNote, data1);
#endif
  offset_midi[ selectedNote + 1 ] = data1;
}

inline void Sampler::SetSoundPitch_Midi( uint8_t data1) {
  /*
    samplePlayer[ selectedNote ].pitch_midi = data1;
    SetSoundPitch( MIDI_NORM * data1 );
  */
#ifdef DEBUG_MIDI
  DEBF("Sampler - Note[%d].pitch_midi: %d\n",  selectedNote, data1);
#endif
  pitch_midi[ selectedNote + 1 ] = data1;
}

inline void Sampler::SetSoundPitch(float value) {
  samplePlayer[ selectedNote ].pitch = pow( 2.0f, 4.0f * ( value - 0.5f ) );
#ifdef DEBUG_MIDI
  DEBF("Sampler - Note[%d] pitch: %0.3f\n",  selectedNote, samplePlayer[ selectedNote ].pitch );
#endif
}


void Sampler::NoteOn( uint8_t note, uint8_t vol ) {

  /* check for null to avoid division by zero */
  if ( sampleInfoCount == 0 ) {
    return;
  }
  int j = note % repeat;
  int param_i = j + 1;

  if ( is_muted[ param_i ] == true) {
    return;
  }

#ifdef GROUP_HATS
  const uint8_t closedHatSlot = directSlotKit ? CH_NUMBER : 6;
  const uint8_t openHatSlot = directSlotKit ? OH_NUMBER : 7;
  if (j == closedHatSlot) {
    samplePlayer[openHatSlot].active = false;
  } else if (j == openHatSlot) {
    samplePlayer[closedHatSlot].active = false;
  }
#endif

#ifdef DEBUG_MIDI
  DEBF("note %d on volume %d\n", note, vol );
 // DEBF("Filename: %s \n", samplePlayer[ j ].filename );
#endif
  /*
    if( global_pitch_decay_midi != global_pitch_decay_midi_old ){
    global_pitch_decay_midi_old = global_pitch_decay_midi;
    if( global_pitch_decay_midi < 63 ){
      global_pitch_decay = (float) (65-global_pitch_decay_midi)/100; // good from -0.2 to +1.0
    }else if( global_pitch_decay_midi > 65 ){
      global_pitch_decay = (float) global_pitch_decay_midi/65; // good from -0.2 to +1.0
    }else{
      global_pitch_decay = 0.0f;
    }
    }
  */

  if ( volume_midi[ param_i ] != samplePlayer[ j ].volume_midi ) {
#ifdef DEBUG_MIDI
    DEB("Volume");
    DEBUG( j );
    DEB(" samplePlayer");
    DEBUG( volume_midi[ param_i ] );
#endif
    samplePlayer[ j ].volume_midi = volume_midi[ param_i ];
  }

  if ( decay_midi[ param_i ] != samplePlayer[ j ].decay_midi ) {
#ifdef DEBUG_MIDI
    DEB("Decay");
    DEBUG( j );
    DEB(" samplePlayer");
    DEBUG( decay_midi[ param_i ] );
#endif
    samplePlayer[ j ].decay_midi = decay_midi[ param_i ];
    float value = MIDI_NORM * decay_midi[ param_i ];
    samplePlayer[ j ].decay = 1 - (0.000005 * pow( 5000, 1.0f - value) );
  }

  if ( pitch_midi[ param_i ] != samplePlayer[ j ].pitch_midi ) {
#ifdef DEBUG_MIDI
    DEB("Pitch");
    DEBUG( j );
    DEB(" samplePlayer");
    DEBUG( pitch_midi[param_i ] );
#endif
    samplePlayer[ j ].pitch_midi = pitch_midi[ param_i ];
    float value = MIDI_NORM * pitch_midi[ param_i ];
    samplePlayer[ j ].pitch = pow( 2.0f, 4.0f * ( value - 0.5f ) );
  }

  if ( pan_midi[ param_i ] != samplePlayer[ j ].pan_midi ) {
#ifdef DEBUG_MIDI
    DEB("Pan");
    DEBUG( j );
    DEB(" samplePlayer");
    DEBUG( pan_midi[ param_i ] );
#endif
    samplePlayer[ j ].pan_midi = pan_midi[ param_i ];
    float value = MIDI_NORM * pan_midi[ param_i ];
    samplePlayer[ j ].pan = value;
  }

  if ( offset_midi[ param_i ] != samplePlayer[ j ].offset_midi ) {
#ifdef DEBUG_MIDI
    DEB("Attack Offset");
    DEBUG( j );
    DEB(" samplePlayer");
    DEBUG( offset_midi[ param_i ] );
#endif

    samplePlayer[ j ].offset_midi = offset_midi[ param_i ];
  }

  if ( pitchdecay_midi[ param_i ] != samplePlayer[ j ].pitchdecay_midi ) {

    samplePlayer[ j ].pitchdecay_midi = pitchdecay_midi[ param_i ];
    samplePlayer[ j ].pitchdecay = 0.0f; // default
    if ( samplePlayer[ j ].pitchdecay_midi < 63 ) {
      samplePlayer[ j ].pitchdecay = (float) (63 - samplePlayer[ j ].pitchdecay_midi ) / 20.0f; // good from -0.2 to +1.0
    } else if ( samplePlayer[ j ].pitchdecay_midi > 65 ) {
      samplePlayer[ j ].pitchdecay = (float) - ( samplePlayer[ j ].pitchdecay_midi - 65) / 30.0f; // good from -0.2 to +1.0
    }
#ifdef DEBUG_MIDI
    DEB("PitchDecay");
    DEBUG( j );
    DEB(" samplePlayer ");
    DEB( pitchdecay_midi[ param_i ] );
    DEB(" FloatValue: " );
    DEBUG( samplePlayer[ j ].pitchdecay );
#endif
  }


  samplePlayerS *newSamplePlayer = &samplePlayer[j];

  if ( newSamplePlayer->active ) {
    /* add last output signal to slow release to avoid noise */
    slowRelease = newSamplePlayer->signal;
  }

  newSamplePlayer->samplePosF = 4.0f * newSamplePlayer->offset_midi; // 0.0f;
  newSamplePlayer->samplePos  = 4 * newSamplePlayer->offset_midi; // 0;

  newSamplePlayer->volume = vol * MIDI_NORM * newSamplePlayer->volume_midi * MIDI_NORM;
  newSamplePlayer->vel    = 1.0f;
 // newSamplePlayer->dataIn = 0;
  newSamplePlayer->sampleSeek = 44 + 4 * newSamplePlayer->offset_midi; // 16 Bit-Samples wee nee

  newSamplePlayer->active = true;
}

void Sampler::NoteOff( uint8_t note ) {
  /*
     nothing to do yet
     we could stop samples if we want to
  */
  if ( sampleInfoCount == 0 ) {
    return;
  }
  // int j = note % sampleInfoCount;
  // samplePlayer[j]->active = false;
}

void Sampler::allNotesOff() {
  for (int i = 0; i < sampleInfoCount; i++) {
    samplePlayer[i].active = false;
    samplePlayer[i].samplePos = 0;
    samplePlayer[i].samplePosF = 0.0f;
  }
}

void Sampler::SetPlaybackSpeed( float value ) {
  value = pow( 2.0f, 4.0f * (value - 0.5) );
  DEBF( "SetPlaybackSpeed: %0.2f\n", value );
  sampler_playback = value;
}

void Sampler::SetProgram( uint8_t prog ) {
  progNumber = prog ;
  Init();
}


void Sampler::PitchBend(int number) {
  //-8192 to 8191, 0 = original pitch
}

void Sampler::ParseCC(uint8_t cc_number , uint8_t cc_value) {
  switch (cc_number) {
    case CC_808_VOLUME:
      SetVolume( cc_value * MIDI_NORM );
      break;
    case CC_808_NOTE_PAN:
      SetNotePan_Midi( cc_value );
      break;
    case CC_808_RESO:
      Effects.SetResonance( cc_value * MIDI_NORM );
      break;
    case CC_808_CUTOFF:
      Effects.SetCutoff( cc_value * MIDI_NORM );
      break;
    case CC_808_NOTE_ATTACK:
      SetNoteOffset_Midi( cc_value );
      break;
    case CC_808_NOTE_DECAY:
      SetNoteDecay_Midi( cc_value );
      break;
    case CC_808_PITCH:
      SetSoundPitch_Midi ( cc_value );
      break;
    case CC_808_DELAY_SEND:
      _sendDelay = cc_value * MIDI_NORM;
      break;
    case CC_808_REVERB_SEND:
      _sendReverb = cc_value * MIDI_NORM;
      break;
    case CC_808_DISTORTION:
      if ( cc_value == 0 ) Effects.SetBitCrusher(0.0f);
      else Effects.SetBitCrusher( 0.66f + (cc_value * MIDI_NORM * 0.23f) );
      break;
    case CC_808_NOTE_SEL:
      SelectNote( cc_value );
      break;
    case CC_808_BD_DECAY:
      SelectNote( 0 ); // BD
      SetNoteDecay_Midi( cc_value );
      break;
    case CC_808_BD_TONE:
      SelectNote( 0 ); // BD
      SetSoundPitch_Midi ( cc_value );
      break;
    case CC_808_BD_LEVEL:
      SelectNote( 0 ); // BD
      SetNoteVolume_Midi ( cc_value );
      break;
    case CC_808_SD_SNAP:
      SelectNote( 1 ); // SD
      SetNoteDecay_Midi( cc_value );
      break;
    case CC_808_SD_TONE:
      SelectNote( 1 ); // SD
      SetSoundPitch_Midi( cc_value );
      break;
    case CC_808_SD_LEVEL:
      SelectNote( 1 ); // SD
      SetNoteVolume_Midi( cc_value );
      break;
    case CC_808_CH_TUNE:
      SelectNote(directSlotKit ? 2 : 6); // CH: direct slot 003, legacy slot 007
      SetSoundPitch_Midi( cc_value );
      break;
    case CC_808_CH_LEVEL:
      SelectNote(directSlotKit ? 2 : 6); // CH: direct slot 003, legacy slot 007
      SetNoteVolume_Midi( cc_value );
      break;
    case CC_808_OH_TUNE:
      SelectNote(directSlotKit ? 3 : 7); // OH: direct slot 004, legacy slot 008
      SetSoundPitch_Midi( cc_value );
      break;
    case CC_808_OH_LEVEL:
      SelectNote(directSlotKit ? 3 : 7); // OH: direct slot 004, legacy slot 008
      SetNoteVolume_Midi( cc_value );
      break;
    case CC_808_OH_DECAY:
      SelectNote(directSlotKit ? 3 : 7); // OH: direct slot 004, legacy slot 008
      SetNoteDecay_Midi( cc_value );
      break;
      /*
        #define CC_808_BD_TONE    21  // Specific per drum control
        #define CC_808_BD_DECAY   23
        #define CC_808_BD_LEVEL   24
        #define CC_808_SD_TONE    25
        #define CC_808_SD_SNAP    26
        #define CC_808_SD_LEVEL   29
        #define CC_808_CH_TUNE    61
        #define CC_808_CH_LEVEL   63
        #define CC_808_OH_TUNE    80
        #define CC_808_OH_DECAY   81
        #define CC_808_OH_LEVEL   82
      */
  }

}


void Sampler::Process( float *left, float *right ) {


  float signal_l = 0.0f;
  //signal_l += slowRelease;
  float signal_r = 0.0f;
  //signal_r += slowRelease;

  //slowRelease = slowRelease * 0.99; // go slowly to zero

  for ( int i = 0; i < sampleInfoCount; i++ ) {

    if ( samplePlayer[i].active  ) {
      samplePlayer[i].samplePos = samplePlayer[i].samplePosF;
      samplePlayer[i].samplePos -= samplePlayer[i].samplePos % 2;

      uint32_t dataOut = samplePlayer[i].samplePos;
      //  DEBUG(dataOut);

      //
      // reconstruct signal from data
      //
      uint8_t byte2 , byte1;
      union {
        uint16_t u16;
        int16_t s16;
      } sampleU;
      byte1 = RamCache[samplePlayer[i].sampleStart + dataOut];
      byte2 = RamCache[samplePlayer[i].sampleStart + dataOut + 1];
      sampleU.s16 = (((uint16_t)byte2) << 8U) + (uint16_t)byte1;

      samplePlayer[i].signal = (float)(samplePlayer[i].volume) * ((float)sampleU.s16) * 0.00005f;

      signal_l += samplePlayer[i].signal * samplePlayer[i].vel * ( 1 - samplePlayer[i].pan );

      signal_r += samplePlayer[i].signal * samplePlayer[i].vel *  samplePlayer[i].pan;

      samplePlayer[i].vel *= samplePlayer[i].decay;

      samplePlayer[i].samplePos += 2; // we have consumed two bytes

      if ( samplePlayer[i].pitchdecay > 0.0f ) {
        samplePlayer[i].samplePosF += 2.0f * sampler_playback * ( samplePlayer[i].pitch + samplePlayer[i].pitchdecay * samplePlayer[i].vel ); // we have consumed two bytes
      } else {
        samplePlayer[i].samplePosF += 2.0f * sampler_playback * ( samplePlayer[i].pitch + samplePlayer[i].pitchdecay * (1 - samplePlayer[i].vel) ); // we have consumed two bytes
      }

 //     samplePlayer[i].samplePosF += 2.0f * sampler_playback * ( samplePlayer[i].pitch  ); // we have consumed two bytes
      if ( samplePlayer[i].samplePos >= samplePlayer[i].sampleSize ) {
        samplePlayer[i].active = false;
        
        samplePlayer[i].samplePos = 0;
        samplePlayer[i].samplePosF = 0.0f;
      }
    }
  }
  Effects.Process( &signal_l, &signal_r );
 // *left  = signal_l * _volume;
 // *right =  signal_r * _volume;
   *left  = fclamp(signal_l * _volume, -1.0f, 1.0f);
   *right = fclamp(signal_r * _volume, -1.0f, 1.0f);
  // *left  = fast_shape(signal_l * _volume);
  // *right = fast_shape(signal_r * _volume);
}

// ============================================================
// KIT BROWSER — scan /ACIDBOX/KITS/ subdirectories
// Uses the same nonce-based .dircache pattern as the MYNAH browser.
// ============================================================

uint32_t Sampler::kitSessionNonce = 0;

void Sampler::readKitDirCache() {
    if (kitSessionNonce == 0) return;

    File f = SD_MMC.open("/ACIDBOX/KITS/.dircache", FILE_READ);
    if (!f) return;

    String firstLine = f.readStringUntil('\n');
    firstLine.trim();
    if (!firstLine.startsWith("nonce:")) { f.close(); return; }
    uint32_t storedNonce = (uint32_t)strtoul(firstLine.substring(6).c_str(), nullptr, 16);
    if (storedNonce != kitSessionNonce) { f.close(); return; }

    kitNames.clear();
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() < 2) continue; // "d:" + at least 1 char
        if (line.charAt(0) != 'd' || line.charAt(1) != ':') continue;
        kitNames.push_back(std::string(line.substring(2).c_str()));
    }
    f.close();
    Serial.printf("[KitCache] HIT  /ACIDBOX/KITS  (%d kits)\n", (int)kitNames.size());
}

void Sampler::writeKitDirCache() {
    if (kitSessionNonce == 0) return;

    char cachePath[160];
    snprintf(cachePath, sizeof(cachePath), "/ACIDBOX/KITS/.dircache");
    if (SD_MMC.exists(cachePath)) SD_MMC.remove(cachePath);

    File f = SD_MMC.open(cachePath, FILE_WRITE);
    if (!f) {
        Serial.printf("[KitCache] WARN: cannot write cache\n");
        return;
    }

    f.printf("nonce:%08X\n", kitSessionNonce);
    for (const auto& name : kitNames) {
        f.printf("d:%s\n", name.c_str());
    }
    f.close();
    Serial.printf("[KitCache] WROTE /ACIDBOX/KITS  (%d kits)\n", (int)kitNames.size());
}

void Sampler::ScanKitDirectories() {
    kitNames.clear();
    kitSelectIndex = 0;

    if (SD_MMC.cardType() == CARD_NONE) {
        kitListReady = true;
        return;
    }

    // Generate nonce once per session
    if (kitSessionNonce == 0) {
        kitSessionNonce = esp_random();
        if (kitSessionNonce == 0) kitSessionNonce = 1;
        Serial.printf("[KitCache] Session nonce: %08X\n", kitSessionNonce);
    }

    // Try cache first
    readKitDirCache();
    if (!kitNames.empty()) {
        kitListReady = true;
        return;
    }

    // Full enumeration
    File root = SD_MMC.open("/ACIDBOX/KITS");
    if (!root || !root.isDirectory()) {
        Serial.println("[Kits] /ACIDBOX/KITS not found on SD");
        kitListReady = true;
        return;
    }

    File entry = root.openNextFile();
    while (entry) {
        if (entry.isDirectory()) {
            String name = entry.name();
            // Extract just the folder name (last component)
            int lastSlash = name.lastIndexOf('/');
            if (lastSlash >= 0) name = name.substring(lastSlash + 1);
            if (name.length() > 0 && !name.startsWith(".") && !name.equalsIgnoreCase("System Volume Information")) {
                kitNames.push_back(std::string(name.c_str()));
                Serial.printf("[Kits] Found: %s\n", name.c_str());
            }
        }
        entry = root.openNextFile();
    }
    root.close();

    // Sort alphabetically
    std::sort(kitNames.begin(), kitNames.end());

    writeKitDirCache();
    kitListReady = true;
    Serial.printf("[Kits] Found %d kit directories\n", (int)kitNames.size());
}

void Sampler::SetKitIndex(int i) {
    if (i < 0) i = (int)kitNames.size() - 1;
    if (i >= (int)kitNames.size()) i = 0;
    kitSelectIndex = i;
}

const char* Sampler::GetKitName(int i) {
    if (i < 0 || i >= (int)kitNames.size()) return "---";
    return kitNames[i].c_str();
}

const char* Sampler::GetCurrentKitName() {
    if (kitSelectIndex < 0 || kitSelectIndex >= (int)kitNames.size()) return "---";
    return kitNames[kitSelectIndex].c_str();
}

void Sampler::LoadKitByIndex(int i) {
    if (i < 0 || i >= (int)kitNames.size()) return;
    // Mute audio output during loading to avoid clicks/pops/distortion
    // from the PSRAM re-allocation and SD card reads.
    float savedVol = _volume;
    SetVolume(0.0f);
    allNotesOff();

    // Extract numeric part from folder name like "KITS1", "KITS2", or "1", "kit_3"
    // Default to index+1 if we can't parse it
    const char* name = kitNames[i].c_str();
    int kitNum = i + 1; // fallback
    // Try to extract trailing digits
    const char* p = name;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            kitNum = atoi(p);
            break;
        }
        p++;
    }
    Serial.printf("[Kits] Loading kit %d (folder: %s)\n", kitNum, name);
    SetProgram((uint8_t)kitNum);

    // Restore volume after load completes
    SetVolume(savedVol);
}