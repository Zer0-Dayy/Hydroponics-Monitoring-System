"""Illustrative meter ranges for the simulated ESP sensor readings."""

from dataclasses import dataclass


@dataclass(frozen=True)
class Target:
    low: float
    high: float
    scale_low: float
    scale_high: float
    unit: str

    def status(self, value: float) -> str:
        if value < self.low:
            return "Low"
        if value > self.high:
            return "High"
        return "Normal"

    def fraction(self, value: float) -> float:
        return max(0.0, min(1.0, (value - self.scale_low) / (self.scale_high - self.scale_low)))

    @property
    def caption(self) -> str:
        return f"Demo target {self.low:g}–{self.high:g} {self.unit}"


# These ranges demonstrate the interface, not crop-specific growing advice.
TARGETS = {
    "level": Target(10, 40, 0, 50, "cm"),
    "ph": Target(5.5, 6.5, 4, 8, "pH"),
    "tds": Target(500, 1000, 0, 1500, "ppm"),
    "climate": Target(18, 30, 0, 40, "°C"),
    "light": Target(250, 850, 0, 1000, "raw"),
}
