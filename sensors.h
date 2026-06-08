#pragma once
#include "i2c_bsp.h"

// Sensor backend: SHTC3 temperature/humidity over the shared I2C bus.
void sensorsBegin(I2cMasterBus &bus);       // probe + init; updates sensorsPresent()
bool sensorsPresent();                      // true if the SHTC3 answered at boot
bool sensorsRead(float *temp, float *hum);  // true on a good (CRC-checked) read
