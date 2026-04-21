#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"
#include "MainWindow.xaml.g.hpp"

#ifdef FindText
#undef FindText
#endif
#include <winrt/Microsoft.UI.Text.h>
#include <microsoft.ui.xaml.window.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#include <format>
#include <filesystem>
#include <fstream>
#include "GitVersion.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

static HWND GetWindowHandle(Microsoft::UI::Xaml::Window const& window)
{
    auto windowNative = window.as<::IWindowNative>();
    HWND hwnd{};
    windowNative->get_WindowHandle(&hwnd);
    return hwnd;
}

namespace winrt::emfe::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        {
            std::string hash = GIT_COMMIT_HASH;
            std::wstring whash(hash.begin(), hash.end());
            Title(winrt::hstring(L"emfe - Emulator Frontend [" + whash + L"]"));
        }
        AppWindow().Resize({ 1100, 750 });

        // Close child windows and destroy plugin instance on main window close
        this->Closed([this](auto&&, auto&&) {
            if (m_framebufferWindow) {
                if (m_fbTimer) { m_fbTimer.Stop(); m_fbTimer = nullptr; }
                m_framebufferWindow.Close();
                m_framebufferWindow = nullptr;
            }
            if (m_callStackWindow) {
                m_callStackWindow.Close();
                m_callStackWindow = nullptr;
            }
            if (m_breakpointsWindow) {
                m_breakpointsWindow.Close();
                m_breakpointsWindow = nullptr;
            }
            if (m_settingsWindow) {
                m_settingsWindow.Close();
                m_settingsWindow = nullptr;
            }
            if (m_consoleWindow) {
                if (m_consoleRenderTimer) { m_consoleRenderTimer.Stop(); m_consoleRenderTimer = nullptr; }
                m_consoleWindow.Close();
                m_consoleWindow = nullptr;
                m_consoleTextBox = nullptr;
            }
            if (m_instance && m_plugin.IsLoaded()) {
                m_plugin.emfe_stop(m_instance);
                m_plugin.emfe_destroy(m_instance);
                m_instance = nullptr;
            }
        });

        m_dispatcherQueue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        LoadPlugin();
    }

    // ========================================================================
    // Plugin loading
    // ========================================================================

    static std::filesystem::path GetAppSettingsPath()
    {
        wchar_t* appData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)) && appData) {
            auto p = std::filesystem::path(appData) / L"emfe_WinUI3Cpp" / L"appsettings.json";
            CoTaskMemFree(appData);
            return p;
        }
        return {};
    }

    std::filesystem::path MainWindow::ReadSavedPluginPath()
    {
        auto settingsPath = GetAppSettingsPath();
        if (settingsPath.empty()) return {};
        std::ifstream ifs(settingsPath);
        if (!ifs.is_open()) return {};
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        auto pos = content.find("\"PluginPath\"");
        if (pos == std::string::npos) return {};
        pos = content.find(':', pos);
        if (pos == std::string::npos) return {};
        auto q1 = content.find('"', pos + 1);
        auto q2 = (q1 != std::string::npos) ? content.find('"', q1 + 1) : std::string::npos;
        if (q1 == std::string::npos || q2 == std::string::npos) return {};
        auto val = content.substr(q1 + 1, q2 - q1 - 1);
        if (val.empty()) return {};
        return std::filesystem::path(winrt::to_hstring(val).c_str());
    }

    void MainWindow::SavePluginPath(const std::filesystem::path& path)
    {
        auto settingsPath = GetAppSettingsPath();
        if (settingsPath.empty()) return;
        std::filesystem::create_directories(settingsPath.parent_path());

        // Read existing content
        std::string content;
        {
            std::ifstream ifs(settingsPath);
            if (ifs.is_open())
                content.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        }

        // Escape backslashes for JSON
        auto pathStr = winrt::to_string(path.wstring());
        std::string escaped;
        for (char c : pathStr) {
            if (c == '\\') escaped += "\\\\";
            else escaped += c;
        }

        auto pos = content.find("\"PluginPath\"");
        if (pos != std::string::npos) {
            // Replace existing value
            auto colon = content.find(':', pos);
            if (colon != std::string::npos) {
                auto q1 = content.find('"', colon + 1);
                auto q2 = (q1 != std::string::npos) ? content.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 != std::string::npos)
                    content.replace(q1 + 1, q2 - q1 - 1, escaped);
            }
        } else if (!content.empty()) {
            // Insert before closing brace
            auto brace = content.rfind('}');
            if (brace != std::string::npos) {
                // Check if there are existing entries (need comma)
                auto lastNonSpace = content.find_last_not_of(" \t\r\n", brace - 1);
                std::string prefix;
                if (lastNonSpace != std::string::npos && content[lastNonSpace] != '{' && content[lastNonSpace] != ',')
                    prefix = ",";
                content.insert(brace, prefix + "\n    \"PluginPath\": \"" + escaped + "\"\n");
            }
        } else {
            content = "{\n    \"PluginPath\": \"" + escaped + "\"\n}\n";
        }

        std::ofstream ofs(settingsPath, std::ios::trunc);
        ofs << content;
    }

    std::vector<std::filesystem::path> MainWindow::ScanPlugins()
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto pluginsDir = std::filesystem::path(exePath).parent_path() / L"plugins";

        std::vector<std::filesystem::path> pluginPaths;
        if (std::filesystem::exists(pluginsDir)) {
            for (auto& entry : std::filesystem::directory_iterator(pluginsDir)) {
                auto name = entry.path().filename().wstring();
                if (name.starts_with(L"emfe_plugin_") && name.ends_with(L".dll"))
                    pluginPaths.push_back(entry.path());
            }
        }
        std::sort(pluginPaths.begin(), pluginPaths.end());
        return pluginPaths;
    }

    bool MainWindow::LoadPluginFromPath(const std::filesystem::path& path, bool savePreference)
    {
        if (!m_plugin.Load(path.wstring()))
            return false;
        m_loadedPluginStem = path.stem().wstring();

        EmfeNegotiateInfo nego{};
        nego.api_version_major = EMFE_API_VERSION_MAJOR;
        nego.api_version_minor = EMFE_API_VERSION_MINOR;
        if (m_plugin.emfe_negotiate(&nego) != EMFE_OK) {
            m_plugin.Unload();
            m_loadedPluginStem.clear();
            return false;
        }

        // Set per-plugin data directory (%LOCALAPPDATA%\emfe_WinUI3Cpp\<plugin-stem>)
        // The per-plugin subdir prevents cross-plugin contamination of the
        // shared appsettings.json — e.g. mc68030's BoardType="MVME147" would
        // otherwise leak into mc6809's settings on plugin switch.
        if (m_plugin.emfe_set_data_dir) {
            wchar_t* localAppData = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) && localAppData) {
                std::filesystem::path p = std::filesystem::path(localAppData)
                    / L"emfe_WinUI3Cpp" / m_loadedPluginStem;
                CoTaskMemFree(localAppData);
                std::filesystem::create_directories(p);
                auto utf8 = winrt::to_string(p.wstring());
                m_plugin.emfe_set_data_dir(utf8.c_str());
            }
        }

        EmfeBoardInfo info{};
        m_plugin.emfe_get_board_info(&info);
        m_capabilities = info.capabilities;
        ApplyCapabilityVisibility();

        if (m_plugin.emfe_create(&m_instance) != EMFE_OK) {
            m_plugin.Unload();
            m_loadedPluginStem.clear();
            return false;
        }

        // Load persisted settings so that subsequent reads (Theme, kernel path, etc.)
        // see user-configured values rather than hardcoded defaults.
        if (m_plugin.emfe_load_settings)
            m_plugin.emfe_load_settings(m_instance);

        RegisterInstanceCallbacks();

        BuildRegisterPanel();
        UpdateRegisters();
        UpdateDisassembly();
        UpdateMemoryDump(0);
        UpdateToolbarState();

        // Apply theme from plugin settings
        const char* themeVal = m_plugin.emfe_get_setting(m_instance, "Theme");
        ApplyTheme(themeVal ? themeVal : "Dark");

        auto cpuName = winrt::to_hstring(info.cpu_name);
        SetStatus(L"Stopped");
        UpdateBoardTypeText(cpuName);

        // Auto-load kernel if a path is persisted for the active target OS.
        AutoLoadKernelFromSettings();

        if (savePreference)
            SavePluginPath(path);
        return true;
    }

    void MainWindow::ApplyCapabilityVisibility()
    {
        // Enable/disable menu items based on the plugin's declared capabilities.
        // Does not gate toolbar state — that's handled by UpdateToolbarState
        // which additionally respects the running/stopped state.
        auto set = [](Controls::MenuFlyoutItem const& item, bool enabled) {
            if (item) item.IsEnabled(enabled);
        };
        set(MenuOpenElf(),     (m_capabilities & EMFE_CAP_LOAD_ELF)    != 0);
        set(MenuOpenSrec(),    (m_capabilities & EMFE_CAP_LOAD_SREC)   != 0);
        set(MenuOpenBinary(),  (m_capabilities & EMFE_CAP_LOAD_BINARY) != 0);
        set(MenuStepOver(),    (m_capabilities & EMFE_CAP_STEP_OVER)   != 0);
        set(MenuStepOut(),     (m_capabilities & EMFE_CAP_STEP_OUT)    != 0);
        set(MenuCallStack(),   (m_capabilities & EMFE_CAP_CALL_STACK)  != 0);
        set(MenuFramebuffer(), (m_capabilities & EMFE_CAP_FRAMEBUFFER) != 0);
    }

    void MainWindow::AutoLoadKernelFromSettings()
    {
        if (!m_instance || !m_plugin.emfe_load_elf || !m_plugin.emfe_get_setting)
            return;
        if ((m_capabilities & EMFE_CAP_LOAD_ELF) == 0)
            return;  // plugin doesn't support ELF loading

        // Determine which setting key holds the kernel path for the active
        // board / target OS combination. Only MVME147 currently auto-loads;
        // other boards / bare-metal targets are loaded manually from File menu.
        const char* boardPtr = m_plugin.emfe_get_setting(m_instance, "BoardType");
        std::string board = boardPtr ? boardPtr : "";
        if (board != "MVME147") return;

        const char* osPtr = m_plugin.emfe_get_setting(m_instance, "TargetOS");
        std::string os = osPtr ? osPtr : "";
        std::string key;
        if (os == "NetBSD") key = "NetBsdKernelImagePath";
        else if (os == "Linux") key = "LinuxKernelImagePath";
        else return;  // 147Bug or unsupported — user loads manually

        const char* pathPtr = m_plugin.emfe_get_setting(m_instance, key.c_str());
        std::string kpath = pathPtr ? pathPtr : "";
        if (kpath.empty()) return;

        std::error_code ec;
        if (!std::filesystem::exists(kpath, ec)) {
            SetStatus(std::wstring(L"Auto-load skipped: kernel file not found"));
            return;
        }

        if (m_plugin.emfe_load_elf(m_instance, kpath.c_str()) == EMFE_OK) {
            auto fileName = std::filesystem::path(kpath).filename().wstring();
            LoadedFileText().Text(winrt::hstring(fileName));
            SetStatus(std::format(L"Auto-loaded: {}", fileName));
            UpdateRegisters();
            UpdateDisassembly();
            UpdateMemoryDump(0);
        } else {
            auto err = m_plugin.emfe_get_last_error(m_instance);
            auto errMsg = winrt::to_hstring(err ? err : "unknown");
            SetStatus(std::format(L"Auto-load failed: {}", std::wstring_view(errMsg)));
        }
    }

    void MainWindow::RegisterInstanceCallbacks()
    {
        if (!m_instance || !m_plugin.IsLoaded()) return;

        // State change callback — marshals to UI thread
        m_plugin.emfe_set_state_change_callback(m_instance,
            [](void* userData, const EmfeStateInfo* si) {
                auto self = static_cast<MainWindow*>(userData);
                auto state = si->state;
                auto reason = si->stop_reason;
                auto addr = si->stop_address;
                self->m_dispatcherQueue.TryEnqueue([self, state, reason, addr]() {
                    self->m_lastStopReason = reason;
                    self->m_lastStopAddress = static_cast<uint32_t>(addr);
                    if (state != EMFE_STATE_RUNNING) {
                        // Remove one-shot breakpoints installed by "Run to
                        // Here" when we stop on them; leave genuine user
                        // breakpoints alone.
                        if (reason == EMFE_STOP_REASON_BREAKPOINT) {
                            uint32_t stopAt = static_cast<uint32_t>(addr);
                            auto it = self->m_tempBreakpoints.find(stopAt);
                            if (it != self->m_tempBreakpoints.end()) {
                                self->m_tempBreakpoints.erase(it);
                                if (self->m_breakpointAddresses.find(stopAt)
                                        == self->m_breakpointAddresses.end())
                                {
                                    self->m_plugin.emfe_remove_breakpoint(
                                        self->m_instance, stopAt);
                                }
                            }
                        }
                        self->UpdateRegisters();
                        self->UpdateDisassembly();
                        self->UpdateMemoryDump(self->m_memoryAddress);
                        self->RefreshBreakpointsWindow();
                        self->RefreshCallStackWindow();
                    }
                    self->UpdateToolbarState();
                    if (reason == EMFE_STOP_REASON_BREAKPOINT)
                        self->SetStatus(std::format(L"Breakpoint at ${:08X}", static_cast<uint32_t>(addr)));
                    else if (reason == EMFE_STOP_REASON_WATCHPOINT)
                        self->SetStatus(std::format(L"Watchpoint at ${:08X}", static_cast<uint32_t>(addr)));
                    else if (state == EMFE_STATE_HALTED)
                        self->SetStatus(std::format(L"CPU halted at ${:08X} (use Reset to restart)", static_cast<uint32_t>(addr)));
                });
            }, this);

        // Console char callback — may fire from the emulation thread
        m_plugin.emfe_set_console_char_callback(m_instance,
            [](void* userData, char ch) {
                auto self = static_cast<MainWindow*>(userData);
                if (self->m_dispatcherQueue.HasThreadAccess()) {
                    self->AppendConsoleChar(ch);
                } else {
                    self->m_dispatcherQueue.TryEnqueue([self, ch]() {
                        self->AppendConsoleChar(ch);
                    });
                }
            }, this);
    }

    void MainWindow::DestroyCurrentInstance()
    {
        if (m_instance && m_plugin.IsLoaded()) {
            m_plugin.emfe_stop(m_instance);
            m_plugin.emfe_destroy(m_instance);
            m_instance = nullptr;
        }
        m_plugin.Unload();
        m_loadedPluginStem.clear();
        m_regEntries.clear();
        m_flagEntries.clear();
    }

    void MainWindow::LoadPlugin()
    {
        auto pluginPaths = ScanPlugins();
        if (pluginPaths.empty()) {
            SetStatus(L"No plugin DLLs found \u2014 place emfe_plugin_*.dll in the plugins\\ directory next to emfe.exe");
            return;
        }

        // Try last-used plugin first. Match by canonical path when possible
        // (honours user intent when there are multiple copies), then by
        // filename (robust against Debug/Release rebuilds or moved projects
        // — the saved absolute path may no longer exist, but the filename
        // still uniquely identifies the plugin family in the scan set).
        auto saved = ReadSavedPluginPath();
        bool savedFailed = false;
        std::wstring savedDisplay;
        if (!saved.empty()) {
            const auto savedName = saved.filename().wstring();
            const std::filesystem::path* match = nullptr;
            for (auto& p : pluginPaths) {
                if (std::filesystem::exists(saved) && std::filesystem::exists(p) &&
                    std::filesystem::canonical(saved) == std::filesystem::canonical(p)) {
                    match = &p;
                    break;
                }
            }
            if (!match) {
                for (auto& p : pluginPaths) {
                    if (p.filename().wstring() == savedName) {
                        match = &p;
                        break;
                    }
                }
            }
            if (match) {
                if (LoadPluginFromPath(*match))
                    return;
                // Saved plugin located in scan set but failed to load. Preserve
                // the user's preference and warn them; fall through to try
                // another plugin without overwriting the saved pref.
                savedFailed = true;
                savedDisplay = match->filename().wstring();
            }
        }

        // Fall back: try each plugin in order. Do NOT overwrite the saved
        // preference when this is a recovery from a failed saved plugin — the
        // user should still see their last explicit choice on next launch.
        bool save = !savedFailed;
        for (auto& p : pluginPaths) {
            if (LoadPluginFromPath(p, save)) {
                if (savedFailed) {
                    SetStatus(std::format(
                        L"Saved plugin '{}' failed to load; using '{}' (preference preserved)",
                        savedDisplay, p.filename().wstring()));
                }
                return;
            }
        }

        SetStatus(L"Failed to load any plugin DLL");
    }

    winrt::fire_and_forget MainWindow::OnSwitchPlugin(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto pluginPaths = ScanPlugins();
        if (pluginPaths.empty()) {
            SetStatus(L"No plugin DLLs found");
            co_return;
        }

        // Get board names for each plugin by temporarily loading them
        struct PluginInfo {
            std::filesystem::path path;
            std::wstring displayName;
        };
        std::vector<PluginInfo> plugins;

        // Save current state
        auto currentInstance = m_instance;
        m_instance = nullptr;

        PluginLoader tempLoader;
        for (auto& p : pluginPaths) {
            PluginInfo pi;
            pi.path = p;
            // Try to get board name
            if (tempLoader.Load(p.wstring())) {
                EmfeNegotiateInfo nego{};
                nego.api_version_major = EMFE_API_VERSION_MAJOR;
                nego.api_version_minor = EMFE_API_VERSION_MINOR;
                if (tempLoader.emfe_negotiate(&nego) == EMFE_OK) {
                    EmfeBoardInfo info{};
                    tempLoader.emfe_get_board_info(&info);
                    std::wstring boardName = info.board_name ? winrt::to_hstring(info.board_name).c_str() : L"Unknown";
                    std::wstring cpuName = info.cpu_name ? winrt::to_hstring(info.cpu_name).c_str() : L"";
                    pi.displayName = boardName;
                    if (!cpuName.empty())
                        pi.displayName += L" (" + cpuName + L")";
                } else {
                    pi.displayName = p.filename().wstring();
                }
                tempLoader.Unload();
            } else {
                pi.displayName = p.filename().wstring() + L" (load error)";
            }
            pi.displayName += L"  [" + p.filename().wstring() + L"]";
            plugins.push_back(std::move(pi));
        }

        // Restore current instance
        m_instance = currentInstance;

        if (plugins.empty()) {
            SetStatus(L"No valid plugins found");
            co_return;
        }

        // Build ContentDialog with ComboBox
        auto dialog = ContentDialog();
        dialog.XamlRoot(this->Content().XamlRoot());
        dialog.Title(box_value(L"Switch Plugin"));
        dialog.PrimaryButtonText(L"Load");
        dialog.CloseButtonText(L"Cancel");

        auto combo = ComboBox();
        combo.Width(400);
        combo.HorizontalAlignment(HorizontalAlignment::Stretch);
        int selectedIdx = 0;
        bool gotExactMatch = false;
        auto savedPath = ReadSavedPluginPath();
        const auto savedName = savedPath.empty()
            ? std::wstring{}
            : savedPath.filename().wstring();
        for (size_t i = 0; i < plugins.size(); i++) {
            combo.Items().Append(box_value(winrt::hstring(plugins[i].displayName)));
            // Prefer exact path match (canonical), fall back to filename match
            // so the dialog still highlights the loaded plugin after Debug/
            // Release rebuilds or project moves.
            bool exactMatch =
                !savedPath.empty() && std::filesystem::exists(savedPath) &&
                std::filesystem::exists(plugins[i].path) &&
                std::filesystem::canonical(savedPath) ==
                    std::filesystem::canonical(plugins[i].path);
            bool nameMatch =
                !savedName.empty() &&
                plugins[i].path.filename().wstring() == savedName;
            if (exactMatch) {
                selectedIdx = static_cast<int>(i);
                gotExactMatch = true;  // lock in selection, ignore later name matches
            } else if (nameMatch && !gotExactMatch && selectedIdx == 0) {
                selectedIdx = static_cast<int>(i);
            }
        }
        combo.SelectedIndex(selectedIdx);

        auto panel = StackPanel();
        panel.Spacing(8);
        auto label = TextBlock();
        label.Text(L"Select a plugin to load:");
        panel.Children().Append(label);
        panel.Children().Append(combo);
        dialog.Content(panel);

        auto result = co_await dialog.ShowAsync();
        if (result != ContentDialogResult::Primary) co_return;

        int idx = combo.SelectedIndex();
        if (idx < 0 || idx >= static_cast<int>(plugins.size())) co_return;

        auto& selected = plugins[idx];

        // Stop and destroy current instance
        DestroyCurrentInstance();

        if (!LoadPluginFromPath(selected.path)) {
            SetStatus(std::format(L"Failed to load plugin: {}", selected.path.filename().wstring()));
        }
    }

    // ========================================================================
    // Register panel — em68030 compatible 2-column layout
    // ========================================================================

    void MainWindow::AddRegPairToGrid(Grid const& grid, int row, int col,
                                       const char* name, uint32_t regId)
    {
        auto sp = StackPanel();
        sp.Orientation(Orientation::Horizontal);
        sp.Spacing(4);
        sp.Margin({ 0, 1, 8, 1 });

        auto label = TextBlock();
        label.Text(winrt::to_hstring(name));
        label.FontFamily(Media::FontFamily(L"Consolas"));
        label.FontSize(13);
        label.Width(30);
        label.VerticalAlignment(VerticalAlignment::Center);
        label.Foreground(GetThemeBrush(L"ThemeForeground"));

        auto valueBox = TextBox();
        valueBox.FontFamily(Media::FontFamily(L"Consolas"));
        valueBox.FontSize(13);
        valueBox.IsReadOnly(true);
        valueBox.Width(95);
        valueBox.Padding({ 6, 3, 6, 4 });
        valueBox.BorderThickness({ 1, 1, 1, 1 });
        valueBox.MinHeight(0);

        sp.Children().Append(label);
        sp.Children().Append(valueBox);

        Grid::SetRow(sp, row);
        Grid::SetColumn(sp, col);
        grid.Children().Append(sp);

        m_regEntries.push_back({ regId, 32, EMFE_REG_INT, valueBox });
    }

    TextBox MainWindow::AddRegRow(StackPanel const& parent, const char* name,
                                   uint32_t regId, int textboxWidth)
    {
        auto sp = StackPanel();
        sp.Orientation(Orientation::Horizontal);
        sp.Spacing(4);
        sp.Margin({ 0, 1, 0, 1 });

        auto label = TextBlock();
        label.Text(winrt::to_hstring(name));
        label.FontFamily(Media::FontFamily(L"Consolas"));
        label.FontSize(13);
        label.Width(35);
        label.VerticalAlignment(VerticalAlignment::Center);
        label.Foreground(GetThemeBrush(L"ThemeForeground"));

        auto valueBox = TextBox();
        valueBox.FontFamily(Media::FontFamily(L"Consolas"));
        valueBox.FontSize(13);
        valueBox.IsReadOnly(true);
        valueBox.Width(static_cast<double>(textboxWidth));
        valueBox.Padding({ 6, 3, 6, 4 });
        valueBox.BorderThickness({ 1, 1, 1, 1 });
        valueBox.MinHeight(0);

        sp.Children().Append(label);
        sp.Children().Append(valueBox);
        parent.Children().Append(sp);

        m_regEntries.push_back({ regId, 32, EMFE_REG_INT, valueBox });
        return valueBox;
    }

    void MainWindow::BuildRegisterPanel()
    {
        if (!m_instance) return;
        m_regEntries.clear();
        m_flagEntries.clear();

        RegGroupsContainer().Children().Clear();

        const EmfeRegisterDef* defs = nullptr;
        int32_t count = m_plugin.emfe_get_register_defs(m_instance, &defs);
        if (count <= 0) return;

        // Discover the PC register id via the EMFE_REG_FLAG_PC flag so that
        // UpdateDisassembly can follow the right register across plugins.
        for (int32_t i = 0; i < count; i++) {
            if (defs[i].flags & EMFE_REG_FLAG_PC) {
                m_pcRegId = defs[i].reg_id;
                break;
            }
        }

        // Collect groups in order of first appearance so plugins can control
        // section order via their register-def table.
        std::vector<std::string> groupOrder;
        for (int32_t i = 0; i < count; i++) {
            if (defs[i].flags & EMFE_REG_FLAG_HIDDEN) continue;
            std::string g = defs[i].group ? defs[i].group : "";
            if (std::find(groupOrder.begin(), groupOrder.end(), g) == groupOrder.end())
                groupOrder.push_back(g);
        }

        auto headerFg = GetThemeBrush(L"ThemeRegHeaderFg");

        for (auto const& grp : groupOrder) {
            // Collect registers for this group
            std::vector<int32_t> indices;
            for (int32_t i = 0; i < count; i++) {
                if (defs[i].flags & EMFE_REG_FLAG_HIDDEN) continue;
                std::string g = defs[i].group ? defs[i].group : "";
                if (g == grp) indices.push_back(i);
            }
            if (indices.empty()) continue;

            // Header
            auto header = TextBlock();
            header.Text(winrt::to_hstring(grp.empty() ? "Registers" : grp));
            header.FontSize(12);
            header.Margin({ 0, 6, 0, 2 });
            if (headerFg) header.Foreground(headerFg);
            RegGroupsContainer().Children().Append(header);

            // Heuristic: if the group has >= 4 narrow regs (bits <= 32) that all
            // share a short (<=3 char) name, use a 2-column grid for compactness.
            // Otherwise, stack vertically.
            bool useGrid = indices.size() >= 4;
            if (useGrid) {
                for (auto idx : indices) {
                    if (defs[idx].bit_width > 32) { useGrid = false; break; }
                    if (!defs[idx].name || strlen(defs[idx].name) > 3) { useGrid = false; break; }
                }
            }

            if (useGrid) {
                auto grid = Grid();
                Controls::ColumnDefinition c0, c1;
                c0.Width(GridLengthHelper::FromValueAndType(1.0, GridUnitType::Star));
                c1.Width(GridLengthHelper::FromValueAndType(1.0, GridUnitType::Star));
                grid.ColumnDefinitions().Append(c0);
                grid.ColumnDefinitions().Append(c1);
                int numRows = (static_cast<int>(indices.size()) + 1) / 2;
                for (int r = 0; r < numRows; r++) {
                    Controls::RowDefinition rd;
                    rd.Height(GridLengthHelper::Auto());
                    grid.RowDefinitions().Append(rd);
                }
                grid.Margin({ 0, 2, 0, 0 });
                for (size_t k = 0; k < indices.size(); k++) {
                    auto const& d = defs[indices[k]];
                    AddRegPairToGrid(grid, static_cast<int>(k / 2),
                                     static_cast<int>(k % 2), d.name, d.reg_id);
                    m_regEntries.back().bitWidth = d.bit_width;
                    m_regEntries.back().type = d.type;
                }
                RegGroupsContainer().Children().Append(grid);
            } else {
                auto panel = StackPanel();
                panel.Spacing(2);
                panel.Margin({ 0, 2, 0, 0 });
                for (auto idx : indices) {
                    auto const& d = defs[idx];
                    int width = (d.bit_width <= 16) ? 70 : (d.bit_width <= 32) ? 95 : 130;
                    auto box = AddRegRow(panel, d.name ? d.name : "?", d.reg_id, width);
                    m_regEntries.back().bitWidth = d.bit_width;
                    m_regEntries.back().type = d.type;
                    if (d.type == EMFE_REG_FLOAT) box.FontSize(11);
                }
                RegGroupsContainer().Children().Append(panel);
            }
        }
    }

    void MainWindow::UpdateRegisters()
    {
        if (!m_instance || m_regEntries.empty()) return;

        std::vector<EmfeRegValue> values(m_regEntries.size());
        for (size_t i = 0; i < m_regEntries.size(); i++)
            values[i].reg_id = m_regEntries[i].regId;

        m_plugin.emfe_get_registers(m_instance, values.data(), static_cast<int32_t>(values.size()));

        for (size_t i = 0; i < m_regEntries.size(); i++) {
            auto& e = m_regEntries[i];
            auto& v = values[i];
            std::wstring text;
            if (e.type == EMFE_REG_FLOAT)
                text = std::format(L"{:.6f}", v.value.f64);
            else if (e.bitWidth <= 16)
                text = std::format(L"{:04X}", static_cast<uint16_t>(v.value.u64));
            else if (e.bitWidth <= 32)
                text = std::format(L"{:08X}", static_cast<uint32_t>(v.value.u64));
            else
                text = std::format(L"{:016X}", v.value.u64);
            e.valueBox.Text(text);
        }

        // Flag checkboxes are only populated for mc68030-style SR; skip
        // when the current plugin didn't build any (e.g. mc6809 CC shown as
        // a plain textbox instead of individual bit checkboxes).
        if (!m_flagEntries.empty()) {

        // Update flag checkboxes from SR (mc68030-specific layout)
        EmfeRegValue srVal{};
        srVal.reg_id = 17;
        m_plugin.emfe_get_registers(m_instance, &srVal, 1);
        uint16_t sr = static_cast<uint16_t>(srVal.value.u64);
        uint8_t ccr = sr & 0xFF;

        for (size_t i = 0; i < m_flagEntries.size(); i++) {
            auto& f = m_flagEntries[i];
            if (i < 5) // X, N, Z, V, C
                f.checkBox.IsChecked((ccr & f.bitMask) != 0);
            else if (i == 5) // S (supervisor)
                f.checkBox.IsChecked((sr & 0x2000) != 0);
            else if (i == 6) // T (trace)
                f.checkBox.IsChecked((sr & 0x8000) != 0);
        }
        }  // end if (!m_flagEntries.empty())

        // Update cycles display
        int64_t cycles = m_plugin.emfe_get_cycle_count(m_instance);
        int64_t instrs = m_plugin.emfe_get_instruction_count(m_instance);
        CyclesText().Text(std::format(L"Cycles: {}  Instrs: {}", cycles, instrs));
    }

    // ========================================================================
    // Disassembly
    // ========================================================================

    void MainWindow::UpdateDisassembly()
    {
        if (!m_instance) return;

        EmfeRegValue pcVal{};
        pcVal.reg_id = m_pcRegId;
        m_plugin.emfe_get_registers(m_instance, &pcVal, 1);
        uint32_t pc = static_cast<uint32_t>(pcVal.value.u64);

        EmfeDisasmLine lines[64]{};
        uint32_t startAddr = pc;
        uint64_t progStart = 0, progEnd = 0;
        if (m_plugin.emfe_get_program_range)
            m_plugin.emfe_get_program_range(m_instance, &progStart, &progEnd);
        if (progStart > 0 && pc >= static_cast<uint32_t>(progStart))
            startAddr = std::max(static_cast<uint32_t>(progStart), pc >= 0x40 ? pc - 0x40 : 0u);
        int32_t count = m_plugin.emfe_disassemble_range(
            m_instance, startAddr, startAddr + 0x200, lines, 64);

        auto items = winrt::single_threaded_observable_vector<Windows::Foundation::IInspectable>();
        m_disasmAddresses.clear();
        m_disasmTexts.clear();
        int pcIndex = -1;

        auto brYellow = GetThemeBrush(L"ThemeHighlightedFg");
        auto brRed    = GetThemeBrush(L"ThemeBreakpointFg");
        auto brNormal = GetThemeBrush(L"ThemeForeground");
        auto brPcBg   = GetThemeBrush(L"ThemeCheckedBg");
        auto brTransp = Media::SolidColorBrush(Windows::UI::Color{ 0x01, 0x00, 0x00, 0x00 });
        auto consolasFont = Media::FontFamily(L"Consolas");

        for (int32_t i = 0; i < count; i++) {
            auto& line = lines[i];
            uint32_t addr = static_cast<uint32_t>(line.address);
            m_disasmAddresses.push_back(addr);

            auto bpIt = m_breakpointAddresses.find(addr);
            bool isBP = bpIt != m_breakpointAddresses.end();
            bool bpEnabled = isBP && bpIt->second;
            bool isPC = (addr == pc);

            auto row = StackPanel();
            row.Orientation(Orientation::Horizontal);
            row.Padding({ 2, 0, 2, 0 });
            row.Background(isPC ? brPcBg : brTransp);

            auto bpIndicator = TextBlock();
            bpIndicator.FontFamily(consolasFont);
            bpIndicator.FontSize(13);
            bpIndicator.Width(16);
            bpIndicator.VerticalAlignment(VerticalAlignment::Center);
            if (isBP) {
                bpIndicator.Text(bpEnabled ? L"\u25CF" : L"\u25CB");
                bpIndicator.Foreground(bpEnabled ? brRed : GetThemeBrush(L"ThemeDimFg"));
            }

            // Main disassembly text
            std::wstring text = std::format(L"{:08X}  {:<12s}  {:<8s} {}",
                addr,
                winrt::to_hstring(line.raw_bytes ? line.raw_bytes : ""),
                winrt::to_hstring(line.mnemonic ? line.mnemonic : ""),
                winrt::to_hstring(line.operands ? line.operands : ""));
            m_disasmTexts.push_back(text);

            auto mainText = TextBlock();
            mainText.FontFamily(consolasFont);
            mainText.FontSize(13);
            mainText.Text(text);
            mainText.VerticalAlignment(VerticalAlignment::Center);
            mainText.Margin({ 4, 0, 0, 0 });

            if (isPC)
                mainText.Foreground(brYellow);
            else if (isBP && bpEnabled)
                mainText.Foreground(brRed);
            else if (isBP)
                mainText.Foreground(GetThemeBrush(L"ThemeDimFg"));
            else
                mainText.Foreground(brNormal);

            row.Children().Append(bpIndicator);
            row.Children().Append(mainText);
            items.Append(row);
            if (isPC) pcIndex = i;
        }

        DisasmList().ItemsSource(items);
        if (pcIndex >= 0)
            DisasmList().ScrollIntoView(items.GetAt(pcIndex));
    }

    void MainWindow::ToggleBreakpoint(uint32_t address)
    {
        if (!m_instance) return;
        auto it = m_breakpointAddresses.find(address);
        if (it != m_breakpointAddresses.end()) {
            if (it->second) {
                m_plugin.emfe_enable_breakpoint(m_instance, address, false);
                it->second = false;
            } else {
                m_plugin.emfe_remove_breakpoint(m_instance, address);
                m_breakpointAddresses.erase(it);
            }
        } else {
            m_plugin.emfe_add_breakpoint(m_instance, address);
            m_breakpointAddresses[address] = true;
        }
        UpdateDisassembly();
        RefreshBreakpointsWindow();
    }

    void MainWindow::OnDisasmDoubleTapped(Windows::Foundation::IInspectable const&,
                                           Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&)
    {
        int idx = DisasmList().SelectedIndex();
        if (idx >= 0 && idx < static_cast<int>(m_disasmAddresses.size()))
            ToggleBreakpoint(m_disasmAddresses[idx]);
    }

    // ------------------------------------------------------------------------
    // Disassembly context menu (Phase B mirror of emfe_CsWPF)
    // ------------------------------------------------------------------------

    void MainWindow::OnDisasmRightTapped(Windows::Foundation::IInspectable const& sender,
                                          Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const& e)
    {
        // Capture the row index the user actually right-clicked, rather
        // than relying on SelectedIndex — WinUI's ListView doesn't move
        // selection on right-click by default.
        m_disasmMenuTargetIndex = -1;
        auto origEl = e.OriginalSource().try_as<Microsoft::UI::Xaml::DependencyObject>();
        while (origEl) {
            if (auto item = origEl.try_as<Microsoft::UI::Xaml::Controls::ListViewItem>()) {
                auto container = DisasmList().ContainerFromItem(item.Content());
                if (!container) container = item;
                m_disasmMenuTargetIndex = DisasmList().IndexFromContainer(container);
                break;
            }
            origEl = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(origEl);
        }
        if (m_disasmMenuTargetIndex < 0)
            m_disasmMenuTargetIndex = DisasmList().SelectedIndex();

        // Let the default ContextFlyout handling open the menu at the click
        // position; we only pre-compute the target index here.
        (void)sender;
    }

    void MainWindow::OnDisasmMenuOpening(Windows::Foundation::IInspectable const&,
                                          Windows::Foundation::IInspectable const&)
    {
        bool haveTarget = m_disasmMenuTargetIndex >= 0
                          && m_disasmMenuTargetIndex < static_cast<int>(m_disasmAddresses.size());
        bool haveInstance = (m_instance != nullptr);
        DisasmMenuRunToHere().IsEnabled(haveTarget && haveInstance);
        DisasmMenuSetPc().IsEnabled(haveTarget && haveInstance);
        DisasmMenuCopy().IsEnabled(DisasmList().SelectedItems().Size() > 0);
    }

    void MainWindow::OnDisasmMenuCancel(Windows::Foundation::IInspectable const&,
                                         Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // No-op — right-click / Escape already dismisses the menu.  Kept as a
        // visible entry at the user's request for parity with emfe_CsWPF.
    }

    void MainWindow::OnDisasmMenuRunToHere(Windows::Foundation::IInspectable const&,
                                            Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_instance) return;
        if (m_disasmMenuTargetIndex < 0
                || m_disasmMenuTargetIndex >= static_cast<int>(m_disasmAddresses.size()))
            return;
        uint32_t addr = m_disasmAddresses[m_disasmMenuTargetIndex];

        if (m_breakpointAddresses.find(addr) == m_breakpointAddresses.end()) {
            if (m_plugin.emfe_add_breakpoint(m_instance, addr) != EMFE_OK) {
                SetStatus(std::format(L"Failed to add temporary breakpoint at ${:08X}", addr));
                return;
            }
            m_tempBreakpoints.insert(addr);
        }
        m_plugin.emfe_run(m_instance);
        UpdateToolbarState();
        SetStatus(std::format(L"Running to ${:08X}...", addr));
    }

    void MainWindow::OnDisasmMenuSetPc(Windows::Foundation::IInspectable const&,
                                        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_instance) return;
        if (m_disasmMenuTargetIndex < 0
                || m_disasmMenuTargetIndex >= static_cast<int>(m_disasmAddresses.size()))
            return;
        uint32_t addr = m_disasmAddresses[m_disasmMenuTargetIndex];
        EmfeRegValue v{};
        v.reg_id = m_pcRegId;
        v.value.u64 = addr;
        if (m_plugin.emfe_set_registers(m_instance, &v, 1) != EMFE_OK) {
            SetStatus(std::format(L"Failed to set PC to ${:08X}", addr));
            return;
        }
        UpdateRegisters();
        UpdateDisassembly();
        UpdateMemoryDump(m_memoryAddress);
        SetStatus(std::format(L"PC set to ${:08X}", addr));
    }

    void MainWindow::OnDisasmMenuCopy(Windows::Foundation::IInspectable const&,
                                       Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        CopySelectedDisasmToClipboard();
    }

    void MainWindow::OnDisasmCopyAccel(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                                        Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& e)
    {
        CopySelectedDisasmToClipboard();
        e.Handled(true);
    }

    void MainWindow::CopySelectedDisasmToClipboard()
    {
        auto selected = DisasmList().SelectedItems();
        if (selected.Size() == 0) return;

        // Walk items in visual (top-to-bottom) order so the pasted block
        // reads naturally regardless of click order.
        auto src = DisasmList().Items();
        std::wstring out;
        int copied = 0;
        for (uint32_t i = 0; i < src.Size(); ++i) {
            bool isSel = false;
            auto item = src.GetAt(i);
            for (uint32_t j = 0; j < selected.Size(); ++j) {
                if (selected.GetAt(j) == item) { isSel = true; break; }
            }
            if (!isSel) continue;
            if (i >= m_disasmTexts.size()) continue;
            if (!out.empty()) out += L"\r\n";
            out += m_disasmTexts[i];
            ++copied;
        }
        if (out.empty()) return;

        Windows::ApplicationModel::DataTransfer::DataPackage dp;
        dp.SetText(winrt::hstring(out));
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
        SetStatus(std::format(L"Copied {} line(s) to clipboard", copied));
    }

    // ========================================================================
    // Memory dump — cell grid (matching em68030 style)
    // ========================================================================

    void MainWindow::BuildMemoryGrid()
    {
        if (m_memGridBuilt) return;
        m_memGridBuilt = true;

        auto panel = MemoryDumpPanel();
        auto consolasFont = Media::FontFamily(L"Consolas");
        auto fgBrush = GetThemeBrush(L"ThemeForeground");
        auto dimBrush = GetThemeBrush(L"ThemeDimFg");
        auto bgBrush = GetThemeBrush(L"ThemeWindowBg");
        auto transpBorder = Media::SolidColorBrush(Windows::UI::Color{ 0x00, 0x00, 0x00, 0x00 });

        // Look up CompactTextBoxStyle from resources
        auto rootGrid = Content().try_as<Controls::Grid>();
        auto compactStyle = rootGrid.Resources().Lookup(winrt::box_value(L"CompactTextBoxStyle"))
                                .as<Microsoft::UI::Xaml::Style>();

        m_memCellBoxes.resize(MemRows);
        m_memAddrLabels.resize(MemRows);
        m_memAsciiLabels.resize(MemRows);

        for (int r = 0; r < MemRows; r++) {
            m_memCellBoxes[r].resize(MemCols, nullptr);

            auto row = StackPanel();
            row.Orientation(Orientation::Horizontal);
            row.Spacing(1);
            row.Margin({ 4, 1, 0, 0 });

            // Address label
            auto addrLabel = TextBlock();
            addrLabel.FontFamily(consolasFont);
            addrLabel.FontSize(13);
            addrLabel.Foreground(fgBrush);
            addrLabel.Width(68);
            addrLabel.VerticalAlignment(VerticalAlignment::Center);
            addrLabel.Text(L"00000000");
            m_memAddrLabels[r] = addrLabel;
            row.Children().Append(addrLabel);

            // Colon
            auto colon = TextBlock();
            colon.FontFamily(consolasFont);
            colon.FontSize(13);
            colon.Foreground(dimBrush);
            colon.VerticalAlignment(VerticalAlignment::Center);
            colon.Text(L": ");
            row.Children().Append(colon);

            // 16 byte cells
            for (int c = 0; c < MemCols; c++) {
                auto cell = TextBox();
                cell.Style(compactStyle);
                cell.Width(26);
                cell.MaxLength(2);
                cell.Padding({ 2, 2, 2, 2 });
                cell.Background(bgBrush);
                cell.BorderBrush(transpBorder);
                cell.BorderThickness({ 1, 1, 1, 1 });
                cell.IsReadOnly(true);
                cell.IsTabStop(false);
                cell.IsHitTestVisible(false);
                cell.TextAlignment(TextAlignment::Center);

                // Edit mode focus handlers
                int capturedR = r, capturedC = c;
                cell.GotFocus([this, capturedR, capturedC](auto&&, auto&&) {
                    if (!m_memEditMode) return;
                    auto& box = m_memCellBoxes[capturedR][capturedC];
                    if (box) {
                        box.IsReadOnly(false);
                        box.Background(GetThemeBrush(L"ThemeInputBg"));
                        box.BorderBrush(GetThemeBrush(L"ThemeBorder"));
                        box.SelectAll();
                    }
                });
                cell.LostFocus([this, capturedR, capturedC](auto&&, auto&&) {
                    auto& box = m_memCellBoxes[capturedR][capturedC];
                    if (box) {
                        box.IsReadOnly(true);
                        box.Background(GetThemeBrush(L"ThemeWindowBg"));
                        box.BorderBrush(Media::SolidColorBrush(Windows::UI::Color{ 0x00, 0x00, 0x00, 0x00 }));
                    }
                });
                // Tab/arrow navigation
                cell.KeyDown([this, capturedR, capturedC](auto&&, Input::KeyRoutedEventArgs const& args) {
                    if (!m_memEditMode) return;
                    int nr = capturedR, nc = capturedC;
                    if (args.Key() == Windows::System::VirtualKey::Tab ||
                        args.Key() == Windows::System::VirtualKey::Right) {
                        nc++; if (nc >= MemCols) { nc = 0; nr++; }
                    } else if (args.Key() == Windows::System::VirtualKey::Left) {
                        nc--; if (nc < 0) { nc = MemCols - 1; nr--; }
                    } else if (args.Key() == Windows::System::VirtualKey::Down) { nr++; }
                    else if (args.Key() == Windows::System::VirtualKey::Up) { nr--; }
                    else return;
                    if (nr >= 0 && nr < MemRows && nc >= 0 && nc < MemCols)
                        SelectMemoryCell(nr, nc);
                    args.Handled(true);
                });

                m_memCellBoxes[r][c] = cell;
                row.Children().Append(cell);

                if (c == 7) {
                    auto gap = Border();
                    gap.Width(8);
                    row.Children().Append(gap);
                }
            }

            // ASCII gap
            auto asciiGap = Border();
            asciiGap.Width(8);
            row.Children().Append(asciiGap);

            // ASCII label
            auto asciiLabel = TextBlock();
            asciiLabel.FontFamily(consolasFont);
            asciiLabel.FontSize(13);
            asciiLabel.Foreground(fgBrush);
            asciiLabel.VerticalAlignment(VerticalAlignment::Center);
            asciiLabel.Text(L"................");
            m_memAsciiLabels[r] = asciiLabel;
            row.Children().Append(asciiLabel);

            panel.Children().Append(row);
        }
    }

    void MainWindow::SelectMemoryCell(int row, int col)
    {
        if (row >= 0 && row < MemRows && col >= 0 && col < MemCols) {
            auto& box = m_memCellBoxes[row][col];
            if (box) {
                box.IsTabStop(true);
                box.IsHitTestVisible(true);
                box.Focus(FocusState::Programmatic);
            }
        }
    }

    void MainWindow::UpdateMemoryDump(uint32_t address)
    {
        if (!m_instance) return;
        m_memoryAddress = address;

        BuildMemoryGrid();

        // Collect active watchpoints for marker display
        struct WpRange { uint32_t start; uint32_t end; EmfeWatchpointType type; };
        std::vector<WpRange> activeWps;
        {
            EmfeWatchpointInfo wps[128];
            int wpCount = m_plugin.emfe_get_watchpoints(m_instance, wps, 128);
            for (int i = 0; i < wpCount; i++) {
                if (!wps[i].enabled) continue;
                uint32_t s = static_cast<uint32_t>(wps[i].address);
                uint32_t e = s + static_cast<uint32_t>(wps[i].size) - 1;
                activeWps.push_back({ s, e, wps[i].type });
            }
        }

        auto transpBorder = Media::SolidColorBrush(Windows::UI::Color{ 0x00, 0x00, 0x00, 0x00 });

        uint8_t data[MemRows * MemCols]{};
        m_plugin.emfe_peek_range(m_instance, address, data, sizeof(data));

        for (int r = 0; r < MemRows; r++) {
            uint32_t rowAddr = address + r * MemCols;
            m_memAddrLabels[r].Text(std::format(L"{:08X}", rowAddr));

            std::wstring ascii;
            for (int c = 0; c < MemCols; c++) {
                uint8_t b = data[r * MemCols + c];
                m_memCellBoxes[r][c].Text(std::format(L"{:02X}", b));
                ascii += (b >= 0x20 && b < 0x7F) ? static_cast<wchar_t>(b) : L'.';

                // Watchpoint marker
                uint32_t cellAddr = rowAddr + c;
                Media::Brush borderBrush = transpBorder;
                for (auto& wp : activeWps) {
                    if (cellAddr >= wp.start && cellAddr <= wp.end) {
                        const wchar_t* key = wp.type == EMFE_WP_READ ? L"ThemeConsoleFg" :
                                             wp.type == EMFE_WP_WRITE ? L"ThemeBreakpointFg" :
                                             L"ThemeHighlightedFg";
                        borderBrush = GetThemeBrush(key);
                        break;
                    }
                }
                m_memCellBoxes[r][c].BorderBrush(borderBrush);
            }
            m_memAsciiLabels[r].Text(ascii);
        }
    }

    // ========================================================================
    // Toolbar state
    // ========================================================================

    void MainWindow::UpdateToolbarState()
    {
        if (!m_instance) return;
        auto state = m_plugin.emfe_get_state(m_instance);
        bool running = (state == EMFE_STATE_RUNNING);
        bool hasOver = (m_capabilities & EMFE_CAP_STEP_OVER) != 0;
        bool hasOut  = (m_capabilities & EMFE_CAP_STEP_OUT)  != 0;
        bool hasElf  = (m_capabilities & EMFE_CAP_LOAD_ELF)  != 0;

        BtnStep().IsEnabled(!running);
        BtnStepOver().IsEnabled(!running && hasOver);
        BtnStepOut().IsEnabled(!running && hasOut);
        BtnRun().IsEnabled(!running);
        BtnStop().IsEnabled(running);
        BtnReset().IsEnabled(!running);
        BtnFullReset().IsEnabled(!running);
        MenuOpenElf().IsEnabled(!running && hasElf);
    }

    void MainWindow::SetStatus(const std::wstring& text)
    {
        StatusText().Text(text);
    }

    // ========================================================================
    // Event handlers
    // ========================================================================

    void MainWindow::OnLoadElf(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;

        OPENFILENAMEW ofn{};
        wchar_t filePath[MAX_PATH]{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetWindowHandle(*this);
        ofn.lpstrFilter = L"ELF Files\0*.*\0\0";
        ofn.lpstrFile = filePath;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (!GetOpenFileNameW(&ofn)) return;

        auto path = winrt::to_string(winrt::hstring(filePath));
        auto result = m_plugin.emfe_load_elf(m_instance, path.c_str());
        if (result == EMFE_OK) {
            auto fileName = std::filesystem::path(filePath).filename().wstring();
            SetStatus(std::format(L"Loaded: {}", fileName));
            LoadedFileText().Text(fileName);
            UpdateRegisters();
            UpdateDisassembly();
            uint64_t progStart = 0, progEnd = 0;
            if (m_plugin.emfe_get_program_range)
                m_plugin.emfe_get_program_range(m_instance, &progStart, &progEnd);
            UpdateMemoryDump(static_cast<uint32_t>(progStart));
        } else {
            auto err = m_plugin.emfe_get_last_error(m_instance);
            auto errMsg = winrt::to_hstring(err ? err : "unknown");
            SetStatus(std::format(L"Load failed: {}", std::wstring_view(errMsg)));
        }
    }

    void MainWindow::OnLoadSrec(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;

        OPENFILENAMEW ofn{};
        wchar_t filePath[MAX_PATH]{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetWindowHandle(*this);
        ofn.lpstrFilter = L"S-Record Files (*.s19;*.srec)\0*.s19;*.srec\0All Files\0*.*\0\0";
        ofn.lpstrFile = filePath;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (!GetOpenFileNameW(&ofn)) return;

        auto path = winrt::to_string(winrt::hstring(filePath));
        auto result = m_plugin.emfe_load_srec(m_instance, path.c_str());
        if (result == EMFE_OK) {
            auto fileName = std::filesystem::path(filePath).filename().wstring();
            SetStatus(std::format(L"Loaded: {}", fileName));
            LoadedFileText().Text(fileName);
            UpdateRegisters();
            UpdateDisassembly();
            uint64_t progStart = 0, progEnd = 0;
            if (m_plugin.emfe_get_program_range)
                m_plugin.emfe_get_program_range(m_instance, &progStart, &progEnd);
            UpdateMemoryDump(static_cast<uint32_t>(progStart));
        } else {
            auto err = m_plugin.emfe_get_last_error(m_instance);
            auto errMsg = winrt::to_hstring(err ? err : "unknown");
            SetStatus(std::format(L"Load failed: {}", std::wstring_view(errMsg)));
        }
    }

    void MainWindow::OnLoadBinary(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;

        OPENFILENAMEW ofn{};
        wchar_t filePath[MAX_PATH]{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetWindowHandle(*this);
        ofn.lpstrFilter = L"Binary Files (*.bin)\0*.bin\0All Files\0*.*\0\0";
        ofn.lpstrFile = filePath;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (!GetOpenFileNameW(&ofn)) return;

        auto path = winrt::to_string(winrt::hstring(filePath));
        auto result = m_plugin.emfe_load_binary(m_instance, path.c_str(), 0);
        if (result == EMFE_OK) {
            auto fileName = std::filesystem::path(filePath).filename().wstring();
            SetStatus(std::format(L"Loaded: {}", fileName));
            LoadedFileText().Text(fileName);
            UpdateRegisters();
            UpdateDisassembly();
            UpdateMemoryDump(0);
        } else {
            auto err = m_plugin.emfe_get_last_error(m_instance);
            auto errMsg = winrt::to_hstring(err ? err : "unknown");
            SetStatus(std::format(L"Load failed: {}", std::wstring_view(errMsg)));
        }
    }

    void MainWindow::OnStep(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;
        m_plugin.emfe_step(m_instance);
        UpdateRegisters();
        UpdateDisassembly();
        UpdateMemoryDump(m_memoryAddress);
        UpdateToolbarState();
    }

    void MainWindow::OnStepOver(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;
        m_plugin.emfe_step_over(m_instance);
        UpdateToolbarState();
    }

    void MainWindow::OnStepOut(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;
        m_plugin.emfe_step_out(m_instance);
        UpdateToolbarState();
    }

    void MainWindow::OnRun(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;
        m_plugin.emfe_run(m_instance);
        UpdateToolbarState();
        SetStatus(L"Running...");
    }

    void MainWindow::OnStop(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;
        m_plugin.emfe_stop(m_instance);
        UpdateRegisters();
        UpdateDisassembly();
        UpdateMemoryDump(m_memoryAddress);
        UpdateToolbarState();
        SetStatus(L"Stopped");
    }

    void MainWindow::OnReset(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;
        m_plugin.emfe_reset(m_instance);
        UpdateRegisters();
        UpdateDisassembly();
        UpdateMemoryDump(0);
        UpdateToolbarState();
        SetStatus(L"Reset");
    }

    void MainWindow::OnFullReset(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;
        // Full reset: destroy and recreate the instance
        m_plugin.emfe_destroy(m_instance);
        m_instance = nullptr;
        if (m_plugin.emfe_create(&m_instance) != EMFE_OK) {
            SetStatus(L"Full reset failed");
            return;
        }
        // Re-register ALL callbacks (console char + state change). Omitting
        // the console char callback was a prior bug — after Full Reset, TX
        // from the next loaded program silently went nowhere.
        RegisterInstanceCallbacks();
        // Re-apply persisted settings so ACIA base etc. match user config.
        if (m_plugin.emfe_load_settings)
            m_plugin.emfe_load_settings(m_instance);
        UpdateRegisters();
        UpdateDisassembly();
        UpdateMemoryDump(0);
        UpdateToolbarState();
        SetStatus(L"Full Reset");
    }

    void MainWindow::OnMemoryGo(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto text = winrt::to_string(TxtMemAddr().Text());
        if (text.empty()) return;
        try {
            uint32_t addr = static_cast<uint32_t>(std::stoul(text, nullptr, 16));
            UpdateMemoryDump(addr);
        } catch (...) {}
    }

    void MainWindow::OnDisasmGo(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto text = winrt::to_string(DisasmAddrBox().Text());
        if (text.empty()) return;
        try {
            uint32_t addr = static_cast<uint32_t>(std::stoul(text, nullptr, 16));
            // Disassemble from specified address
            EmfeDisasmLine lines[64]{};
            int32_t count = m_plugin.emfe_disassemble_range(
                m_instance, addr, addr + 0x200, lines, 64);

            auto items = winrt::single_threaded_observable_vector<Windows::Foundation::IInspectable>();
            for (int32_t i = 0; i < count; i++) {
                std::wstring lineText = std::format(L"  {:08X}  {:<12s}  {:<8s} {}",
                    static_cast<uint32_t>(lines[i].address),
                    winrt::to_hstring(lines[i].raw_bytes ? lines[i].raw_bytes : ""),
                    winrt::to_hstring(lines[i].mnemonic ? lines[i].mnemonic : ""),
                    winrt::to_hstring(lines[i].operands ? lines[i].operands : ""));
                auto tb = TextBlock();
                tb.Text(lineText);
                tb.FontFamily(Media::FontFamily(L"Consolas"));
                tb.FontSize(13);
                tb.Foreground(GetThemeBrush(L"ThemeForeground"));
                items.Append(tb);
            }
            DisasmList().ItemsSource(items);
        } catch (...) {}
    }

    // ========================================================================
    // Register edit mode
    // ========================================================================

    void MainWindow::OnRegEdit(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        for (auto& e : m_regEntries)
            e.valueBox.IsReadOnly(false);
        for (auto& f : m_flagEntries)
            f.checkBox.IsEnabled(true);
        BtnRegEdit().Visibility(Visibility::Collapsed);
        BtnRegApply().Visibility(Visibility::Visible);
        BtnRegCancel().Visibility(Visibility::Visible);
    }

    void MainWindow::OnRegApply(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;

        std::vector<EmfeRegValue> values;
        for (auto& e : m_regEntries) {
            auto text = winrt::to_string(e.valueBox.Text());
            EmfeRegValue v{};
            v.reg_id = e.regId;
            try {
                if (e.type == EMFE_REG_FLOAT)
                    v.value.f64 = std::stod(text);
                else
                    v.value.u64 = std::stoull(text, nullptr, 16);
            } catch (...) {
                continue;
            }
            values.push_back(v);
        }

        // Build SR from flag checkboxes (only relevant for mc68030 layout).
        if (!m_flagEntries.empty()) {
            EmfeRegValue srVal{};
            srVal.reg_id = 17; // SR
            m_plugin.emfe_get_registers(m_instance, &srVal, 1);
            uint16_t sr = static_cast<uint16_t>(srVal.value.u64);
            uint8_t ccr = sr & 0xFF;
            for (size_t i = 0; i < m_flagEntries.size(); i++) {
                bool checked = m_flagEntries[i].checkBox.IsChecked().Value();
                switch (i) {
                    case 0: ccr = checked ? (ccr | 0x10) : (ccr & ~0x10); break; // X
                    case 1: ccr = checked ? (ccr | 0x08) : (ccr & ~0x08); break; // N
                    case 2: ccr = checked ? (ccr | 0x04) : (ccr & ~0x04); break; // Z
                    case 3: ccr = checked ? (ccr | 0x02) : (ccr & ~0x02); break; // V
                    case 4: ccr = checked ? (ccr | 0x01) : (ccr & ~0x01); break; // C
                    case 5: sr = checked ? (sr | 0x2000) : (sr & ~0x2000); break; // S
                    case 6: sr = checked ? (sr | 0x8000) : (sr & ~0x8000); break; // T
                }
            }
            sr = (sr & 0xFF00) | ccr;
            srVal.value.u64 = sr;
            values.push_back(srVal);
        }

        if (!values.empty())
            m_plugin.emfe_set_registers(m_instance, values.data(), static_cast<int32_t>(values.size()));

        for (auto& e : m_regEntries)
            e.valueBox.IsReadOnly(true);
        for (auto& f : m_flagEntries)
            f.checkBox.IsEnabled(false);
        BtnRegEdit().Visibility(Visibility::Visible);
        BtnRegApply().Visibility(Visibility::Collapsed);
        BtnRegCancel().Visibility(Visibility::Collapsed);

        UpdateRegisters();
        UpdateDisassembly();
    }

    void MainWindow::OnRegCancel(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        for (auto& e : m_regEntries)
            e.valueBox.IsReadOnly(true);
        for (auto& f : m_flagEntries)
            f.checkBox.IsEnabled(false);
        BtnRegEdit().Visibility(Visibility::Visible);
        BtnRegApply().Visibility(Visibility::Collapsed);
        BtnRegCancel().Visibility(Visibility::Collapsed);

        UpdateRegisters();
    }

    // ========================================================================
    // Memory edit mode
    // ========================================================================

    void MainWindow::OnMemEdit(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        m_memEditMode = true;
        for (auto& row : m_memCellBoxes)
            for (auto& box : row)
                if (box) { box.IsTabStop(true); box.IsHitTestVisible(true); }
        BtnMemEdit().Visibility(Visibility::Collapsed);
        BtnMemApply().Visibility(Visibility::Visible);
        BtnMemCancel().Visibility(Visibility::Visible);
        SelectMemoryCell(0, 0);
    }

    void MainWindow::OnMemApply(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;

        for (int r = 0; r < MemRows; r++) {
            for (int c = 0; c < MemCols; c++) {
                auto& box = m_memCellBoxes[r][c];
                if (!box) continue;
                auto text = winrt::to_string(box.Text());
                if (text.empty()) continue;
                try {
                    uint8_t val = static_cast<uint8_t>(std::stoul(text, nullptr, 16));
                    uint32_t addr = m_memoryAddress + r * MemCols + c;
                    m_plugin.emfe_poke_byte(m_instance, addr, val);
                } catch (...) {}
            }
        }

        m_memEditMode = false;
        for (auto& row : m_memCellBoxes)
            for (auto& box : row)
                if (box) {
                    box.IsTabStop(false);
                    box.IsHitTestVisible(false);
                    box.IsReadOnly(true);
                    box.Background(GetThemeBrush(L"ThemeWindowBg"));
                    box.BorderBrush(Media::SolidColorBrush(Windows::UI::Color{ 0x00, 0x00, 0x00, 0x00 }));
                }
        BtnMemEdit().Visibility(Visibility::Visible);
        BtnMemApply().Visibility(Visibility::Collapsed);
        BtnMemCancel().Visibility(Visibility::Collapsed);

        UpdateMemoryDump(m_memoryAddress);
        UpdateDisassembly();
    }

    void MainWindow::OnMemCancel(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        m_memEditMode = false;
        for (auto& row : m_memCellBoxes)
            for (auto& box : row)
                if (box) {
                    box.IsTabStop(false);
                    box.IsHitTestVisible(false);
                    box.IsReadOnly(true);
                    box.Background(GetThemeBrush(L"ThemeWindowBg"));
                    box.BorderBrush(Media::SolidColorBrush(Windows::UI::Color{ 0x00, 0x00, 0x00, 0x00 }));
                }
        BtnMemEdit().Visibility(Visibility::Visible);
        BtnMemApply().Visibility(Visibility::Collapsed);
        BtnMemCancel().Visibility(Visibility::Collapsed);

        UpdateMemoryDump(m_memoryAddress);
    }

    // ========================================================================
    // Console (separate window)
    // ========================================================================

    void MainWindow::EnsureConsoleWindow()
    {
        if (m_consoleWindow) return;

        m_consoleWindow = Microsoft::UI::Xaml::Window();
        m_consoleWindow.Title(winrt::hstring(std::format(L"Console ({}x{})",
            m_terminal.GetCols(), m_terminal.GetRows())));

        // Clean up when console window is closed (via x button or programmatically)
        m_consoleWindow.Closed([this](auto&&, auto&&) {
            if (m_consoleRenderTimer) {
                m_consoleRenderTimer.Stop();
                m_consoleRenderTimer = nullptr;
            }
            m_consoleWindow = nullptr;
            m_consoleTextBox = nullptr;
            m_searchBox = nullptr;
            m_searchStatus = nullptr;
            m_caseSensitiveToggle = nullptr;
            m_regexToggle = nullptr;
            m_searchBar = nullptr;
            m_consoleSearchMode = false;
            m_consoleSearchIndex = -1;
            m_consoleLastSearchText.clear();
        });

        // Size the console window so its client area fits exactly
        // cols × rows character cells for the configured font. Falls back
        // to hard-coded Consolas 14pt metrics if Measure() is unavailable.
        {
            int cols = m_terminal.GetCols();
            int rows = m_terminal.GetRows();

            // Probe: measure a run of 10 glyphs and divide, which is more
            // accurate than a single glyph (single-char DesiredSize sometimes
            // includes subpixel trailing that vanishes when chars concatenate).
            // Consolas is monospaced so 10*advance == rendered width.
            Controls::TextBlock probe;
            probe.FontFamily(Media::FontFamily(L"Consolas"));
            probe.FontSize(14.0);
            probe.Text(L"MMMMMMMMMM");
            probe.Measure({ std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::infinity() });
            auto sz = probe.DesiredSize();
            double charW = sz.Width  > 10.0 ? sz.Width / 10.0 : 7.7;  // fallback
            double lineH = sz.Height > 1.0  ? sz.Height        : 16.3;

            // TextBox client area = cols×cellW + 8 (padding 4+4).
            // WinUI3 TextBox uses overlay scrollbars — they don't occupy
            // layout space, so no extra allowance is needed.
            double clientDIP_W = std::ceil(cols * charW) + 8.0;
            double clientDIP_H = std::ceil(rows * lineH) + 8.0;

            // Add outer chrome: ~32 px title bar + ~2 px borders (Windows 11).
            double outerDIP_W = clientDIP_W + 2.0;
            double outerDIP_H = clientDIP_H + 32.0;

            // Convert DIPs to physical pixels using the window's DPI.
            HWND hwnd = GetWindowHandle(m_consoleWindow);
            UINT dpi = hwnd ? ::GetDpiForWindow(hwnd) : 96;
            double scale = dpi > 0 ? dpi / 96.0 : 1.0;

            int pxW = static_cast<int>(std::ceil(outerDIP_W * scale));
            int pxH = static_cast<int>(std::ceil(outerDIP_H * scale));
            m_consoleWindow.AppWindow().Resize({ pxW, pxH });
        }

        // Create TextBox for output
        auto consoleFg = GetThemeBrush(L"ThemeConsoleFg");
        auto consoleBg = GetThemeBrush(L"ThemeConsoleBg");

        m_consoleTextBox = TextBox();
        m_consoleTextBox.IsReadOnly(true);
        m_consoleTextBox.AcceptsReturn(true);
        m_consoleTextBox.TextWrapping(TextWrapping::Wrap);
        m_consoleTextBox.FontFamily(Media::FontFamily(L"Consolas"));
        m_consoleTextBox.FontSize(14);
        m_consoleTextBox.Background(consoleBg);
        m_consoleTextBox.Foreground(consoleFg);
        m_consoleTextBox.BorderThickness({ 0, 0, 0, 0 });
        m_consoleTextBox.Padding({ 4, 4, 4, 4 });
        ScrollViewer::SetHorizontalScrollBarVisibility(m_consoleTextBox, ScrollBarVisibility::Disabled);
        SetupConsoleContextMenu();

        // Override theme resources to prevent color changes on PointerOver/Focused
        auto res = m_consoleTextBox.Resources();
        res.Insert(winrt::box_value(L"TextControlForeground"), consoleFg);
        res.Insert(winrt::box_value(L"TextControlForegroundPointerOver"), consoleFg);
        res.Insert(winrt::box_value(L"TextControlForegroundFocused"), consoleFg);
        res.Insert(winrt::box_value(L"TextControlBackground"), consoleBg);
        res.Insert(winrt::box_value(L"TextControlBackgroundPointerOver"), consoleBg);
        res.Insert(winrt::box_value(L"TextControlBackgroundFocused"), consoleBg);

        // Key input handler
        m_consoleTextBox.PreviewKeyDown([this](auto&&, Input::KeyRoutedEventArgs const& args) {
            using VirtualKey = Windows::System::VirtualKey;
            auto key = args.Key();

            // Ctrl+Shift+F: open search
            auto ctrlState = Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(VirtualKey::Control);
            auto shiftState = Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(VirtualKey::Shift);
            bool ctrlDown = (static_cast<int>(ctrlState) & static_cast<int>(Windows::UI::Core::CoreVirtualKeyStates::Down)) != 0;
            bool shiftDown = (static_cast<int>(shiftState) & static_cast<int>(Windows::UI::Core::CoreVirtualKeyStates::Down)) != 0;
            if (ctrlDown && shiftDown && key == VirtualKey::F) {
                OpenConsoleSearch();
                args.Handled(true);
                return;
            }
            // F3/Shift+F3 and Escape in search mode
            if (m_consoleSearchMode) {
                if (key == VirtualKey::F3) {
                    if (shiftDown) ConsoleFindPrev(); else ConsoleFindNext();
                    args.Handled(true); return;
                }
                if (key == VirtualKey::Escape) {
                    CloseConsoleSearch();
                    args.Handled(true); return;
                }
            }

            if (!m_instance) return;

            // Arrow keys send escape sequences (DECCKM aware)
            std::wstring seq;
            switch (key) {
                case VirtualKey::Up:    seq = m_terminal.GetApplicationCursorKeys() ? L"\x1BOA" : L"\x1B[A"; break;
                case VirtualKey::Down:  seq = m_terminal.GetApplicationCursorKeys() ? L"\x1BOB" : L"\x1B[B"; break;
                case VirtualKey::Right: seq = m_terminal.GetApplicationCursorKeys() ? L"\x1BOC" : L"\x1B[C"; break;
                case VirtualKey::Left:  seq = m_terminal.GetApplicationCursorKeys() ? L"\x1BOD" : L"\x1B[D"; break;
                default: break;
            }
            if (!seq.empty()) {
                for (wchar_t wc : seq)
                    m_plugin.emfe_send_char(m_instance, static_cast<char>(wc));
                args.Handled(true);
                return;
            }

            // Keys that produce control codes or that CharacterReceived
            // wouldn't surface (Enter/Back/Tab/Escape produce chars < 0x20
            // which we skip in CharacterReceived; Ctrl+letter is also
            // intercepted here so the TextBox doesn't swallow it).
            char ch = 0;
            if (key == VirtualKey::Enter) ch = '\n';
            else if (key == VirtualKey::Back) ch = '\b';
            else if (key == VirtualKey::Escape) ch = 0x1B;
            else if (key == VirtualKey::Tab) ch = '\t';
            else if (key == VirtualKey::Space) ch = ' ';   // guard: some TextBox configs eat Space before CharacterReceived
            else if (ctrlDown && key >= VirtualKey::A && key <= VirtualKey::Z) {
                ch = static_cast<char>(static_cast<int>(key) - static_cast<int>(VirtualKey::A) + 1);
            }

            if (ch != 0) {
                m_plugin.emfe_send_char(m_instance, ch);
                args.Handled(true);
            }
            // All other printable characters flow through CharacterReceived
            // (see handler below) — that path handles the real keyboard
            // layout, Shift state, numpad, and IME correctly.
        });

        // Printable-character path: honors keyboard layout / Shift / numpad / IME.
        m_consoleTextBox.CharacterReceived(
            [this](auto&&, Input::CharacterReceivedRoutedEventArgs const& args) {
                if (!m_instance) return;
                wchar_t wch = args.Character();
                if (wch == 0) return;
                char ch = static_cast<char>(wch & 0x7F);
                // PreviewKeyDown already delivered control codes; skip them here
                // to avoid double-send for Enter/Tab/Backspace etc.
                if (ch < 0x20) return;
                m_plugin.emfe_send_char(m_instance, ch);
                args.Handled(true);
            });

        // Build search bar
        auto searchBar = Grid();
        searchBar.Visibility(Visibility::Collapsed);
        searchBar.Background(Media::SolidColorBrush(Windows::UI::Color{0xFF, 0x1E, 0x1E, 0x1E}));
        searchBar.Padding({4, 4, 4, 4});
        searchBar.Height(32);

        // 7 columns: SearchBox(*), Status(Auto), CaseSensitive(Auto), Regex(Auto), Prev(Auto), Next(Auto), Close(Auto)
        auto col0 = ColumnDefinition(); col0.Width({1, GridUnitType::Star});
        auto col1 = ColumnDefinition(); col1.Width({0, GridUnitType::Auto});
        auto col2 = ColumnDefinition(); col2.Width({0, GridUnitType::Auto});
        auto col3 = ColumnDefinition(); col3.Width({0, GridUnitType::Auto});
        auto col4 = ColumnDefinition(); col4.Width({0, GridUnitType::Auto});
        auto col5 = ColumnDefinition(); col5.Width({0, GridUnitType::Auto});
        auto col6 = ColumnDefinition(); col6.Width({0, GridUnitType::Auto});
        searchBar.ColumnDefinitions().Append(col0);
        searchBar.ColumnDefinitions().Append(col1);
        searchBar.ColumnDefinitions().Append(col2);
        searchBar.ColumnDefinitions().Append(col3);
        searchBar.ColumnDefinitions().Append(col4);
        searchBar.ColumnDefinitions().Append(col5);
        searchBar.ColumnDefinitions().Append(col6);

        auto searchBoxBg = Media::SolidColorBrush(Windows::UI::Color{0xFF, 0x2D, 0x2D, 0x2D});
        auto searchBoxFg = Media::SolidColorBrush(Windows::UI::Color{0xFF, 0xCC, 0xCC, 0xCC});
        auto searchBorder = Media::SolidColorBrush(Windows::UI::Color{0xFF, 0x3F, 0x3F, 0x3F});
        auto dimText = Media::SolidColorBrush(Windows::UI::Color{0xFF, 0x80, 0x80, 0x80});

        // Search box (col 0)
        m_searchBox = TextBox();
        m_searchBox.FontFamily(Media::FontFamily(L"Consolas"));
        m_searchBox.FontSize(13);
        m_searchBox.PlaceholderText(L"Search...");
        m_searchBox.Background(searchBoxBg);
        m_searchBox.Foreground(searchBoxFg);
        m_searchBox.BorderBrush(searchBorder);
        m_searchBox.BorderThickness({1, 1, 1, 1});
        m_searchBox.Padding({4, 2, 4, 2});
        m_searchBox.VerticalAlignment(VerticalAlignment::Center);
        m_searchBox.Margin({0, 0, 4, 0});
        {
            auto res = m_searchBox.Resources();
            res.Insert(winrt::box_value(L"TextControlForeground"), searchBoxFg);
            res.Insert(winrt::box_value(L"TextControlForegroundPointerOver"), searchBoxFg);
            res.Insert(winrt::box_value(L"TextControlForegroundFocused"), searchBoxFg);
            res.Insert(winrt::box_value(L"TextControlBackground"), searchBoxBg);
            res.Insert(winrt::box_value(L"TextControlBackgroundPointerOver"), searchBoxBg);
            res.Insert(winrt::box_value(L"TextControlBackgroundFocused"), searchBoxBg);
        }
        m_searchBox.KeyDown([this](auto&&, Input::KeyRoutedEventArgs const& e) {
            using VirtualKey = Windows::System::VirtualKey;
            if (e.Key() == VirtualKey::Enter) { ConsoleFindNext(); e.Handled(true); }
            else if (e.Key() == VirtualKey::Escape) { CloseConsoleSearch(); e.Handled(true); }
            else if (e.Key() == VirtualKey::F3) {
                auto shiftState = Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(VirtualKey::Shift);
                bool shiftDown = (static_cast<int>(shiftState) & static_cast<int>(Windows::UI::Core::CoreVirtualKeyStates::Down)) != 0;
                if (shiftDown) ConsoleFindPrev(); else ConsoleFindNext();
                e.Handled(true);
            }
        });
        Grid::SetColumn(m_searchBox, 0);
        searchBar.Children().Append(m_searchBox);

        // Search status (col 1)
        m_searchStatus = TextBlock();
        m_searchStatus.FontFamily(Media::FontFamily(L"Consolas"));
        m_searchStatus.FontSize(12);
        m_searchStatus.Foreground(dimText);
        m_searchStatus.VerticalAlignment(VerticalAlignment::Center);
        m_searchStatus.Margin({4, 0, 4, 0});
        m_searchStatus.MinWidth(50);
        Grid::SetColumn(m_searchStatus, 1);
        searchBar.Children().Append(m_searchStatus);

        // Case-sensitive toggle (col 2)
        m_caseSensitiveToggle = Controls::Primitives::ToggleButton();
        m_caseSensitiveToggle.Content(winrt::box_value(L"Aa"));
        m_caseSensitiveToggle.FontFamily(Media::FontFamily(L"Consolas"));
        m_caseSensitiveToggle.FontSize(12);
        m_caseSensitiveToggle.Padding({4, 2, 4, 2});
        m_caseSensitiveToggle.MinWidth(0);
        m_caseSensitiveToggle.MinHeight(0);
        m_caseSensitiveToggle.VerticalAlignment(VerticalAlignment::Center);
        m_caseSensitiveToggle.Margin({2, 0, 2, 0});
        Grid::SetColumn(m_caseSensitiveToggle, 2);
        searchBar.Children().Append(m_caseSensitiveToggle);

        // Regex toggle (col 3)
        m_regexToggle = Controls::Primitives::ToggleButton();
        m_regexToggle.Content(winrt::box_value(L".*"));
        m_regexToggle.FontFamily(Media::FontFamily(L"Consolas"));
        m_regexToggle.FontSize(12);
        m_regexToggle.Padding({4, 2, 4, 2});
        m_regexToggle.MinWidth(0);
        m_regexToggle.MinHeight(0);
        m_regexToggle.VerticalAlignment(VerticalAlignment::Center);
        m_regexToggle.Margin({2, 0, 2, 0});
        Grid::SetColumn(m_regexToggle, 3);
        searchBar.Children().Append(m_regexToggle);

        // Prev button (col 4) - Unicode up arrow
        auto btnPrev = Button();
        btnPrev.Content(winrt::box_value(L"\u25B2"));
        btnPrev.FontSize(10);
        btnPrev.Padding({4, 2, 4, 2});
        btnPrev.MinWidth(0);
        btnPrev.MinHeight(0);
        btnPrev.VerticalAlignment(VerticalAlignment::Center);
        btnPrev.Margin({2, 0, 0, 0});
        btnPrev.Click([this](auto&&, auto&&) { ConsoleFindPrev(); });
        Grid::SetColumn(btnPrev, 4);
        searchBar.Children().Append(btnPrev);

        // Next button (col 5) - Unicode down arrow
        auto btnNext = Button();
        btnNext.Content(winrt::box_value(L"\u25BC"));
        btnNext.FontSize(10);
        btnNext.Padding({4, 2, 4, 2});
        btnNext.MinWidth(0);
        btnNext.MinHeight(0);
        btnNext.VerticalAlignment(VerticalAlignment::Center);
        btnNext.Margin({2, 0, 0, 0});
        btnNext.Click([this](auto&&, auto&&) { ConsoleFindNext(); });
        Grid::SetColumn(btnNext, 5);
        searchBar.Children().Append(btnNext);

        // Close button (col 6) - Unicode X mark
        auto btnClose = Button();
        btnClose.Content(winrt::box_value(L"\u2715"));
        btnClose.FontSize(10);
        btnClose.Padding({4, 2, 4, 2});
        btnClose.MinWidth(0);
        btnClose.MinHeight(0);
        btnClose.VerticalAlignment(VerticalAlignment::Center);
        btnClose.Margin({2, 0, 0, 0});
        btnClose.Click([this](auto&&, auto&&) { CloseConsoleSearch(); });
        Grid::SetColumn(btnClose, 6);
        searchBar.Children().Append(btnClose);

        m_searchBar = searchBar;

        // Wrap TextBox + search bar in a container Grid
        auto container = Grid();
        auto row0 = RowDefinition(); row0.Height({1, GridUnitType::Star});
        auto row1 = RowDefinition(); row1.Height({0, GridUnitType::Auto});
        container.RowDefinitions().Append(row0);
        container.RowDefinitions().Append(row1);
        Grid::SetRow(m_consoleTextBox, 0);
        Grid::SetRow(searchBar, 1);
        container.Children().Append(m_consoleTextBox);
        container.Children().Append(searchBar);

        m_consoleWindow.Content(container);

        // Set up render timer for VT100 terminal
        if (!m_consoleRenderTimer) {
            m_consoleRenderTimer = DispatcherQueue().CreateTimer();
            m_consoleRenderTimer.Interval(std::chrono::milliseconds(100));
            m_consoleRenderTimer.Tick([this](auto&&, auto&&) {
                // Drain output queue
                {
                    std::lock_guard<std::mutex> lock(m_consoleOutputMutex);
                    while (!m_consoleOutputQueue.empty()) {
                        m_terminal.Write(m_consoleOutputQueue.front());
                        m_consoleOutputQueue.pop();
                    }
                }

                if (!m_terminal.IsDirty()) return;
                m_terminal.ClearDirty();

                if (m_consoleTextBox) {
                    auto rendered = m_terminal.RenderFullWithCursor();
                    m_consoleTextBox.Text(winrt::to_hstring(rendered));
                    auto textLen = static_cast<int32_t>(m_consoleTextBox.Text().size());
                    m_consoleTextBox.Select(textLen, 0);
                }
            });
            m_consoleRenderTimer.Start();
        }

        // Apply current theme
        if (m_instance) {
            const char* themeVal = m_plugin.emfe_get_setting(m_instance, "Theme");
            std::string t = themeVal ? themeVal : "Dark";
            bool isDark = (t != "Light");
            if (t == "System") {
                DWORD useLight = 0, size = sizeof(useLight);
                if (::RegGetValueW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                    L"AppsUseLightTheme", RRF_RT_DWORD, nullptr, &useLight, &size) == ERROR_SUCCESS)
                    isDark = !useLight;
            }
            ApplyThemeToWindow(m_consoleWindow, isDark);
        }
    }

    void MainWindow::SetupConsoleContextMenu()
    {
        if (!m_consoleTextBox) return;

        auto flyout = Controls::MenuFlyout();

        auto itemCopy = Controls::MenuFlyoutItem();
        itemCopy.Text(L"Copy (Ctrl+C)");
        itemCopy.Click([this](auto&&, auto&&) { DoConsoleCopy(); });
        flyout.Items().Append(itemCopy);

        auto itemPaste = Controls::MenuFlyoutItem();
        itemPaste.Text(L"Paste");
        itemPaste.Click([this](auto&&, auto&&) { DoConsolePaste(); });
        flyout.Items().Append(itemPaste);

        flyout.Items().Append(Controls::MenuFlyoutSeparator());

        auto itemSelectAll = Controls::MenuFlyoutItem();
        itemSelectAll.Text(L"Select All (Ctrl+A)");
        itemSelectAll.Click([this](auto&&, auto&&) { DoConsoleSelectAll(); });
        flyout.Items().Append(itemSelectAll);

        m_consoleTextBox.ContextFlyout(flyout);
    }

    void MainWindow::DoConsoleCopy()
    {
        if (!m_consoleTextBox) return;
        auto selected = std::wstring(m_consoleTextBox.SelectedText());
        if (selected.empty())
            selected = std::wstring(m_consoleTextBox.Text());
        if (selected.empty()) return;

        Windows::ApplicationModel::DataTransfer::DataPackage dp;
        dp.SetText(winrt::hstring(selected));
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
    }

    void MainWindow::DoConsoleSelectAll()
    {
        if (m_consoleTextBox)
            m_consoleTextBox.SelectAll();
    }

    // Paste implementation mirrors emfe_CsWPF's PasteWithHandshake:
    //   - If the plugin exports emfe_console_tx_space (bounded UARTs like
    //     Uart16550 return exact RX-FIFO headroom; unbounded like Z8530
    //     return INT32_MAX), throttle to that headroom plus a 10 ms pause
    //     between unbounded bursts so the guest kernel's line-discipline
    //     buffer has room to drain (NetBSD reports "zstty: ibuf flood"
    //     otherwise).
    //   - Without the export, fall back to a 64-char / 1 ms burst.
    //   - CR / CRLF normalized to LF.
    //
    // Progress surfaces through the console window title so the user can
    // see whether the host sent everything or whether backpressure stalled.
    void MainWindow::DoConsolePaste()
    {
        if (!m_instance || !m_plugin.IsLoaded()) return;

        // Retrieve clipboard text on the UI thread (await here is allowed
        // because winrt::fire_and_forget resumes back on the dispatcher).
        auto pasteOp = [this]() -> winrt::fire_and_forget {
            auto dp = Windows::ApplicationModel::DataTransfer::Clipboard::GetContent();
            if (!dp.Contains(Windows::ApplicationModel::DataTransfer::StandardDataFormats::Text()))
                co_return;
            winrt::hstring raw;
            try {
                raw = co_await dp.GetTextAsync();
            } catch (...) {
                co_return;
            }
            if (raw.empty()) co_return;

            // Normalize line endings to LF (matches emfe_CsWPF convention).
            std::wstring text(raw);
            std::wstring normalized;
            normalized.reserve(text.size());
            for (size_t k = 0; k < text.size(); ++k) {
                wchar_t c = text[k];
                if (c == L'\r') {
                    normalized.push_back(L'\n');
                    if (k + 1 < text.size() && text[k + 1] == L'\n') ++k;
                } else {
                    normalized.push_back(c);
                }
            }
            if (normalized.empty()) co_return;

            // Cancel any in-flight paste; mark new one active.
            if (m_consolePasteActive.exchange(true)) {
                m_consolePasteCancel.store(true);
                // Tight wait loop: hand the scheduler 1 ms ticks until
                // the previous paste observes the cancel and drops.
                for (int i = 0; i < 500 && m_consolePasteActive.load(); ++i) {
                    co_await winrt::resume_after(std::chrono::milliseconds(1));
                }
                co_await wil::resume_foreground(m_dispatcherQueue);
                m_consolePasteCancel.store(false);
                m_consolePasteActive.store(true);
            }

            auto origTitle = m_consoleWindow ? std::wstring(m_consoleWindow.Title()) : std::wstring{};
            m_consoleTitleOrig = origTitle;
            int total = static_cast<int>(normalized.size());
            int sent = 0;

            // Probe backpressure once; stick with the chosen mode for the
            // whole paste so the title label stays coherent.
            int probe = -1;
            if (m_plugin.emfe_console_tx_space)
                probe = m_plugin.emfe_console_tx_space(m_instance);
            bool haveHandshake = probe >= 0;
            std::wstring mode = haveHandshake ? L"handshake" : L"burst";

            auto updateTitle = [this, &origTitle, total, &mode](int s, bool finished) {
                if (!m_consoleWindow) return;
                m_consoleWindow.Title(winrt::hstring(std::format(
                    L"{} {}/{} via {} — {}",
                    finished ? L"Pasted" : L"Pasting",
                    s, total, mode, origTitle)));
            };

            constexpr int Chunk = 64;
            constexpr int UnboundedThreshold = 1024;
            constexpr int UnboundedBurstPauseMs = 10;
            constexpr int StallBreakMs = 5000;

            // winrt::resume_after resumes on the thread pool — any
            // subsequent UI access (including Window::Title) would throw.
            // Always follow each delay with a foreground hop so the whole
            // paste loop stays on the dispatcher.
            if (haveHandshake) {
                int stallMs = 0;
                while (sent < total) {
                    if (m_consolePasteCancel.load()) break;
                    int space = m_plugin.emfe_console_tx_space(m_instance);
                    if (space <= 0) {
                        co_await winrt::resume_after(std::chrono::milliseconds(1));
                        co_await wil::resume_foreground(m_dispatcherQueue);
                        stallMs += 1;
                        if (stallMs >= StallBreakMs) break;
                        continue;
                    }
                    stallMs = 0;
                    bool unbounded = space >= UnboundedThreshold;
                    int n = (std::min)((std::min)(space, Chunk), total - sent);
                    for (int k = 0; k < n; ++k)
                        m_plugin.emfe_send_char(m_instance,
                            static_cast<char>(normalized[sent + k]));
                    sent += n;
                    updateTitle(sent, false);
                    if (unbounded) {
                        co_await winrt::resume_after(std::chrono::milliseconds(UnboundedBurstPauseMs));
                        co_await wil::resume_foreground(m_dispatcherQueue);
                    } else {
                        co_await wil::resume_foreground(m_dispatcherQueue);
                    }
                }
            } else {
                int count = 0;
                while (sent < total) {
                    if (m_consolePasteCancel.load()) break;
                    m_plugin.emfe_send_char(m_instance,
                        static_cast<char>(normalized[sent++]));
                    if (++count >= Chunk) {
                        count = 0;
                        updateTitle(sent, false);
                        co_await winrt::resume_after(std::chrono::milliseconds(1));
                        co_await wil::resume_foreground(m_dispatcherQueue);
                    }
                }
            }

            updateTitle(sent, true);
            co_await winrt::resume_after(std::chrono::milliseconds(2500));
            co_await wil::resume_foreground(m_dispatcherQueue);
            if (m_consoleWindow)
                m_consoleWindow.Title(winrt::hstring(origTitle));

            m_consolePasteActive.store(false);
            m_consolePasteCancel.store(false);
        };
        pasteOp();
    }

    void MainWindow::AppendConsoleChar(char ch)
    {
        EnsureConsoleWindow();

        {
            std::lock_guard<std::mutex> lock(m_consoleOutputMutex);
            m_consoleOutputQueue.push(ch);
        }

        // Auto-show on first output
        if (m_consoleWindow && !m_consoleWindow.Visible()) {
            m_consoleWindow.Activate();
        }
    }

    // ========================================================================
    // Console search
    // ========================================================================

    void MainWindow::OpenConsoleSearch()
    {
        m_consoleSearchMode = true;
        if (m_searchBar) m_searchBar.Visibility(Visibility::Visible);
        if (m_searchBox) {
            if (m_consoleTextBox && m_consoleTextBox.SelectionLength() > 0)
                m_searchBox.Text(m_consoleTextBox.SelectedText());
            m_searchBox.Focus(FocusState::Programmatic);
            m_searchBox.SelectAll();
        }
    }

    void MainWindow::CloseConsoleSearch()
    {
        m_consoleSearchMode = false;
        m_consoleSearchIndex = -1;
        m_consoleLastSearchText.clear();
        if (m_searchBar) m_searchBar.Visibility(Visibility::Collapsed);
        if (m_searchStatus) m_searchStatus.Text(L"");
        if (m_consoleTextBox) m_consoleTextBox.Focus(FocusState::Programmatic);
    }

    void MainWindow::ConsoleFindNext()
    {
        if (!m_consoleTextBox || !m_searchBox) return;
        auto searchText = winrt::to_string(m_searchBox.Text());
        if (searchText.empty()) return;
        auto text = winrt::to_string(m_consoleTextBox.Text());
        if (text.empty()) return;

        bool regexMode = m_regexToggle && m_regexToggle.IsChecked().Value();
        bool caseSensitive = m_caseSensitiveToggle && m_caseSensitiveToggle.IsChecked().Value();

        if (searchText != m_consoleLastSearchText) {
            m_consoleSearchIndex = -1;
            m_consoleLastSearchText = searchText;
        }

        auto matches = ConsoleCollectMatches(text, searchText, regexMode, caseSensitive);
        if (matches.empty()) {
            m_consoleSearchIndex = -1;
            if (m_searchStatus) m_searchStatus.Text(regexMode ? L"No match" : L"Not found");
            return;
        }

        // Find next match after current position
        int nextIdx = -1;
        for (int i = 0; i < static_cast<int>(matches.size()); i++) {
            if (matches[i].first > m_consoleSearchIndex) { nextIdx = i; break; }
        }
        if (nextIdx < 0) nextIdx = 0; // wrap around

        m_consoleSearchIndex = matches[nextIdx].first;
        ConsoleHighlightMatch(matches[nextIdx].first, matches[nextIdx].second,
                              nextIdx + 1, static_cast<int>(matches.size()));
    }

    void MainWindow::ConsoleFindPrev()
    {
        if (!m_consoleTextBox || !m_searchBox) return;
        auto searchText = winrt::to_string(m_searchBox.Text());
        if (searchText.empty()) return;
        auto text = winrt::to_string(m_consoleTextBox.Text());
        if (text.empty()) return;

        bool regexMode = m_regexToggle && m_regexToggle.IsChecked().Value();
        bool caseSensitive = m_caseSensitiveToggle && m_caseSensitiveToggle.IsChecked().Value();

        if (searchText != m_consoleLastSearchText) {
            m_consoleSearchIndex = static_cast<int>(text.size());
            m_consoleLastSearchText = searchText;
        }

        auto matches = ConsoleCollectMatches(text, searchText, regexMode, caseSensitive);
        if (matches.empty()) {
            m_consoleSearchIndex = -1;
            if (m_searchStatus) m_searchStatus.Text(regexMode ? L"No match" : L"Not found");
            return;
        }

        // Find previous match before current position
        int prevIdx = -1;
        for (int i = static_cast<int>(matches.size()) - 1; i >= 0; i--) {
            if (matches[i].first < m_consoleSearchIndex) { prevIdx = i; break; }
        }
        if (prevIdx < 0) prevIdx = static_cast<int>(matches.size()) - 1; // wrap

        m_consoleSearchIndex = matches[prevIdx].first;
        ConsoleHighlightMatch(matches[prevIdx].first, matches[prevIdx].second,
                              prevIdx + 1, static_cast<int>(matches.size()));
    }

    std::vector<std::pair<int, int>> MainWindow::ConsoleCollectMatches(
        const std::string& text, const std::string& searchText,
        bool regexMode, bool caseSensitive)
    {
        std::vector<std::pair<int, int>> matches;
        if (searchText.empty() || text.empty()) return matches;

        if (regexMode) {
            try {
                auto flags = caseSensitive ? std::regex::ECMAScript : std::regex::icase;
                std::regex re(searchText, flags);
                auto begin = std::sregex_iterator(text.begin(), text.end(), re);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end; ++it)
                    matches.emplace_back(static_cast<int>(it->position()),
                                         static_cast<int>(it->length()));
            } catch (const std::regex_error&) {
                // Invalid regex - return empty
            }
        }
        else if (caseSensitive) {
            size_t pos = 0;
            while ((pos = text.find(searchText, pos)) != std::string::npos) {
                matches.emplace_back(static_cast<int>(pos), static_cast<int>(searchText.size()));
                pos += searchText.size();
            }
        }
        else {
            auto textLower = text;
            auto searchLower = searchText;
            for (auto& c : textLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (auto& c : searchLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            size_t pos = 0;
            while ((pos = textLower.find(searchLower, pos)) != std::string::npos) {
                matches.emplace_back(static_cast<int>(pos), static_cast<int>(searchLower.size()));
                pos += searchLower.size();
            }
        }
        return matches;
    }

    void MainWindow::ConsoleHighlightMatch(int pos, int length, int current, int total)
    {
        if (!m_consoleTextBox) return;

        m_consoleTextBox.Focus(FocusState::Programmatic);
        m_consoleTextBox.Select(pos, length);

        if (m_searchStatus)
            m_searchStatus.Text(winrt::to_hstring(std::format("{}/{}", current, total)));
    }

    void MainWindow::OnToggleConsole(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_consoleWindow) {
            m_consoleWindow.Close();
        } else {
            EnsureConsoleWindow();
            m_consoleWindow.Activate();
        }
    }

    // ========================================================================
    // Theme
    // ========================================================================

    void MainWindow::ApplyThemeToWindow(Microsoft::UI::Xaml::Window const& window, bool isDark)
    {
        if (!window) return;
        auto theme = isDark ? ElementTheme::Dark : ElementTheme::Light;
        if (auto root = window.Content().try_as<FrameworkElement>())
            root.RequestedTheme(theme);
        if (auto native = window.try_as<::IWindowNative>()) {
            HWND hwnd = nullptr;
            native->get_WindowHandle(&hwnd);
            if (hwnd) {
                BOOL mode = isDark ? TRUE : FALSE;
                ::DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                    &mode, sizeof(mode));
            }
        }
    }

    Media::Brush MainWindow::GetThemeBrush(const wchar_t* key)
    {
        try {
            auto app = Application::Current();
            auto themeDicts = app.Resources().ThemeDictionaries();
            auto themeKey = m_isDark ? L"Dark" : L"Light";
            auto dict = themeDicts.Lookup(box_value(hstring(themeKey))).as<ResourceDictionary>();
            return dict.Lookup(box_value(hstring(key))).as<Media::Brush>();
        } catch (...) {
            return Media::SolidColorBrush(Windows::UI::Color{ 0xFF, 0xFF, 0xFF, 0xFF });
        }
    }

    void MainWindow::RefreshCodeBehindBrushes()
    {
        // Force memory grid rebuild with new theme brushes
        if (m_memGridBuilt) {
            m_memGridBuilt = false;
            m_memCellBoxes.clear();
            m_memAddrLabels.clear();
            m_memAsciiLabels.clear();
            MemoryDumpPanel().Children().Clear();
        }

        // Rebuild the register panel from scratch — the dynamic layout
        // (plugin-driven group list) makes it simpler than walking the tree
        // to re-color individual labels.
        BuildRegisterPanel();
        UpdateRegisters();

        UpdateDisassembly();
        UpdateMemoryDump(m_memoryAddress);
    }

    void MainWindow::ApplyTheme(const std::string& themeName)
    {
        std::string effective = themeName;
        if (effective == "System" || effective.empty()) {
            DWORD useLight = 0;
            DWORD size = sizeof(useLight);
            if (::RegGetValueW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme", RRF_RT_DWORD, nullptr, &useLight, &size) == ERROR_SUCCESS)
                effective = useLight ? "Light" : "Dark";
            else
                effective = "Dark";
        }
        bool isDark = (effective != "Light");
        if (isDark == m_isDark) return;
        m_isDark = isDark;

        ApplyThemeToWindow(*this, isDark);
        ApplyThemeToWindow(m_consoleWindow, isDark);
        ApplyThemeToWindow(m_settingsWindow, isDark);
        ApplyThemeToWindow(m_breakpointsWindow, isDark);
        ApplyThemeToWindow(m_callStackWindow, isDark);
        ApplyThemeToWindow(m_framebufferWindow, isDark);
        RefreshCodeBehindBrushes();
        RefreshBreakpointsWindow();
        RefreshCallStackWindow();
    }

    // ========================================================================
    // Framebuffer window + Input capture
    // ========================================================================

    void MainWindow::OnOpenFramebuffer(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;
        if (m_framebufferWindow) { m_framebufferWindow.Activate(); return; }

        m_framebufferWindow = Microsoft::UI::Xaml::Window();
        m_framebufferWindow.Title(L"Framebuffer");
        m_framebufferWindow.AppWindow().Resize({ 660, 520 });
        m_framebufferWindow.Closed([this](auto&&, auto&&) {
            // Cursor restored automatically when window closes
            if (m_fbTimer) { m_fbTimer.Stop(); m_fbTimer = nullptr; }
            m_framebufferWindow = nullptr;
            m_fbImage = nullptr;
            m_fbBitmap = nullptr;
            m_fbGrid = nullptr;
            m_fbStatusText = nullptr;
            m_fbInputStatus = nullptr;
            m_fbLastWidth = m_fbLastHeight = m_fbLastBpp = 0;
            m_fbInputCaptured = false;
        });

        // Build UI
        auto outerPanel = Grid();
        outerPanel.RowDefinitions().Append(RowDefinition());
        outerPanel.RowDefinitions().GetAt(0).Height({ 1, GridUnitType::Star });
        outerPanel.RowDefinitions().Append(RowDefinition());
        outerPanel.RowDefinitions().GetAt(1).Height(GridLengthHelper::Auto());

        m_fbGrid = winrt::make_self<CursorGrid>();
        m_fbGrid->Background(Media::SolidColorBrush(Windows::UI::Color{ 0xFF, 0x00, 0x00, 0x00 }));
        m_fbGrid->SetCursor(Microsoft::UI::Input::InputSystemCursor::Create(
            Microsoft::UI::Input::InputSystemCursorShape::Cross));
        m_fbImage = Controls::Image();
        m_fbImage.Stretch(Media::Stretch::Uniform);
        m_fbGrid->Children().Append(m_fbImage);

        auto disabledText = TextBlock();
        disabledText.Text(L"Framebuffer is disabled. Enable it in Settings \u2192 Framebuffer.");
        disabledText.HorizontalAlignment(HorizontalAlignment::Center);
        disabledText.VerticalAlignment(VerticalAlignment::Center);
        disabledText.FontSize(14);
        disabledText.TextWrapping(TextWrapping::Wrap);
        disabledText.TextAlignment(TextAlignment::Center);
        disabledText.Foreground(GetThemeBrush(L"ThemeDimFg"));
        disabledText.Name(L"DisabledText");
        m_fbGrid->Children().Append(disabledText);

        Grid::SetRow(*m_fbGrid, 0);
        outerPanel.Children().Append(*m_fbGrid);

        auto statusBar = StackPanel();
        statusBar.Orientation(Orientation::Horizontal);
        statusBar.Background(GetThemeBrush(L"ThemePanelBg"));
        statusBar.Padding({ 8, 4, 8, 4 });
        m_fbStatusText = TextBlock();
        m_fbStatusText.FontSize(12);
        m_fbStatusText.Foreground(GetThemeBrush(L"ThemeForeground"));
        statusBar.Children().Append(m_fbStatusText);
        m_fbInputStatus = TextBlock();
        m_fbInputStatus.FontSize(12);
        m_fbInputStatus.Margin({ 16, 0, 0, 0 });
        m_fbInputStatus.Foreground(GetThemeBrush(L"ThemeDimFg"));
        statusBar.Children().Append(m_fbInputStatus);
        Grid::SetRow(statusBar, 1);
        outerPanel.Children().Append(statusBar);

        m_framebufferWindow.Content(outerPanel);
        ApplyThemeToWindow(m_framebufferWindow, m_isDark);

        // Input handlers
        m_fbGrid->PointerPressed([this](auto&&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e) {
            if (!m_fbInputCaptured) {
                m_fbInputCaptured = true;
                m_fbGrid->CapturePointer(e.Pointer());
                // Hide cursor via disposed InputSystemCursor (em68030 pattern)
                auto blankCursor = Microsoft::UI::Input::InputSystemCursor::Create(
                    Microsoft::UI::Input::InputSystemCursorShape::Arrow);
                blankCursor.Close();
                m_fbGrid->SetCursor(blankCursor);
                m_framebufferWindow.Content().as<FrameworkElement>().Focus(FocusState::Programmatic);
            } else {
                auto props = e.GetCurrentPoint(*m_fbGrid).Properties();
                int btn = props.IsLeftButtonPressed() ? 0 : props.IsRightButtonPressed() ? 1 : 2;
                m_plugin.emfe_push_mouse_button(m_instance, btn, true);
            }
        });
        m_fbGrid->PointerReleased([this](auto&&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e) {
            if (!m_fbInputCaptured) return;
            auto props = e.GetCurrentPoint(*m_fbGrid).Properties();
            if (!props.IsLeftButtonPressed()) m_plugin.emfe_push_mouse_button(m_instance, 0, false);
            if (!props.IsRightButtonPressed()) m_plugin.emfe_push_mouse_button(m_instance, 1, false);
        });
        m_fbGrid->PointerMoved([this](auto&&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e) {
            if (!m_fbInputCaptured || m_fbLastWidth == 0 || m_fbLastHeight == 0) return;
            auto pos = e.GetCurrentPoint(*m_fbGrid).Position();
            double gridW = m_fbGrid->ActualWidth(), gridH = m_fbGrid->ActualHeight();
            if (gridW <= 0 || gridH <= 0) return;
            double imgAspect = static_cast<double>(m_fbLastWidth) / m_fbLastHeight;
            double gridAspect = gridW / gridH;
            double renderW, renderH, offX, offY;
            if (gridAspect > imgAspect) { renderH = gridH; renderW = gridH * imgAspect; offX = (gridW - renderW) / 2; offY = 0; }
            else { renderW = gridW; renderH = gridW / imgAspect; offX = 0; offY = (gridH - renderH) / 2; }
            int x = static_cast<int>((pos.X - offX) / renderW * m_fbLastWidth);
            int y = static_cast<int>((pos.Y - offY) / renderH * m_fbLastHeight);
            x = std::clamp(x, 0, static_cast<int>(m_fbLastWidth) - 1);
            y = std::clamp(y, 0, static_cast<int>(m_fbLastHeight) - 1);
            m_plugin.emfe_push_mouse_absolute(m_instance, x, y);
        });

        auto contentElement = m_framebufferWindow.Content().as<FrameworkElement>();
        contentElement.KeyDown([this](auto&&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
            if (!m_fbInputCaptured) return;
            if (e.Key() == Windows::System::VirtualKey::Escape) {
                m_fbInputCaptured = false;
                m_fbGrid->ReleasePointerCaptures();
                m_fbGrid->SetCursor(Microsoft::UI::Input::InputSystemCursor::Create(
                    Microsoft::UI::Input::InputSystemCursorShape::Cross));
                e.Handled(true);
                return;
            }
            uint32_t vk = static_cast<uint32_t>(e.Key());
            uint32_t sc = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            if (sc != 0) { m_plugin.emfe_push_key(m_instance, sc, true); e.Handled(true); }
        });
        contentElement.KeyUp([this](auto&&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
            if (!m_fbInputCaptured) return;
            uint32_t vk = static_cast<uint32_t>(e.Key());
            uint32_t sc = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            if (sc != 0) { m_plugin.emfe_push_key(m_instance, sc, false); e.Handled(true); }
        });

        // Timer for frame refresh
        m_fbTimer = DispatcherTimer();
        m_fbTimer.Interval(std::chrono::milliseconds(33));
        m_fbTimer.Tick([this](auto&&, auto&&) { RefreshFramebufferFrame(); });
        m_fbFpsStart = std::chrono::steady_clock::now();
        m_fbFrameCount = 0;
        m_fbTimer.Start();

        m_framebufferWindow.Activate();
        RefreshFramebufferFrame();
    }

    void MainWindow::RefreshFramebufferFrame()
    {
        if (!m_instance || !m_framebufferWindow) return;

        EmfeFramebufferInfo info{};
        auto res = m_plugin.emfe_get_framebuffer_info(m_instance, &info);
        if (res != EMFE_OK || info.pixels == nullptr || info.width == 0 || info.height == 0) {
            if (m_fbImage) m_fbImage.Source(nullptr);
            m_fbStatusText.Text(L"Framebuffer disabled");
            m_fbInputStatus.Text(L"");
            return;
        }

        // Hide disabled text
        if (m_fbGrid->Children().Size() > 1) {
            auto disText = m_fbGrid->Children().GetAt(1).try_as<TextBlock>();
            if (disText) disText.Visibility(Visibility::Collapsed);
        }

        int width = static_cast<int>(info.width);
        int height = static_cast<int>(info.height);

        if (!m_fbBitmap || m_fbLastWidth != info.width || m_fbLastHeight != info.height || m_fbLastBpp != info.bpp) {
            m_fbBitmap = Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap(width, height);
            m_fbImage.Source(m_fbBitmap);
            m_fbLastWidth = info.width;
            m_fbLastHeight = info.height;
            m_fbLastBpp = info.bpp;
        }

        int srcSize = static_cast<int>(info.stride) * height;
        int dstStride = width * 4;
        std::vector<uint8_t> src(srcSize);
        memcpy(src.data(), info.pixels, srcSize);

        // Write to bitmap
        auto pixelBuffer = m_fbBitmap.PixelBuffer();
        uint8_t* dst = pixelBuffer.data();
        ConvertToBgra(info, src.data(), dst, dstStride);
        m_fbBitmap.Invalidate();

        // FPS
        m_fbFrameCount++;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_fbFpsStart).count();
        if (elapsed >= 1000) {
            m_fbCurrentFps = m_fbFrameCount * 1000.0 / elapsed;
            m_fbFrameCount = 0;
            m_fbFpsStart = now;
        }

        m_fbStatusText.Text(std::format(L"{}x{} {}bpp  ${:08X}  {:.1f} fps",
            width, height, info.bpp, static_cast<uint32_t>(info.base_address), m_fbCurrentFps));
        m_fbInputStatus.Text(m_fbInputCaptured ? L"Input: captured (Esc to release)" : L"Click framebuffer to capture input");
    }

    void MainWindow::ConvertToBgra(const EmfeFramebufferInfo& info, const uint8_t* src, uint8_t* dst, int dstStride)
    {
        int w = static_cast<int>(info.width);
        int h = static_cast<int>(info.height);
        int srcStride = static_cast<int>(info.stride);

        switch (info.bpp) {
        case 8: { // Indexed8
            uint32_t palette[256]{};
            m_plugin.emfe_get_palette(m_instance, palette, 256);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    uint32_t argb = palette[src[y * srcStride + x]];
                    dst[y * dstStride + x * 4 + 0] = static_cast<uint8_t>(argb & 0xFF);
                    dst[y * dstStride + x * 4 + 1] = static_cast<uint8_t>((argb >> 8) & 0xFF);
                    dst[y * dstStride + x * 4 + 2] = static_cast<uint8_t>((argb >> 16) & 0xFF);
                    dst[y * dstStride + x * 4 + 3] = 0xFF;
                }
            }
            break;
        }
        case 16: { // RGB565 big-endian
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    uint16_t px = static_cast<uint16_t>((src[y * srcStride + x * 2] << 8) | src[y * srcStride + x * 2 + 1]);
                    uint8_t r = static_cast<uint8_t>((px >> 11) & 0x1F);
                    uint8_t g = static_cast<uint8_t>((px >> 5) & 0x3F);
                    uint8_t b = static_cast<uint8_t>(px & 0x1F);
                    dst[y * dstStride + x * 4 + 0] = static_cast<uint8_t>((b << 3) | (b >> 2));
                    dst[y * dstStride + x * 4 + 1] = static_cast<uint8_t>((g << 2) | (g >> 4));
                    dst[y * dstStride + x * 4 + 2] = static_cast<uint8_t>((r << 3) | (r >> 2));
                    dst[y * dstStride + x * 4 + 3] = 0xFF;
                }
            }
            break;
        }
        case 24: { // RGB888
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    dst[y * dstStride + x * 4 + 0] = src[y * srcStride + x * 3 + 2];
                    dst[y * dstStride + x * 4 + 1] = src[y * srcStride + x * 3 + 1];
                    dst[y * dstStride + x * 4 + 2] = src[y * srcStride + x * 3 + 0];
                    dst[y * dstStride + x * 4 + 3] = 0xFF;
                }
            }
            break;
        }
        case 32: { // RGBA8888
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    dst[y * dstStride + x * 4 + 0] = src[y * srcStride + x * 4 + 2];
                    dst[y * dstStride + x * 4 + 1] = src[y * srcStride + x * 4 + 1];
                    dst[y * dstStride + x * 4 + 2] = src[y * srcStride + x * 4 + 0];
                    dst[y * dstStride + x * 4 + 3] = src[y * srcStride + x * 4 + 3];
                }
            }
            break;
        }
        }
    }

    // ========================================================================
    // Settings dialog (separate window, data-driven from plugin defs)
    // ========================================================================

    // ========================================================================
    // Breakpoints window
    // ========================================================================

    void MainWindow::OnOpenBreakpoints(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;
        if (m_breakpointsWindow) {
            m_breakpointsWindow.Activate();
            return;
        }
        m_breakpointsWindow = Microsoft::UI::Xaml::Window();
        m_breakpointsWindow.Title(L"Breakpoints");
        m_breakpointsWindow.AppWindow().Resize({ 500, 420 });
        m_breakpointsWindow.Closed([this](auto&&, auto&&) { m_breakpointsWindow = nullptr; });

        BuildBreakpointsUI();
        ApplyThemeToWindow(m_breakpointsWindow, m_isDark);
        m_breakpointsWindow.Activate();
    }

    void MainWindow::BuildBreakpointsUI()
    {
        if (!m_breakpointsWindow || !m_instance) return;

        auto consolasFont = Media::FontFamily(L"Consolas");
        auto fgBrush = GetThemeBrush(L"ThemeForeground");
        auto dimBrush = GetThemeBrush(L"ThemeDimFg");
        auto headerBrush = GetThemeBrush(L"ThemeRegHeaderFg");
        auto wpBrush = GetThemeBrush(L"ThemeWarningFg");
        auto bpBrush = GetThemeBrush(L"ThemeBreakpointFg");
        auto accentBrush = GetThemeBrush(L"ThemeAccent");

        auto rootPanel = StackPanel();
        rootPanel.Spacing(2);
        rootPanel.Padding({ 8, 8, 8, 0 });
        rootPanel.Background(GetThemeBrush(L"ThemeWindowBg"));

        // --- Breakpoints section ---
        EmfeBreakpointInfo bps[128];
        int bpCount = m_plugin.emfe_get_breakpoints(m_instance, bps, 128);
        std::sort(bps, bps + bpCount, [](auto& a, auto& b) { return a.address < b.address; });
        if (bpCount > 0) {
            auto hdr = TextBlock();
            hdr.Text(L"Breakpoints");
            hdr.FontSize(12);
            hdr.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
            hdr.Foreground(headerBrush);
            hdr.Margin({ 8, 4, 0, 2 });
            rootPanel.Children().Append(hdr);
        }
        for (int i = 0; i < bpCount; i++) {
            uint32_t addr = static_cast<uint32_t>(bps[i].address);
            bool enabled = bps[i].enabled;
            std::string cond = bps[i].condition ? bps[i].condition : "";

            bool isActiveBreak = (m_lastStopReason == EMFE_STOP_REASON_BREAKPOINT && m_lastStopAddress == addr);
            auto row = Grid();
            row.Margin({ 2, 1, 2, 1 });
            row.Padding({ 4, 2, 4, 2 });
            auto defaultBg = isActiveBreak ? GetThemeBrush(L"ThemeCheckedBg")
                : Media::SolidColorBrush(Windows::UI::Color{ 0x00, 0x00, 0x00, 0x00 });
            row.Background(defaultBg);
            auto hoverBrush = GetThemeBrush(L"ThemeControlBg");
            row.PointerEntered([row, hoverBrush](auto&&, auto&&) { row.Background(hoverBrush); });
            row.PointerExited([row, defaultBg](auto&&, auto&&) { row.Background(defaultBg); });
            row.DoubleTapped([this, addr](auto&&, auto&&) { NavigateDisassemblyTo(addr); });
            row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().GetAt(1).Width({ 1, GridUnitType::Star });
            row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().GetAt(2).Width(GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().GetAt(3).Width(GridLengthHelper::Auto());

            auto cb = CheckBox();
            cb.IsChecked(enabled);
            cb.MinWidth(0);
            cb.Padding({ 0, 0, 0, 0 });
            cb.VerticalAlignment(VerticalAlignment::Center);
            cb.Checked([this, addr](auto&&, auto&&) { m_plugin.emfe_enable_breakpoint(m_instance, addr, true); SyncBreakpointsFromPlugin(); });
            cb.Unchecked([this, addr](auto&&, auto&&) { m_plugin.emfe_enable_breakpoint(m_instance, addr, false); SyncBreakpointsFromPlugin(); });
            Grid::SetColumn(cb, 0);
            row.Children().Append(cb);

            auto textStack = StackPanel();
            textStack.VerticalAlignment(VerticalAlignment::Center);
            textStack.Margin({ 4, 0, 0, 0 });
            auto addrText = TextBlock();
            addrText.Text(winrt::to_hstring(std::format("${:08X}", addr)));
            addrText.FontFamily(consolasFont);
            addrText.FontSize(13);
            addrText.Foreground(fgBrush);
            textStack.Children().Append(addrText);
            if (!cond.empty()) {
                auto condText = TextBlock();
                condText.Text(winrt::to_hstring("  if " + cond));
                condText.FontFamily(consolasFont);
                condText.FontSize(11);
                condText.Foreground(dimBrush);
                textStack.Children().Append(condText);
            }
            Grid::SetColumn(textStack, 1);
            row.Children().Append(textStack);

            auto editBtn = Button();
            editBtn.Content(winrt::box_value(L"Edit Condition..."));
            editBtn.FontSize(11);
            editBtn.Padding({ 6, 2, 6, 2 });
            editBtn.Margin({ 4, 0, 0, 0 });
            editBtn.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(editBtn, 2);
            editBtn.Click([this, addr, cond](auto&&, auto&&) {
                auto dialog = ContentDialog();
                dialog.Title(winrt::box_value(winrt::to_hstring(std::format("Breakpoint at ${:08X}", addr))));
                auto condBox = TextBox();
                condBox.PlaceholderText(L"e.g. D0==5  (empty to clear)");
                condBox.FontFamily(Media::FontFamily(L"Consolas"));
                condBox.Text(winrt::to_hstring(cond));
                dialog.Content(condBox);
                dialog.PrimaryButtonText(L"OK");
                dialog.CloseButtonText(L"Cancel");
                dialog.DefaultButton(ContentDialogButton::Primary);
                dialog.XamlRoot(m_breakpointsWindow.Content().XamlRoot());
                dialog.PrimaryButtonClick([this, addr, condBox](auto&&, auto&&) {
                    auto text = winrt::to_string(condBox.Text());
                    m_plugin.emfe_set_breakpoint_condition(m_instance, addr,
                        text.empty() ? nullptr : text.c_str());
                    RefreshBreakpointsWindow();
                });
                dialog.ShowAsync();
            });
            row.Children().Append(editBtn);

            auto delBtn = Button();
            delBtn.Content(winrt::box_value(L"Delete"));
            delBtn.FontSize(11);
            delBtn.Padding({ 6, 2, 6, 2 });
            delBtn.Margin({ 4, 0, 4, 0 });
            delBtn.Foreground(bpBrush);
            delBtn.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(delBtn, 3);
            delBtn.Click([this, addr](auto&&, auto&&) {
                m_plugin.emfe_remove_breakpoint(m_instance, addr);
                SyncBreakpointsFromPlugin();
                RefreshBreakpointsWindow();
            });
            row.Children().Append(delBtn);

            rootPanel.Children().Append(row);
        }

        // --- Watchpoints section ---
        EmfeWatchpointInfo wps[128];
        int wpCount = m_plugin.emfe_get_watchpoints(m_instance, wps, 128);
        std::sort(wps, wps + wpCount, [](auto& a, auto& b) { return a.address < b.address; });
        if (wpCount > 0) {
            auto hdr = TextBlock();
            hdr.Text(L"Watchpoints");
            hdr.FontSize(12);
            hdr.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
            hdr.Foreground(headerBrush);
            hdr.Margin({ 8, 8, 0, 2 });
            rootPanel.Children().Append(hdr);
        }
        for (int i = 0; i < wpCount; i++) {
            uint32_t addr = static_cast<uint32_t>(wps[i].address);
            auto sizeStr = wps[i].size == EMFE_WP_SIZE_BYTE ? ".B" :
                           wps[i].size == EMFE_WP_SIZE_LONG ? ".L" : ".W";
            auto typeStr = wps[i].type == EMFE_WP_READ ? "[R]" :
                           wps[i].type == EMFE_WP_WRITE ? "[W]" : "[RW]";
            std::string wpCond = wps[i].condition ? wps[i].condition : "";

            bool isActiveWp = (m_lastStopReason == EMFE_STOP_REASON_WATCHPOINT && m_lastStopAddress == addr);
            auto row = Grid();
            row.Margin({ 2, 1, 2, 1 });
            row.Padding({ 4, 2, 4, 2 });
            auto defaultBgW = isActiveWp ? GetThemeBrush(L"ThemeCheckedBg")
                : Media::SolidColorBrush(Windows::UI::Color{ 0x00, 0x00, 0x00, 0x00 });
            row.Background(defaultBgW);
            auto hoverBrushW = GetThemeBrush(L"ThemeControlBg");
            row.PointerEntered([row, hoverBrushW](auto&&, auto&&) { row.Background(hoverBrushW); });
            row.PointerExited([row, defaultBgW](auto&&, auto&&) { row.Background(defaultBgW); });
            row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().GetAt(1).Width({ 1, GridUnitType::Star });
            row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().GetAt(2).Width(GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().GetAt(3).Width(GridLengthHelper::Auto());

            auto cb = CheckBox();
            cb.IsChecked(wps[i].enabled);
            cb.MinWidth(0);
            cb.Padding({ 0, 0, 0, 0 });
            cb.VerticalAlignment(VerticalAlignment::Center);
            cb.Checked([this, addr](auto&&, auto&&) { m_plugin.emfe_enable_watchpoint(m_instance, addr, true); SyncBreakpointsFromPlugin(); });
            cb.Unchecked([this, addr](auto&&, auto&&) { m_plugin.emfe_enable_watchpoint(m_instance, addr, false); SyncBreakpointsFromPlugin(); });
            Grid::SetColumn(cb, 0);
            row.Children().Append(cb);

            auto textStack = StackPanel();
            textStack.VerticalAlignment(VerticalAlignment::Center);
            textStack.Margin({ 4, 0, 0, 0 });
            auto addrText = TextBlock();
            addrText.Text(winrt::to_hstring(std::format("${:08X}{} {}", addr, sizeStr, typeStr)));
            addrText.FontFamily(consolasFont);
            addrText.FontSize(13);
            addrText.Foreground(wpBrush);
            textStack.Children().Append(addrText);
            if (!wpCond.empty()) {
                auto condText = TextBlock();
                condText.Text(winrt::to_hstring("  if " + wpCond));
                condText.FontFamily(consolasFont);
                condText.FontSize(11);
                condText.Foreground(dimBrush);
                textStack.Children().Append(condText);
            }
            Grid::SetColumn(textStack, 1);
            row.Children().Append(textStack);

            auto editBtn = Button();
            editBtn.Content(winrt::box_value(L"Edit..."));
            editBtn.FontSize(11);
            editBtn.Padding({ 6, 2, 6, 2 });
            editBtn.Margin({ 4, 0, 0, 0 });
            editBtn.Foreground(accentBrush);
            editBtn.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(editBtn, 2);
            editBtn.Click([this, addr, wps, i, wpCond](auto&&, auto&&) {
                auto dialog = ContentDialog();
                dialog.Title(winrt::box_value(L"Edit Watchpoint"));
                auto grid = Grid(); grid.RowSpacing(8); grid.ColumnSpacing(8);
                grid.ColumnDefinitions().Append(ColumnDefinition()); grid.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::Auto());
                grid.ColumnDefinitions().Append(ColumnDefinition()); grid.ColumnDefinitions().GetAt(1).Width({ 1, GridUnitType::Star });
                for (int r = 0; r < 4; r++) { grid.RowDefinitions().Append(RowDefinition()); grid.RowDefinitions().GetAt(r).Height(GridLengthHelper::Auto()); }

                auto lbl = [&](int row, const wchar_t* text) { auto t = TextBlock(); t.Text(text); t.VerticalAlignment(VerticalAlignment::Center); Grid::SetRow(t, row); Grid::SetColumn(t, 0); grid.Children().Append(t); };
                lbl(0, L"Address (hex):"); lbl(1, L"Size:"); lbl(2, L"Type:"); lbl(3, L"Condition:");

                auto addrBox = TextBox(); addrBox.FontFamily(Media::FontFamily(L"Consolas"));
                addrBox.Text(winrt::to_hstring(std::format("{:08X}", addr)));
                Grid::SetRow(addrBox, 0); Grid::SetColumn(addrBox, 1); grid.Children().Append(addrBox);

                auto sizeCombo = ComboBox(); sizeCombo.Items().Append(winrt::box_value(L"Byte (.B)")); sizeCombo.Items().Append(winrt::box_value(L"Word (.W)")); sizeCombo.Items().Append(winrt::box_value(L"Long (.L)"));
                sizeCombo.SelectedIndex(wps[i].size == EMFE_WP_SIZE_BYTE ? 0 : wps[i].size == EMFE_WP_SIZE_LONG ? 2 : 1);
                Grid::SetRow(sizeCombo, 1); Grid::SetColumn(sizeCombo, 1); grid.Children().Append(sizeCombo);

                auto typeCombo = ComboBox(); typeCombo.Items().Append(winrt::box_value(L"Read")); typeCombo.Items().Append(winrt::box_value(L"Write")); typeCombo.Items().Append(winrt::box_value(L"Read+Write"));
                typeCombo.SelectedIndex(wps[i].type == EMFE_WP_READ ? 0 : wps[i].type == EMFE_WP_READWRITE ? 2 : 1);
                Grid::SetRow(typeCombo, 2); Grid::SetColumn(typeCombo, 1); grid.Children().Append(typeCombo);

                auto condBox = TextBox(); condBox.FontFamily(Media::FontFamily(L"Consolas"));
                condBox.Text(winrt::to_hstring(wpCond));
                Grid::SetRow(condBox, 3); Grid::SetColumn(condBox, 1); grid.Children().Append(condBox);

                dialog.Content(grid);
                dialog.PrimaryButtonText(L"OK");
                dialog.CloseButtonText(L"Cancel");
                dialog.DefaultButton(ContentDialogButton::Primary);
                dialog.XamlRoot(m_breakpointsWindow.Content().XamlRoot());
                dialog.PrimaryButtonClick([this, addr, addrBox, sizeCombo, typeCombo, condBox](auto&&, auto&&) {
                    m_plugin.emfe_remove_watchpoint(m_instance, addr);
                    auto text = winrt::to_string(addrBox.Text());
                    if (!text.empty() && text[0] == '$') text = text.substr(1);
                    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text = text.substr(2);
                    uint64_t newAddr = std::stoull(text, nullptr, 16);
                    EmfeWatchpointSize sz = sizeCombo.SelectedIndex() == 0 ? EMFE_WP_SIZE_BYTE : sizeCombo.SelectedIndex() == 2 ? EMFE_WP_SIZE_LONG : EMFE_WP_SIZE_WORD;
                    EmfeWatchpointType tp = typeCombo.SelectedIndex() == 0 ? EMFE_WP_READ : typeCombo.SelectedIndex() == 2 ? EMFE_WP_READWRITE : EMFE_WP_WRITE;
                    m_plugin.emfe_add_watchpoint(m_instance, newAddr, sz, tp);
                    auto cond = winrt::to_string(condBox.Text());
                    if (!cond.empty())
                        m_plugin.emfe_set_watchpoint_condition(m_instance, newAddr, cond.c_str());
                    SyncBreakpointsFromPlugin();
                    RefreshBreakpointsWindow();
                });
                dialog.ShowAsync();
            });
            row.Children().Append(editBtn);

            auto delBtn = Button();
            delBtn.Content(winrt::box_value(L"Delete"));
            delBtn.FontSize(11);
            delBtn.Padding({ 6, 2, 6, 2 });
            delBtn.Margin({ 4, 0, 4, 0 });
            delBtn.Foreground(bpBrush);
            delBtn.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(delBtn, 3);
            delBtn.Click([this, addr](auto&&, auto&&) {
                m_plugin.emfe_remove_watchpoint(m_instance, addr);
                SyncBreakpointsFromPlugin();
                RefreshBreakpointsWindow();
            });
            row.Children().Append(delBtn);

            rootPanel.Children().Append(row);
        }

        // --- Toolbar ---
        auto toolbar = StackPanel();
        toolbar.Orientation(Orientation::Horizontal);
        toolbar.Spacing(8);
        toolbar.Padding({ 8, 8, 8, 8 });
        toolbar.Background(GetThemeBrush(L"ThemePanelBg"));

        auto addWpBtn = Button();
        addWpBtn.Content(winrt::box_value(L"Add Watchpoint..."));
        addWpBtn.Padding({ 10, 4, 10, 4 });
        addWpBtn.Click([this](auto&&, auto&&) {
            auto dialog = ContentDialog();
            dialog.Title(winrt::box_value(L"Add Watchpoint"));
            auto grid = Grid(); grid.RowSpacing(8); grid.ColumnSpacing(8);
            grid.ColumnDefinitions().Append(ColumnDefinition()); grid.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::Auto());
            grid.ColumnDefinitions().Append(ColumnDefinition()); grid.ColumnDefinitions().GetAt(1).Width({ 1, GridUnitType::Star });
            for (int r = 0; r < 4; r++) { grid.RowDefinitions().Append(RowDefinition()); grid.RowDefinitions().GetAt(r).Height(GridLengthHelper::Auto()); }

            auto lbl = [&](int row, const wchar_t* text) { auto t = TextBlock(); t.Text(text); t.VerticalAlignment(VerticalAlignment::Center); Grid::SetRow(t, row); Grid::SetColumn(t, 0); grid.Children().Append(t); };
            lbl(0, L"Address (hex):"); lbl(1, L"Size:"); lbl(2, L"Type:"); lbl(3, L"Condition:");

            auto addrBox = TextBox(); addrBox.FontFamily(Media::FontFamily(L"Consolas"));
            Grid::SetRow(addrBox, 0); Grid::SetColumn(addrBox, 1); grid.Children().Append(addrBox);

            auto sizeCombo = ComboBox(); sizeCombo.Items().Append(winrt::box_value(L"Byte (.B)")); sizeCombo.Items().Append(winrt::box_value(L"Word (.W)")); sizeCombo.Items().Append(winrt::box_value(L"Long (.L)")); sizeCombo.SelectedIndex(1);
            Grid::SetRow(sizeCombo, 1); Grid::SetColumn(sizeCombo, 1); grid.Children().Append(sizeCombo);

            auto typeCombo = ComboBox(); typeCombo.Items().Append(winrt::box_value(L"Read")); typeCombo.Items().Append(winrt::box_value(L"Write")); typeCombo.Items().Append(winrt::box_value(L"Read+Write")); typeCombo.SelectedIndex(1);
            Grid::SetRow(typeCombo, 2); Grid::SetColumn(typeCombo, 1); grid.Children().Append(typeCombo);

            auto condBox = TextBox(); condBox.FontFamily(Media::FontFamily(L"Consolas"));
            Grid::SetRow(condBox, 3); Grid::SetColumn(condBox, 1); grid.Children().Append(condBox);
            dialog.Content(grid);
            dialog.PrimaryButtonText(L"Add");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(ContentDialogButton::Primary);
            dialog.XamlRoot(Content().XamlRoot());

            dialog.PrimaryButtonClick([this, addrBox, sizeCombo, typeCombo, condBox](auto&&, auto&&) {
                auto text = winrt::to_string(addrBox.Text());
                // Strip $ or 0x prefix
                if (!text.empty() && text[0] == '$') text = text.substr(1);
                if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text = text.substr(2);
                uint64_t addr = std::stoull(text, nullptr, 16);
                EmfeWatchpointSize sz = sizeCombo.SelectedIndex() == 0 ? EMFE_WP_SIZE_BYTE :
                                        sizeCombo.SelectedIndex() == 2 ? EMFE_WP_SIZE_LONG : EMFE_WP_SIZE_WORD;
                EmfeWatchpointType tp = typeCombo.SelectedIndex() == 0 ? EMFE_WP_READ :
                                        typeCombo.SelectedIndex() == 2 ? EMFE_WP_READWRITE : EMFE_WP_WRITE;
                m_plugin.emfe_add_watchpoint(m_instance, addr, sz, tp);
                auto cond = winrt::to_string(condBox.Text());
                if (!cond.empty())
                    m_plugin.emfe_set_watchpoint_condition(m_instance, addr, cond.c_str());
                SyncBreakpointsFromPlugin();
                RefreshBreakpointsWindow();
            });
            dialog.ShowAsync();
        });
        toolbar.Children().Append(addWpBtn);

        auto clearBtn = Button();
        clearBtn.Content(winrt::box_value(L"Clear All"));
        clearBtn.Padding({ 10, 4, 10, 4 });
        clearBtn.Click([this](auto&&, auto&&) {
            m_plugin.emfe_clear_breakpoints(m_instance);
            m_plugin.emfe_clear_watchpoints(m_instance);
            SyncBreakpointsFromPlugin();
            RefreshBreakpointsWindow();
        });
        toolbar.Children().Append(clearBtn);

        auto closeBtn = Button();
        closeBtn.Content(winrt::box_value(L"Close"));
        closeBtn.Padding({ 10, 4, 10, 4 });
        closeBtn.Click([this](auto&&, auto&&) { if (m_breakpointsWindow) m_breakpointsWindow.Close(); });
        toolbar.Children().Append(closeBtn);

        auto outerPanel = Grid();
        outerPanel.RowDefinitions().Append(RowDefinition());
        outerPanel.RowDefinitions().GetAt(0).Height({ 1, GridUnitType::Star });
        outerPanel.RowDefinitions().Append(RowDefinition());
        outerPanel.RowDefinitions().GetAt(1).Height(GridLengthHelper::Auto());

        auto scrollViewer = ScrollViewer();
        scrollViewer.Content(rootPanel);
        scrollViewer.VerticalScrollBarVisibility(Controls::ScrollBarVisibility::Auto);
        Grid::SetRow(scrollViewer, 0);
        outerPanel.Children().Append(scrollViewer);
        Grid::SetRow(toolbar, 1);
        outerPanel.Children().Append(toolbar);

        m_breakpointsWindow.Content(outerPanel);
    }

    void MainWindow::RefreshBreakpointsWindow()
    {
        if (!m_breakpointsWindow) return;
        BuildBreakpointsUI();
    }

    void MainWindow::UpdateBoardTypeText(winrt::hstring cpuName)
    {
        if (!m_instance) return;
        auto boardType = m_plugin.emfe_get_setting(m_instance, "BoardType");

        // Pull board_info once: we need CpuName (if caller didn't pass one) and
        // BoardName (fallback when the plugin has no BoardType setting).
        EmfeBoardInfo info{};
        bool haveInfo = (m_plugin.emfe_get_board_info(&info) == EMFE_OK);

        std::string board;
        if (boardType && boardType[0] != '\0') {
            board = boardType;
        } else if (haveInfo && info.board_name && info.board_name[0] != '\0') {
            board = info.board_name;
        } else {
            board = "Generic";
        }

        if (cpuName.empty()) {
            if (haveInfo && info.cpu_name) cpuName = winrt::to_hstring(info.cpu_name);
            else                           cpuName = L"MC68030";
        }

        // Prepend the plugin DLL stem (e.g. "emfe_plugin_z8000") when known.
        std::wstring text;
        if (!m_loadedPluginStem.empty()) {
            text = std::format(L"[{}] {} / {}",
                m_loadedPluginStem, winrt::to_hstring(board), std::wstring_view(cpuName));
        } else {
            text = std::format(L"{} / {}",
                winrt::to_hstring(board), std::wstring_view(cpuName));
        }
        BoardTypeText().Text(text);
    }

    void MainWindow::NavigateDisassemblyTo(uint32_t address)
    {
        if (!m_instance) return;
        try {
            // Check if address is in current disassembly view
            for (size_t i = 0; i < m_disasmAddresses.size(); i++) {
                if (m_disasmAddresses[i] == address) {
                    DisasmList().SelectedIndex(static_cast<int32_t>(i));
                    DisasmList().ScrollIntoView(DisasmList().Items().GetAt(static_cast<uint32_t>(i)));
                    return;
                }
            }
            // Address not in view: re-center disassembly around target
            uint64_t progStart = 0, progEnd = 0;
            if (m_plugin.emfe_get_program_range)
                m_plugin.emfe_get_program_range(m_instance, &progStart, &progEnd);
            uint32_t startAddr = address >= 0x40 ? address - 0x40 : 0;
            if (progStart > 0 && startAddr < static_cast<uint32_t>(progStart))
                startAddr = static_cast<uint32_t>(progStart);

            EmfeDisasmLine lines[64]{};
            int count = m_plugin.emfe_disassemble_range(m_instance, startAddr, startAddr + 0x200, lines, 64);

            // Re-use UpdateDisassembly to rebuild the view properly
            UpdateDisassembly();

            // Scroll to target address
            for (size_t i = 0; i < m_disasmAddresses.size(); i++) {
                if (m_disasmAddresses[i] == address) {
                    DisasmList().SelectedIndex(static_cast<int32_t>(i));
                    if (i < DisasmList().Items().Size())
                        DisasmList().ScrollIntoView(DisasmList().Items().GetAt(static_cast<uint32_t>(i)));
                    break;
                }
            }
        } catch (...) {
            // Prevent crash from propagating
        }
    }

    void MainWindow::SyncBreakpointsFromPlugin()
    {
        m_breakpointAddresses.clear();
        EmfeBreakpointInfo bps[128];
        int n = m_plugin.emfe_get_breakpoints(m_instance, bps, 128);
        for (int i = 0; i < n; i++)
            m_breakpointAddresses[static_cast<uint32_t>(bps[i].address)] = bps[i].enabled;
        UpdateDisassembly();
        UpdateMemoryDump(m_memoryAddress);
    }

    // ========================================================================
    // Call Stack window
    // ========================================================================

    void MainWindow::OnOpenCallStack(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;
        if (m_callStackWindow) { m_callStackWindow.Activate(); return; }
        m_callStackWindow = Microsoft::UI::Xaml::Window();
        m_callStackWindow.Title(L"Call Stack");
        m_callStackWindow.AppWindow().Resize({ 620, 380 });
        m_callStackWindow.Closed([this](auto&&, auto&&) { m_callStackWindow = nullptr; });
        BuildCallStackUI();
        ApplyThemeToWindow(m_callStackWindow, m_isDark);
        m_callStackWindow.Activate();
    }

    void MainWindow::BuildCallStackUI()
    {
        if (!m_callStackWindow || !m_instance) return;

        // Update title with current mode
        auto modeVal = m_plugin.emfe_get_setting(m_instance, "CallStackMode");
        std::string mode = modeVal ? modeVal : "ShadowStack";
        m_callStackWindow.Title(winrt::to_hstring(std::format("Call Stack ({})", mode)));

        auto consolasFont = Media::FontFamily(L"Consolas");
        auto fgBrush = GetThemeBrush(L"ThemeForeground");
        auto dimBrush = GetThemeBrush(L"ThemeDimFg");
        auto headerBrush = GetThemeBrush(L"ThemeRegHeaderFg");

        EmfeCallStackEntry entries[64];
        int count = m_plugin.emfe_get_call_stack(m_instance, entries, 64);

        auto rootPanel = StackPanel();
        rootPanel.Padding({ 8, 8, 8, 0 });
        rootPanel.Background(GetThemeBrush(L"ThemeWindowBg"));

        // Column header
        auto headerRow = Grid();
        headerRow.Margin({ 4, 2, 4, 4 });
        double colWidths[] = { 30, 70, 80, 80, 80, 80, 100 };
        const wchar_t* colHeaders[] = { L"#", L"Kind", L"Call PC", L"Target PC", L"Return PC", L"Frame", L"Label" };
        for (int c = 0; c < 7; c++) {
            headerRow.ColumnDefinitions().Append(ColumnDefinition());
            headerRow.ColumnDefinitions().GetAt(c).Width({ colWidths[c], GridUnitType::Pixel });
            auto tb = TextBlock();
            tb.Text(colHeaders[c]);
            tb.FontSize(12);
            tb.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
            tb.Foreground(headerBrush);
            Grid::SetColumn(tb, c);
            headerRow.Children().Append(tb);
        }
        rootPanel.Children().Append(headerRow);

        // Entries
        for (int i = 0; i < count; i++) {
            auto& e = entries[i];
            uint32_t callPc = static_cast<uint32_t>(e.call_pc);
            uint32_t targetPc = static_cast<uint32_t>(e.target_pc);
            uint32_t returnPc = static_cast<uint32_t>(e.return_pc);
            uint32_t frame = static_cast<uint32_t>(e.frame_pointer);
            std::string label = e.label ? e.label : "";
            const char* kindStr = e.kind == EMFE_CALL_KIND_CALL ? "CALL" :
                                  e.kind == EMFE_CALL_KIND_EXCEPTION ? "EXCEPTION" : "INTERRUPT";

            auto row = Grid();
            row.Margin({ 4, 1, 4, 1 });
            row.Padding({ 2, 2, 2, 2 });
            row.Background(Media::SolidColorBrush(Windows::UI::Color{ 0x00, 0x00, 0x00, 0x00 }));
            auto hoverBrush = GetThemeBrush(L"ThemeControlBg");
            auto transpBrush = Media::SolidColorBrush(Windows::UI::Color{ 0x00, 0x00, 0x00, 0x00 });
            row.PointerEntered([row, hoverBrush](auto&&, auto&&) { row.Background(hoverBrush); });
            row.PointerExited([row, transpBrush](auto&&, auto&&) { row.Background(transpBrush); });
            row.DoubleTapped([this, callPc](auto&&, auto&&) { NavigateDisassemblyTo(callPc); });

            for (int c = 0; c < 7; c++) {
                row.ColumnDefinitions().Append(ColumnDefinition());
                row.ColumnDefinitions().GetAt(c).Width({ colWidths[c], GridUnitType::Pixel });
            }

            std::wstring vals[] = {
                std::format(L"{}", i),
                std::wstring(winrt::to_hstring(kindStr)),
                std::format(L"${:08X}", callPc),
                std::format(L"${:08X}", targetPc),
                std::format(L"${:08X}", returnPc),
                std::format(L"${:08X}", frame),
                std::wstring(winrt::to_hstring(label))
            };

            for (int c = 0; c < 7; c++) {
                auto tb = TextBlock();
                tb.Text(vals[c]);
                tb.FontFamily(consolasFont);
                tb.FontSize(13);
                tb.Foreground(fgBrush);
                tb.VerticalAlignment(VerticalAlignment::Center);
                Grid::SetColumn(tb, c);
                row.Children().Append(tb);
            }

            rootPanel.Children().Append(row);
        }

        // Bottom bar
        auto toolbar = StackPanel();
        toolbar.Orientation(Orientation::Horizontal);
        toolbar.Spacing(8);
        toolbar.Padding({ 8, 8, 8, 8 });
        toolbar.Background(GetThemeBrush(L"ThemePanelBg"));

        auto statusText = TextBlock();
        statusText.Text(std::format(L"{} frame(s)", count));
        statusText.Foreground(dimBrush);
        statusText.VerticalAlignment(VerticalAlignment::Center);
        toolbar.Children().Append(statusText);

        auto refreshBtn = Button();
        refreshBtn.Content(winrt::box_value(L"Refresh"));
        refreshBtn.Padding({ 10, 4, 10, 4 });
        refreshBtn.Click([this](auto&&, auto&&) { RefreshCallStackWindow(); });
        toolbar.Children().Append(refreshBtn);

        auto closeBtn = Button();
        closeBtn.Content(winrt::box_value(L"Close"));
        closeBtn.Padding({ 10, 4, 10, 4 });
        closeBtn.Click([this](auto&&, auto&&) { if (m_callStackWindow) m_callStackWindow.Close(); });
        toolbar.Children().Append(closeBtn);

        auto outerPanel = Grid();
        outerPanel.RowDefinitions().Append(RowDefinition());
        outerPanel.RowDefinitions().GetAt(0).Height({ 1, GridUnitType::Star });
        outerPanel.RowDefinitions().Append(RowDefinition());
        outerPanel.RowDefinitions().GetAt(1).Height(GridLengthHelper::Auto());

        auto scrollViewer = ScrollViewer();
        scrollViewer.Content(rootPanel);
        scrollViewer.VerticalScrollBarVisibility(Controls::ScrollBarVisibility::Auto);
        Grid::SetRow(scrollViewer, 0);
        outerPanel.Children().Append(scrollViewer);
        Grid::SetRow(toolbar, 1);
        outerPanel.Children().Append(toolbar);

        m_callStackWindow.Content(outerPanel);
    }

    void MainWindow::RefreshCallStackWindow()
    {
        if (!m_callStackWindow) return;
        BuildCallStackUI();
    }

    // ========================================================================
    // Settings dialog
    // ========================================================================

    // Lazily snapshot the plugin's current list state into m_pendingLists.
    // All subsequent dialog edits act on the snapshot; the plugin is only
    // updated when OK is clicked (via ApplyStagedListsToPlugin).
    void MainWindow::EnsureListStaged(std::string const& listKey)
    {
        if (m_pendingLists.count(listKey)) return;

        ListEdit edit;
        const EmfeListItemDef* defs = nullptr;
        int nFields = m_plugin.emfe_get_list_item_defs(
            m_instance, listKey.c_str(), &defs);
        int nItems = m_plugin.emfe_get_list_item_count(
            m_instance, listKey.c_str());
        for (int i = 0; i < nItems; i++) {
            ListItemEdit item;
            for (int f = 0; f < nFields; f++) {
                if (!defs[f].key) continue;
                std::string fkey = defs[f].key;
                auto raw = m_plugin.emfe_get_list_item_field(
                    m_instance, listKey.c_str(), i, fkey.c_str());
                item.fields[fkey] = raw ? raw : "";
            }
            edit.items.push_back(std::move(item));
        }
        m_pendingLists.emplace(listKey, std::move(edit));
    }

    // Push the staged list state into the plugin by wiping and rebuilding.
    // Called from the Settings OK handler just before emfe_apply_settings
    // so the list changes commit atomically with other staged settings.
    void MainWindow::ApplyStagedListsToPlugin()
    {
        for (auto& [listKey, edit] : m_pendingLists) {
            // Wipe existing plugin list (from tail — stable indices).
            int existing = m_plugin.emfe_get_list_item_count(
                m_instance, listKey.c_str());
            for (int i = existing - 1; i >= 0; i--) {
                m_plugin.emfe_remove_list_item(m_instance, listKey.c_str(), i);
            }
            // Rebuild from the edit model.
            for (auto& item : edit.items) {
                int idx = m_plugin.emfe_add_list_item(m_instance, listKey.c_str());
                if (idx < 0) continue;
                for (auto& [field, val] : item.fields) {
                    m_plugin.emfe_set_list_item_field(
                        m_instance, listKey.c_str(), idx,
                        field.c_str(), val.c_str());
                }
            }
        }
    }

    std::unordered_set<int> MainWindow::GetUsedScsiIds(std::string const& listKey, int excludeIndex)
    {
        std::unordered_set<int> used;

        // CD-ROM ID — check staged value if the user has touched it this
        // session, otherwise fall back to the committed plugin value.
        std::string cdromVal;
        for (auto& sc : m_settingControls) {
            if (sc.key == "Mvme147ScsiCdromId") {
                if (auto combo = sc.control.try_as<ComboBox>()) {
                    if (auto sel = combo.SelectedItem())
                        cdromVal = winrt::to_string(
                            winrt::unbox_value<winrt::hstring>(sel));
                }
                break;
            }
        }
        if (cdromVal.empty()) {
            if (auto cdromRaw = m_plugin.emfe_get_setting(
                    m_instance, "Mvme147ScsiCdromId"))
                cdromVal = cdromRaw;
        }
        try {
            int cid = std::stoi(cdromVal);
            if (cid >= 0 && cid <= 7) used.insert(cid);
        } catch (...) {}

        // Other disk entries — read from the edit model (NOT the plugin).
        auto it = m_pendingLists.find(listKey);
        if (it != m_pendingLists.end()) {
            for (int i = 0; i < static_cast<int>(it->second.items.size()); i++) {
                if (i == excludeIndex) continue;
                auto fit = it->second.items[i].fields.find("ScsiId");
                if (fit == it->second.items[i].fields.end()) continue;
                try {
                    int id = std::stoi(fit->second);
                    if (id >= 0 && id <= 7) used.insert(id);
                } catch (...) {}
            }
        }
        return used;
    }

    // Render a dynamic list setting (currently the only user is the MVME147
    // SCSI Disks list, with sub-fields "ScsiId" and "Path"). Works against
    // the staged edit model (m_pendingLists) rather than the live plugin
    // state — changes apply only on OK via ApplyStagedListsToPlugin.
    void MainWindow::BuildListControl(Controls::StackPanel const& parent,
                                      std::string const& listKey,
                                      std::string const& label)
    {
        EnsureListStaged(listKey);
        auto& edit = m_pendingLists[listKey];

        auto header = TextBlock();
        header.Text(winrt::to_hstring(label));
        header.FontSize(13);
        header.FontWeight(Windows::UI::Text::FontWeight{600});  // SemiBold
        header.Margin({ 0, 8, 0, 4 });
        header.Foreground(GetThemeBrush(L"ThemeRegHeaderFg"));
        parent.Children().Append(header);

        int itemCount = static_cast<int>(edit.items.size());

        for (int idx = 0; idx < itemCount; idx++) {
            auto itemRow = StackPanel();
            itemRow.Orientation(Orientation::Horizontal);
            itemRow.Margin({ 0, 2, 0, 2 });
            itemRow.Spacing(4);

            int capturedIdx = idx;

            // SCSI ID combo (0..7, filtered to avoid duplicates with the
            // CD-ROM and other disks as represented in the edit model).
            int currentId = 0;
            {
                auto it = edit.items[idx].fields.find("ScsiId");
                if (it != edit.items[idx].fields.end()) {
                    try { currentId = std::stoi(it->second); } catch (...) {}
                }
            }
            auto used = GetUsedScsiIds(listKey, idx);

            auto idLabel = TextBlock();
            idLabel.Text(L"ID:");
            idLabel.Width(25);
            idLabel.VerticalAlignment(VerticalAlignment::Center);
            idLabel.FontSize(12);
            itemRow.Children().Append(idLabel);

            auto idCombo = ComboBox();
            idCombo.Width(80);
            idCombo.MinWidth(80);
            idCombo.FontSize(13);
            int selIdx = -1, itemsAdded = 0;
            for (int id = 0; id <= 7; id++) {
                if (used.count(id) == 0 || id == currentId) {
                    idCombo.Items().Append(
                        winrt::box_value(winrt::to_hstring(std::to_string(id))));
                    if (id == currentId) selIdx = itemsAdded;
                    itemsAdded++;
                }
            }
            if (selIdx >= 0) idCombo.SelectedIndex(selIdx);
            std::string listKeyCopy = listKey;
            idCombo.SelectionChanged(
                [this, listKeyCopy, capturedIdx, idCombo](auto&&, auto&&) {
                    if (m_settingsRebuilding) return;
                    if (auto sel = idCombo.SelectedItem()) {
                        std::string sv = winrt::to_string(
                            winrt::unbox_value<winrt::hstring>(sel));
                        auto it = m_pendingLists.find(listKeyCopy);
                        if (it != m_pendingLists.end() &&
                            capturedIdx < static_cast<int>(it->second.items.size())) {
                            it->second.items[capturedIdx].fields["ScsiId"] = sv;
                        }
                        SaveSettingsToStaging();  // other setting controls
                        BuildSettingsContent();   // refresh to re-filter combos
                    }
                });
            itemRow.Children().Append(idCombo);

            // Path textbox + browse button
            std::string currentPath;
            {
                auto it = edit.items[idx].fields.find("Path");
                if (it != edit.items[idx].fields.end()) currentPath = it->second;
            }

            auto pathBox = TextBox();
            pathBox.Text(winrt::to_hstring(currentPath));
            pathBox.Width(220);
            pathBox.FontSize(13);
            pathBox.Margin({ 8, 0, 0, 0 });
            pathBox.LostFocus([this, listKeyCopy, capturedIdx, pathBox](auto&&, auto&&) {
                auto s = winrt::to_string(pathBox.Text());
                auto it = m_pendingLists.find(listKeyCopy);
                if (it != m_pendingLists.end() &&
                    capturedIdx < static_cast<int>(it->second.items.size())) {
                    it->second.items[capturedIdx].fields["Path"] = s;
                }
            });
            itemRow.Children().Append(pathBox);

            auto browseBtn = Button();
            browseBtn.Content(winrt::box_value(L"..."));
            browseBtn.FontSize(11);
            browseBtn.Padding({ 6, 1, 6, 1 });
            browseBtn.Margin({ 4, 0, 0, 0 });
            browseBtn.Click([this, listKeyCopy, capturedIdx, pathBox](auto&&, auto&&) {
                OPENFILENAMEW ofn{};
                wchar_t filePath[MAX_PATH]{};
                auto cur = winrt::to_string(pathBox.Text());
                if (!cur.empty()) {
                    wcsncpy_s(filePath,
                              std::filesystem::path(cur).wstring().c_str(),
                              MAX_PATH - 1);
                }
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = GetWindowHandle(m_settingsWindow);
                ofn.lpstrFilter = L"Disk Images\0*.img;*.raw;*.iso\0All Files\0*.*\0\0";
                ofn.lpstrFile = filePath;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    pathBox.Text(winrt::hstring(filePath));
                    auto s = winrt::to_string(std::wstring(filePath));
                    auto it = m_pendingLists.find(listKeyCopy);
                    if (it != m_pendingLists.end() &&
                        capturedIdx < static_cast<int>(it->second.items.size())) {
                        it->second.items[capturedIdx].fields["Path"] = s;
                    }
                }
            });
            itemRow.Children().Append(browseBtn);

            // Remove button
            auto removeBtn = Button();
            removeBtn.Content(winrt::box_value(L"\u00D7"));   // ×
            removeBtn.FontSize(13);
            removeBtn.Padding({ 6, 1, 6, 1 });
            removeBtn.Margin({ 8, 0, 0, 0 });
            removeBtn.Click([this, listKeyCopy, capturedIdx](auto&&, auto&&) {
                auto it = m_pendingLists.find(listKeyCopy);
                if (it != m_pendingLists.end() &&
                    capturedIdx < static_cast<int>(it->second.items.size())) {
                    it->second.items.erase(it->second.items.begin() + capturedIdx);
                }
                SaveSettingsToStaging();
                BuildSettingsContent();
            });
            itemRow.Children().Append(removeBtn);

            parent.Children().Append(itemRow);
        }

        // Add button
        auto addBtn = Button();
        addBtn.Content(winrt::box_value(L"+ Add Disk"));
        addBtn.FontSize(11);
        addBtn.Padding({ 8, 2, 8, 2 });
        addBtn.Margin({ 0, 4, 0, 0 });
        std::string listKeyAdd = listKey;
        addBtn.Click([this, listKeyAdd](auto&&, auto&&) {
            SaveSettingsToStaging();
            auto it = m_pendingLists.find(listKeyAdd);
            if (it == m_pendingLists.end()) return;

            // Pick the first SCSI ID that is free, excluding -1 to match
            // the "excludeIndex" contract.
            auto used = GetUsedScsiIds(listKeyAdd, -1);
            int freeId = 0;
            while (used.count(freeId) && freeId <= 7) freeId++;

            ListItemEdit item;
            if (freeId <= 7) item.fields["ScsiId"] = std::to_string(freeId);
            item.fields["Path"] = "";
            it->second.items.push_back(std::move(item));

            BuildSettingsContent();
        });
        parent.Children().Append(addBtn);
    }

    void MainWindow::SaveSettingsToStaging()
    {
        for (auto& sc : m_settingControls) {
            std::string val;
            switch (sc.type) {
            case EMFE_SETTING_BOOL: {
                auto toggle = sc.control.as<ToggleSwitch>();
                val = toggle.IsOn() ? "true" : "false";
                break;
            }
            case EMFE_SETTING_COMBO: {
                auto combo = sc.control.as<ComboBox>();
                if (combo.SelectedItem())
                    val = winrt::to_string(winrt::unbox_value<winrt::hstring>(combo.SelectedItem()));
                break;
            }
            default: {
                auto box = sc.control.as<TextBox>();
                val = winrt::to_string(box.Text());
                break;
            }
            }
            if (!val.empty())
                m_plugin.emfe_set_setting(m_instance, sc.key.c_str(), val.c_str());
        }
    }

    bool MainWindow::IsSettingVisible(const EmfeSettingDef* defs, int32_t count, int32_t idx)
    {
        if (!defs[idx].depends_on || !defs[idx].depends_value) return true;
        auto depVal = m_plugin.emfe_get_setting(m_instance, defs[idx].depends_on);
        if (!depVal || std::string(depVal) != std::string(defs[idx].depends_value)) return false;
        // Chain check: is the dependency itself visible?
        for (int32_t j = 0; j < count; j++) {
            if (defs[j].key && std::string(defs[j].key) == std::string(defs[idx].depends_on))
                return IsSettingVisible(defs, count, j);
        }
        return true;
    }

    void MainWindow::BuildSettingsContent()
    {
        if (!m_settingsWindow || !m_instance) return;

        const EmfeSettingDef* defs = nullptr;
        int32_t count = m_plugin.emfe_get_setting_defs(m_instance, &defs);
        if (count <= 0) return;

        std::vector<std::string> groups;
        for (int32_t i = 0; i < count; i++) {
            std::string g = defs[i].group ? defs[i].group : "";
            if (std::find(groups.begin(), groups.end(), g) == groups.end())
                groups.push_back(g);
        }

        // Remember selected tab
        std::string selectedTabHeader;
        auto oldContent = m_settingsWindow.Content();
        if (oldContent) {
            if (auto grid = oldContent.try_as<Grid>()) {
                for (uint32_t ci = 0; ci < grid.Children().Size(); ci++) {
                    if (auto tv = grid.Children().GetAt(ci).try_as<Controls::TabView>()) {
                        if (auto sel = tv.SelectedItem().try_as<Controls::TabViewItem>()) {
                            if (auto hdr = sel.Header().try_as<winrt::hstring>())
                                selectedTabHeader = winrt::to_string(hdr.value());
                        }
                        break;
                    }
                }
            }
        }

        m_settingControls.clear();
        auto consolasFont = Media::FontFamily(L"Consolas");

        auto tabView = Controls::TabView();
        tabView.IsAddTabButtonVisible(false);
        tabView.CanDragTabs(false);
        tabView.CanReorderTabs(false);
        tabView.TabWidthMode(Controls::TabViewWidthMode::SizeToContent);

        for (auto& group : groups) {
            auto panel = StackPanel();
            panel.Spacing(4);
            panel.Padding({ 12, 8, 12, 8 });
            bool hasVisibleItems = false;

            for (int32_t i = 0; i < count; i++) {
                if (std::string(defs[i].group ? defs[i].group : "") != group) continue;

                if (!IsSettingVisible(defs, count, i)) continue;

                std::string key = defs[i].key;
                std::string currentVal = m_plugin.emfe_get_setting(m_instance, key.c_str());

                auto row = StackPanel();
                row.Orientation(Orientation::Horizontal);
                row.Spacing(8);
                row.Margin({ 0, 4, 0, 4 });

                auto label = TextBlock();
                label.Text(winrt::to_hstring(defs[i].label ? defs[i].label : key));
                label.Width(180);
                label.VerticalAlignment(VerticalAlignment::Center);
                label.FontSize(13);
                row.Children().Append(label);

                // Pending (deferred) indicator: REQUIRES_RESET setting whose staged
                // value differs from the currently-applied value.
                bool isPending = false;
                if ((defs[i].flags & EMFE_SETTING_FLAG_REQUIRES_RESET) && m_plugin.emfe_get_applied_setting) {
                    std::string appliedVal = m_plugin.emfe_get_applied_setting(m_instance, key.c_str());
                    if (appliedVal != currentVal) isPending = true;
                }
                if (isPending) {
                    auto pendingMark = TextBlock();
                    pendingMark.Text(L"*");
                    pendingMark.FontSize(14);
                    pendingMark.FontWeight(Windows::UI::Text::FontWeight{700});
                    pendingMark.VerticalAlignment(VerticalAlignment::Center);
                    pendingMark.Margin({-6, 0, 4, 0});
                    pendingMark.Foreground(Media::SolidColorBrush(
                        Windows::UI::Color{ 0xFF, 0xFF, 0x99, 0x00 }));
                    Controls::ToolTipService::SetToolTip(pendingMark, winrt::box_value(winrt::hstring(
                        L"This change is staged but not yet applied. It will take effect on the next full reset or when emfe restarts.")));
                    row.Children().Append(pendingMark);
                }

                switch (defs[i].type) {
                case EMFE_SETTING_BOOL: {
                    auto toggle = ToggleSwitch();
                    toggle.IsOn(currentVal == "true" || currentVal == "1");
                    row.Children().Append(toggle);
                    m_settingControls.push_back({ key, defs[i].type, toggle });
                    break;
                }
                case EMFE_SETTING_COMBO: {
                    auto combo = ComboBox();
                    combo.Width(180);
                    combo.FontSize(13);
                    if (defs[i].constraints) {
                        std::string cs = defs[i].constraints;
                        size_t pos = 0;
                        int selIdx = -1, idx = 0;
                        while (pos < cs.size()) {
                            auto sep = cs.find('|', pos);
                            auto item = cs.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
                            combo.Items().Append(winrt::box_value(winrt::to_hstring(item)));
                            if (item == currentVal) selIdx = idx;
                            pos = sep == std::string::npos ? cs.size() : sep + 1;
                            idx++;
                        }
                        if (selIdx >= 0) combo.SelectedIndex(selIdx);
                    }
                    combo.SelectionChanged([this](auto&&, auto&&) {
                        if (m_settingsRebuilding) return;
                        SaveSettingsToStaging();
                        BuildSettingsContent();
                    });
                    row.Children().Append(combo);
                    m_settingControls.push_back({ key, defs[i].type, combo });
                    break;
                }
                case EMFE_SETTING_INT: {
                    auto box = TextBox();
                    box.Text(winrt::to_hstring(currentVal));
                    box.Width(100);
                    box.FontFamily(consolasFont);
                    box.FontSize(13);
                    row.Children().Append(box);
                    if (defs[i].constraints) {
                        auto hint = TextBlock();
                        hint.Text(winrt::to_hstring(std::string("(") + defs[i].constraints + ")"));
                        hint.FontSize(11);
                        hint.Foreground(GetThemeBrush(L"ThemeDimFg"));
                        hint.VerticalAlignment(VerticalAlignment::Center);
                        row.Children().Append(hint);
                    }
                    m_settingControls.push_back({ key, defs[i].type, box });
                    break;
                }
                case EMFE_SETTING_LIST: {
                    std::string listLabel = defs[i].label ? defs[i].label : key;
                    BuildListControl(panel, key, listLabel);
                    hasVisibleItems = true;
                    continue;  // Skip the standard row append — BuildListControl
                               // writes directly to `panel`.
                }
                case EMFE_SETTING_FILE: {
                    auto box = TextBox();
                    box.Text(winrt::to_hstring(currentVal));
                    box.Width(260);
                    box.FontSize(13);
                    row.Children().Append(box);
                    auto browseBtn = Button();
                    browseBtn.Content(winrt::box_value(L"..."));
                    browseBtn.FontSize(11);
                    browseBtn.Padding({ 6, 1, 6, 1 });
                    browseBtn.Margin({ 4, 0, 0, 0 });
                    browseBtn.Click([this, box](auto&&, auto&&) {
                        OPENFILENAMEW ofn{};
                        wchar_t filePath[MAX_PATH]{};
                        auto curText = winrt::to_string(box.Text());
                        if (!curText.empty()) {
                            auto dir = std::filesystem::path(curText).parent_path().wstring();
                            wcsncpy_s(filePath, std::filesystem::path(curText).wstring().c_str(), MAX_PATH - 1);
                        }
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = GetWindowHandle(m_settingsWindow);
                        ofn.lpstrFilter = L"All Files\0*.*\0\0";
                        ofn.lpstrFile = filePath;
                        ofn.nMaxFile = MAX_PATH;
                        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                        if (GetOpenFileNameW(&ofn))
                            box.Text(winrt::hstring(filePath));
                    });
                    row.Children().Append(browseBtn);
                    m_settingControls.push_back({ key, defs[i].type, box });
                    break;
                }
                case EMFE_SETTING_STRING:
                default: {
                    auto box = TextBox();
                    box.Text(winrt::to_hstring(currentVal));
                    box.Width(300);
                    box.FontSize(13);
                    row.Children().Append(box);
                    m_settingControls.push_back({ key, defs[i].type, box });
                    break;
                }
                }

                panel.Children().Append(row);
                hasVisibleItems = true;
            }

            auto tabItem = Controls::TabViewItem();
            tabItem.Header(winrt::box_value(winrt::to_hstring(group)));
            tabItem.IsClosable(false);
            if (hasVisibleItems) {
                auto scrollViewer = ScrollViewer();
                scrollViewer.Content(panel);
                scrollViewer.VerticalScrollBarVisibility(Controls::ScrollBarVisibility::Auto);
                tabItem.Content(scrollViewer);
            } else {
                auto emptyText = TextBlock();
                emptyText.Text(L"No settings available for this tab.");
                emptyText.FontSize(12);
                emptyText.Margin({12, 16, 0, 0});
                emptyText.Foreground(GetThemeBrush(L"ThemeDimFg"));
                tabItem.Content(emptyText);
            }
            tabView.TabItems().Append(tabItem);
        }

        // Block selection of disabled tabs
        tabView.SelectionChanged([tabView](auto&&, auto&&) {
            if (auto sel = tabView.SelectedItem().try_as<Controls::TabViewItem>()) {
                if (!sel.IsEnabled()) {
                    // Find first enabled tab
                    for (uint32_t j = 0; j < tabView.TabItems().Size(); j++) {
                        if (auto ti = tabView.TabItems().GetAt(j).try_as<Controls::TabViewItem>()) {
                            if (ti.IsEnabled()) { tabView.SelectedIndex(static_cast<int32_t>(j)); return; }
                        }
                    }
                }
            }
        });

        // Restore selected tab
        if (!selectedTabHeader.empty()) {
            for (uint32_t i = 0; i < tabView.TabItems().Size(); i++) {
                if (auto ti = tabView.TabItems().GetAt(i).try_as<Controls::TabViewItem>()) {
                    if (auto hdr = ti.Header().try_as<winrt::hstring>()) {
                        if (winrt::to_string(hdr.value()) == selectedTabHeader) {
                            tabView.SelectedIndex(static_cast<int32_t>(i));
                            break;
                        }
                    }
                }
            }
        } else if (tabView.TabItems().Size() > 0) {
            tabView.SelectedIndex(0);
        }

        auto outerGrid = Grid();
        outerGrid.Background(GetThemeBrush(L"ThemeWindowBg"));
        outerGrid.RowDefinitions().Append(RowDefinition());
        outerGrid.RowDefinitions().GetAt(0).Height({ 1, GridUnitType::Star });
        outerGrid.RowDefinitions().Append(RowDefinition());
        outerGrid.RowDefinitions().GetAt(1).Height(GridLengthHelper::Auto());

        Grid::SetRow(tabView, 0);
        outerGrid.Children().Append(tabView);

        auto btnPanel = StackPanel();
        btnPanel.Orientation(Orientation::Horizontal);
        btnPanel.HorizontalAlignment(HorizontalAlignment::Right);
        btnPanel.Spacing(8);
        btnPanel.Padding({ 16, 8, 16, 16 });

        auto btnOK = Button();
        btnOK.Content(winrt::box_value(L"OK"));
        btnOK.Padding({ 20, 6, 20, 6 });
        btnOK.Click([this](auto&&, auto&&) {
            SaveSettingsToStaging();
            ApplyStagedListsToPlugin();
            m_plugin.emfe_apply_settings(m_instance);
            m_plugin.emfe_save_settings(m_instance);
            m_pendingLists.clear();
            if (m_settingsWindow) m_settingsWindow.Close();
            UpdateRegisters();
            UpdateDisassembly();
            UpdateMemoryDump(m_memoryAddress);
            UpdateToolbarState();
            const char* themeVal = m_plugin.emfe_get_setting(m_instance, "Theme");
            ApplyTheme(themeVal ? themeVal : "Dark");
            UpdateBoardTypeText();
            RefreshCallStackWindow();
            SetStatus(L"Settings applied");
        });

        auto btnCancel = Button();
        btnCancel.Content(winrt::box_value(L"Cancel"));
        btnCancel.Padding({ 20, 6, 20, 6 });
        btnCancel.Click([this](auto&&, auto&&) {
            m_pendingLists.clear();  // discard list edits
            if (m_settingsWindow) m_settingsWindow.Close();
        });

        btnPanel.Children().Append(btnOK);
        btnPanel.Children().Append(btnCancel);
        Grid::SetRow(btnPanel, 1);
        outerGrid.Children().Append(btnPanel);

        m_settingsRebuilding = true;
        m_settingsWindow.Content(outerGrid);
        m_settingsRebuilding = false;
    }

    void MainWindow::OnOpenSettings(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_instance) return;
        if (m_settingsWindow) { m_settingsWindow.Activate(); return; }

        m_settingsWindow = Microsoft::UI::Xaml::Window();
        m_settingsWindow.Title(L"Emulator Settings");
        m_settingsWindow.AppWindow().Resize({ 600, 700 });
        m_settingsWindow.Closed([this](auto&&, auto&&) {
            m_settingsWindow = nullptr;
            m_settingControls.clear();
            m_pendingLists.clear();
        });

        BuildSettingsContent();
        ApplyThemeToWindow(m_settingsWindow, m_isDark);
        m_settingsWindow.Activate();
    }
}
