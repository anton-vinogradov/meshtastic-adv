#pragma once

#include <cstdint>

namespace advui
{

// Called immediately after LittleFS is mounted, before NodeDB creates defaults.
void advProfileCaptureBootState();

// Called after the shared SPI bus is ready. A successful restore reboots before returning.
void advProfileRestoreIfNeeded();

// Settings writers call this after their internal transaction commits.
void advProfileMarkDirty();

// PhoneAPI config streams and SD profile transactions share the same small,
// fragmented heap. These hooks serialize their peak-allocation windows.
void advProfileApiConfigStart();
void advProfileApiConfigEnd();

// Cheap unless a debounced SD synchronization is due.
void advProfileSyncTick();

} // namespace advui
