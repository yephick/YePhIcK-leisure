#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

#include <cwctype>
#include <string>
#include <winrt/Windows.Storage.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Windows::ApplicationModel::Activation;

namespace winrt::llvc::implementation{

namespace{

std::wstring extractLaunchPathToken(const hstring& arguments){
    std::wstring text{arguments.c_str()};
    const auto isSpace{[](wchar_t ch){ return std::iswspace(ch) != 0; }};

    const auto start{text.find_first_not_of(L" \t\r\n")};
    if(start == std::wstring::npos){
        return {};
    }

    if(text[start] == L'"'){
        const auto endQuote{text.find(L'"', start + 1)};
        if(endQuote != std::wstring::npos){
            return text.substr(start + 1, endQuote - (start + 1));
        }

        return text.substr(start + 1);
    }

    auto end{start};
    while(end < text.size() && !isSpace(text[end])){
        ++end;
    }

    return text.substr(start, end - start);
}

bool isLlvcPath(const std::wstring& path){
    if(path.size() < 5){
        return false;
    }

    const auto ext{path.substr(path.size() - 5)};
    return _wcsicmp(ext.c_str(), L".llvc") == 0;
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

void App::ensureMainWindow(){
    if(!window){
        window = make<MainWindow>();
    }
}

void App::tryHandleLaunchArguments(const hstring& arguments){
    const auto maybePath{extractLaunchPathToken(arguments)};
    if(maybePath.empty() || !isLlvcPath(maybePath)){
        return;
    }

    if(const auto mainWindow{window.try_as<llvc::MainWindow>()}){
        (void)mainWindow.OpenProjectPath(hstring(maybePath));
    }
}

/// <summary>
/// Invoked when the application is launched.
/// </summary>
/// <param name="e">Details about the launch request and process.</param>
void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& e){
    ensureMainWindow();
    window.Activate();
    tryHandleLaunchArguments(e.Arguments());
}

void App::OnActivated(IActivatedEventArgs const& e){
    ensureMainWindow();
    window.Activate();

    const auto mainWindow{window.try_as<llvc::MainWindow>()};
    if(!mainWindow){
        return;
    }

    if(e.Kind() == ActivationKind::File){
        if(const auto fileArgs{e.try_as<FileActivatedEventArgs>()}){
            const auto files{fileArgs.Files()};
            if(files.Size() > 0){
                if(const auto file{files.GetAt(0).try_as<Windows::Storage::StorageFile>()}){
                    (void)mainWindow.OpenProjectFile(file);
                }
            }
        }
        return;
    }

    if(e.Kind() == ActivationKind::Launch){
        if(const auto launchArgs{e.try_as<Windows::ApplicationModel::Activation::LaunchActivatedEventArgs>()}){
            tryHandleLaunchArguments(launchArgs.Arguments());
        }
    }
}

}
