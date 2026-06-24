#ifndef CONFIG_H
#define CONFIG_H

#define PROG_NAME       "ESP32 AcidBox"
#define VERSION         "v.1.3.3"

#define BOARD_HAS_UART_CHIP

#define JUKEBOX                 // real-time endless auto-compose acid tunes
#define JUKEBOX_PLAY_ON_START   // should it play on power on, or should it wait for "boot" button to be pressed
//#define MIDI_RAMPS              // this is what makes automated Cutoff-Reso-FX turn
//#define TEST_POTS               // experimental interactivity with potentiometers connected to POT_PINS[] defined below

//#define USE_INTERNAL_DAC      // use this for testing, SOUND QUALITY SACRIFICED: NOISY 8BIT STEREO
// NO_PSRAM is NOT defined — the ESP32-S3 SuperMini has 2MB quad-SPI PSRAM.
// See boards/supermini.json: "psram_size": "2MB"

//#define LOLIN_RGB               // Flashes the LOLIN S3 built-in RGB-LED

//#define DEBUG_ON              // note that debugging eats ticks initially belonging to real-time tasks, so sound output will be spoild in most cases, turn it off for production build
//#define DEBUG_MASTER_OUT      // serial monitor plotter will draw the output waveform
//#define DEBUG_SAMPLER
//#define DEBUG_SYNTH
//#define DEBUG_JUKEBOX
//#define DEBUG_FX
//#define DEBUG_TIMING
//#define DEBUG_MIDI

#define MIDI_VIA_SERIAL       // use this option to enable Hairless MIDI on Serial port @115200 baud (USB connector), THIS WILL BLOCK SERIAL DEBUGGING as well
//#define MIDI_VIA_SERIAL2        // use this option if you want to operate by standard MIDI @31250baud, UART2 (Serial2), 
#define MIDIRX_PIN      4       // this pin is used for input when MIDI_VIA_SERIAL2 defined (note that default pin 17 won't work with PSRAM)
#define MIDITX_PIN      15      // this pin will be used for output (not implemented yet) when MIDI_VIA_SERIAL2 defined

#define POT_NUM 3
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define I2S_BCLK_PIN    6       // I2S BIT CLOCK pin (BCL BCK CLK)
#define I2S_WCLK_PIN    7       // I2S WORD CLOCK pin (WCK WCL LCK)
#define I2S_DOUT_PIN    8       // to I2S DATA IN pin (DIN D DAT)
const uint8_t POT_PINS[POT_NUM] = {15, 16, 17};
#elif defined(CONFIG_IDF_TARGET_ESP32)
#define I2S_BCLK_PIN    5       // I2S BIT CLOCK pin (BCL BCK CLK)
#define I2S_WCLK_PIN    19      // I2S WORD CLOCK pin (WCK WCL LCK)
#define I2S_DOUT_PIN    18      // to I2S DATA IN pin (DIN D DAT)
const uint8_t POT_PINS[POT_NUM] = {34, 35, 36};
#endif


// float bpm = 130.0f; // Must not be defined in header

#define MAX_CUTOFF_FREQ 4000.0f
#define MIN_CUTOFF_FREQ 250.0f

#ifdef USE_INTERNAL_DAC
#define SAMPLE_RATE     22050   // price for increasing this value having NO_PSRAM is less delay time, you won't hear the difference at 8bit/sample
#else
#define SAMPLE_RATE     44100   // 44100 seems to be the right value, 48000 is also OK. Other values haven't been tested.
#endif

// const float DIV_SAMPLE_RATE = 1.0f / (float)SAMPLE_RATE;
// const float DIV_2SAMPLE_RATE = 0.5f / (float)SAMPLE_RATE;
// const float TWO_DIV_16383 = 1.22077763e-04f;
// Since these are const floats, C++ gives them internal linkage so it is fine, but macros are safer for header files
#define DIV_SAMPLE_RATE (1.0f / (float)SAMPLE_RATE)
#define DIV_2SAMPLE_RATE (0.5f / (float)SAMPLE_RATE)
#define TWO_DIV_16383 (1.22077763e-04f)

#define TABLE_BIT  		        10UL				// bits per index of lookup tables for waveforms, exp(), sin(), cos() etc. 10 bit means 2^10 = 1024 samples
#define TABLE_SIZE            (1<<TABLE_BIT)        // samples used for lookup tables (it works pretty well down to 32 samples due to linear interpolation, so listen and free some memory at your choice)
#define TABLE_MASK  	        (TABLE_SIZE-1)        // strip MSB's and remain within our desired range of TABLE_SIZE
#define CICLE_INDEX(i)        (((int32_t)(i)) & TABLE_MASK ) // this way we can operate with periodic functions or waveforms without phase-reset ("if's" are pretty costly in the matter of time)

