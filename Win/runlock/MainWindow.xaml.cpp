#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Windows::ApplicationModel::DataTransfer;

namespace winrt::runlock::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
    }

    void MainWindow::BrowseArchive_Click(IInspectable const&, RoutedEventArgs const&)
    {
        StatusText().Text(L"Browse archive clicked");
    }

    void MainWindow::Archive_DragOver(IInspectable const&, DragEventArgs const& e)
    {
        e.AcceptedOperation(DataPackageOperation::Copy);
    }

    IAsyncAction MainWindow::Archive_Drop(IInspectable const&, DragEventArgs const& e)
    {
        if (e.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            auto items = co_await e.DataView().GetStorageItemsAsync();
            if (items.Size() > 0)
            {
                if (auto file = items.GetAt(0).try_as<StorageFile>())
                {
                    ArchivePath().Text(file.Path());
                }
            }
        }
    }

    void MainWindow::Rules_DragOver(IInspectable const&, DragEventArgs const& e)
    {
        e.AcceptedOperation(DataPackageOperation::Copy);
    }

    IAsyncAction MainWindow::Rules_Drop(IInspectable const&, DragEventArgs const& e)
    {
        if (e.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            auto items = co_await e.DataView().GetStorageItemsAsync();
            if (items.Size() > 0)
            {
                if (auto file = items.GetAt(0).try_as<StorageFile>())
                {
                    PasswordRules().Text(co_await FileIO::ReadTextAsync(file));
                }
            }
        }
    }

    void MainWindow::GeneratePasswords_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto label = unbox_value<hstring>(GenerateButton().Content());
        if (label == L"Generate Passwords")
        {
            GenerateButton().Content(box_value(L"Cancel"));
            StatusText().Text(L"Generating passwords...");
        }
        else
        {
            GenerateButton().Content(box_value(L"Generate Passwords"));
            StatusText().Text(L"Generation cancelled");
        }
    }

    void MainWindow::StartPause_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto label = unbox_value<hstring>(StartPauseButton().Content());
        if (label == L"Start")
        {
            StartPauseButton().Content(box_value(L"Pause"));
            StatusText().Text(L"Unlocking...");
        }
        else if (label == L"Pause")
        {
            StartPauseButton().Content(box_value(L"Resume"));
            StatusText().Text(L"Paused");
        }
        else if (label == L"Resume")
        {
            StartPauseButton().Content(box_value(L"Stop"));
            StatusText().Text(L"Running");
        }
        else
        {
            StartPauseButton().Content(box_value(L"Start"));
            StatusText().Text(L"Stopped");
        }
    }

    void MainWindow::SaveProject_Click(IInspectable const&, RoutedEventArgs const&)
    {
        StatusText().Text(L"Save project clicked");
    }

    void MainWindow::LoadProject_Click(IInspectable const&, RoutedEventArgs const&)
    {
        StatusText().Text(L"Load project clicked");
    }
}
