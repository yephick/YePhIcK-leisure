#pragma once
#include "App.g.h"

namespace winrt::runlock::implementation
{
    struct App : AppT<App>
    {
        App();
        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
    private:
        Microsoft::UI::Xaml::Window m_window{ nullptr };
    };
}

namespace winrt::runlock::factory_implementation
{
    struct App : AppT<App, implementation::App>
    {
    };
}
