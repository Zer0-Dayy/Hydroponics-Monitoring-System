// Boot-time safe output defaults (§5 "thin main", §10 actuator policy).
// Called first in setup(), before any network or sensor start.
#pragma once

void gpioApplySafeDefaults(void);
