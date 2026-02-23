#pragma once

#include "MainWindow.g.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct _GUID;
import llvc.Project;
import llvc.Timeline;

namespace winrt::llvc::implementation{

struct Ratio final{
    std::uint32_t num{};
    std::uint32_t den{};
    constexpr operator double() const noexcept{ return den != 0 ? 1.0 * num / den : 0; }
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
    Ratio frameRate{};
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
    using RBVArgs = winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs;
    using SVVCArgs = winrt::Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs;
    using SCArgs = winrt::Microsoft::UI::Xaml::SizeChangedEventArgs;
    using KRArgs = winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs;
    using DEArgs = winrt::Microsoft::UI::Xaml::DragEventArgs;
    using WEArgs = winrt::Microsoft::UI::Xaml::WindowEventArgs;
    using MPSession = winrt::Windows::Media::Playback::MediaPlaybackSession;
    using MP = winrt::Windows::Media::Playback::MediaPlayer;
    using SFile = winrt::Windows::Storage::StorageFile;
    using DTS = winrt::Microsoft::UI::Xaml::DispatcherTimer;
    using IOpBool = winrt::Windows::Foundation::IAsyncOperation<bool>;
    using TS = winrt::Windows::Foundation::TimeSpan;
    using FState = winrt::Microsoft::UI::Xaml::FocusState;

    MainWindow();

    void startButton_Click(const Control& sender, const REArgs& args);
    void pauseButton_Click(const Control& sender, const REArgs& args);
    void stopButton_Click(const Control& sender, const REArgs& args);
    void timelineZoomSlider_ValueChanged(const Control& sender, const RBVArgs& args);
    void keepAudioCheckBox_Changed(const Control& sender, const REArgs& args);
    void audioCrossfadeComboBox_SelectionChanged(const Control& sender, const Control& args);
    void timelineHorizontalScrollBar_ValueChanged(const Control& sender, const RBVArgs& args);
    void timelineScrollViewer_ViewChanged(const Control& sender, const SVVCArgs& args);
    void timelineScrollViewer_SizeChanged(const Control& sender, const SCArgs& args);
    void timelineCanvas_PointerPressed(const Control& sender, const PREArgs& args);
    void timelineCanvas_PointerMoved(const Control& sender, const PREArgs& args);
    void timelineCanvas_PointerReleased(const Control& sender, const PREArgs& args);
    void timelineCanvas_PointerCanceled(const Control& sender, const PREArgs& args);
    void timelineCanvas_PointerCaptureLost(const Control& sender, const PREArgs& args);
    void timelineCanvas_Loaded(const Control& sender, const REArgs& args);
    void timelineTickCanvas_PointerReleased(const Control& sender, const PREArgs& args);
    void window_PreviewKeyDown(const Control& sender, const KRArgs& args);
    void window_KeyDown(const Control& sender, const KRArgs& args);
    void rootGrid_PointerReleased(const Control& sender, const PREArgs& args);
    AAction newProjectMenuItem_Click(const Control& sender, const REArgs& args);
    AAction openProjectMenuItem_Click(const Control& sender, const REArgs& args);
    AAction saveProjectMenuItem_Click(const Control& sender, const REArgs& args);
    AAction saveProjectAsMenuItem_Click(const Control& sender, const REArgs& args);
    AAction closeProjectMenuItem_Click(const Control& sender, const REArgs& args);
    AAction loadVideoMenuItem_Click(const Control& sender, const REArgs& args);
    AAction exportVideoMenuItem_Click(const Control& sender, const REArgs& args);
    AAction recentVideoMenuItem_Click(const Control& sender, const REArgs& args);
    AAction recentProjectMenuItem_Click(const Control& sender, const REArgs& args);
    AAction propertiesMenuItem_Click(const Control& sender, const REArgs& args);
    AAction exitMenuItem_Click(const Control& sender, const REArgs& args);
    AAction aboutMenuItem_Click(const Control& sender, const REArgs& args);
    AAction optionsMenuItem_Click(const Control& sender, const REArgs& args);
    void window_DragOver(const Control& sender, const DEArgs& args);
    AAction window_Drop(const Control& sender, const DEArgs& args);
    void onClosed(const Control& sender, const WEArgs& args);
    void onNaturalDurationChanged(const MPSession& sender, const Control& args);
    void onPositionTimerTick(const Control& sender, const Control& args);

private:
    MP m_player{nullptr};
    MPSession::NaturalDurationChanged_revoker m_naturalDurationChangedRevoker{};
    DTS m_positionTimer{nullptr};
    double m_timelineDurationSeconds{0};
    std::uint64_t m_timelineRenderVersion{0};
    MediaInspectionResult m_mediaInfo{};
    std::vector<winrt::hstring> m_recentVideos{};
    std::vector<winrt::hstring> m_recentProjects{};
    std::uint32_t m_maxRecentVideos{5};
    std::uint32_t m_maxRecentProjects{5};
    winrt::hstring m_projectPath{};
    bool m_isClosing{false};
    bool m_isTimelineDragging{false};
    bool m_timelineDragMoved{false};
    std::uint32_t m_timelineDragPointerId{0};
    double m_timelineDragStartX{0};
    double m_timelineDragStartOffset{0};
    ::llvc::Project m_prj{};
    ::llvc::Timeline m_tl{};

private:
    HWND getWindowHandle() const;
    void restoreWindowPlacement();
    void saveWindowPlacement() const;
    AAction pickAndLoadVideoAsync();
    void loadAppSettings();
    void saveAppSettings() const;
    void refreshRecentVideosMenu();
    void refreshRecentProjectsMenu();
    void addRecentVideo(const winrt::hstring& path);
    void addRecentProject(const winrt::hstring& path);
    AAction showInfoDialogAsync(const winrt::hstring& title, const winrt::hstring& message);
    AAction showPropertiesDialogAsync();
    AAction showOptionsDialogAsync();
    AAction openProjectFileAsync(const SFile& file);
    AAction saveProjectFileAsync(const SFile& file);
    void resetProjectState();
    void updateWindowTitle();
    IOpBool ensureProjectSavedBeforeContinuingAsync();
    static MediaInspectionResult inspectMediaFile(const std::wstring& filePath);
    static bool isSupportedVideoSubtype(const _GUID& subtype);
    static std::wstring guidToCodecName(const _GUID& subtype, bool isVideo);
    AAction loadVideoFileAsync(const SFile& file);
    winrt::fire_and_forget renderTimelineAsync();
    void updateTimelineCursorFromPlayback();
    void syncTimelineHorizontalScrollBar();
    void renderTimelineTicks();
    void seekTimelineToCanvasX(double pointerX);
    void renderKeyframeTicks();
    void renderCutOverlays();
    bool toggleSelectedKeyframeAtCanvasX(double pointerX);
    bool toggleCutBlockAtCanvasX(double pointerX);
    bool trySkipCurrentCutDuringPlayback();
    void stepByFrame(int delta);
    void ensureTimelineCursorVisible(double cursorLeft);
    void tryFocusTimelineCanvas(const FState focusState);
    bool handleStorylineKeyDown(const KRArgs& args);
    void updateAudioUiAndPlaybackState();
    void applyAudioSettingsToPlayer();
    void syncAudioCrossfadeComboSelection();
    bool sourceHasAudio() const;
    static TS secondsToTimeSpan(double seconds);
    static bool isRectVisibleOnAnyMonitor(const RECT& rect);
};

}

namespace winrt::llvc::factory_implementation{

struct MainWindow: MainWindowT<MainWindow, implementation::MainWindow>{};

}
