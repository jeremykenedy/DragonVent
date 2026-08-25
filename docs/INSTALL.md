# Installing PandaVent

The whole install happens in a browser, over the air. Stock firmware stays
one upload away at all times (see [REVERT.md](REVERT.md)).

## Before you start

1. A BIQU Panda Vent running stock firmware, already on your Wi-Fi (or
   fresh out of the box).
2. The PandaVent OTA image (`dragonvent-vX.Y.Z-*-ota.bin`) from this
   project's releases.
3. Two minutes.

Keep a copy of BigTreeTech's stock firmware around as your way back:
download it from [bigtreetech/Panda-Vent](https://github.com/bigtreetech/Panda-Vent)
before you flash anything.

## Flash from stock

1. Open the stock web page (the address shown in the stock setup, or its
   hostname on your network).
2. Settings, then the firmware update card.
3. Choose the PandaVent `.bin` and upload. The device flashes the inactive
   slot, verifies it, and reboots into PandaVent.

Your Wi-Fi credentials and printer binding CARRY OVER: PandaVent reads the
stock settings on first boot, connects to the same network, and adopts the
bound Bambu printer automatically.

## First boot

- Already on Wi-Fi (normal case): browse to `http://pandavent.local/`.
- Fresh device with no Wi-Fi saved: PandaVent opens a setup hotspot named
  `PandaVent_XXXX` (password `987654321`). Join it and browse to
  `http://192.168.254.1/`; the setup screen opens by itself.

On the setup screen, in order:

1. **Device identity**: name your device. The name becomes
   `http://<name>.local/`. Restart to apply it.
2. **Wi-Fi**: pick your network and save (the device reboots onto it).
3. **Printer source**: Bambu LAN (use "Search for printers on the network",
   pick yours, enter the LAN access code) or Klipper/Moonraker, or
   Standalone.

Everything applies immediately except the name, which needs the one
restart. That is the whole install.

## After installing

- Lighting, per-state effects, and animations: the Lighting page.
- Vent thresholds and filament seal rules: the Auto page.
- If your printer's IP changes later, PandaVent rediscovers it by serial
  and rebinds on its own.
