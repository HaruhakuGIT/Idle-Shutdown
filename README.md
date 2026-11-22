# Shutdown Warning

Shutdown Warning watches for keyboard and mouse inactivity and shows a synchronized warning on every screen. The warning plays a low siren, counts down, and ends with an animated blackout. Activity during the countdown cancels an automatic warning.

- **Start after inactivity** controls when the automatic warning begins.
- **Force shutdown after countdown** opts in to real power-off. An unavailable forced operation can fall back to an ordinary shutdown when the OS allows it. Unsaved work can be lost.
- **Preview** runs the animation and sound without requesting power-off.
- **Start at login** is opt-in. The checkbox follows the effective startup registration where the desktop exposes it.

## Install

**Windows x64.** Requires Windows 10 version 1703 or newer. Run the installer under your own account. It installs to `%LOCALAPPDATA%\Programs\Shutdown Warning` by default and adds an entry to **Settings → Apps → Installed apps** for removal. Click the tray icon to open settings. The installer and uninstaller close only the instance running from that installation. Existing settings are retained.

**Arch Linux x86_64.** Install the local package with `sudo pacman -U ./shutdown-warning-1.0.5-1-x86_64.pkg.tar.zst`. Run `shutdown-warning` to start in the background or `shutdown-warning --settings` to open settings. On a desktop without a tray, keep the settings window open for automatic warnings. Use `sudo pacman -R shutdown-warning` to uninstall.

### Hyprland and other Wayland compositors

Hyprland uses a user systemd service when **Start at login** is enabled. Launch once after upgrading to migrate an existing XDG autostart entry. For sessions that do not start their systemd graphical target, add the following line inside the existing `hyprland.start` handler. This path still checks **Start at login**.

```lua
hl.exec_cmd("/usr/bin/shutdown-warning --autostart")
```

Wayland compositors supporting `ext-idle-notify` v2 can report input inactivity while a video keeps the screen awake. Other sessions use KIdleTime. The tray requires a desktop StatusNotifier host or an X11 tray. In Quickshell configurations that hide new items, open the tray overflow and pin the icon. Power-off follows the local systemd-logind and polkit permissions.

## Build from source

### Windows

Install Visual Studio with its **Desktop development with C++** workload, the Windows 11 SDK (10.0.22000.0 or newer), CMake 3.24+, and NSIS to build the installer. From the repository root:

```powershell
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
pwsh -File ./packaging/windows/build-installer.ps1
```

The portable executable is in `build/windows/Release`, and the installer is in `dist`. On an already running copy, launch `ShutdownWarning.exe --settings` to open its settings. The installation can be removed using **Installed apps** or the generated `Uninstall.exe`.

### Arch Linux

From a local checkout:

```bash
sudo pacman -S --needed base-devel
makepkg -si
```

The PKGBUILD builds and tests from this checkout. To build and test directly with CMake, install the PKGBUILD dependencies and run:

```bash
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
```

The Windows and Linux interfaces use native desktop APIs, with shared audio synthesis in `app/common`. The test suite validates the generated PCM files, and the Linux renderer also has an offscreen frame test.

GitHub Actions builds Windows and Arch artifacts. Publishing releases and signing the Windows installer are separate from building them.
