#pragma once

#include "MainWindow.g.h"

#include <cstdint>
#include <functional>
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
    using Control = winrt::Windows::Foundation::IInspectable;
    using REArgs = winrt::Microsoft::UI::Xaml::RoutedEventArgs;
    using PREArgs = winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs;
    using AAction = winrt::Windows::Foundation::IAsyncAction;

    MainWindow();

    void startButton_Click(const Control& sender, const REArgs& args);
    void pauseButton_Click(const Control& sender, const REArgs& args);
    void stopButton_Click(const Control& sender, const REArgs& args);
    void timelineZoomSlider_ValueChanged(const Control& sender, const winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs& args);
    void timelineHorizontalScrollBar_ValueChanged(const Control& sender, const winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs& args);
    void timelineScrollViewer_ViewChanged(const Control& sender, const winrt::Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs& args);
    void timelineScrollViewer_SizeChanged(const Control& sender, const winrt::Microsoft::UI::Xaml::SizeChangedEventArgs& args);
    void timelineCanvas_PointerPressed(const Control& sender, const PREArgs& args);
    void timelineCanvas_PointerMoved(const Control& sender, const PREArgs& args);
    void timelineCanvas_PointerReleased(const Control& sender, const PREArgs& args);
    void timelineCanvas_PointerCanceled(const Control& sender, const PREArgs& args);
    void timelineCanvas_PointerCaptureLost(const Control& sender, const PREArgs& args);
    void timelineCanvas_Loaded(const Control& sender, const REArgs& args);
    void timelineTickCanvas_PointerReleased(const Control& sender, const PREArgs& args);
    void window_PreviewKeyDown(const Control& sender, const winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs& args);
    void window_KeyDown(const Control& sender, const winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs& args);
    void rootGrid_PointerReleased(const Control& sender, const PREArgs& args);
    AAction newProjectMenuItem_Click(const Control& sender, const REArgs& args);
    AAction openProjectMenuItem_Click(const Control& sender, const REArgs& args);
    AAction saveProjectMenuItem_Click(const Control& sender, const REArgs& args);
    AAction closeProjectMenuItem_Click(const Control& sender, const REArgs& args);
    AAction loadVideoMenuItem_Click(const Control& sender, const REArgs& args);
    AAction recentVideoMenuItem_Click(const Control& sender, const REArgs& args);
    AAction recentProjectMenuItem_Click(const Control& sender, const REArgs& args);
    AAction propertiesMenuItem_Click(const Control& sender, const REArgs& args);
    AAction exitMenuItem_Click(const Control& sender, const REArgs& args);
    AAction aboutMenuItem_Click(const Control& sender, const REArgs& args);
    AAction optionsMenuItem_Click(const Control& sender, const REArgs& args);
    void window_DragOver(const Control& sender, const winrt::Microsoft::UI::Xaml::DragEventArgs& args);
    AAction window_Drop(const Control& sender, const winrt::Microsoft::UI::Xaml::DragEventArgs& args);
    void onClosed(const Control& sender, const winrt::Microsoft::UI::Xaml::WindowEventArgs& args);
    void onNaturalDurationChanged(const winrt::Windows::Media::Playback::MediaPlaybackSession& sender, const Control& args);
    void onPositionTimerTick(const Control& sender, const Control& args);

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
    std::vector<std::uint32_t> m_selectedKeyFrames{};
    std::vector<IndexedFrameSample> m_frameIndex{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> m_cutIntervals{};
    std::vector<std::wstring> m_projectUnknownLines{};
    winrt::hstring m_projectPath{};
    std::wstring m_lastSavedProjectSnapshot{};
    bool m_isClosing{false};
    bool m_isTimelineDragging{false};
    bool m_timelineDragMoved{false};
    std::uint32_t m_timelineDragPointerId{0};
    double m_timelineDragStartX{0};
    double m_timelineDragStartOffset{0};
    HWND getWindowHandle() const;

private:
    void restoreWindowPlacement();
    void saveWindowPlacement() const;
    winrt::Windows::Foundation::IAsyncAction pickAndLoadVideoAsync();
    void loadAppSettings();
    void saveAppSettings() const;
    void refreshRecentVideosMenu();
    void refreshRecentProjectsMenu();
    void addRecentVideo(const winrt::hstring& path);
    void addRecentProject(const winrt::hstring& path);
    winrt::Windows::Foundation::IAsyncAction showInfoDialogAsync(const winrt::hstring& title, const winrt::hstring& message);
    winrt::Windows::Foundation::IAsyncAction showPropertiesDialogAsync();
    winrt::Windows::Foundation::IAsyncAction showOptionsDialogAsync();
    winrt::Windows::Foundation::IAsyncAction openProjectFileAsync(const winrt::Windows::Storage::StorageFile& file);
    winrt::Windows::Foundation::IAsyncAction saveProjectFileAsync(const winrt::Windows::Storage::StorageFile& file);
    void resetProjectState(bool clearLoadedVideo);
    std::wstring buildProjectSnapshot();
    bool isProjectDirty();
    winrt::Windows::Foundation::IAsyncOperation<bool> ensureProjectSavedBeforeContinuingAsync();
    static MediaInspectionResult inspectMediaFile(const std::wstring& filePath);
    static bool isSupportedVideoSubtype(const _GUID& subtype);
    static std::vector<IndexedFrameSample> buildKeyframeIndexForFile(const std::wstring& filePath, const std::function<void(double)>& onProgress);
    static std::wstring guidToCodecName(const _GUID& subtype, bool isVideo);
    winrt::Windows::Foundation::IAsyncAction loadVideoFileAsync(const winrt::Windows::Storage::StorageFile& file, const std::vector<IndexedFrameSample>* preloadedKeyframeIndex = nullptr);
    winrt::fire_and_forget renderTimelineAsync();
    void updateTimelineCursorFromPlayback();
    void syncTimelineHorizontalScrollBar();
    void renderTimelineTicks();
    void seekTimelineToCanvasX(double pointerX, bool bypassSnap);
    void renderKeyframeTicks();
    void renderCutOverlays();
    bool toggleSelectedKeyframeAtCanvasX(double pointerX);
    bool toggleCutBlockAtCanvasX(double pointerX);
    bool trySkipCurrentCutDuringPlayback();
    void stepByFrame(int delta);
    void stepByKeyframe(int delta);
    void ensureTimelineCursorVisible(double cursorLeft);
    void tryFocusTimelineCanvas(const winrt::Microsoft::UI::Xaml::FocusState focusState);
    bool handleStorylineKeyDown(const winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs& args);
    static winrt::Windows::Foundation::TimeSpan secondsToTimeSpan(double seconds);
    static bool isRectVisibleOnAnyMonitor(const RECT& rect);
};

}

namespace winrt::llvc::factory_implementation{

struct MainWindow: MainWindowT<MainWindow, implementation::MainWindow>{};

}
