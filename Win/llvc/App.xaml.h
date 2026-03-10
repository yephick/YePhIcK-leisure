#pragma once

#include "App.xaml.g.h"

namespace winrt::llvc::implementation{

struct App: AppT<App>{
    App();

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
    void OnFileActivated(winrt::Windows::ApplicationModel::Activation::FileActivatedEventArgs const&);

private:
    winrt::Microsoft::UI::Xaml::Window window{ nullptr };
};

}
