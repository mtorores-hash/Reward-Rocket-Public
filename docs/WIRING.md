# Wiring

Use the terminal labels on your actual boards. GPIO numbers below are ESP32-S3 GPIOs, not physical pin positions or breakout row numbers.

| Connection | Destination |
| --- | --- |
| Regulated 5 V supply | Breakout 5 V / GND bus |
| Breakout 5 V / GND bus | ULN2003 driver + / − power terminals |
| ESP32 GPIO4 | ULN2003 IN1 |
| ESP32 GPIO5 | ULN2003 IN2 |
| ESP32 GPIO6 | ULN2003 IN3 |
| ESP32 GPIO7 | ULN2003 IN4 |
| Motor connector | Matching ULN2003 motor socket |
| ESP32 3V3 | Hall module VCC |
| Common GND | Hall module GND |
| ESP32 GPIO15 | Hall module digital S / OUT |
| ESP32 GPIO17 | One B3F-5050 normally-open switch contact |
| Common GND | The other switch contact |

All grounds are common. The prototype’s Hall module switches reliably at **3.3 V** and is **active-low**. Do not put a 5 V signal onto an ESP32 GPIO. Check a replacement Hall module’s pin labels and output voltage before connecting it.

Solder the two button wires directly to the switch pins, then connect the other ends to GPIO17 and GND. Use a contact pair that is open when released and closes when pressed; a continuity meter can identify it. The two pins on each side of this switch are internally connected, so connecting both wires to the same internally paired side will make it appear permanently pressed. The firmware uses an internal pull-up; no separate button header assembly or external pull-up is required.

The development board is powered through the breakout’s supported **5 V** input, not its 3V3 rail. The panel USB-C inlet is for power. Connect the driver to the same 5 V supply and ground bus. Confirm your breakout’s power routing and polarity before powering it.

For mechanical mounting, insert sizes, screw lengths and wire routing, use the [animated assembly guide](https://aimaketech.com/makes/reward-rocket/assembly/). The [purchased-parts BOM](https://aimaketech.com/makes/reward-rocket/assembly/BOM) includes sourcing links.
