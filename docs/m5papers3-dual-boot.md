# M5PaperS3 dual-boot: two projects, one device

The M5PaperS3 dev unit carries **two firmwares at once**, so more than one
project can be developed against the same physical board without reflashing the
layout every time we switch:

| slot | subtype | offset     | size   | app                                        |
|------|---------|------------|--------|--------------------------------------------|
| app0 | `ota_0` | `0x20000`  | 6656K  | CrossPoint reader (this repo, ~5.2MB today) |
| app1 | `ota_1` | `0x6A0000` | 6656K  | MicroBASIC (`MicroBASIC-PaperS3`)           |

The bootloader picks between them from the 8KB `otadata` partition, so
switching apps writes 32 bytes and never touches an app image.

The layout lives in [`partitions_m5papers3.csv`](../partitions_m5papers3.csv),
and the identical table lives in `MicroBASIC-PaperS3/editor/partitions.csv`.
**Those two files must stay byte-identical below their comment headers** —
there is one table on the device, and each project only describes it.

```
nvs       data  nvs       0x9000     32K
otadata   data  ota       0x11000     8K
app0      app   ota_0     0x20000   6656K   <- CrossPoint
app1      app   ota_1     0x6A0000  6656K   <- MicroBASIC
coredump  data  coredump  0xD20000    64K
spiffs    data  spiffs    0xD30000  2880K   (reserved; nothing uses it today)
```

Slots are symmetric at 6656K rather than sized to today's binaries. CrossPoint
needs ~5.2MB and sets the size for both; MicroBASIC uses ~1.7MB of its slot.
Whatever project lands in a slot next should not force a re-partition — the
whole point is that this table is written **once**.

`nvs` is 32K rather than stock CrossPoint's 20K: 16/20K could not hold BLE
bonds, saved WiFi credentials and the WiFi radio's own PHY calibration blob at
the same time, and that calibration write failing silently is what stopped WiFi
associating at all on this unit.

## One-time migration

The unit currently runs MicroBASIC alone on a 3-partition layout with a 3MB
`app0` — CrossPoint does not fit in it. Moving to the shared table means
writing the table itself plus both apps, once.

Deliberately **not** via `pio run -t upload`: that also writes a freshly
compiled `bootloader.bin` over the one on the device. The bootloader currently
in flash is M5Launcher's original and is the only one ever confirmed to work
here. It is no longer *believed* to be load-bearing (the "only Launcher's
bootloader works" theory was disproven — see freeink-sdk's
`docs/m5papers3-support.md`), but a fresh one has not been tested since, so
leave it alone and there is nothing to undo.

Every command below runs **from this repo's root** (the paths to
`partitions_m5papers3.csv` and to the build output are relative to it), with
`PORT` set:

```bash
cd ~/github/crosspoint-reader-m5papers3
export PORT=$(ls /dev/cu.usbmodem* | head -1)
```

Back up first:

```bash
esptool.py --chip esp32s3 --port "$PORT" --baud 921600 read_flash 0x0 0x1000000 backup.bin
```

Then:

```bash
python3 ~/.platformio/packages/framework-espidf/components/partition_table/gen_esp32part.py \
    --flash-size 16MB partitions_m5papers3.csv /tmp/pt.bin
esptool.py --chip esp32s3 --port "$PORT" --baud 921600 \
    write_flash 0x8000 /tmp/pt.bin
esptool.py --chip esp32s3 --port "$PORT" erase_region 0x9000 0x8000    # nvs, moved/resized
esptool.py --chip esp32s3 --port "$PORT" erase_region 0x11000 0x2000   # otadata -> boots app0
esptool.py --chip esp32s3 --port "$PORT" --baud 921600 \
    write_flash 0x20000 .pio/build/m5papers3/firmware.bin
```

Then build MicroBASIC and write its `firmware.bin` at `0x6A0000`:

```bash
cd ~/github/MicroBASIC-PaperS3/editor && pio run
esptool.py --chip esp32s3 --port "$PORT" --baud 921600 \
    write_flash 0x6A0000 .pio/build/m5papers3/firmware.bin
```

An erased `otadata` makes the bootloader pick `ota_0`, so the unit comes up in
CrossPoint. Power-cycle with the physical button.

## Day to day

Build, then write only the app, only into your own slot:

