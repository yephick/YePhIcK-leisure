#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

#include <winrt/Windows.Storage.h>
#include <shellapi.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::llvc::implementation{

namespace{

void showLaunchDebugPopup(const std::wstring& message){
#if defined(_DEBUG)
    ::MessageBoxW(nullptr, message.c_str(), L"llvc launch debug", MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
#else
    (void)message;
#endif
}

hstring getLaunchTargetFromOnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& e){
    if(!e.Arguments().empty()){
        return e.Arguments();
    }

    int argc{};
    wchar_t** argv{::CommandLineToArgvW(::GetCommandLineW(), &argc)};
    if(argc <= 1 || !argv){
        if(argv){
            ::LocalFree(argv);
        }
        return {};
    }

    const hstring target{argv[1]};
    ::LocalFree(argv);
    return target;
}

}

/// <summary>
/// Initializes the singleton application object.  This is the first line of authored code
/// executed, and as such is the logical equivalent of main() or WinMain().
/// </summary>
App::App(){
    // Xaml objects should not call InitializeComponent during construction.
    // See https://github.com/microsoft/cppwinrt/tree/master/nuget#initializecomponent

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e){
        if (IsDebuggerPresent()){
            auto errorMessage = e.Message();
            __debugbreak();
        }
    });
#endif
}

/// <summary>
/// Invoked when the application is launched.
/// </summary>
/// <param name="e">Details about the launch request and process.</param>
void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& e){
    const auto launchTarget{getLaunchTargetFromOnLaunched(e)};

    showLaunchDebugPopup(
        L"OnLaunched arguments: [" + std::wstring(e.Arguments().c_str()) +
        L"]\nCommand line target fallback: [" + std::wstring(launchTarget.c_str()) + L"]");

    window = make<MainWindow>(launchTarget);
    window.Activate();
}

void App::OnFileActivated(winrt::Windows::ApplicationModel::Activation::FileActivatedEventArgs const& e){
    std::wstring launchPath{};
    if(e.Files().Size() > 0){
        if(const auto file{e.Files().GetAt(0).try_as<winrt::Windows::Storage::StorageFile>()}){
            launchPath = file.Path().c_str();
        }
    }

    showLaunchDebugPopup(L"OnFileActivated path: " + launchPath);

    window = make<MainWindow>(winrt::hstring(launchPath));
    window.Activate();
}

}
