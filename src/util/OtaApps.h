#pragma once

// Dual-boot sibling apps.
//
// The M5PaperS3 dev unit carries two unrelated firmwares at once -- this reader
// in app0 and MicroBASIC in app1 -- selected by the bootloader from otadata
// (see docs/m5papers3-dual-boot.md). This is what puts the sibling on the Home
// menu and switches to it.
//
// The register/detect/switch scheme is ported from MicroSlate
// (github.com/Josh-writes/microslate-firmware), itself adapted from CrossInk's
// build-time dual-boot patch (uxjulia/CrossInk), and matches the patch sets in
// MicroBASIC-PaperS3/patches/ so the two stay diffable. Identifier names are
// deliberately kept the same as those patches.
//
// The low-level otadata write is NOT duplicated here: ota_boot::switchTo()
// already does it, and already works around esp_ota_set_boot_partition()'s
// bogus efuse-blk-rev verification failure on this silicon.
//
// Compiled to stubs unless CROSSPOINT_DUAL_BOOT is set (see [env:m5papers3] in
// platformio.ini), so every other target behaves exactly as before: detect
// finds nothing and the Home menu grows no entries.

#ifndef CROSSPOINT_DUAL_BOOT
#define CROSSPOINT_DUAL_BOOT 0
#endif

constexpr int MAX_OTA_APPS = 4;

struct OtaAppEntry {
  char name[32];
  int partitionSubtype;
};

// Records this app's display name in shared NVS, keyed by the OTA slot it is
// running from, so a sibling app can list it by name instead of by slot number.
// Call once at boot.
void registerOtaAppName(const char* name);

// Fills `apps[]` with the sibling apps in the other OTA slots and returns how
// many were found. A slot is a sibling only if it holds a *different* project
// than the one running (same esp_app_desc_t.project_name test the self-update
// guard uses), so an empty slot, or a stale A/B copy of this same firmware,
// is not offered as something to switch to.
int detectOtaApps(OtaAppEntry* apps, int maxApps);

// Points otadata at `partitionSubtype` and restarts into it. Does nothing if
// that partition does not exist or the otadata write fails.
void switchToOtaApp(int partitionSubtype);
