#include "OtaApps.h"

#if CROSSPOINT_DUAL_BOOT

#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <spi_flash_mmap.h>

#include <cstdio>
#include <cstring>

#include "network/FirmwareFlasher.h"
#include "network/OtaBootSwitch.h"

namespace {

constexpr const char* kNamespace = "ota_names";

// "ota_0".."ota_15"
void slotKey(int slot, char* out, size_t outSize) {
  snprintf(out, outSize, "ota_%d", slot);
}

int slotOf(const esp_partition_t* p) {
  return p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
}

// ota_boot::switchTo() marks the newly selected slot's otadata state as "new"
// (pending verify). That is right for the reader's own firmware self-update,
// where the bootloader's rollback net should cover fresh untested code, and
// wrong for a dual-boot switch: we only ever point at an already-flashed,
// previously-working sibling. Left as "new", the next reset before the app
// confirms itself -- and neither app calls
// esp_ota_mark_app_valid_cancel_rollback() -- is silently rolled back to the
// other slot, which shows up as waking from sleep in the previous app.
//
// This finds the entry switchTo() just wrote (highest seq) and flips only its
// state to valid, leaving switchTo() itself, and so the self-update path, with
// rollback protection intact.
void confirmLastOtaSwitch() {
  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata || otadata->size < 2 * SPI_FLASH_SEC_SIZE) return;

  ota_boot::SelectEntry slots[2] = {};
  if (esp_partition_read(otadata, 0, &slots[0], sizeof(ota_boot::SelectEntry)) != ESP_OK ||
      esp_partition_read(otadata, SPI_FLASH_SEC_SIZE, &slots[1], sizeof(ota_boot::SelectEntry)) != ESP_OK) {
    return;
  }

  int newestIdx = -1;
  uint32_t newestSeq = 0;
  for (int i = 0; i < 2; ++i) {
    if (slots[i].ota_seq == 0xFFFFFFFFu) continue;
    if (slots[i].crc != ota_boot::computeSeqCrc(slots[i].ota_seq)) continue;
    if (newestIdx < 0 || slots[i].ota_seq > newestSeq) {
      newestIdx = i;
      newestSeq = slots[i].ota_seq;
    }
  }
  if (newestIdx < 0) return;

  constexpr uint32_t kOtaImgValid = 2;  // ESP_OTA_IMG_VALID
  slots[newestIdx].ota_state = kOtaImgValid;

  const size_t off = static_cast<size_t>(newestIdx) * SPI_FLASH_SEC_SIZE;
  if (esp_partition_erase_range(otadata, off, SPI_FLASH_SEC_SIZE) != ESP_OK) return;
  esp_partition_write(otadata, off, &slots[newestIdx], sizeof(slots[newestIdx]));
}

}  // namespace

void registerOtaAppName(const char* name) {
  const esp_partition_t* self = esp_ota_get_running_partition();
  if (!self || self->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_0 || self->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_15) {
    return;
  }
  char key[8];
  slotKey(slotOf(self), key, sizeof(key));

  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.putString(key, name);
  prefs.end();
}

int detectOtaApps(OtaAppEntry* apps, int maxApps) {
  int count = 0;
  const esp_partition_t* running = esp_ota_get_running_partition();

  Preferences prefs;
  const bool prefsOpen = prefs.begin(kNamespace, true);

  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it && count < maxApps) {
    const esp_partition_t* p = esp_partition_get(it);
    const bool isOtaSlot = p && p != running && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
                           p->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15;
    // Only a *different* project counts. This also rejects an empty or
    // unreadable slot, which has no app_desc to compare.
    if (isOtaSlot && firmware_flash::destHoldsForeignApp(p)) {
      const int slot = slotOf(p);
      char key[8];
      slotKey(slot, key, sizeof(key));

      OtaAppEntry& entry = apps[count];
      const String nvsName = prefsOpen ? prefs.getString(key, "") : String();
      if (nvsName.length() > 0) {
        strncpy(entry.name, nvsName.c_str(), sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = '\0';
      } else {
        // Never booted here, so it has not registered a display name yet.
        snprintf(entry.name, sizeof(entry.name), "OTA Slot %d", slot);
      }
      entry.partitionSubtype = p->subtype;
      count++;
    }
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  if (prefsOpen) prefs.end();
  return count;
}

void switchToOtaApp(int partitionSubtype) {
  const esp_partition_t* target =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, static_cast<esp_partition_subtype_t>(partitionSubtype), nullptr);
  if (!target) {
    LOG_ERR("BOOT", "dual-boot target subtype 0x%02X not found", partitionSubtype);
    return;
  }
  if (!ota_boot::switchTo(target)) return;
  confirmLastOtaSwitch();
  LOG_INF("BOOT", "switching to %s", target->label);
  esp_restart();
}

#else  // !CROSSPOINT_DUAL_BOOT

void registerOtaAppName(const char*) {}
int detectOtaApps(OtaAppEntry*, int) { return 0; }
void switchToOtaApp(int) {}

#endif
