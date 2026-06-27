#include <Arduino.h>
#include "general.h"
#include "synthvoice.h"
#include "sampler.h"

float bpm = 130.0f;

#include "fx_delay.h"
#ifndef NO_PSRAM
#include "fx_reverb.h"
#endif
#include "compressor.h"


void drums_generate() {
    for (int i=0; i < DMA_BUF_LEN; i++){
      Drums.Process( &drums_buf_l[current_gen_buf][i], &drums_buf_r[current_gen_buf][i] );      
    } 
}

void synth1_generate() {
    for (int i=0; i < DMA_BUF_LEN; i++){
      synth1_buf[current_gen_buf][i] = Synth1.getSample() ;      
    } 
}

void synth2_generate() {
    for (int i=0; i < DMA_BUF_LEN; i++){
      synth2_buf[current_gen_buf][i] = Synth2.getSample() ;      
    } 
}

float easeInExpo(float x) {
    return (x == 0.0f) ? 0.0f : std::exp2f(10.0f * x - 10.0f);
}

void IRAM_ATTR mixer() { // sum buffers 
#ifdef DEBUG_MASTER_OUT
  static float meter = 0.0f;
#endif
  static float synth1_out_l, synth1_out_r, synth2_out_l, synth2_out_r, drums_out_l, drums_out_r;
  static float dly_l, dly_r, rvb_l, rvb_r;
  static float mono_mix;
    dly_k1 = Synth1._sendDelay;
    dly_k2 = Synth2._sendDelay;
    dly_k3 = Drums._sendDelay;
#ifndef NO_PSRAM 
    rvb_k1 = Synth1._sendReverb;
    rvb_k2 = Synth2._sendReverb;
    rvb_k3 = Drums._sendReverb;
#endif
    for (int i=0; i < DMA_BUF_LEN; i++) { 
      drums_out_l = drums_buf_l[current_out_buf][i];
      drums_out_r = drums_buf_r[current_out_buf][i];

      synth1_out_l = Synth1.GetPan() * synth1_buf[current_out_buf][i];
      synth1_out_r = (1.0f - Synth1.GetPan()) * synth1_buf[current_out_buf][i];
      synth2_out_l = Synth2.GetPan() * synth2_buf[current_out_buf][i];
      synth2_out_r = (1.0f - Synth2.GetPan()) * synth2_buf[current_out_buf][i];

      
      dly_l = dly_k1 * synth1_out_l + dly_k2 * synth2_out_l + dly_k3 * drums_out_l; // delay bus
      dly_r = dly_k1 * synth1_out_r + dly_k2 * synth2_out_r + dly_k3 * drums_out_r;
      Delay.Process( &dly_l, &dly_r );
#ifndef NO_PSRAM
      rvb_l = rvb_k1 * synth1_out_l + rvb_k2 * synth2_out_l + rvb_k3 * drums_out_l; // reverb bus
      rvb_r = rvb_k1 * synth1_out_r + rvb_k2 * synth2_out_r + rvb_k3 * drums_out_r;
      Reverb.Process( &rvb_l, &rvb_r );

      mix_buf_l[current_out_buf][i] = (synth1_out_l + synth2_out_l + drums_out_l + dly_l + rvb_l);
      mix_buf_r[current_out_buf][i] = (synth1_out_r + synth2_out_r + drums_out_r + dly_r + rvb_r);
#else
      mix_buf_l[current_out_buf][i] = (synth1_out_l + synth2_out_l + drums_out_l + dly_l);
      mix_buf_r[current_out_buf][i] = (synth1_out_r + synth2_out_r + drums_out_r + dly_r);
#endif
      mono_mix = 0.5f * (mix_buf_l[current_out_buf][i] + mix_buf_r[current_out_buf][i]);
  //    Comp.Process(mono_mix);     // calculate gain based on a mono mix

      Comp.Process(drums_out_l*0.25f);  // calc compressor gain, side-chain driven by drums


      mix_buf_l[current_out_buf][i] = (Comp.Apply( 0.25f * mix_buf_l[current_out_buf][i]));
      mix_buf_r[current_out_buf][i] = (Comp.Apply( 0.25f * mix_buf_r[current_out_buf][i]));

      
#ifdef DEBUG_MASTER_OUT
      if ( i % 16 == 0) meter = meter * 0.95f + fabs( mono_mix); 
#endif
  //    mix_buf_l[current_out_buf][i] = fclamp(mix_buf_l[current_out_buf][i] , -1.0f, 1.0f); // clipper
  //    mix_buf_r[current_out_buf][i] = fclamp(mix_buf_r[current_out_buf][i] , -1.0f, 1.0f);
     mix_buf_l[current_out_buf][i] = fast_shape( mix_buf_l[current_out_buf][i]); // soft limitter/saturator
     mix_buf_r[current_out_buf][i] = fast_shape( mix_buf_r[current_out_buf][i]);
    }
#ifdef DEBUG_MASTER_OUT
  meter *= 0.95f;
  meter += fabs(mono_mix); 
  DEBF("out= %0.5f\r\n", meter);
#endif
}
