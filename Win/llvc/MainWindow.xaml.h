#pragma once

#include "MainWindow.g.h"

namespace winrt::llvc::implementation{
struct MainWindow: MainWindowT<MainWindow>{
    MainWindow();

    int32_t MyProperty();
    void MyProperty(int32_t value);

    void StartButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void PauseButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void StopButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void Window_DragOver(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction Window_Drop(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::DragEventArgs const& args);

private:
    winrt::Windows::Media::Playback::MediaPlayer m_player{nullptr};
    winrt::Windows::Foundation::IAsyncAction LoadVideoFileAsync(winrt::Windows::Storage::StorageFile const& file);
};
}

namespace winrt::llvc::factory_implementation{
struct MainWindow: MainWindowT<MainWindow, implementation::MainWindow>{};
}
