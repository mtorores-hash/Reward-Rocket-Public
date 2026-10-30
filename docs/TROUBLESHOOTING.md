# Troubleshooting

| Symptom | Check |
| --- | --- |
| No USB port | Use a data cable and the development board’s programming connector, not the panel power inlet. Match UART versus native-USB settings in [Flashing](FLASHING.md). |
| Serial Monitor is silent | Select the correct port, 115200 baud, reset once, and send `HELP` with a newline. Native USB needs USB CDC enabled in the source build. |
| Wi-Fi does not connect | Use a supported 2.4 GHz network. Re-enter `WIFI SSID` and `WIFI PASSWORD`, then `WIFI SAVE`; `STATUS` shows the assigned IP. Keep case and spaces in your values. |
| Phone cannot open the page | Use the IP from `STATUS`. Phone and rocket must share a LAN without client isolation. mDNS names are optional; the numeric IP avoids discovery issues. |
| Activate is unavailable | Finish Hall referencing and pickup teaching, return to pickup and check Config for Recovery or an OTA maintenance window. |
| Button does not start a reward | First activate a reward on the phone, then release and press the physical button. Check GPIO17 / GND and the normally-open switch pair. |
| Motor does not move | Check power directly at the ULN2003, common ground, IN1–IN4 order and the motor plug. Use hold-to-jog after referencing; a 32-halfstep move can be too small to see clearly. |
| Hall is never ACTIVE | Pass the magnet over the sensor and check its polarity/orientation, 3V3 supply, GND and GPIO15. Use the digital output, not an analog output. |
| Recovery after a restart or update | No automatic motion is expected. Follow [Recovery](SETUP.md#recovery), then go to taught pickup. OTA intentionally invalidates position. |
| Delivery is slightly misaligned | Confirm the mechanism and Hall datum, then adjust the saved signed Hall alignment in Config. Pickup is taught separately. |
| Reward works but sound does not | Reload the phone page after updating firmware, enable voice in Config, check media volume and Preview voice, and keep the page in the foreground. Each phone has its own sound settings. |
| Liftoff is early or late | Use Config’s phone-local Countdown offset after confirming the mechanical delivery point. Positive is later; negative is earlier. |
| OTA port is missing | Provision an OTA password, enable the five-minute window in Config, and keep the computer and controller on the same LAN. Use USB if discovery still fails. |

`WIFI CLEAR` removes stored Wi-Fi and OTA credentials. `NVS RESET CONFIRM` clears motion/calibration state but retains network credentials; it requires idle motion. Neither is part of a normal firmware update. Use them only when you deliberately want to set up those settings again.