#define DIV_TABLE_SIZE (1.0f / (float)TABLE_SIZE)

// illinear shaper, choose preferred parameters basing on your audial experience
//#define SHAPER_USE_TANH             // use tanh() function to introduce illeniarity into the filter and compressor, it won't impact performance as this will be pre-calculated 
#define SHAPER_USE_CUBIC              // use the cubic curve to introduce illeniarity into the filter and compressor, it won't impact performance as this will be pre-calculated

// curve will be pre-calculated within -X..X range, outside this interval the function is assumed to be flat
#define SHAPER_LOOKUP_MAX 5.0f        // maximum X argument value for tanh(X) lookup table, tanh(X)~=1 if X>4 
#define SHAPER_LOOKUP_COEF ((float)TABLE_SIZE / SHAPER_LOOKUP_MAX)
#define DMA_BUF_LEN     32          // there should be no problems with low values, down to 32 samples, 64 seems to be OK with some extra
#define DMA_NUM_BUF     2           // I see no reasom to set more than 2 DMA buffers, but...

#define DMA_BUF_TIME ((uint32_t)(1000000.0f / (float)SAMPLE_RATE * (float)DMA_BUF_LEN)) // microseconds per buffer, used for debugging output of time-slots

#define SYNTH1_MIDI_CHAN        1
#define SYNTH2_MIDI_CHAN        2

#define DRUM_MIDI_CHAN          10

#define TWOPI (PI*2.0f)
#define MIDI_NORM (1.0f/127.0f)
#define ONE_DIV_PI (1.0f/PI)
#define ONE_DIV_TWOPI (1.0f/TWOPI)
#define FORMAT_LITTLEFS_IF_FAILED true

#define GROUP_HATS  // if so, instruments CH_NUMBER and OH_NUMBER will terminate each other (sampler module)
#define CH_NUMBER  6 // closed hat instrument number in kit (for groupping, zero-based)
#define OH_NUMBER  7 // open hat instrument number in kit (for groupping, zero-based)

// PSRAM configuration for ESP32-S3 SuperMini (2MB quad-SPI PSRAM)
// Memory budget:
//   ~352 KB  — delay buffer (44100 samples × 2 ch × 4 bytes)
//   ~1.5 MB  — sample cache (PSRAM_SAMPLER_CACHE)
//   ~172 KB  — headroom for stack, heap fragmentation, etc.
//   Total:   ~2.0 MB
#define PSRAM_SAMPLER_CACHE 1572864 // 1.5 MB — fits comfortably in 2MB PSRAM with delay buffer
#define SAMPLECNT       (7 * 12)    // how many samples we prepare (7 octaves by 12 samples)
#define DEFAULT_DRUMKIT 0           // kit 0 = folder /0/ on LittleFS (matches CreateDefaultSamples())

#define TINY 1e-32;

#ifndef LED_BUILTIN
#define LED_BUILTIN 0
#endif

#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))

#if (defined ARDUINO_LOLIN_S3_PRO) || (defined ARDUINO_USB_CDC_ON_BOOT) || (defined ARDUINO_ESP32S3_DEV)
#undef BOARD_HAS_UART_CHIP
#endif

#if (defined BOARD_HAS_UART_CHIP)
  #define MIDI_PORT_TYPE HardwareSerial
  #define MIDI_PORT Serial
  #define DEBUG_PORT Serial
#else
  #if (ESP_ARDUINO_VERSION_MAJOR < 3)
    #define MIDI_PORT_TYPE HWCDC
    #define MIDI_PORT Serial
    #define DEBUG_PORT Serial
  #else
    #define MIDI_PORT_TYPE HWCDC
    #define MIDI_PORT Serial
    #define DEBUG_PORT Serial
  #endif
#endif

#ifdef MIDI_VIA_SERIAL
  #undef DEBUG_ON
#endif

// debug macros
#ifdef DEBUG_ON
  #define DEB(...)    DEBUG_PORT.print(__VA_ARGS__) 
  #define DEBF(...)   DEBUG_PORT.printf(__VA_ARGS__)
  #define DEBUG(...)  DEBUG_PORT.println(__VA_ARGS__)
#else
  #define DEB(...)
  #define DEBF(...)
  #define DEBUG(...)
#endif


// normalizing matrices for TB filter and distortion/overdrive pairs
#define NORM1_DEPTH 1.0f 
#define NORM2_DEPTH 1.0f

#define cutoff_reso_avg (2.506875f)

#define wfolder_overdrive_avg (14.70303f)

extern float bpm;

extern const float cutoff_reso[16][16];
extern const float wfolder_overdrive[16][16];
extern const float tuning[128];

inline float fast_shape(float x);
static __attribute__((always_inline)) inline float one_div(float a);

#endif