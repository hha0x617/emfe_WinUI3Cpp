#pragma once

#include "MainWindow.g.h"
#include "PluginLoader.h"
#include "Vt100Terminal.h"
#include <queue>
#include <mutex>
#include <regex>
#include <filesystem>
#include <unordered_set>

namespace winrt::emfe::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        // XAML event handlers
        void OnLoadElf(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnLoadSrec(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnLoadBinary(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStep(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStepOver(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnRun(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStop(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStepOut(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnReset(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnFullReset(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMemoryGo(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnDisasmGo(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnDisasmDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        // Disassembly context menu (Phase B mirror of emfe_CsWPF).
        void OnDisasmRightTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
        void OnDisasmMenuOpening(Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&);
        void OnDisasmMenuCancel(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnDisasmMenuRunToHere(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnDisasmMenuSetPc(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnDisasmMenuCopy(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnDisasmCopyAccel(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&, Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const&);
        void OnToggleConsole(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenBreakpoints(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenCallStack(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenFramebuffer(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenSettings(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnRegEdit(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnRegApply(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnRegCancel(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnSwitchPlugin(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMemEdit(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMemApply(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMemCancel(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        void LoadPlugin();
        std::vector<std::filesystem::path> ScanPlugins();
        bool LoadPluginFromPath(const std::filesystem::path& path, bool savePreference = true);
        void AutoLoadKernelFromSettings();
        void ApplyCapabilityVisibility();
        void SavePluginPath(const std::filesystem::path& path);
        std::filesystem::path ReadSavedPluginPath();
        void DestroyCurrentInstance();
        void RegisterInstanceCallbacks();
        void BuildRegisterPanel();
        void UpdateRegisters();
        void UpdateDisassembly();
        void UpdateMemoryDump(uint32_t address);
        void UpdateToolbarState();
        void SetStatus(const std::wstring& text);

        // Replace a TextBox's default ContextFlyout / SelectionFlyout with
        // an English Cut/Copy/Paste/Select All menu. WinUI 3's built-in text
        // flyouts come localized in the OS UI language (so Japanese users
        // see 元に戻す / 切り取り / 貼り付け), and they also occasionally
        // render with the wrong theme colors. emfe's UI is English-only
        // (see feedback_emfe_ui_english_only), so we install our own.
        void AttachEnglishTextFlyout(Microsoft::UI::Xaml::Controls::TextBox const& tb);

        // Helper: add a register row (label + textbox) to a StackPanel
        Microsoft::UI::Xaml::Controls::TextBox AddRegRow(
            Microsoft::UI::Xaml::Controls::StackPanel const& parent,
            const char* name, uint32_t regId, int textboxWidth = 95);

        // Helper: add a register pair to a 2-column grid
        void AddRegPairToGrid(
            Microsoft::UI::Xaml::Controls::Grid const& grid,
            int row, int col,
            const char* name, uint32_t regId);

        PluginLoader m_plugin;
        EmfeInstance m_instance = nullptr;
        std::wstring m_loadedPluginStem;  // DLL filename without extension
        uint64_t m_capabilities = 0;       // EMFE_CAP_* bitmask reported by current plugin
        uint32_t m_pcRegId = 16;           // reg_id of the PC register (discovered via EMFE_REG_FLAG_PC)

        struct RegUIEntry {
            uint32_t regId;
            uint32_t bitWidth;
            EmfeRegType type;
            Microsoft::UI::Xaml::Controls::TextBox valueBox{ nullptr };
        };
        std::vector<RegUIEntry> m_regEntries;

        // SR flag checkboxes
        struct FlagCheckEntry {
            uint8_t bitMask;
            Microsoft::UI::Xaml::Controls::CheckBox checkBox{ nullptr };
        };
        std::vector<FlagCheckEntry> m_flagEntries;

        // Memory cell grid
        static constexpr int MemRows = 16;
        static constexpr int MemCols = 16;
        std::vector<std::vector<Microsoft::UI::Xaml::Controls::TextBox>> m_memCellBoxes;
        std::vector<Microsoft::UI::Xaml::Controls::TextBlock> m_memAddrLabels;
        std::vector<Microsoft::UI::Xaml::Controls::TextBlock> m_memAsciiLabels;
        bool m_memGridBuilt = false;
        bool m_memEditMode = false;
        void BuildMemoryGrid();
        void SelectMemoryCell(int row, int col);

        // Breakpoints
        std::unordered_map<uint32_t, bool> m_breakpointAddresses; // address → enabled
        void ToggleBreakpoint(uint32_t address);

        // Disassembly line addresses (parallel to DisasmList items)
        std::vector<uint32_t> m_disasmAddresses;
        // Cached display text for each row so Copy can reconstruct the
        // visible block without re-running disassembly.
        std::vector<std::wstring> m_disasmTexts;
        // One-shot execution breakpoints installed by "Run to Here" —
        // removed from the plugin in the state-change callback when the
        // CPU stops at the target address, unless a user-installed BP
        // is already there.
        std::unordered_set<uint32_t> m_tempBreakpoints;
        // Row index the disassembly context menu is targeting.  Captured
        // on RightTapped (before the flyout opens) so the Run-to-Here /
        // Set-PC actions hit the right-clicked row even when it isn't
        // currently selected.  -1 = nothing targeted.
        int m_disasmMenuTargetIndex = -1;
        void CopySelectedDisasmToClipboard();

        // Console window (separate Window)
        Microsoft::UI::Xaml::Window m_consoleWindow{ nullptr };
        // Settings window (separate Window)
        Microsoft::UI::Xaml::Window m_settingsWindow{ nullptr };
        struct SettingUI {
            std::string key;
            EmfeSettingType type;
            Windows::Foundation::IInspectable control{ nullptr };
        };
        std::vector<SettingUI> m_settingControls;
        bool m_settingsRebuilding = false;
        // Staged edit model for EMFE_SETTING_LIST settings. Populated lazily
        // when the Settings dialog is opened; all dialog mutations (add,
        // remove, edit field) go here. Applied to the plugin only on OK.
        struct ListItemEdit {
            std::unordered_map<std::string, std::string> fields;
        };
        struct ListEdit {
            std::vector<ListItemEdit> items;
        };
        std::unordered_map<std::string, ListEdit> m_pendingLists;
        void BuildSettingsContent();
        void SaveSettingsToStaging();
        void EnsureListStaged(std::string const& listKey);
        void ApplyStagedListsToPlugin();
        bool IsSettingVisible(const EmfeSettingDef* defs, int32_t count, int32_t idx);
        void BuildListControl(Microsoft::UI::Xaml::Controls::StackPanel const& parent,
                              std::string const& listKey, std::string const& label);
        std::unordered_set<int> GetUsedScsiIds(std::string const& listKey, int excludeIndex);
        // Breakpoints window (separate Window)
        Microsoft::UI::Xaml::Window m_breakpointsWindow{ nullptr };
        void BuildBreakpointsUI();
        void RefreshBreakpointsWindow();
        void SyncBreakpointsFromPlugin();
        // Call Stack window (separate Window)
        Microsoft::UI::Xaml::Window m_callStackWindow{ nullptr };
        void BuildCallStackUI();
        void RefreshCallStackWindow();

        // Custom Grid subclass exposing ProtectedCursor (for cursor control)
        struct CursorGrid : Microsoft::UI::Xaml::Controls::GridT<CursorGrid>
        {
            void SetCursor(Microsoft::UI::Input::InputCursor const& cursor) { ProtectedCursor(cursor); }
        };

        // Framebuffer window (separate Window)
        Microsoft::UI::Xaml::Window m_framebufferWindow{ nullptr };
        Microsoft::UI::Xaml::Controls::Image m_fbImage{ nullptr };
        Microsoft::UI::Xaml::Controls::TextBlock m_fbStatusText{ nullptr };
        Microsoft::UI::Xaml::Controls::TextBlock m_fbInputStatus{ nullptr };
        winrt::com_ptr<CursorGrid> m_fbGrid;
        Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap m_fbBitmap{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer m_fbTimer{ nullptr };
        uint32_t m_fbLastWidth = 0, m_fbLastHeight = 0, m_fbLastBpp = 0;
        bool m_fbInputCaptured = false;
        int m_fbFrameCount = 0;
        std::chrono::steady_clock::time_point m_fbFpsStart;
        double m_fbCurrentFps = 0;
        void RefreshFramebufferFrame();
        void ConvertToBgra(const EmfeFramebufferInfo& info, const uint8_t* src, uint8_t* dst, int dstStride);
        void NavigateDisassemblyTo(uint32_t address);
        void UpdateBoardTypeText(winrt::hstring cpuName = L"");
        uint32_t m_lastStopAddress = 0;
        EmfeStopReason m_lastStopReason = EMFE_STOP_REASON_NONE;
        Microsoft::UI::Xaml::Controls::TextBox m_consoleTextBox{ nullptr };
        Vt100Terminal m_terminal{80, 25, 2000};
        std::queue<char> m_consoleOutputQueue;
        std::mutex m_consoleOutputMutex;
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_consoleRenderTimer{nullptr};
        void EnsureConsoleWindow();
        void AppendConsoleChar(char ch);

        // Periodic MHz/MIPS update while the emulator is running, modeled
        // after em68030_WinUI3Cpp's MainViewModel: every ~500ms sample
        // cycle/instruction counters, compute rate over the interval, and
        // refresh the toolbar text on the UI thread. The text is clickable
        // and cycles through three views so all three fit in the toolbar:
        //   0 = Cycles / Instrs
        //   1 = MHz / MIPS (instantaneous over the last 500 ms)
        //   2 = avg MHz / MIPS (since the current Run started)
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_statsTimer{nullptr};
        std::chrono::steady_clock::time_point m_statsLastInstant{};
        int64_t m_statsLastCycles = 0;
        int64_t m_statsLastInstrs = 0;
        std::chrono::steady_clock::time_point m_runStartInstant{};
        int64_t m_runStartCycles = 0;
        int64_t m_runStartInstrs = 0;
        double m_instMhz = 0;
        double m_instMips = 0;
        double m_avgMhz = 0;
        double m_avgMips = 0;
        int m_statsViewMode = 0;
        void StartStatsTimer();
        void UpdateStatsDisplay();
        void ResetRunStatsBaseline();  // snapshot counters at Run start for the avg view
        void OnCyclesTextTapped(Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);

        // Console context menu: Copy / Paste / Select All.  Paste uses
        // emfe_console_tx_space (when the plugin exports it) to throttle
        // clipboard text at exactly the rate the guest UART drains.
        void SetupConsoleContextMenu();
        void DoConsoleCopy();
        void DoConsolePaste();
        // Member coroutine so `this` is a real pointer (not a lambda's
        // captured-this, which dangles once the enclosing method returns).
        winrt::fire_and_forget RunConsolePaste(std::wstring normalized);
        void DoConsoleSelectAll();
        std::atomic<bool> m_consolePasteCancel{false};
        std::atomic<bool> m_consolePasteActive{false};
        std::wstring m_consoleTitleOrig;  // restored after paste reports finish

        // Console search state
        bool m_consoleSearchMode = false;
        int m_consoleSearchIndex = -1;
        std::string m_consoleLastSearchText;
        Microsoft::UI::Xaml::Controls::TextBox m_searchBox{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock m_searchStatus{nullptr};
        Microsoft::UI::Xaml::Controls::Primitives::ToggleButton m_caseSensitiveToggle{nullptr};
        Microsoft::UI::Xaml::Controls::Primitives::ToggleButton m_regexToggle{nullptr};
        Microsoft::UI::Xaml::UIElement m_searchBar{nullptr};

        void OpenConsoleSearch();
        void CloseConsoleSearch();
        void ConsoleFindNext();
        void ConsoleFindPrev();
        std::vector<std::pair<int, int>> ConsoleCollectMatches(
            const std::string& text, const std::string& searchText,
            bool regexMode, bool caseSensitive);
        void ConsoleHighlightMatch(int pos, int length, int current, int total);

        // Theme
        bool m_isDark = true;
        void ApplyTheme(const std::string& themeName);
        void ApplyThemeToWindow(Microsoft::UI::Xaml::Window const& window, bool isDark);
        Microsoft::UI::Xaml::Media::Brush GetThemeBrush(const wchar_t* key);
        void RefreshCodeBehindBrushes();

        uint32_t m_memoryAddress = 0;
        Microsoft::UI::Dispatching::DispatcherQueue m_dispatcherQueue{ nullptr };
    };
}

namespace winrt::emfe::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
