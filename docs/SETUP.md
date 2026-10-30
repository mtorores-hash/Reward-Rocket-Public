# First startup, calibration and normal operation

## 1. First power-on

Complete the mechanical assembly and [wiring](WIRING.md), then [flash and provision Wi-Fi](FLASHING.md). Start calibration with the capsule feed empty so alignment movements do not release a reward.

On a fresh board, **Recovery / unknown position** is expected. The firmware does not move on boot. Open its LAN URL and select **Config**. All movement, calibration, sound and recovery controls live there.

## 2. Establish the Hall datum

Open **Hall reference and recovery** and **Diagnostics**.

- If the magnet is already over the sensor and Hall reads **ACTIVE**, use **Confirm Hall is active · set position to zero** while in Recovery.
- If Hall is clear, use **Find Hall datum −** or **Find Hall datum +** in the direction that moves your mechanism toward the magnet. The seek is bounded and stops at the Hall sensor. After it finishes, if the controller is still in Recovery, confirm the now-active Hall to set zero and leave Recovery.

Use STOP if movement is obstructed or going the wrong way. STOP releases the motor and makes position unknown; re-establish the Hall reference before continuing. The Hall sensor is the reference for delivery, not a detector that a capsule has fallen.

## 3. Align delivery

Set the mechanism’s physical delivery alignment at the Hall datum following the mechanical assembly guide. The default delivery point is Hall zero. If a small correction is needed, **Configuration → Optional Hall alignment** stores a signed offset in **halfsteps**, applied after the Hall is found during reward delivery. It survives restarts. It is not a second taught endpoint.

Leave the offset at **0** initially. Do not use a large software offset to compensate for incorrect mechanical assembly. Manual Hall seeks stop at zero and do not apply this delivery offset.

## 4. Teach pickup

1. With a known Hall reference and the controller Locked, open **Pickup and movement setup**.
2. Use **Hold ◀ / Hold ▶** for travel, or **Jog − / Jog +** for short counted moves, until the mechanism is aligned to receive the next capsule. Counted jog accepts 1–256 halfsteps; hold-to-jog continues while held. Releasing a held control stops renewal, with up to 500 ms of remaining motion.
3. Select **Save pickup here**. Pickup must be different from delivery/Hall zero. On an unconfigured board this also initializes the delivery correction to zero.
4. Check Diagnostics: the pickup is taught and position is known. **Go to taught pickup** is available for returning to this saved point when position is known.

Keep the phone page visible while jogging. Switching apps or losing the connection stops jog renewal.

## 5. Set timing and check audio

Fresh boards default to **5000 µs per halfstep**. A smaller value is faster; Config accepts **3000–20000 µs**. The built prototype runs at **3000 µs**, but start with the default on your mechanism and check for missed steps. Delivery and pickup dwell times are separate settings. Use **Save configuration** after changes.

Under **Mission Control voice**, enable sound and use **Preview voice** to check the phone’s media volume. The voice plays on the phone, not through the ESP32. Keep the LAN page in the foreground. **Countdown offset** is a phone-local audio adjustment: negative finishes earlier, positive later; it does not change motion. Motor timing and pickup/dropoff calibration are stored on the controller.

## 6. Deliver a reward

Load capsules as shown in the assembly guide and return to the normal parent page.

1. Tap **Activate one reward** on the phone. This enables one reward; it does not start the motor.
2. Release the rocket’s physical button if it is held, then press it once.
3. The motor seeks the Hall datum, applies the saved delivery alignment, waits at delivery, then returns to the taught pickup. The phone plays Mission Control, a ten-to-one countdown and the launch sound. Liftoff targets delivery arrival.
4. The authorization is consumed. Another reward needs another parent activation and fresh physical button press. An unused reward can be cancelled in Config.

## Normal startup sequence

Every power-on starts with motor outputs off and checks the saved state. It never automatically homes or dispenses a reward.

| Saved condition | What happens |
| --- | --- |
| Clean, commissioned pickup position | Position is restored without moving; controller is Locked and unarmed. |
| Clean pickup with an unused authorization | The authorization remains pending but unarmed; use **Resume authorized reward** before the child presses the button. |
| Hall is active and the saved state permits a trusted datum | Position can be set to zero without motion. Return to taught pickup when available. |
| Interrupted motion, unknown/corrupt state, or contradictory Hall reading | Controller remains still in Recovery. |

Restoring pickup assumes the mechanism has not been moved by hand while unpowered. If it has, re-reference it rather than trusting the saved coordinate.

## Recovery

1. Inspect and clear the mechanism. If a reward was interrupted, check whether a capsule has already dropped before authorizing another one.
2. Open Config. If Hall is ACTIVE, use **Confirm Hall is active · set position to zero**.
3. If Hall is clear and no reward was interrupted (for example, after an OTA update), use a bounded **Find Hall datum** in the appropriate direction, then confirm the active Hall if Recovery remains displayed.
4. After an interrupted reward, a saved reward-in-progress marker blocks Hall seeks. If Hall is clear, do not keep retrying. The supported fallback is USB Serial **`NVS RESET CONFIRM`** while motion is idle. It clears **all motion and calibration settings**, including pickup, delivery alignment and timing; Wi-Fi and OTA credentials remain. Use it only after checking whether a capsule already dropped and accepting the loss of calibration, then repeat first-startup calibration above. The reset clears the interrupted-reward marker; after reset, use the bounded Hall seek above if Hall reads clear.
5. If pickup is already taught, select **Go to taught pickup**. If not, jog and **Save pickup here**.
6. Return to the parent page and activate a new reward only when you intend to deliver one. Recovery does not automatically replace a consumed authorization.

After OTA, Recovery is intentional; the saved calibration is still available. You do not need to teach pickup again unless the mechanical alignment has changed.
