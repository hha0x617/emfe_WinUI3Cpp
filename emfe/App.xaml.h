#pragma once

#include "App.g.h"

namespace winrt::emfe::implementation
{
    struct App : AppT<App>
    {
        App();
        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

    private:
        winrt::Microsoft::UI::Xaml::Window m_window{ nullptr };
    };
}

namespace winrt::emfe::factory_implementation
{
    struct App : AppT<App, implementation::App>
    {
    };
}
