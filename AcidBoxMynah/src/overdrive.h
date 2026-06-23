#pragma once

#include <Arduino.h>
#include "general.h"
#include "rosic_BiquadFilter.h"

class Overdrive
{
  public:
    Overdrive() {}
    ~Overdrive() {}
 
    void Init();
    float Process(float in);
    void SetDrive(float drive);

  private:
    float _drive;
    float _pre_gain;
    float _post_gain;
    BiquadFilter midBoost1;
    BiquadFilter midBoost2;
}; 
