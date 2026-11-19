// Arduino entry points defined in C.
// The Arduino-ESP32 core (cores/esp32/main.cpp) calls setup() and loop() with
// C++ linkage, i.e. the mangled symbols _Z5setupv and _Z4loopv. These
// declarations give the C definitions those symbol names. Include this header
// in every file that defines setup()/loop() (src/main.c, test/bringup/*.c).
#pragma once

void setup(void) __asm__("_Z5setupv");
void loop(void) __asm__("_Z4loopv");
