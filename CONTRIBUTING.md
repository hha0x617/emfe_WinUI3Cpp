# Contributing to emfe_WinUI3Cpp

Thanks for your interest!  This is the **C++/WinRT + WinUI 3** host for the
emfe plugin architecture.  Plugin DLLs (`emfe_plugin_*.dll`) are loaded
dynamically at runtime via `LoadLibraryW`, but the project **does depend on
emfe_plugins at build time** for the C ABI header `emfe_plugin.h`, which is
vendored as a git submodule.

## Getting the source

This repository contains one submodule (`external/emfe_plugins`).  Clone
recursively:

```bash
git clone --recurse-submodules https://github.com/hha0x617/emfe_WinUI3Cpp.git
```

Or, after a plain clone:

```bash
git submodule update --init
```

(Non-recursive — the inner submodules of emfe_plugins are not required to
build this host.)

## Build prerequisites

- Visual Studio 2022+ with the **Desktop development with C++** workload
- **Windows App SDK** (installed via VS installer or NuGet restore)
- Windows 10 1809 or later

## Building

```bash
msbuild emfe_WinUI3Cpp.sln -restore -p:Configuration=Release -p:Platform=x64
```

Note: the vcxproj targets `PlatformToolset=v145` (Visual Studio 18).  On
older runners/VS versions, override with
`-p:PlatformToolset=v143`.

Output lands under `emfe/x64/Release/`.  At runtime, drop
`emfe_plugin_*.dll` into a `plugins/` directory next to `emfe.exe`.

## Making a change

1. Fork the repository and create a feature branch off `master`.
2. Keep commits focused; write commit messages that explain the *why*.
3. Open a pull request against `master`.  CI must pass before merge.

## Commit style

- Subject line ≤ 72 chars, imperative mood (`fix: ...`, `feat(ui): ...`,
  `docs: ...`, `ci: ...`, `chore: ...`).
- Body wrapped to 72 chars, focused on motivation and trade-offs.

## WinUI 3 theming — code-behind windows

WinUI 3's `Microsoft.UI.Xaml.Window` is **not** a `FrameworkElement`,
so it cannot hold `RequestedTheme` itself. The theme lives on
`Window.Content`'s root, and is **dropped on every Content swap** —
the next render falls back to the system default (Light) until something
re-applies the theme.

To prevent this from silently breaking dark-themed dialogs that rebuild
their content from code-behind (e.g. the Settings dialog rebuilds on
every combo change), this codebase has the rule:

> **Do not call `Window.Content(...)` directly. Always go through
> `MainWindow::SetThemedWindowContent(window, root)`.**

The helper does `window.Content(root)` followed by
`ApplyThemeToWindow(window, m_isDark)`, so theme survives the swap.
A grep for `\.Content\(` against `Microsoft::UI::Xaml::Window` instances
should find zero direct call sites — anything that does is a regression.

Related: `m_themeApplied` is a one-shot flag separate from `m_isDark`
so the very first `ApplyTheme()` at startup is never short-circuited
by the `(isDark == m_isDark)` early-return — `RequestedTheme` and
`DwmSetWindowAttribute` need to be applied at least once before they
can be skipped on subsequent no-op calls.

See user memory `pitfall_winui3_theme_propagation.md` for the rationale.

## Reporting bugs / requesting features

Use the issue templates in [`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/).
Security vulnerabilities go through [`SECURITY.md`](SECURITY.md) instead.

## License

By submitting a contribution you agree it will be licensed under the
**Apache-2.0** terms as the rest of the repository.
