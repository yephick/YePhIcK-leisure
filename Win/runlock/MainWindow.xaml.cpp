#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <thread>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <microsoft.ui.xaml.windowinterop.h>
#include <shobjidl_core.h>

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

    Windows::Foundation::IAsyncAction MainWindow::GeneratePasswords_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto hwnd = winrt::Microsoft::UI::Windowing::WindowNative::GetWindowHandle(*this);
        Windows::Storage::Pickers::FileSavePicker picker;
        picker.SuggestedStartLocation(Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary);
        picker.FileTypeChoices().Insert(L"Text", single_threaded_vector<hstring>({ L".txt" }));

        auto initialize = picker.as<::IInitializeWithWindow>();
        if (initialize)
        {
            initialize->Initialize(hwnd);
        }

        auto file = co_await picker.PickSaveFileAsync();
        if (!file)
        {
            co_return;
        }

        m_cancelGenerate = false;
        CancelGenerateButton().IsEnabled(true);
        GenerateButton().IsEnabled(false);
        StatusText().Text(L"Generating...");

        co_await GeneratePasswordFileAsync(file);

        CancelGenerateButton().IsEnabled(false);
        GenerateButton().IsEnabled(true);
        StatusText().Text(m_cancelGenerate ? L"Cancelled" : L"Done");
    }

    void MainWindow::CancelGenerate_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_cancelGenerate = true;
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

    Windows::Foundation::IAsyncAction MainWindow::GeneratePasswordFileAsync(Windows::Storage::StorageFile const& file)
    {
        using namespace Windows::Storage;
        using namespace Windows::Storage::Streams;

        auto uiDispatcher = DispatcherQueue();

        auto rulesText = PasswordRulesBox().Text();
        int minLen = static_cast<int>(MinLengthBox().Value());
        int maxLen = static_cast<int>(MaxLengthBox().Value());

        co_await winrt::resume_background();

        auto stream = co_await file.OpenAsync(FileAccessMode::ReadWrite);
        DataWriter writer(stream);

        std::vector<std::vector<std::wstring>> groups;
        std::wstring rules = rulesText.c_str();
        std::wstringstream ss(rules);
        std::wstring line;
        while (std::getline(ss, line))
        {
            if (!line.empty() && line.back() == L'\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }
            std::wstringstream ls(line);
            std::wstring token;
            std::vector<std::wstring> options;
            while (std::getline(ls, token, L'|'))
            {
                if (!token.empty())
                {
                    options.push_back(token);
                }
            }
            options.push_back(L"");
            groups.push_back(std::move(options));
        }

        std::wstring current;
        GenerateRecursive(groups, 0, current, minLen, maxLen, writer);

        co_await writer.StoreAsync();
        writer.DetachStream();
        stream.Close();

        co_await winrt::resume_foreground(uiDispatcher);
    }

    void MainWindow::GenerateRecursive(
        std::vector<std::vector<std::wstring>> const& groups,
        size_t index,
        std::wstring& current,
        int minLen,
        int maxLen,
        Windows::Storage::Streams::DataWriter& writer)
    {
        if (m_cancelGenerate)
        {
            return;
        }

        if (index == groups.size())
        {
            if (static_cast<int>(current.size()) >= minLen && static_cast<int>(current.size()) <= maxLen)
            {
                auto toWrite = current + L"\n";
                writer.WriteString(hstring(toWrite));
            }
            return;
        }

        for (auto const& part : groups[index])
        {
            auto prevLen = current.size();
            current += part;
            if (static_cast<int>(current.size()) <= maxLen)
            {
                GenerateRecursive(groups, index + 1, current, minLen, maxLen, writer);
            }
            current.resize(prevLen);
            if (m_cancelGenerate)
            {
                return;
            }
        }
    }
}
