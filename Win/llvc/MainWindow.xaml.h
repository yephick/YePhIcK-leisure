#pragma once

#include "MainWindow.g.h"

namespace winrt::llvc::implementation{

struct MainWindow: MainWindowT<MainWindow>{
    MainWindow();

    void StartButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void PauseButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void StopButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void TimelineZoomSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
    void TimelineCanvas_PointerPressed(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void Window_DragOver(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction Window_Drop(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
    void OnClosed(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::WindowEventArgs const& args);
    void OnNaturalDurationChanged(winrt::Windows::Media::Playback::MediaPlaybackSession const& sender, winrt::Windows::Foundation::IInspectable const& args);
    void OnPositionTimerTick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Windows::Foundation::IInspectable const& args);

private:
    winrt::Windows::Media::Playback::MediaPlayer m_player{nullptr};
    winrt::Windows::Storage::StorageFile m_loadedFile{nullptr};
    winrt::Windows::Media::Playback::MediaPlaybackSession::NaturalDurationChanged_revoker m_naturalDurationChangedRevoker{};
    winrt::Microsoft::UI::Xaml::DispatcherTimer m_positionTimer{nullptr};
    double m_timelineDurationSeconds{0};
    std::uint64_t m_timelineRenderVersion{0};
    bool m_isClosing{false};
    HWND GetWindowHandle() const;

private:
    void RestoreWindowPlacement();
    void SaveWindowPlacement() const;
    winrt::Windows::Foundation::IAsyncAction LoadVideoFileAsync(winrt::Windows::Storage::StorageFile const& file);
    winrt::fire_and_forget RenderTimelineAsync();
    void UpdateTimelineCursorFromPlayback();
    static winrt::Windows::Foundation::TimeSpan SecondsToTimeSpan(double seconds);
    static bool IsRectVisibleOnAnyMonitor(RECT const& rect);
};

}

namespace winrt::llvc::factory_implementation{

struct MainWindow: MainWindowT<MainWindow, implementation::MainWindow>{};

}
