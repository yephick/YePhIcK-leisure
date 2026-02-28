#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <thread>
#include <fstream>
#include <filesystem>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::runlock::implementation
{
    namespace
    {
        std::wstring TryGetStartupProjectPath()
        {
            int argc = 0;
            std::wstring startupProjectPath;
            LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
            if (argv == nullptr)
            {
                return startupProjectPath;
            }

            for (int i = 1; i < argc; ++i)
            {
                std::filesystem::path candidate{ argv[i] };
                if (_wcsicmp(candidate.extension().c_str(), L".llvc") == 0 && std::filesystem::exists(candidate))
                {
                    startupProjectPath = candidate.wstring();
                    break;
                }
            }

            ::LocalFree(argv);
            return startupProjectPath;
        }
    }

    MainWindow::MainWindow()
    {
        InitializeComponent();
//        Loaded({ this, &MainWindow::Window_Loaded });
    }

    void MainWindow::Window_Loaded(IInspectable const&, RoutedEventArgs const&)
    {
        uint32_t cores = std::thread::hardware_concurrency();
        if (cores == 0) { cores = 1; }
        for (uint32_t i = 1; i <= cores; ++i)
        {
            CpuCoresComboBox().Items().Append(box_value(i));
        }
        CpuCoresComboBox().SelectedIndex(0);

        auto startupProjectPath = TryGetStartupProjectPath();
        if (!startupProjectPath.empty())
        {
            LoadProjectFromPath(startupProjectPath);
        }
    }

    void MainWindow::BrowseArchive_Click(IInspectable const&, RoutedEventArgs const&)
    {
        // TODO: Implement file picker for RAR files
    }

    void MainWindow::Archive_DragOver(IInspectable const&, DragEventArgs const& e)
    {
        e.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
    }

    void MainWindow::Archive_Drop(IInspectable const&, DragEventArgs const&)
    {
        // TODO: Handle dropped RAR file
    }

    void MainWindow::PasswordRules_DragOver(IInspectable const&, DragEventArgs const& e)
    {
        e.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
    }

    void MainWindow::PasswordRules_Drop(IInspectable const&, DragEventArgs const&)
    {
        // TODO: Populate password rules from dropped text file
    }

    void MainWindow::GeneratePasswords_Click(IInspectable const&, RoutedEventArgs const&)
    {
        // TODO: Generate list of passwords
        CancelGenerateButton().IsEnabled(true);
        GenerateButton().IsEnabled(false);
    }

    void MainWindow::CancelGenerate_Click(IInspectable const&, RoutedEventArgs const&)
    {
        // TODO: Cancel password generation
        CancelGenerateButton().IsEnabled(false);
        GenerateButton().IsEnabled(true);
    }

    void MainWindow::UnlockButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        switch (m_unlockState)
        {
        case UnlockState::Stopped:
            m_unlockState = UnlockState::Running;
            UnlockButton().Content(box_value(L"Pause"));
            StatusText().Text(L"Running...");
            break;
        case UnlockState::Running:
            m_unlockState = UnlockState::Paused;
            UnlockButton().Content(box_value(L"Resume"));
            StatusText().Text(L"Paused");
            break;
        case UnlockState::Paused:
            m_unlockState = UnlockState::Stopped;
            UnlockButton().Content(box_value(L"Start"));
            StatusText().Text(L"Stopped");
            break;
        }
    }

    void MainWindow::SaveProject_Click(IInspectable const&, RoutedEventArgs const&)
    {
        // TODO: Save project settings
    }

    void MainWindow::LoadProject_Click(IInspectable const&, RoutedEventArgs const&)
    {
        // TODO: Load project settings
    }

    bool MainWindow::LoadProjectFromPath(std::wstring const& projectPath)
    {
        std::wifstream stream(projectPath);
        if (!stream)
        {
            StatusText().Text(L"Failed to load project");
            return false;
        }

        std::wstring projectJsonText((std::istreambuf_iterator<wchar_t>(stream)), std::istreambuf_iterator<wchar_t>());
        Windows::Data::Json::JsonObject json;
        if (!Windows::Data::Json::JsonObject::TryParse(projectJsonText, json))
        {
            StatusText().Text(L"Invalid project file");
            return false;
        }

        if (json.HasKey(L"archivePath"))
        {
            ArchivePathBox().Text(json.GetNamedString(L"archivePath", L""));
        }

        if (json.HasKey(L"passwordRules"))
        {
            PasswordRulesBox().Text(json.GetNamedString(L"passwordRules", L""));
        }

        auto minLength = json.GetNamedNumber(L"minLength", 0.0);
        auto maxLength = json.GetNamedNumber(L"maxLength", 0.0);
        MinLengthBox().Value(minLength);
        MaxLengthBox().Value(maxLength);

        auto cpuCoresValue = static_cast<uint32_t>(json.GetNamedNumber(L"cpuCores", 1.0));
        auto itemCount = CpuCoresComboBox().Items().Size();
        if (itemCount > 0)
        {
            uint32_t clamped = (cpuCoresValue == 0) ? 1 : cpuCoresValue;
            clamped = (clamped > itemCount) ? itemCount : clamped;
            CpuCoresComboBox().SelectedIndex(clamped - 1);
        }

        StatusText().Text(L"Loaded project: " + hstring(projectPath));
        return true;
    }

    int32_t MainWindow::MyProperty()
    {
        throw hresult_not_implemented();
    }

    void MainWindow::MyProperty(int32_t /* value */)
    {
        throw hresult_not_implemented();
    }
}
