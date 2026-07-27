/*
 * This is a simple implementation of a delay line
 * - level adjustable
 * - feedback
 * - length adjustable
 *
 * Author: Marcel Licence
 */

#ifndef FX_DELAY_H
#define FX_DELAY_H

class FxDelay {
	public:
		FxDelay() {}
    
		// max delay can be changed but changes also the PSRAM consumption
		void Init( void ){
#ifdef NO_PSRAM
  #define MAX_DELAY 42000/4 // 
    // heap_caps_print_heap_info(MALLOC_CAP_8BIT);
     delayLine_l = (float *)malloc(sizeof(float) * MAX_DELAY);
      if( delayLine_l == NULL){
        DEBUG("No more RAM for delay L !");
      } else {
        DEBUG("Memory allocated for delay L");
      }
   // heap_caps_print_heap_info(MALLOC_CAP_8BIT);
      delayLine_r = (float *)malloc(sizeof(float) * MAX_DELAY);
      if( delayLine_r == NULL ){
        DEBF("No more RAM for delay R !");
      } else {
        DEBUG("Memory allocated for delay R");
      }
      Reset();
      
#else
#define MAX_DELAY SAMPLE_RATE // 1 second
//#define MAX_DELAY 38000 // 1 second
			delayLine_l = (float *)ps_malloc(sizeof(float) * MAX_DELAY);
			if( delayLine_l == NULL){
				DEBF("No more PSRAM!\n");
			}
			delayLine_r = (float *)ps_malloc(sizeof(float) * MAX_DELAY);
			if( delayLine_r == NULL ){
				DEBF("No more PSRAM!\n");
			}
			Reset();
#endif
		};

		void Reset( void ){
			for (int i = 0; i < MAX_DELAY; i++ ){
				delayLine_l[i] = 0;
				delayLine_r[i] = 0;
			}
			delayTimeBeats = 1.5f; // dotted quarter note
			recalcDelayLen(currentBpm);
			delayToMix = 1.0f;
			delayFeedback = 0.2f;
		};

		inline void Process( float *signal_l, float *signal_r ){

		  delayLine_l[delayIn] = *signal_l;
		  delayLine_r[delayIn] = *signal_r;
		  
		  delayOut = delayIn + (1 + MAX_DELAY - delayLen);
		  
		  if( delayOut >= MAX_DELAY ){
			  delayOut -= MAX_DELAY;
		  }

		  *signal_l += delayLine_l[delayOut] * delayToMix;
		  *signal_r += delayLine_r[delayOut] * delayToMix;
		  
		  delayLine_l[delayIn] += delayLine_l[delayOut] * delayFeedback;
		  delayLine_r[delayIn] += delayLine_r[delayOut] * delayFeedback;
		  
		  delayIn ++;
		  
		  if( delayIn >= MAX_DELAY ){
			  delayIn = 0;
		  }
		};

		inline void SetFeedback( float value){
			delayFeedback = value;
#ifdef DEBUG_FX
			DEBF("delay feedback: %0.3f\n", value);
#endif
		};

		inline void SetLevel( float value ){
			delayToMix = value;
#ifdef DEBUG_FX
			DEBF("delay level: %0.3f\n", value);
#endif
		};

		inline void SetLength( float value ){
			// Convert the 0-1 normalized value to a beat count (number of quarter notes)
			// This makes the delay tempo-synced: when BPM changes, the beat count stays
			// the same and the sample count is recalculated.
			delayTimeBeats = value * (float)MAX_DELAY * currentBpm / (60.0f * (float)SAMPLE_RATE);
			recalcDelayLen(currentBpm);
#ifdef DEBUG_FX
			DEBF("delay length: %0.3fms\n", delayLen * (1000.0f / ((float)SAMPLE_RATE)));
#endif
		};

		inline void SetBPM( float bpm ){
			currentBpm = bpm;
			recalcDelayLen(bpm);
		};

	private:
		// Recalculate the delay sample count from the stored beat count and current BPM
		inline void recalcDelayLen( float bpm ){
			float len = delayTimeBeats * 60.0f / bpm * (float)SAMPLE_RATE;
			if( len >= (float)MAX_DELAY ) len = (float)(MAX_DELAY - 1);
			if( len < 1.0f ) len = 1.0f;
			delayLen = (uint32_t)len;
		};

		//  module variables
		float *delayLine_l;
		float *delayLine_r;
		float delayToMix = 0.2f;
		float delayFeedback = 0.1f;
		 
		uint32_t delayLen = MAX_DELAY/4;
		uint32_t delayIn = 0;
		uint32_t delayOut = 0;

		// Tempo sync members
		float delayTimeBeats = 1.5f;  // delay length in quarter notes (default: dotted quarter)
		float currentBpm = 130.0f;     // current BPM for tempo-synced delay calculation

};

#endif // FX_DELAY_H