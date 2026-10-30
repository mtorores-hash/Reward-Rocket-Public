# Flashing and Wi-Fi setup

## First installation: use USB

1. Download this repository with **Code → Download ZIP**, extract it, and open `firmware/RewardRocket393/RewardRocket393.ino` in Arduino IDE. Keep `Config334.h`, `Control334.h`, `Audio342.h` and `WebUi342.h` in that same sketch folder.
2. Install **esp32 by Espressif Systems, version 3.3.11**, through Boards Manager. If it is not listed, add `https://espressif.github.io/arduino-esp32/package_esp32_index.json` under **Additional Boards Manager URLs** in IDE Preferences. See [Espressif’s installation guide](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html).
3. Select **Tools → Board → esp32 → ESP32S3 Dev Module**. This is an ESP32-S3 sketch, not the original ESP32 Dev Module target.
4. Connect the development board with a USB **data** cable. The panel-mounted USB-C power inlet on the assembled rocket supplies power; it is not the programming connection. Use one intended power source at a time unless your board’s power arrangement explicitly supports simultaneous inputs.
5. For a board with a USB-to-UART connector, use that port and leave **USB CDC On Boot: Disabled**, matching the tested settings. Keep **Partition Scheme: Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)** and match Flash Size to your board. This firmware needs OTA app slots; do not select Huge APP / No OTA. The tested build uses the generic target’s default 4 MB layout.
6. If your board has only native USB, select **USB CDC On Boot: Enabled** and **Upload Mode: UART0 / Hardware CDC**. Select the native USB port. Follow [Espressif’s USB CDC instructions](https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/cdc_dfu_flash.html) for your connector.
7. Select the board’s port under **Tools → Port** and click **Upload**. If the IDE remains at “Connecting”, hold BOOT, press/release RESET, then release BOOT and select the download-mode port before retrying. Reset the board after the initial upload if it does not start automatically.
8. Open Serial Monitor at **115200 baud** with a newline line ending. The boot message identifies **Reward Rocket 393**. Send `HELP` to confirm the serial connection.

For ordinary firmware updates, leave **Erase All Flash Before Sketch Upload: Disabled**. Do not erase NVS: it stores Wi-Fi, OTA credentials, motor timing and calibration. A new board starts unconfigured; an already-used board can retain an earlier configuration.

## Connect to Wi-Fi

In Serial Monitor, send these lines one at a time, replacing the angle-bracket text with your own values:

```text
WIFI SSID <your 2.4 GHz network name>
WIFI PASSWORD <your network password>
OTA PASSWORD <your own upload password of at least 12 characters>
WIFI SAVE
STATUS
```

Use the command prefixes in uppercase. Network names and passwords are case-sensitive; do not include the angle brackets. The firmware accepts Wi-Fi passwords of 8–63 characters. The OTA password is optional for LAN control but required to enable wireless firmware uploads. Values are saved on the device and are not echoed. Never put your real credentials in repository files.

`STATUS` shows the assigned IP once connected. Open `http://<that-IP>/` on a phone on the same LAN. `http://reward-rocket.local/` may also work on networks with mDNS. No access point is created: if it does not connect, correct the credentials using USB Serial Monitor. Continue with [first startup and calibration](SETUP.md).

## Later updates: Arduino IDE OTA

1. Keep the board type and OTA-capable partition layout consistent with your installed firmware. Open the current sketch in Arduino IDE.
2. Leave the rocket idle, cancel any unused reward, and open **Config → Arduino IDE firmware update** on its LAN page. The controller must be Locked, Idle, with no pending reward and coils released.
3. Select **Enable Arduino IDE OTA · 5 min**. This opens the upload service for five minutes and inhibits motion.
4. On a computer on the same LAN, select the `reward-rocket` network port / displayed controller IP under **Tools → Port**. Close Serial Monitor if needed, click Upload and enter the OTA password provisioned over USB when prompted.
5. Wait for upload and reboot to finish. Reload the phone page so it uses the new embedded interface. The controller intentionally marks its position unknown during an OTA update, so use [Recovery](SETUP.md#recovery) before the next reward. Network and calibration settings are preserved.

If no network port appears, check that the maintenance window is open, both devices are on the same LAN, and local firewall / client-isolation settings are not blocking discovery. USB upload remains available.

## Optional command-line build

With Arduino CLI and the Espressif 3.3.11 core installed, run from the repository root:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s3 --build-path .build/rocket firmware/RewardRocket393
```

For the native-USB serial variant, use `esp32:esp32:esp32s3:CDCOnBoot=cdc`. This repository distributes source; Arduino IDE compiles the firmware for your selected board settings and uploads the bootloader, partition table and application for the first installation.
