# SteamIconFix

Native Windows utility for replacing icons on Steam `.lnk` and `.url` shortcuts.

## Build

Requirements: Visual Studio 2022 with the C++ desktop workload, Windows 10 SDK, and CMake.

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run those commands from **x64 Native Tools Command Prompt for VS 2022** (or first run `vcvars64.bat`). The distributable is `build/SteamIconFix.exe`. It is a native x64 executable linked to the static MSVC runtime; it does not require .NET or a separate application runtime.

Drag a Steam `.lnk` or `.url` shortcut into the window, select an executable icon, and choose **修复所选图标**. The shortcut is the only file modified. Logs are written to `%TEMP%\SteamIconFix.log`.

## Icon display delay

Windows Explorer may keep an icon cache after a shortcut is repaired, so the new icon may not appear immediately even after a normal desktop refresh. To present all previous repair results at once, click the **重启资源管理器** button in the bottom-right corner. This briefly restarts the Windows Explorer shell (the taskbar and desktop may disappear and reappear, and open Explorer windows may close).
