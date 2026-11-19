// Versioned configuration record persisted by services/configuration (§17).
// Pure C. Work package WP8 (Stage 5).
//
// Stored in two NVS slots (A/B). A change writes the inactive slot with the
// next revision; boot loads the newest slot whose magic, schema and CRC are
// valid, else the compiled defaults (policy_config.h). The revision is what
// every Reading carries as configRevision.
//
// Wi-Fi credentials and the SGP30 baseline are NOT in here: they have their
// own NVS keys and write rates.
//
// The sensor mask and the pH / TDS / light calibrations are set by commands.
// The lamp policy has no command, so it always comes from the compiled
// defaults (policy_config.h). configBlobRefreshDefaults() copies the compiled
// values on every load, so a re-measured value in the source is never shadowed
// by an old copy in flash - except for a calibration whose CAL_BIT_* is set in
// calOverridden: the user fitted it on the device and it is kept.
//
// Schema 2 (calibration commands) added `light` and `calOverridden`. A schema 1
// slot is migrated at boot (configBlobMigrateV1) so the sensor mask survives
// the firmware update.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "domain/calibration.h"
#include "domain/lamp_control.h"
#include "domain/sensor_set.h"

#define CONFIG_BLOB_MAGIC 0x42445948u  // "HYDB" little-endian
#define CONFIG_BLOB_SCHEMA 2u

typedef struct ConfigBlob {
  uint32_t magic;
  uint16_t schema;
  uint16_t revision;          // 1.., wraps past 0xFFFF to 1
  SensorMask sensorEnabled;
  bool lampAutoEnabled;
  uint8_t calOverridden;      // CAL_BIT_* (calibration.h): fitted by the user
  uint8_t reserved;           // keep zero (CRC covers every byte)
  LampControlConfig lamp;
  LinearCalibration ph;
  LinearCalibration tds;
  LinearCalibration light;    // x = pin mV, y = lux
  uint32_t crc;               // CRC-32 (IEEE) of every byte before this field
} ConfigBlob;

// Schema 1 layout (firmware up to Stage 8.4), only for migration.
typedef struct ConfigBlobV1 {
  uint32_t magic;
  uint16_t schema;
  uint16_t revision;
  SensorMask sensorEnabled;
  bool lampAutoEnabled;
  uint8_t reserved[2];
  LampControlConfig lamp;
  LinearCalibration ph;
  LinearCalibration tds;
  uint32_t crc;
} ConfigBlobV1;

// Compiled defaults of every field except the sensor mask.
typedef struct ConfigDefaults {
  bool lampAutoEnabled;
  LampControlConfig lamp;
  LinearCalibration ph;
  LinearCalibration tds;
  LinearCalibration light;
} ConfigDefaults;
// The CRC covers padding bytes too: copy a sealed blob with memcpy (struct
// assignment may not preserve padding), or re-seal after assigning.

uint32_t crc32Ieee(const void* data, uint32_t len);

// Revision 1, all sensors enabled, nothing overridden, the given defaults.
void configBlobInit(ConfigBlob* b, const ConfigDefaults* d);

void configBlobSeal(ConfigBlob* b);

// Magic, schema, CRC and value ranges (lamp band, faultsToOff, sensor mask).
bool configBlobValid(const ConfigBlob* b);

// Newest valid of two slots (either may be NULL or invalid); NULL if none.
// Revisions compare with wrap-around (serial-number arithmetic).
const ConfigBlob* configBlobPickNewest(const ConfigBlob* a, const ConfigBlob* b);

// next = cur with the sensor switched, a new revision and a fresh CRC.
// Returns false (next untouched) if nothing would change.
bool configBlobWithSensor(const ConfigBlob* cur, SensorId sensor, bool enabled, ConfigBlob* next);

uint16_t configRevisionNext(uint16_t revision);

// Copies the compiled lamp policy and every calibration NOT marked in
// calOverridden into b. If anything differed, b gets a new revision and a
// fresh CRC and the result is true (the caller stores it); otherwise b is
// untouched.
bool configBlobRefreshDefaults(ConfigBlob* b, const ConfigDefaults* d);

// next = cur with the sensor's calibration replaced, a new revision and a
// fresh CRC. overridden = true for a user fit (its revision becomes the new
// config revision, reported as cal_rev), false for a reset to the compiled
// default. false (next untouched) for a sensor without a calibration.
bool configBlobWithCalibration(const ConfigBlob* cur, SensorId sensor, const LinearCalibration* cal,
                               bool overridden, ConfigBlob* next);

// The stored calibration of a calibratable sensor (NULL otherwise).
const LinearCalibration* configBlobCalibration(const ConfigBlob* b, SensorId sensor);
uint8_t configCalibrationBit(SensorId sensor);  // CAL_BIT_*, 0 if none

// A valid schema 1 blob (len must be sizeof(ConfigBlobV1)) becomes a schema 2
// blob with the same revision and sensor mask, the defaults for everything
// else and nothing overridden. false if v1 is not valid.
bool configBlobMigrateV1(const void* v1, uint32_t len, const ConfigDefaults* d, ConfigBlob* out);
