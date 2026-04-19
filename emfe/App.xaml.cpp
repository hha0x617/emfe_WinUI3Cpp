#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include <fstream>
#include <filesystem>
#include <shlobj.h>

void* winrt_make_emfe_App()
{
    return winrt::detach_abi(winrt::make<winrt::emfe::factory_implementation::App>());
}

using namespace winrt;
using namespace Microsoft::UI::Xaml;

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    ::winrt::Microsoft::UI::Xaml::Application::Start(
        [](auto&&) {
            ::winrt::make<::winrt::emfe::implementation::App>();
        });

    return 0;
}

namespace winrt::emfe::implementation
{
    App::App()
    {
        // Read theme from settings before any XAML loads.
        // Application.RequestedTheme can only be set in constructor.
        {
            std::string theme = "Dark";
            wchar_t* appData = nullptr;
            if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)) && appData) {
                auto path = std::filesystem::path(appData) / L"emfe_WinUI3Cpp" / L"appsettings.json";
                ::CoTaskMemFree(appData);
                std::ifstream ifs(path);
                if (ifs.is_open()) {
                    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                    // Simple search: "Theme": "Light" or "Theme": "System"
                    auto pos = content.find("\"Theme\"");
                    if (pos != std::string::npos) {
                        pos = content.find(':', pos);
                        if (pos != std::string::npos) {
                            auto q1 = content.find('"', pos + 1);
                            auto q2 = (q1 != std::string::npos) ? content.find('"', q1 + 1) : std::string::npos;
                            if (q1 != std::string::npos && q2 != std::string::npos)
                                theme = content.substr(q1 + 1, q2 - q1 - 1);
                        }
                    }
                }
            }
            if (theme == "Light")
                RequestedTheme(ApplicationTheme::Light);
            else if (theme != "System")
                RequestedTheme(ApplicationTheme::Dark);
        }

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& args)
    {
        m_window = make<MainWindow>();
        m_window.Activate();
    }
}
