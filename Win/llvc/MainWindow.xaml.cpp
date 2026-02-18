#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"

#include <algorithm>

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::llvc::implementation{

MainWindow::MainWindow(){
    InitializeComponent();

    m_player = Windows::Media::Playback::MediaPlayer();
    PreviewPlayer().SetMediaPlayer(m_player);
}

void MainWindow::StartButton_Click(IInspectable const&, RoutedEventArgs const&){
    if(m_player){
        m_player.Play();
    }
}

void MainWindow::PauseButton_Click(IInspectable const&, RoutedEventArgs const&){
    if(m_player){
        m_player.Pause();
    }
}

void MainWindow::StopButton_Click(IInspectable const&, RoutedEventArgs const&){
    if(m_player){
        m_player.Pause();
        m_player.PlaybackSession().Position(std::chrono::seconds(0));
    }
}

void MainWindow::Window_DragOver(IInspectable const&, DragEventArgs const& e){
    e.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
}

Windows::Foundation::IAsyncAction MainWindow::Window_Drop(IInspectable const&, DragEventArgs const& e){
    const auto view{e.DataView()};
    if(!view.Contains(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems())){
        StatusText().Text(L"Dropped content is not a file");
        co_return;
    }

    const auto items{co_await view.GetStorageItemsAsync()};
    if(items.Size() != 1){
        StatusText().Text(L"Only support a single .mp4 or .mov file");
        co_return;
    }

    const auto file{items.GetAt(0).try_as<Windows::Storage::StorageFile>()};
    if(!file){
        StatusText().Text(L"Dropped content is not a file");
        __debugbreak(); // this should have been caught ealier in this function!
        co_return;
    }

    {
        const auto ext{file.FileType()};
        std::wstring lower{ext.c_str()};
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        if(lower != L".mp4" && lower != L".mov"){
            StatusText().Text(L"Only .mp4 and .mov files are supported");
            co_return;
        }
    }

    co_await LoadVideoFileAsync(file);
}

Windows::Foundation::IAsyncAction MainWindow::LoadVideoFileAsync(Windows::Storage::StorageFile const& file){
    const auto source{Windows::Media::Core::MediaSource::CreateFromStorageFile(file)};
    m_player.Source(source);

    std::wstring status{L"Loaded: "};
    status += file.Name().c_str();
    StatusText().Text(status);

    co_return;
}

int32_t MainWindow::MyProperty(){
    throw hresult_not_implemented();
}

void MainWindow::MyProperty(int32_t){
    throw hresult_not_implemented();
}

}
