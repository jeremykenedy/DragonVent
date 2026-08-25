# Reverting to stock firmware

PandaVent deliberately keeps the stock bootloader, partition table, NVS
namespace, and OTA layout. Going back is one upload, and stock arrives with
all its factory features working.

## Over the air (the normal path)

1. Get the stock image for the Panda Vent from
   [bigtreetech/Panda-Vent](https://github.com/bigtreetech/Panda-Vent).
2. In PandaVent: Settings, "Open device setup", firmware update card.
3. Upload the stock `.bin`. The updater accepts it (stock `panda_vent`
   images are explicitly allowed), flashes the inactive slot, and reboots
   into stock.

Your Wi-Fi and printer binding normally survive, since PandaVent stored
them in the stock keys. If anything looks off, run stock's factory reset:
it also clears the handful of PandaVent-only settings left in NVS, which
are otherwise harmless.

## Rolling back one version

The previous firmware stays in the inactive OTA slot after every update,
so re-uploading the prior PandaVent release the same way is equally easy.

## If the web page never comes back (USB recovery)

An interrupted upload cannot corrupt the running system (the updater
verifies the image before switching), but if a device is ever left with no
working web server, recovery is over USB with esptool:

```
python -m esptool --chip esp32 -b 460800 write_flash 0x10000 <image.bin>
```

The stock bootloader and partition table live below 0x10000 and are never
touched by OTA updates, so flashing the app at 0x10000 is sufficient.
