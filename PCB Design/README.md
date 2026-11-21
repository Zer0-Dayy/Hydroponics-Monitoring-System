# Hydroponics Monitoring System · PCB revision 1.8

Start with **Documentation/Port_and_LED_Map.pdf** for the board locations and **Documentation/Assembly_and_Bench_Guide.pdf** for population and first power-up. Use J1 for native USB data and J3 for external 5V. J15 and J16 are the fitted backup headers; keep their breakouts unplugged during normal use. Print **Documentation/USB_Fit_Template_1to1.pdf** at actual size and check both real modules before ordering boards.

## Manufacturing handoff

| Directory | Contents |
| --- | --- |
| Hardware | Editable KiCad project, schematic, board, local symbols, footprints and library tables. Open PCB Design.kicad_pro. The project was exported and checked with KiCad 9.0.2. |
| Fabrication | 2-layer Gerbers, both silkscreens, plated and non-plated drill files with G85 slots, Gerber job and IPC-D-356 electrical test netlist. |
| Assembly | Assembly_BOM.csv gives the factory population status for every schematic item. Placement_Top_SMT.csv is the default SMT pick-and-place file. Positions_All_Design.csv is a coordinate reference including optional items. Sourcing_Notes.csv retains earlier candidate suppliers and adds the Rev 1.8 components. |
| Documentation | Schematic, 1:1 top assembly and mirrored back markings, copper drawings, physical fit template, locator and four-page assembly/bench guide. |
| Firmware/Heartbeat | Standalone ESP-IDF C project for the active-high GPIO9 LED. It can replace a test board’s application after the read-only ROM check. |
| Validation | Native ERC / DRC reports, drill maps and revision comparison with USB geometry, connector isolation and physical test limits. |

**Factory build**. Fit the J15/J16 1×5 female through-hole sockets, the J17 1×4 male header, J18 1×2 header with removable shorting shunt and all five LEDs. Fit both Micro-B receptacles and all three 0Ω links. The front stencil includes these five parts. Solder and inspect all four shell anchors on each receptacle. Leave J9, J10 and J12 empty for the owner’s modules. Supply both silkscreens. Maintain the original 165.5 × 90.5 mm outline, four M3 mounting positions, 1.5 mm FR-4 and 35 µm copper per side. Review plated shell slots and the four new non-plated standoff slots with the fabricator.

## USB service options

| Function | Plug-in breakout | Factory-installed primary | If a port is damaged |
| --- | --- | --- | --- |
| Native data | J15. Pin 2 D−, pin 3 D+, pin 5 GND. Pins 1 and 4 isolated. | Fit a mechanically matched J1 and both 0Ω links R25/R26. | Disconnect power. Remove both links to isolate the direct D lines. Inspect the footprint for shorts to power or ground before installing a supported J15 breakout. |
| External 5V | J16. Pin 1 VBUS and pin 5 GND. Pins 2–4 isolated. | Fit a mechanically matched J3 and R27. Use a jumper rated ≥1.5A. | Disconnect power. Remove R27 to isolate J3 VBUS. Inspect for shorts and supply 5V through J16. |
| ROM recovery | J17. Pin 1 GND, pin 2 PCB TX, pin 3 PCB RX, pin 4 ESP_3V3. | 3.3V USB-to-UART adapter with crossed TX/RX and either board power (J18 closed, pin 4 free) or ESP-only power (J18 open). | The ROM can return its MAC address over UART even when the native USB path is unusable. |

Use a single host for native data and a single source for EXT_5V. With power off, desolder the indicated 0Ω component(s) and leave their copper pads intact. Verify isolation before attaching the replacement breakout. The J15 carrier and J1 occupy a common plug-access area. For a different carrier shape, secure the carrier to the enclosure and use a short pin-mapped harness. The unknown generic loose receptacle has no verified land pattern. Check the proposed part against the Amphenol footprint before assembly; flag a mismatch before fitting a substitute.

## Field connector pinout

| Ref | Terminals in numerical order | ESP32-S3 signal |
| --- | --- | --- |
| J2 · BME280 and J11 · SGP30 | SDA, SCL, GND, 3.3V | GPIO21 SDA, GPIO47 SCL |
| J4 · DS18B20 | GND, 3.3V, DQ | GPIO14 |
| J5 · SEN0244 | GND, EXT_5V, AOUT | GPIO6 |
| J7 · HC-SR04 | EXT_5V, TRIG, ECHO, GND | GPIO17 trigger, GPIO18 divided echo |
| J8 · DFR0026 | Signal, 3.3V, GND | GPIO1 |
| J13 · Relay | EXT_5V, relay control, GND | GPIO15 |
| J14 · pH-4502C | EXT_5V, GND, GND, PO | GPIO7 through divider |

J6 accepts battery positive on pin 1 and ground on pin 2. J9 produces BAT_5V. The common diode cathodes feed J12 at SYS_IN. External-rail sensing remains on GPIO8 through 10kΩ over 13.6kΩ. J5 pin 2 now reads EXT_5V in its schematic symbol. Its electrical connection is the same as Rev 1.5.

## Bench evidence

At first power-up, compare external 5V, SYS IN and U1 3.3V against the bench guide. EN should return high after reset. Enter the ROM loader for the read-only MAC test, then load the heartbeat on a test board if desired. A toggling GPIO9 proves the diagnostic task is running. Qualify the sensor channels, UPS handover, USB connection under cable movement and loaded rails on a physical first article.

To repeat the CAD checks from the Hardware directory:

```sh
kicad-cli sch erc --exit-code-violations -o erc.rpt 'PCB Design.kicad_sch'
kicad-cli pcb drc --all-track-errors --schematic-parity --exit-code-violations -o drc.rpt 'PCB Design.kicad_pcb'
```

## ESP-only programming

With all power off, remove the J18 shunt and disconnect external 5V, battery, charger and sensor/relay cables. Feed regulated 3.3V rated for at least 500mA into J17 pin 4 with common ground on pin 1. Use 3.3V UART logic. Use a separate 3.3V supply if the adapter cannot supply that current. After flashing, disconnect programmer power before reinstalling the J18 shunt for normal operation. D6 indicates the local ESP rail. Before plugging in sensors, verify that J12 OUT+ and J2 pin 4 remain near 0V in this mode. Peripheral GPIOs can feed unpowered devices through their protection diodes.
