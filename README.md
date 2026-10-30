# Reward Rocket

A little win deserves a rocket launch. Reward Rocket turns an earned reward into a countdown, a big red button press and a surprise capsule. I built mine to help with potty training; you can fill the reusable capsules with your own small rewards.

A parent taps **Activate one reward** on their phone. The child presses the rocket’s button, the phone plays Mission Control and a countdown, and the mechanism delivers one capsule before returning to its pickup position. The rocket holds ten capsules.

**This is a prototype and has not undergone extensive testing. Build and use it at your own risk.**

## Build one

- [Watch the build on YouTube](https://youtu.be/5GSi_oSHNV4)
- [Download the CAD and printable parts on Printables](https://www.printables.com/model/1894689-reward-rocket-phone-controlled-capsule-dispenser)
- [Animated assembly instructions and parts BOM](https://aimaketech.com/makes/reward-rocket/assembly/)
- [AI Make project page](https://aimaketech.com/makes/reward-rocket)

This repository contains the firmware and instructions for setting up the controller. Get the mechanical parts and assembly sequence from the links above. Print the parts in PLA at 0.2 mm layers using their supplied orientations.

## Start here

1. Assemble the rocket and check the [wiring](docs/WIRING.md).
2. [Flash the ESP32-S3 over USB and connect it to Wi-Fi](docs/FLASHING.md).
3. [Establish the Hall reference and teach the pickup position](docs/SETUP.md).
4. Open the rocket’s page on your phone, tap **Activate one reward**, then press the physical button.

Keep the phone page in the foreground for the voice/countdown and turn up its media volume. Config contains calibration, movement, sound and OTA settings; the normal parent page stays simple.

## What you need

The controller uses an ESP32-S3 development board and breakout, a 5 V 28BYJ-48 motor with ULN2003 driver, one active-low Hall sensor and an Omron B3F-5050 momentary switch. Use a regulated 5 V supply rated for at least 2 A. See the assembly guide’s [full BOM and purchase links](https://aimaketech.com/makes/reward-rocket/assembly/BOM) for mechanical hardware, inserts, bearings, tube and wiring supplies.

The button wires are soldered directly to the switch pins, then connected to GPIO17 and GND. No separate button header assembly is needed.

## Startup and everyday use

Power-on never moves the motor or dispenses a reward. A clean saved pickup position is restored without motion. The parent must still activate a reward, and the child must make a fresh button press. A saved unused authorization needs **Resume authorized reward** after a restart.

If power was interrupted during movement, the position is uncertain or the saved position conflicts with the Hall sensor, the rocket stays still in **Recovery**. Use the [recovery steps](docs/SETUP.md#recovery) before activating another reward. An OTA update also deliberately returns the controller to Recovery.

## Firmware

Release **1.0.0** supplies firmware build **393**. The assembled prototype’s builder has tested delivery, startup/recovery and phone audio. This does not establish compatibility with every board, mechanism or phone.

- `firmware/RewardRocket393/`: Arduino sketch and its four required headers.
- `docs/`: flashing, setup, wiring and troubleshooting.
- `tests/`: optional automated audio-sequence regression for developers.

To run the optional audio regression, install Node.js and run `node tests/audio_sequence.test.cjs` from the repository root. It simulates timing; it does not operate a connected rocket.

The tested build target is **ESP32S3 Dev Module**, Espressif Arduino core **3.3.11**, with an OTA-capable default partition layout. No extra Arduino library downloads are required beyond that board package. [Flashing instructions](docs/FLASHING.md) explain both USB and Arduino IDE OTA.

The controller uses your local Wi-Fi, with no cloud service. Anyone who can reach its URL on the LAN can activate and configure it; there is no parent-page password. Keep it on your trusted home network and do not expose it to the internet. OTA uploads use a separate password.

## License

Copyright 2026 Mauricio Toro. Project files are shared under [CC BY-NC-SA 4.0](LICENSE): attribution, noncommercial use and share alike. Framework dependencies retain their upstream licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).
