#ifndef ASSET_STORAGE_H
#define ASSET_STORAGE_H

// Where the pictures come from, chosen with ASSETS_ON_SD_CARD in platformio.ini.
// Both are mounted at /sdcard, so the rest of the code does not care which one it is.
#if ASSETS_ON_SD_CARD
#include "sd_storage.h"
using AssetStorage = SdStorage;
#else
#include "flash_storage.h"
using AssetStorage = FlashStorage;
#endif

#endif
