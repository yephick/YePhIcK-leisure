#pragma once

#include "MainWindow.g.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct _GUID;

namespace winrt::llvc::implementation{

struct IndexedFrameSample{
    std::int64_t time100ns{};
    std::int64_t duration100ns{};
    bool cleanPoint{};
    std::uint32_t sampleIndex{};
};

struct MediaInspectionResult{
    bool isValid{false};
    std::wstring errorMessage{};
    std::wstring container{};
    std::wstring videoCodec{};
    std::wstring audioCodec{};
    std::wstring duration{};
    std::wstring fileSize{};
    std::wstring resolution{};
    std::wstring frameRate{};
    std::wstring videoBitrate{};
    std::wstring audioBitrate{};
    std::wstring keyFrameSummary{};
    std::wstring keyFrameInterval{};
    std::wstring allSamplesIndependent{};
    std::wstring maxKeyFrameSpacing{};
};

struct MainWindow: MainWindowT<MainWindow>{
    MainWindow();

    void StartButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void PauseButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void StopButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void TimelineZoomSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
    void TimelineHorizontalScrollBar_ValueChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
    void TimelineScrollViewer_ViewChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const& args);
    void TimelineScrollViewer_SizeChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
    void TimelineCanvas_PointerPressed(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void TimelineCanvas_PointerMoved(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void TimelineCanvas_PointerReleased(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void TimelineCanvas_PointerCanceled(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void TimelineCanvas_PointerCaptureLost(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void TimelineCanvas_Loaded(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void Window_PreviewKeyDown(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void Window_KeyDown(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void RootGrid_PointerReleased(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void KeyFrameSnapMode_Checked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction NewProjectMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction OpenProjectMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction SaveProjectMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction CloseProjectMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction LoadVideoMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction RecentVideoMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction RecentProjectMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction PropertiesMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction ExitMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction AboutMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::Windows::Foundation::IAsyncAction OptionsMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
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
    MediaInspectionResult m_mediaInfo{};
    std::vector<winrt::hstring> m_recentVideos{};
    std::vector<winrt::hstring> m_recentProjects{};
    std::uint32_t m_maxRecentVideos{5};
    std::uint32_t m_maxRecentProjects{5};
    std::wstring m_keyFrameSnapMode{L"Nearest"};
    std::vector<double> m_selectedKeyFrames{};
    std::vector<IndexedFrameSample> m_frameIndex{};
    std::vector<std::pair<double, double>> m_cutIntervals{};
    std::vector<std::wstring> m_projectUnknownLines{};
    winrt::hstring m_projectPath{};
    std::wstring m_lastSavedProjectSnapshot{};
    bool m_isClosing{false};
    bool m_isTimelineDragging{false};
    bool m_timelineDragMoved{false};
    std::uint32_t m_timelineDragPointerId{0};
    double m_timelineDragStartX{0};
    double m_timelineDragStartOffset{0};
    HWND GetWindowHandle() const;

private:
    void RestoreWindowPlacement();
    void SaveWindowPlacement() const;
    winrt::Windows::Foundation::IAsyncAction PickAndLoadVideoAsync();
    void LoadAppSettings();
    void SaveAppSettings() const;
    void RefreshRecentVideosMenu();
    void RefreshRecentProjectsMenu();
    void AddRecentVideo(winrt::hstring const& path);
    void AddRecentProject(winrt::hstring const& path);
    winrt::Windows::Foundation::IAsyncAction ShowInfoDialogAsync(winrt::hstring const& title, winrt::hstring const& message);
    winrt::Windows::Foundation::IAsyncAction ShowPropertiesDialogAsync();
    winrt::Windows::Foundation::IAsyncAction ShowOptionsDialogAsync();
    winrt::Windows::Foundation::IAsyncAction OpenProjectFileAsync(winrt::Windows::Storage::StorageFile const& file);
    winrt::Windows::Foundation::IAsyncAction SaveProjectFileAsync(winrt::Windows::Storage::StorageFile const& file);
    void ResetProjectState(bool clearLoadedVideo);
    std::wstring BuildProjectSnapshot();
    bool IsProjectDirty();
    winrt::Windows::Foundation::IAsyncOperation<bool> EnsureProjectSavedBeforeContinuingAsync();
    static MediaInspectionResult InspectMediaFile(std::wstring const& filePath);
    static bool IsSupportedVideoSubtype(_GUID const& subtype);
    static std::vector<IndexedFrameSample> BuildKeyframeIndexForFile(std::wstring const& filePath);
    static std::wstring GuidToCodecName(_GUID const& subtype, bool isVideo);
    winrt::Windows::Foundation::IAsyncAction LoadVideoFileAsync(winrt::Windows::Storage::StorageFile const& file);
    winrt::fire_and_forget RenderTimelineAsync();
    void UpdateTimelineCursorFromPlayback();
    void SyncTimelineHorizontalScrollBar();
    void RenderTimelineTicks();
    void SeekTimelineToCanvasX(double pointerX, bool bypassSnap);
    void RenderKeyframeTicks();
    void StepByFrame(int delta);
    void StepByKeyframe(int delta);
    void EnsureTimelineCursorVisible(double cursorLeft);
    void TryFocusTimelineCanvas(winrt::Microsoft::UI::Xaml::FocusState const focusState);
    bool HandleStorylineKeyDown(winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    static winrt::Windows::Foundation::TimeSpan SecondsToTimeSpan(double seconds);
    static bool IsRectVisibleOnAnyMonitor(RECT const& rect);
};

}

namespace winrt::llvc::factory_implementation{

struct MainWindow: MainWindowT<MainWindow, implementation::MainWindow>{};

}
