#include "domain/config_blob.h"

#include <stddef.h>
#include <string.h>

uint32_t crc32Ieee(const void* data, uint32_t len) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t crc = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < len; ++i) {
    crc ^= p[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
  }
  return ~crc;
}

static uint32_t blobCrc(const ConfigBlob* b) {
  return crc32Ieee(b, (uint32_t)offsetof(ConfigBlob, crc));
}

void configBlobInit(ConfigBlob* b, const ConfigDefaults* d) {
  memset(b, 0, sizeof(*b));  // padding bytes are part of the CRC
  b->magic = CONFIG_BLOB_MAGIC;
  b->schema = (uint16_t)CONFIG_BLOB_SCHEMA;
  b->revision = 1;
  b->sensorEnabled = SENSOR_MASK_ALL;
  b->lampAutoEnabled = d->lampAutoEnabled;
  memcpy(&b->lamp, &d->lamp, sizeof(b->lamp));
  memcpy(&b->ph, &d->ph, sizeof(b->ph));
  memcpy(&b->tds, &d->tds, sizeof(b->tds));
  memcpy(&b->light, &d->light, sizeof(b->light));
  configBlobSeal(b);
}

void configBlobSeal(ConfigBlob* b) { b->crc = blobCrc(b); }

bool configBlobValid(const ConfigBlob* b) {
  if (b == NULL || b->magic != CONFIG_BLOB_MAGIC || b->schema != CONFIG_BLOB_SCHEMA) return false;
  if (b->crc != blobCrc(b)) return false;
  if (b->revision == 0) return false;
  if ((b->sensorEnabled & (SensorMask)~SENSOR_MASK_ALL) != 0) return false;
  if ((b->calOverridden & (uint8_t)~CAL_BITS_ALL) != 0) return false;
  if (!(b->lamp.onBelowMv < b->lamp.offAboveMv) || b->lamp.faultsToOff == 0) return false;
  if (!linearCalibrationFinite(&b->ph) || !linearCalibrationFinite(&b->tds) || !linearCalibrationFinite(&b->light)) {
    return false;
  }
  return true;
}

// true if a is newer than b (RFC 1982 style on 16 bits).
static bool revisionNewer(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) > 0; }

const ConfigBlob* configBlobPickNewest(const ConfigBlob* a, const ConfigBlob* b) {
  const bool va = configBlobValid(a);
  const bool vb = configBlobValid(b);
  if (va && vb) return revisionNewer(b->revision, a->revision) ? b : a;
  if (va) return a;
  if (vb) return b;
  return NULL;
}

uint16_t configRevisionNext(uint16_t revision) {
  const uint16_t next = (uint16_t)(revision + 1u);
  return next == 0 ? 1 : next;
}

bool configBlobWithSensor(const ConfigBlob* cur, SensorId sensor, bool enabled, ConfigBlob* next) {
  if (sensor >= SENSOR_COUNT) return false;
  const SensorMask bit = (SensorMask)(1u << sensor);
  const SensorMask mask = enabled ? (SensorMask)(cur->sensorEnabled | bit)
                                  : (SensorMask)(cur->sensorEnabled & (SensorMask)~bit);
  if (mask == cur->sensorEnabled) return false;
  memcpy(next, cur, sizeof(*next));
  next->sensorEnabled = mask;
  next->revision = configRevisionNext(cur->revision);
  configBlobSeal(next);
  return true;
}

static bool sameCal(const LinearCalibration* a, const LinearCalibration* b) {
  return a->slope == b->slope && a->offset == b->offset && a->revision == b->revision && a->valid == b->valid;
}

static void copyCal(LinearCalibration* dst, const LinearCalibration* src) { memcpy(dst, src, sizeof(*dst)); }

bool configBlobRefreshDefaults(ConfigBlob* b, const ConfigDefaults* d) {
  ConfigBlob fresh;
  memcpy(&fresh, b, sizeof(fresh));
  fresh.lampAutoEnabled = d->lampAutoEnabled;
  memcpy(&fresh.lamp, &d->lamp, sizeof(fresh.lamp));
  if ((b->calOverridden & CAL_BIT_PH) == 0) copyCal(&fresh.ph, &d->ph);
  if ((b->calOverridden & CAL_BIT_TDS) == 0) copyCal(&fresh.tds, &d->tds);
  if ((b->calOverridden & CAL_BIT_LIGHT) == 0) copyCal(&fresh.light, &d->light);
  // Compare field by field: the inputs may carry padding garbage.
  const bool same = fresh.lampAutoEnabled == b->lampAutoEnabled &&
                    d->lamp.onBelowMv == b->lamp.onBelowMv && d->lamp.offAboveMv == b->lamp.offAboveMv &&
                    d->lamp.minOnMs == b->lamp.minOnMs && d->lamp.minOffMs == b->lamp.minOffMs &&
                    d->lamp.faultsToOff == b->lamp.faultsToOff && sameCal(&fresh.ph, &b->ph) &&
                    sameCal(&fresh.tds, &b->tds) && sameCal(&fresh.light, &b->light);
  if (same) return false;
  fresh.revision = configRevisionNext(b->revision);
  configBlobSeal(&fresh);
  memcpy(b, &fresh, sizeof(*b));
  return true;
}

uint8_t configCalibrationBit(SensorId sensor) {
  switch (sensor) {
    case SENSOR_PH: return CAL_BIT_PH;
    case SENSOR_TDS: return CAL_BIT_TDS;
    case SENSOR_LIGHT: return CAL_BIT_LIGHT;
    default: return 0;
  }
}

const LinearCalibration* configBlobCalibration(const ConfigBlob* b, SensorId sensor) {
  switch (sensor) {
    case SENSOR_PH: return &b->ph;
    case SENSOR_TDS: return &b->tds;
    case SENSOR_LIGHT: return &b->light;
    default: return NULL;
  }
}

bool configBlobWithCalibration(const ConfigBlob* cur, SensorId sensor, const LinearCalibration* cal,
                               bool overridden, ConfigBlob* next) {
  const uint8_t bit = configCalibrationBit(sensor);
  if (bit == 0) return false;
  memcpy(next, cur, sizeof(*next));
  next->revision = configRevisionNext(cur->revision);
  LinearCalibration* dst = (LinearCalibration*)configBlobCalibration(next, sensor);
  copyCal(dst, cal);
  if (overridden) {
    dst->revision = next->revision;
    next->calOverridden = (uint8_t)(next->calOverridden | bit);
  } else {
    next->calOverridden = (uint8_t)(next->calOverridden & (uint8_t)~bit);
  }
  configBlobSeal(next);
  return true;
}

bool configBlobMigrateV1(const void* v1, uint32_t len, const ConfigDefaults* d, ConfigBlob* out) {
  if (v1 == NULL || len != sizeof(ConfigBlobV1)) return false;
  ConfigBlobV1 old;
  memcpy(&old, v1, sizeof(old));
  if (old.magic != CONFIG_BLOB_MAGIC || old.schema != 1u || old.revision == 0) return false;
  if (old.crc != crc32Ieee(&old, (uint32_t)offsetof(ConfigBlobV1, crc))) return false;
  if ((old.sensorEnabled & (SensorMask)~SENSOR_MASK_ALL) != 0) return false;
  configBlobInit(out, d);
  out->revision = old.revision;
  out->sensorEnabled = old.sensorEnabled;
  configBlobSeal(out);
  return true;
}
