# Hydroponics controller PCB

Revision 1.5 is a two-layer, 165.5 × 90.5 mm controller board for the
ESP32-S3-WROOM-1-N16R8. It brings the seven sensor interfaces, native USB,
a grow-lamp relay header, and the external and battery power paths onto
one board. The manufacturing record specifies a 1.5 mm board with 35 µm
copper per side. [Manufacturing checks](Validation/Manufacturing%20Checks.json)

![Rev 1.5 top assembly drawing with connector and module designators](Documentation/Assembly%20Preview.png)

## Open and review the design

Open [PCB Design.kicad_pro](Hardware/PCB%20Design.kicad_pro) in KiCad 10
with the standard symbol libraries available. Keep the whole `Hardware/`
directory together so the project symbol and footprint tables can resolve
their local libraries.

| File or directory | What to use it for |
| --- | --- |
| [Schematic](Hardware/PCB%20Design.kicad_sch) and [schematic PDF](Documentation/PCB%20Design.pdf) | Check net labels, connector pin numbers, dividers, and power paths. |
| [PCB layout](Hardware/PCB%20Design.kicad_pcb) | Review placement, routing, clearances, and the board outline. |
| [Fabrication](Fabrication/) | Top and bottom Gerbers, edge cuts, separate plated and non-plated drill files, and the IPC-D-356 netlist. Includes top SMT and all-footprint placement CSVs. |
| [BOM](BOM/) | The KiCad export records values and footprints. The component sourcing sheet lists candidate parts and open fit checks. |
| [Assembly drawings and previews](Documentation/) | Orient the board, identify component locations, and check the back markings. |
| [Validation](Validation/) | Recorded ERC/DRC and manufacturing checks, plus plated and non-plated drill maps. |

## Rev 1.5 connections

Read the pin numbers from the schematic and match them to the connector
orientation on the assembly drawing. The GPIO column uses ESP32-S3 signal
numbers.

| Connector | Board pins in order | ESP32-S3 signals |
| --- | --- | --- |
| J2 · BME280 | 1 SDA, 2 SCL, 3 GND, 4 3V3 | GPIO21 SDA, GPIO47 SCL |
| J11 · SGP30 | 1 SDA, 2 SCL, 3 GND, 4 3V3 | Same I²C bus as J2 |
| J4 · DS18B20 | 1 GND, 2 3V3, 3 DQ | GPIO14 |
| J8 · DFR0026 | 1 signal, 2 3V3, 3 GND | GPIO1 |
| J7 · HC-SR04 | 1 EXT_5V, 2 TRIG, 3 ECHO, 4 GND | GPIO17 TRIG, GPIO18 ECHO through the on-board divider |
| J5 · SEN0244 | 1 GND, 2 EXT_5V, 3 AOUT | GPIO6 |
| J14 · pH-4502C | 1 EXT_5V, 2 GND, 3 GND, 4 PO | GPIO7 through the on-board divider |
| J13 · Relay interface | 1 EXT_5V, 2 RelayIN, 3 GND | GPIO15, active high in the firmware |

J5 pin 2 is connected to `EXT_5V`. The schematic symbol still labels that
pin `3V3`. At J13, `RelayIN` connects directly to the 3.3 V GPIO15 signal.
Select and verify a compatible relay interface before connecting a load.
The earlier pin drawing shows a different relay drive circuit.
[Rev 1.5 schematic](Documentation/PCB%20Design.pdf) · [October pin layout](../Documentation/Pin%20Layout.pdf)

## Power and programming

- **J1 USB DATA** routes D− and D+ to the ESP32-S3 native USB pins (GPIO19
  and GPIO20). Its VBUS is unconnected. Use **J3 5V IN** for the external
  operating supply when programming the PCB on the bench.
- **J6** is the battery connection. The battery load path passes through F1
  and the 5 V regulator module at **J9**. **J10** is the footprint for the
  separate 3S charger module. The two source paths feed the **J12** 3.3 V
  regulator for the controller and retained sensors.
- **EXT_5V** supplies J5, J7, J13, and J14 directly on this revision. The
  ESP32-S3 senses the external rail on GPIO8. The routed divider uses R13
  (10 kΩ) above R11 and R12 (6.8 kΩ each, in series), about 2.88 V nominal
  at a 5.00 V input. The HC-SR04 ECHO divider uses R15 (10 kΩ) and R14 (15 kΩ).

For component references and placement, use the Rev 1.5 schematic. The
October pinout and the original firmware bench notes show earlier divider
or relay configurations. The electrical design and load estimates are in
the [power circuit diagram](../Documentation/Power%20Circuit%20Diagram.pdf)
and [power tree](../Documentation/Power%20Tree%20Analysis.pdf).

## Fabrication and assembly review

1. Run the schematic electrical rules check (ERC) and the board design
   rules check (DRC) with schematic parity. Rev 1.5 records zero violations,
   unconnected items, and parity issues in KiCad 10.0.8. If KiCad reports
   missing `Device`, `Mechanical`, or `power` symbols, initialize the standard
   symbol library table and rerun the checks.
   [Recorded checks](Validation/Manufacturing%20Checks.json)
2. Preview the Gerbers and both drill files together in a Gerber viewer.
   Compare the outline, layer alignment, mounting holes, and the plated and
   non-plated drill maps. Confirm the fabricator can meet the stackup and
   geometry in the manufacturing record.
3. For assembly, match references in the KiCad BOM to the top SMT placement
   file and the assembly drawing. Check connector fit, polarity, and module
   orientation against the schematic and actual parts. Verify the **N16R8**
   suffix on U1 to match the firmware's 16 MB flash and 8 MB PSRAM. Check the
   Micro-B connector shell pads and the 470 µF capacitor body and lead spacing.
   [Component sourcing notes](BOM/PCB%20Components.csv) · [Assembly drawing](Documentation/Assembly%201%20To%201.pdf)

Run the following from the repository root on Linux to save check reports outside the checkout.

```sh
reports="$(mktemp -d)"
kicad-cli sch erc --exit-code-violations --format json -o "$reports/erc.json" "PCB Design/Hardware/PCB Design.kicad_sch"
kicad-cli pcb drc --schematic-parity --exit-code-violations --format json -o "$reports/drc.json" "PCB Design/Hardware/PCB Design.kicad_pcb"
```

## First bench session

1. Compare the assembled board with the schematic and assembly drawing.
   Verify the power modules, polarity, and rail continuity before
   connecting sensors, a battery, or a relay load.
2. Start with external 5 V and check the retained 3.3 V rail. Use J1 and a
   USB data cable for upload. Bring up the BME280 first, then add the
   remaining sensor branches and verify their supply and readings.
   [Firmware bench bring-up](../Firmware/README.md#bench-bring-up)
3. Qualify GPIO8 and the two rails under load across external-power loss
   and return before enabling firmware power handover. Verify the relay
   interface separately with a low-voltage test load.
   [External-power bench notes](../Firmware/docs/bench-notes.md#external-power-sense-bring-up-test_power_sense-wp9-gate)

The KiCad reports establish design-rule and connectivity results. Loaded
power transfer, module compatibility, and assembled-board behavior need
measurements on the actual hardware.
