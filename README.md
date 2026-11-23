# TRMNL Mirror for the Sunton ESP32-2432S028R

This firmware turns the 2.8-inch 320 × 240 ILI9341 version of the Sunton ESP32-2432S028R into a mirror of an existing TRMNL. It checks the [TRMNL current-screen API](https://docs.trmnl.com/go/private-api/screens) every 30 seconds. The original device controls its own playlist. The mirror trims fully white columns on each side by up to one eighth of the source width, then scales the remaining frame to 320 × 240. Area-weighted scaling keeps fine lines and text legible.

The current-screen API uses the original TRMNL's **Device API Key** from **Devices → Edit → Developer Perks**. The original device needs Developer Edition or an existing BYOD license. A separate license for the mirror is unnecessary.

## Build and install

With PlatformIO and Python installed, run the following from this repository. Replace `COM6` with your device's serial port.

```powershell
python -m platformio run
python -m platformio run -t upload --upload-port COM6
python -m pip install pyserial
python tools/provision.py COM6
```

Paste the Device API Key when prompted. Provisioning sends the key directly over USB and saves it in the ESP32's non-volatile storage. Updates installed without a full flash erase retain the saved key and Wi-Fi settings. A full flash backup contains those credentials, so keep it private.

When no saved Wi-Fi network is available at startup, join the **TRMNL-Mirror** Wi-Fi network. Use the password shown on the display and open `http://192.168.4.1` to configure the 2.4 GHz network. Hold **BOOT** for 2.5 seconds while the firmware is running to reopen setup. Briefly press **BOOT** to check for a newer frame.

## Verify

```powershell
python -m platformio device monitor --port COM6 --baud 115200
```

The USB log reports `TRMNL FRAME` when a decoded PNG has been sent to the TFT and `TRMNL unchanged` when the source still shows the same frame. Send `TRMNL STATUS` followed by a newline to check Wi-Fi, clock, key, image storage and frame status. The mirror keeps displaying the most recent frame during a network or download error.
