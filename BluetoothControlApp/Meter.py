"""Display scales and indicative target bands per metric.

The bands show the interface working; they are not crop-specific growing
advice and must be set for the real crop and calibrated sensors. A band only
applies when the reading's unit matches. The light level is mV until it is
calibrated on the device, then lux (target_for picks the matching scale).
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class Target:
    scale_low: float
    scale_high: float
    unit: str
    low: float | None = None
    high: float | None = None

    @property
    def has_band(self) -> bool:
        return self.low is not None and self.high is not None

    def status(self, value: float, unit: str) -> str | None:
        if not self.has_band or unit != self.unit:
            return None
        if value < self.low:
            return "Low"
        if value > self.high:
            return "High"
        return "Normal"

    def fraction(self, value: float) -> float:
        span = self.scale_high - self.scale_low
        return max(0.0, min(1.0, (value - self.scale_low) / span)) if span else 0.0

    @property
    def caption(self) -> str:
        unit = unit_label(self.unit)
        if self.has_band:
            return f"Indicative {self.low:g}–{self.high:g} {unit}"
        return f"Scale {self.scale_low:g}–{self.scale_high:g} {unit}"


TARGETS = {
    "air_temperature": Target(0, 40, "degC", 18, 30),
    "air_humidity": Target(0, 100, "%RH", 40, 80),
    "air_pressure": Target(95000, 105000, "Pa"),
    "water_temperature": Target(0, 35, "degC", 18, 24),
    "light_level": Target(0, 3300, "mV"),
    "water_distance": Target(0, 400, "mm"),
    "ph": Target(4, 8, "pH", 5.5, 6.5),
    "tds": Target(0, 1500, "ppm", 500, 1000),
    "tvoc": Target(0, 1000, "ppb", 0, 220),
    "co2eq": Target(400, 2000, "ppm", 400, 1000),
}

# Alternative scales by (metric, unit) once a sensor is calibrated on the device.
CALIBRATED_TARGETS = {
    ("light_level", "lux"): Target(0, 20000, "lux"),
}

UNIT_LABELS = {"degC": "°C", "%RH": "% RH", "Pa": "Pa", "mV": "mV", "mm": "mm",
               "pH": "pH", "ppm": "ppm", "ppb": "ppb", "raw": "raw", "lux": "lux"}


def target_for(key: str, unit: str | None) -> Target | None:
    return CALIBRATED_TARGETS.get((key, unit)) or TARGETS.get(key)


def unit_label(unit: str) -> str:
    return UNIT_LABELS.get(unit, unit)
