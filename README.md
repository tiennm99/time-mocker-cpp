# TimeMocker — C++ / MS Detours Edition

A Windows tool that injects fake time into running processes by hooking Win32 time APIs using **Microsoft Detours**.

Port/rewrite of the original C# / EasyHook version with identical IPC design but in native C++.

---

## Architecture

```
TimeMocker.sln
├── Shared/
│   └── MockTimeInfo.h          ─ Named MMF layout (shared between UI and Hook)
│
├── TimeMocker.Hook/            ─ DLL injected into target processes
│   ├── dllmain.cpp             ─ DllMain: opens MMF, installs/removes Detours hooks
│   └── exports.cpp             ─ Sentinel export for version check
│
├── TimeMocker.Injector/        ─ Static library: injection + IPC management
│   ├── InjectionManager.h/.cpp ─ LoadLibrary remote-thread injection + SharedMemoryHandle
│   └── ProcessWatcher.h        ─ Background thread: auto-inject via glob/regex rules
│
└── TimeMocker.UI/
    └── main.cpp                ─ Interactive console controller
```

---

## Hooked APIs

| API | DLL |
|-----|-----|
| `GetSystemTime` | kernel32 |
| `GetLocalTime` | kernel32 |
| `GetSystemTimeAsFileTime` | kernel32 |
| `GetSystemTimePreciseAsFileTime` | kernel32 |
| `NtQuerySystemTime` | ntdll |

All hooks read `DeltaTicks` from the named MMF on each call and return
`real_utc_filetime_ticks + DeltaTicks`. When `DeltaTicks == 0` the real
time passes through unchanged.

---

## IPC Design

```
Named Memory-Mapped File: TimeMocker_<PID>
Size: 8 bytes

[0..7]  DeltaTicks  (LONGLONG, 100-ns units, signed)
        = fakeUtcTicks - realUtcTicks at the moment the user clicks "Set"
```

- The UI creates the MMF **before** injection so the hook can open it in `DllMain`.
- Updates are written as atomic `InterlockedExchange64` — no locks on the hot path.
- The hook reads with a single volatile 8-byte load (~4 ns, no syscall).
- When the UI closes the MMF (on eject/exit), the hook's next read returns `0`,
  silently falling back to real time.

---

## How Detours Works Here

```
Before injection:
  kernel32!GetSystemTime → [real implementation]

After injection:
  kernel32!GetSystemTime → Hook_GetSystemTime (our function)
                         → Real_GetSystemTime (trampoline, via DetourAttach)
```

`DetourAttach` patches the first 5–14 bytes of the real function with a
`JMP` to our hook, and saves the overwritten bytes + a JMP back in a
trampoline so we can call the original.

---

## Requirements

- **Windows 10/11 x64**
- **Visual Studio 2022** (v143 toolset) or **CMake 3.20+**
- **Microsoft Detours** (installed via `scripts/setup.ps1` → vcpkg)
- Must run as **Administrator** (cross-process injection requires elevated privileges)

---

## Setup & Build

### Option A — Visual Studio 2022

```powershell
# 1. Install Detours via vcpkg (one-time)
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1

# 2. Open solution
start TimeMocker.sln

# 3. Build → Release | x64
```

Output:
```
x64\Release\TimeMocker.exe            ← controller
x64\Release\TimeMocker.Hook.x64.dll  ← hook DLL (must be next to .exe)
x64\Release\TimeMocker.Hook.x86.dll  ← hook DLL for 32-bit targets
```

### Option B — CMake + vcpkg

```powershell
# Install vcpkg + detours
git clone https://github.com/microsoft/vcpkg vcpkg
.\vcpkg\bootstrap-vcpkg.bat -disableMetrics
.\vcpkg\vcpkg install detours:x64-windows detours:x86-windows

# Configure + build
cmake -B build -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake -A x64
cmake --build build --config Release
```

---

## Usage (Console Controller)

