# emfe_WinUI3Cpp

[![Build and Release](https://github.com/hha0x617/emfe_WinUI3Cpp/actions/workflows/build.yml/badge.svg)](https://github.com/hha0x617/emfe_WinUI3Cpp/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/hha0x617/emfe_WinUI3Cpp?include_prereleases&sort=semver)](https://github.com/hha0x617/emfe_WinUI3Cpp/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)

[English documentation (README.md)](README.md)

emfe プラグインアーキテクチャの **C++ WinUI3** フロントエンド。

[emfe_plugins/mc68030](../emfe_plugins/mc68030/) 等のプラグイン DLL を `LoadLibrary` + `GetProcAddress` で動的ロードし、レジスタ・逆アセンブリ・メモリダンプ・コンソールを表示します。

## 機能

- em68030 互換レイアウト (メニュー + ツールバー + レジスタ/逆アセンブリ/メモリ/ステータスバー)
- レジスタパネル: データ駆動で動的生成 (D0-D7 / A0-A7 の 2列グリッド + Flags チェックボックス + Special/FPU/MMU)
- 逆アセンブリ: PC行の背景ハイライト + ブレークポイントインジケータ (`●`)
- メモリダンプ: 16x16 セルグリッド + ASCII 表示 + 編集モード
- 実行制御: Step (F10), Step Over (F11), Step Out (Shift+F11), Run (F5), Stop (Shift+F5), Reset, Full Reset
- コンソールウィンドウ: 別ウィンドウ、緑/黒配色、自動表示、キー入力
- 設定ダイアログ: プラグインから取得した setting defs を動的 UI 化
- ダークテーマ (タイトルバー含む, DWM_USE_IMMERSIVE_DARK_MODE)

## ディレクトリ構造

```
emfe_WinUI3Cpp/
├── emfe_WinUI3Cpp.sln
├── README.md
└── emfe/
    ├── emfe.vcxproj
    ├── App.xaml / App.xaml.h / App.xaml.cpp
    ├── MainWindow.xaml / MainWindow.xaml.h / MainWindow.xaml.cpp
    ├── PluginLoader.h      プラグイン DLL の LoadLibrary ラッパ
    ├── pch.h / pch.cpp
    └── x64/Release/        ビルド成果物
```

## 依存関係

| 依存先 | 想定パス | 内容 |
|-------|---------|-----|
| `emfe_plugins/api` | `../../emfe_plugins/api` | `emfe_plugin.h` (共通 C ABI ヘッダ) |
| `emfe_plugin_mc68030.dll` 等 | `../../emfe_plugins/{mc68030,em8,z8000,mc6809}/build/bin/Release/` (mc6809 は `target/release/`) | プラグイン DLL |

vcxproj にはビルド時に上記パスからプラグイン DLL を出力ディレクトリの
`plugins\` サブディレクトリへコピーする `Content` アイテムを設定済み。
フロントエンドは `plugins\emfe_plugin_*.dll` をスキャンして "Switch Plugin"
ダイアログに列挙する。

### システム要件

- Windows 10 (10.0.17763.0) / 11
- Visual Studio 2026 (v145 toolset)
- Windows App SDK 1.8.x (NuGet で自動取得)
- Windows SDK 10.0.26100

## ビルド

```bash
# NuGet 復元
MSBuild emfe/emfe.vcxproj -t:Restore -p:Configuration=Release -p:Platform=x64

# ビルド
MSBuild emfe/emfe.vcxproj -p:Configuration=Release -p:Platform=x64
```

Visual Studio 2026 から `emfe_WinUI3Cpp.sln` を開いてビルド / F5 デバッグ実行も可能。

出力: `emfe/x64/Release/emfe.exe`

## 実行方法

### プラグイン DLL の配置

プラグイン DLL (`emfe_plugin_*.dll`) を `emfe.exe` と同じディレクトリの
`plugins\` サブディレクトリに配置する。vcxproj が各プラグインのビルド出力
から自動コピーする (手動配置も可)。

DLL スキャン:
- `<exe_dir>\plugins\emfe_plugin_*.dll` のみをスキャン対象とする
- 前回起動時に選択したプラグインパスを `%LOCALAPPDATA%\emfe_WinUI3Cpp\appsettings.json`
  に記録、起動時に再利用する
- 候補が複数ある場合は **File → Switch Plugin...** で切替可能

### 基本操作

1. `emfe.exe` を実行
2. **File → Open ELF...** (Ctrl+E) または **Open S-Record...** (Ctrl+S) でプログラムをロード
3. **View → Console** でコンソールウィンドウを開く (自動表示もされる)
4. **Run (F5)** / **Step (F10)** で実行
5. **逆アセンブリ行のダブルクリック** でブレークポイントをトグル
6. **Settings → Emulator Settings...** で設定ダイアログを開く (BoardType, メモリサイズ, SCSI ディスク等)

## 関連プロジェクト

- [emfe_plugins/api](../emfe_plugins/api/) — 共通 C ABI ヘッダ + 開発者ドキュメント
- [emfe_plugins/mc68030](../emfe_plugins/mc68030/) — MC68030 プラグイン DLL
- [emfe_plugins/em8](../emfe_plugins/em8/) — EM8 (自作 8-bit 学習用) プラグイン
- [emfe_plugins/z8000](../emfe_plugins/z8000/) — Zilog Z8000 ファミリープラグイン
- [emfe_plugins/mc6809](../emfe_plugins/mc6809/) — Motorola MC6809 プラグイン (Rust)
- [emfe_CsWPF](../emfe_CsWPF/) — C# WPF フロントエンド (同等機能)

## ライセンス

Apache License 2.0 — 詳細は [LICENSE](LICENSE) を参照