```bash
pio run -e m5papers3
esptool.py --chip esp32s3 --port "$PORT" --baud 921600 \
    write_flash 0x20000 .pio/build/m5papers3/firmware.bin
```

`board_upload.offset_address` and `board_upload.maximum_size` in
`[env:m5papers3]` track the `app0` row, so "Checking size" measures against the
real 6656K ceiling.

**Do not use `pio run -t upload` for routine flashing.** It writes four images,
not one: `bootloader.bin` at `0x0`, the partition table at `0x8000`,
`boot_app0.bin` at `0x11000` (which resets `otadata` to "boot slot 0"), and the
app. From this repo that silently forces the reader to be the booted app; from
MicroBASIC it flashes app1 and then boots app0 instead, which reads as "my
flash didn't take".

## Switching which app boots

### From the host

```bash
scripts/m5papers3-boot-slot.sh        # what is selected now
scripts/m5papers3-boot-slot.sh 0      # CrossPoint
scripts/m5papers3-boot-slot.sh 1      # MicroBASIC
```

That wraps ESP-IDF's own `otatool.py`, so the `otadata` entry (a sequence
number plus its CRC) is written by the vendor's implementation rather than a
hand-rolled one. Power-cycle with the physical button afterwards.

### From the device itself

The Home menu lists any dual-boot sibling **after Settings** — "MicroBASIC"
once that firmware has registered its own name, "OTA Slot 1" before it ever
has. Selecting it points otadata at that slot and reboots straight into it.

There is no way back from this side yet: MicroBASIC has to grow the same entry
point. Until it does, `editor/boot-slot.sh 0` over USB is the return path.

`src/util/OtaApps.h` holds the whole mechanism, and only three things had to
change around it — the Home menu's item count, its `default:` case, and one
`registerOtaAppName("CrossPoint")` in `main.cpp`. It is gated behind
`CROSSPOINT_DUAL_BOOT`, set only in `[env:m5papers3]`, so every other target
compiles the stubs: nothing is detected and the menu grows no entries.

A slot only counts as a sibling if it holds a *different* project, tested with
the same `esp_app_desc_t.project_name` comparison the self-update guard uses.
An empty slot, or a stale A/B copy of this same firmware, is not offered.

Two details worth knowing, both inherited from MicroWriter's patch sets
(`MicroBASIC-PaperS3/patches/`, `MicroWriter/patches/crosspoint-1.5.0/`), which
this is a direct port of:

* The switch goes through `ota_boot::switchTo()` rather than
  `esp_ota_set_boot_partition()`, which fails on this silicon with a bogus
  efuse-blk-rev verification error.
* `switchTo()` leaves the new slot's otadata state as "new" (pending verify),
  which is right for a genuine firmware update and wrong here: neither app
  calls `esp_ota_mark_app_valid_cancel_rollback()`, so the next reset would be
  rolled back to the other slot — it shows up as waking from sleep in the
  previous app. `confirmLastOtaSwitch()` flips just that entry to valid,
  leaving the self-update path's rollback protection intact.

## The sibling-slot guard

`esp_ota_get_next_update_partition()` returns "the other OTA slot" — which on
this unit is the other *project*, not a spare copy of this firmware. Without a
guard, CrossPoint's own SD/OTA self-update would erase MicroBASIC.

`firmware_flash::destHoldsForeignApp()` compares the target partition's
embedded `esp_app_desc_t.project_name` against the running app's and refuses
with `Result::SIBLING_APP_PROTECTED` before erasing a single byte. An unflashed
or unreadable target counts as safe — there is no sibling to protect.

This means **self-update is disabled on this unit while both slots are
occupied**, by design. To update the reader, flash `app0` over USB.

## If something goes wrong

Full-flash backups live outside this repo in `~/Desktop/M5PaperS3-backup/`,
each with its own `RESTAURAR.md`, including one with M5Launcher + CrossPoint
intact if the Launcher picker is ever wanted back.

Read the live table before trusting any offset here:

```bash
esptool.py --chip esp32s3 --port "$PORT" read_flash 0x8000 0xC00 /tmp/pt.bin && \
python3 ~/.platformio/packages/framework-espidf/components/partition_table/gen_esp32part.py /tmp/pt.bin
```

Offsets have moved once already, and `partitions.csv` drifting from the device
caused two separate misfires before that.
