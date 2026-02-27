#pragma once
// =============================================================================
// TimeMocker Shared IPC
// Named Memory-Mapped File layout shared between UI and Hook DLL.
// One MMF per injected process, named: TimeMocker_<PID>
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

static const wchar_t* MMF_PREFIX = L"TimeMocker_";
static const DWORD    MMF_SIZE   = sizeof(LONGLONG); // 8 bytes: DeltaTicks (Int64)

// The only field: signed tick offset to add to QueryUnbiasedInterruptTime / FILETIME.
// A tick = 100 nanoseconds (same unit as FILETIME / SYSTEMTIME internals).
// DeltaTicks == 0  → real time (pass-through)
// DeltaTicks  > 0  → future (clock is ahead)
// DeltaTicks  < 0  → past   (clock is behind)
#pragma pack(push, 1)
struct MockTimeInfo
{
    LONGLONG DeltaTicks; // Offset to add to the real UTC FILETIME ticks
};
#pragma pack(pop)

// Helper: build the MMF name for a given PID
inline void GetMmfName(DWORD pid, wchar_t* buf, size_t cchBuf)
{
    swprintf_s(buf, cchBuf, L"%ls%lu", MMF_PREFIX, pid);
}
