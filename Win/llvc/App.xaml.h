#pragma once

#include "App.xaml.g.h"

namespace winrt::llvc::implementation{

struct App: AppT<App>{
    App();

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
    void OnActivated(winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs const&);

private:
    void ensureMainWindow();
    void tryHandleLaunchArguments(const winrt::hstring& arguments);
    void dispatchProjectOpenPath(const winrt::hstring& projectPath);

    winrt::Microsoft::UI::Xaml::Window window{ nullptr };
    winrt::hstring m_lastActivationProjectPath{};
};

}
