#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <thread>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::runlock::implementation
{
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

    int32_t MainWindow::MyProperty()
    {
        throw hresult_not_implemented();
    }

    void MainWindow::MyProperty(int32_t /* value */)
    {
        throw hresult_not_implemented();
    }
}
