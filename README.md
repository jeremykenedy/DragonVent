# PandaVent

Custom firmware for the **BIQU Panda Vent** chamber vent: per-state LED
lighting, print-progress effects, RAM animations, a live dashboard, and
self-healing printer binding, in a fast single-page web UI served from the
device itself.

PandaVent is built on [DragonVent](https://github.com/justinh-rahb/DragonVent)
by Justin Hayes and the shared
[dragon-core](https://github.com/justinh-rahb/dragon-core) components, over
the stock BIQU/BigTreeTech partition layout, so installing and reverting are
both plain over-the-air uploads. No cables, no soldering, no printer mods.

## Highlights

- **Dashboard**: live printer telemetry (nozzle, bed, chamber, fans, Wi-Fi),
  a real progress job card, a "Lighting now" card showing exactly what the
  strips are rendering, and a top bar with the printer state at a glance.
- **Per-state lighting**: every printer state (idle, preparing, printing,
  paused, completed, error) can have its own effect, brightness, speed, and
  direction, or run one global effect.
- **Progress effects**: progress bar, animated progress with a breathing
  head, and a two-color striped barber pole that crawls through the fill.
- **RAM animations**: upload any image and its pixel rows play as frames on
  the strips (kept in RAM by design, so the stock partition table and the
  revert path stay untouched).
- **Hot-bed warning layer** above the temperature gradient, below the
  print-error flash.
- **Self-healing printer binding**: connect failures say why (wrong access
  code versus unreachable), config changes apply instantly, and if DHCP
  moves your printer the vent rediscovers it by serial and rebinds itself.
- **Vent control**: the automatic bed-temperature policy, filament seal
  rules, manual control, calibration, and printer-fan control from upstream
  DragonVent, all kept.
- **Quality of life**: device naming from the setup screen, printer unbind,
  a plain restart button, a configurable power-ring, English and Simplified
  Chinese.

## Install

Over the air from stock firmware, in a browser. See
[docs/INSTALL.md](docs/INSTALL.md).

## Revert to stock

One OTA upload of BigTreeTech's stock image, any time. See
[docs/REVERT.md](docs/REVERT.md). PandaVent keeps the stock bootloader,
partition table, and OTA layout precisely so this stays true.

## Safety model

- The device never writes to your printer, with one deliberate exception:
  the Fans screen sends `M106` fan gcode. Everything else is read-only LAN
  MQTT (Bambu) or Moonraker polling.
- The OTA updater refuses images that are not PandaVent/DragonVent or stock
  Panda Vent, so a wrong file cannot be made the boot image.
- Settings live in the stock NVS namespace; stock's factory reset clears
  them completely after a revert.

## Vendored components

`firmware/components/` carries product components plus vendored copies of
`dc_bambu`, `dc_wifi`, `dc_ui`, and `dc_lighting` from dragon-core, extended
for this hardware. The remaining dragon-core dependencies are pinned by
exact version in `firmware/main/idf_component.yml`.

## Building

ESP-IDF v5.3.1, target esp32:

```
bash tools/idf-build.sh firmware esp32 build
```

The OTA image lands at `firmware/build/dragonvent.bin`.

## Credits and license

MIT. PandaVent is a derivative of DragonVent (c) Justin Hayes, and both
notices are retained in [LICENSE](LICENSE). Panda Vent is a BIQU /
BigTreeTech product; this project is not affiliated with or endorsed by
BIQU, BigTreeTech, or Bambu Lab.