```
timemocker> help

  list [filter]              list running processes
  inject <pid>               inject hook DLL into process
  eject  <pid>               remove hook from process
  time   YYYY-MM-DD HH:MM:SS set fake time (local)
  time   now                 reset to real time
  status                     show injected processes + delta
  rule add <pattern>         add glob auto-inject rule
  rule add -r <pattern>      add regex auto-inject rule
  rule list                  list rules
  rule del <index>           remove rule by index
  watch start / stop         control auto-inject watcher
  quit                       exit
```

### Examples

```
# List all processes with "notepad" in name/path
timemocker> list notepad

# Inject into Notepad (PID 12345) and set time to Jan 1 2020
timemocker> time 2020-01-01 00:00:00
timemocker> inject 12345

# Change time while injected (takes effect immediately)
timemocker> time 2025-12-31 23:59:00

# Auto-inject any process matching a glob
timemocker> rule add C:\Games\MyGame\*

# Auto-inject chrome.exe by name
timemocker> rule add *chrome*

# Auto-inject using regex
timemocker> rule add -r ^.*\\MyApp\.exe$

# Reset to real time and eject
timemocker> time now
timemocker> eject 12345
```

---

## Project Details

### DLL Injection Method

Uses the classic **LoadLibrary remote thread** technique:

1. `OpenProcess` with `PROCESS_CREATE_THREAD | PROCESS_VM_*` rights
2. `VirtualAllocEx` → write DLL path string into target's memory
3. `CreateRemoteThread(LoadLibraryW, dllPath)` → loads the DLL in-process
4. `WaitForSingleObject` → confirm load completed
5. On `DLL_PROCESS_ATTACH` the hook's `DllMain` opens the MMF and installs Detours

For **new processes** (before they start), `DetourCreateProcessWithDllEx()` can also
be used — it's included in the Detours SDK and injects before the first instruction
runs, before any DRM/anti-cheat initialises.

### 32-bit vs 64-bit

- **64-bit target**: `TimeMocker.Hook.x64.dll` is injected
- **32-bit target** (WOW64): `TimeMocker.Hook.x86.dll` is injected
- Both Hook DLLs are identical source; the platform is selected at build time
- The UI (x64) determines which DLL to use by calling `IsWow64Process`

### Auto-Inject Watcher

`ProcessWatcher` runs a background thread that polls `CreateToolhelp32Snapshot`
every 1.5 seconds. Any process whose full path or `.exe` name matches an enabled
rule is automatically injected with the current fake time delta.

Previously-seen PIDs are tracked in a `HashSet` so they're only matched once,
even if the process restarts with the same PID (very rare on Windows).

### Limitations

| Limitation | Notes |
|-----------|-------|
| **64-bit injector only** | The UI is x64-only; a separate x86 injector would be needed for 32-bit-only scenarios |
| **Anti-cheat / protected processes** | EAC, BattlEye, and system PPL processes block `OpenProcess` |
| **QueryPerformanceCounter** | Not affected — it reads a hardware MSR; EasyHook/Detours cannot intercept kernel MSR reads |
| **GetTickCount / timeGetTime** | Not hooked in this version — easy to add following the same pattern |
| **Hot eject** | DLL stays loaded but hooks are removed on unload (not forcible without `FreeLibrary` in a remote thread) |

---

## Adding a New Hook

1. Declare the real function pointer in `dllmain.cpp`:
   ```cpp
   static DWORD (WINAPI* Real_GetTickCount)() = GetTickCount;
   ```
2. Write the hook function:
   ```cpp
   static DWORD WINAPI Hook_GetTickCount()
   {
       // Convert fake UTC ticks to milliseconds since boot (approximate)
       return static_cast<DWORD>(GetFakeUtcTicks() / 10000);
   }
   ```
3. Add `DetourAttach` / `DetourDetach` calls in `InstallHooks` / `RemoveHooks`.

---

## License

Apache 2.0 (same as the original C# project).
