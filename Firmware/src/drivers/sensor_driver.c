#include "drivers/sensor_driver.h"

const char* driverStatusName(DriverStatus s) {
  switch (s) {
    case DRIVER_OK: return "OK";
    case DRIVER_PENDING: return "PENDING";
    case DRIVER_READY: return "READY";
    case DRIVER_NOT_PRESENT: return "NOT_PRESENT";
    case DRIVER_FAULT: return "FAULT";
    case DRIVER_TIMEOUT: return "TIMEOUT";
    case DRIVER_UNPOWERED: return "UNPOWERED";
    case DRIVER_NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
  }
  return "?";
}
