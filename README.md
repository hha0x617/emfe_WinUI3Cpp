# emfe_WinUI3Cpp

[![Build and Release](https://github.com/hha0x617/emfe_WinUI3Cpp/actions/workflows/build.yml/badge.svg)](https://github.com/hha0x617/emfe_WinUI3Cpp/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/hha0x617/emfe_WinUI3Cpp?include_prereleases&sort=semver)](https://github.com/hha0x617/emfe_WinUI3Cpp/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)

[日本語 (README_ja.md)](README_ja.md)

**C++ / WinUI 3** front-end for the emfe plugin architecture.

The host dynamically loads plugin DLLs such as
[emfe_plugins/mc68030](https://github.com/hha0x617/emfe_plugins/tree/master/mc68030)
via `LoadLibrary` + `GetProcAddress` and surfaces a register panel,
disassembly view, memory dump, and console window.

*Developed through vibe coding with
[Claude Code](https://docs.anthropic.com/en/docs/claude-code).*

## Features

- Em68030-style layout (menu + toolbar + register / disassembly / memory /
  status bar)
- Register panel: data-driven dynamic layout (D0–D7 / A0–A7 two-column
  grid + Flags checkboxes + Special / FPU / MMU)
- Disassembly: PC-line background highlight + breakpoint indicator (`●`)
- Memory dump: 16×16 cell grid + ASCII column + edit mode
- Execution control: Step (F10), Step Over (F11), Step Out (Shift+F11),
  Run (F5), Stop (Shift+F5), Reset, Full Reset
- Serial Console window: separate window, green/black colour scheme,
  auto-show, keyboard input
- Settings dialog: plugin-supplied setting defs rendered as dynamic UI
- Dark theme (including title bar via `DWM_USE_IMMERSIVE_DARK_MODE`)

## Directory layout

```
emfe_WinUI3Cpp/
├── emfe_WinUI3Cpp.sln
├── README.md
└── emfe/
    ├── emfe.vcxproj
    ├── App.xaml / App.xaml.h / App.xaml.cpp
    ├── MainWindow.xaml / MainWindow.xaml.h / MainWindow.xaml.cpp
    ├── PluginLoader.h      LoadLibrary wrapper for plugin DLLs
    ├── pch.h / pch.cpp
    └── x64/Release/        build output
```

## Dependencies

[emfe_plugins](https://github.com/hha0x617/emfe_plugins) is vendored as a
submodule at `external/emfe_plugins`.  `git clone --recurse-submodules`
pulls it automatically; a plain clone needs `git submodule update --init`.

The vcxproj references these paths **relative to itself**
(`emfe/emfe.vcxproj`):

| Depends on | Expected path (from `emfe/emfe.vcxproj`) | Purpose |
|-----------|------------------------------------------|---------|
| `emfe_plugin.h` | `..\external\emfe_plugins\api\` | C ABI header, required at build time |
| `emfe_plugin_mc68030.dll` | `..\external\emfe_plugins\mc68030\build\bin\Release\` | Runtime plugin (copied if built) |
| `emfe_plugin_em8.dll` | `..\external\emfe_plugins\em8\build\bin\Release\` | Runtime plugin (copied if built) |
| `emfe_plugin_z8000.dll` | `..\external\emfe_plugins\z8000\build\bin\Release\` | Runtime plugin (copied if built) |
| `emfe_plugin_mc6809.dll` | `..\external\emfe_plugins\mc6809\target\release\` | Runtime plugin (copied if built) |

Each `<Content Include>` guards with `Condition="Exists(...)"`, so missing
plugin DLLs are silently skipped — the host simply won't see them in the
Switch Plugin dialog.  Build the plugins inside
`external/emfe_plugins/<name>/` first if you want them bundled.

At build time the vcxproj copies the DLLs into the output directory's
`plugins\` subdirectory.  At startup the front-end scans
`plugins\emfe_plugin_*.dll` and lists the results in the "Switch Plugin"
dialog.

### System requirements

- Windows 10 (10.0.17763.0) / 11
- Visual Studio 2026 (v145 toolset)
- Windows App SDK 1.8.x (restored automatically via NuGet)
- Windows SDK 10.0.26100

## Build

```bash
# NuGet restore
MSBuild emfe/emfe.vcxproj -t:Restore -p:Configuration=Release -p:Platform=x64

# Build
MSBuild emfe/emfe.vcxproj -p:Configuration=Release -p:Platform=x64
```

Open `emfe_WinUI3Cpp.sln` in Visual Studio 2026 to build / debug (F5) as
well.

Output: `emfe/x64/Release/emfe.exe`

## Running

### Placing plugin DLLs

Put plugin DLLs (`emfe_plugin_*.dll`) in a `plugins\` subdirectory next to
`emfe.exe`.  The vcxproj copies them automatically from each plugin's build
output (manual placement is also fine).

DLL discovery:

- Only `<exe_dir>\plugins\emfe_plugin_*.dll` is scanned
- The previously-selected plugin path is persisted to
  `%LOCALAPPDATA%\emfe_WinUI3Cpp\appsettings.json` and restored on
  launch
- When several candidates are available, switch via **File → Switch
  Plugin...**

### Basic usage

1. Run `emfe.exe`
2. **File → Open ELF...** (Ctrl+E) or **Open S-Record...** (Ctrl+S) to load
   a program
3. **View → Serial Console** to open the serial console window (auto-shown as well)
4. **Run (F5)** / **Step (F10)** to execute
5. **Double-click a disassembly line** to toggle a breakpoint
6. **Settings → Emulator Settings...** to open the settings dialog
   (BoardType, memory size, SCSI disks, etc.)

## Keyboard shortcuts

### Serial Console window

| Shortcut | Action |
|----------|--------|
| **Ctrl+Shift+C** | Copy the current selection to the host clipboard |
| **Ctrl+Shift+V** | Paste host clipboard text into the guest UART |
| **Ctrl+Shift+A** | Select all |

### Framebuffer window

| Shortcut | Action |
|----------|--------|
| **Click**        | Capture host keyboard and hide the host cursor |
| **Esc**          | Release captured input |
| **Ctrl+Shift+V** | Paste host clipboard text as synthetic key events |

Framebuffer paste sends Linux `KEY_*` scancodes that match a **US
keyboard layout**.  The layer that converts scancodes into characters
lives in the guest (kernel keymap at the Linux text console, `xkb`
keymap in X11).  If the guest X session is configured for a non-US
layout (e.g. JIS), characters will be substituted — Shift+9 will
produce `)` instead of `(`, etc.  Workarounds are documented in the
guest setup_framebuffer guide:

- [Em68030-Guest-Linux setup_framebuffer.md](https://github.com/hha0x617/Em68030-Guest-Linux/blob/main/docs/setup_framebuffer.md)
- [Em68030-Guest-NetBSD setup_framebuffer.md](https://github.com/hha0x617/Em68030-Guest-NetBSD/blob/main/docs/setup_framebuffer.md)

## Related projects

- [emfe_plugins/api](https://github.com/hha0x617/emfe_plugins/tree/master/api) — shared C ABI headers + developer docs
- [emfe_plugins/mc68030](https://github.com/hha0x617/emfe_plugins/tree/master/mc68030) — MC68030 plugin DLL
- [emfe_plugins/em8](https://github.com/hha0x617/emfe_plugins/tree/master/em8) — EM8 (custom 8-bit teaching CPU) plugin
- [emfe_plugins/z8000](https://github.com/hha0x617/emfe_plugins/tree/master/z8000) — Zilog Z8000 family plugin
- [emfe_plugins/mc6809](https://github.com/hha0x617/emfe_plugins/tree/master/mc6809) — Motorola MC6809 plugin (Rust)
- [emfe_CsWPF](https://github.com/hha0x617/emfe_CsWPF) — C# WPF front-end (feature-parity)

## Contributing and Policies

- Contribution workflow: [`CONTRIBUTING.md`](CONTRIBUTING.md)
- Code of Conduct: [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) (Contributor Covenant 2.1)
- Security: [`SECURITY.md`](SECURITY.md)

## License

Apache License 2.0 — see [LICENSE](LICENSE).
