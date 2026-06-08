#include "sensors.h"
#include "i2c_equipment.h"  // Shtc3Port
#include "logging.h"

static Shtc3Port *shtc3 = nullptr;
static bool present = false;

void sensorsBegin(I2cMasterBus &bus) {
  shtc3 = new Shtc3Port(bus);
  present = (shtc3->Shtc3_GetShtc3Id() != 0x00);
  if (present) logInfo("SHTC3 found.");
  else logError("SHTC3 not found.");
}

bool sensorsPresent() {
  return present;
}

bool sensorsRead(float *temp, float *hum) {
  if (!present || !shtc3) return false;
  if (shtc3->Shtc3_ReadTempHumi(temp, hum) == NO_ERROR) return true;
  logError("SHTC3 read failed");  // error reporting kept at the source
  return false;
}
