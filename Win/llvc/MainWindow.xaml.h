#pragma once

#include "MainWindow.g.h"

#include <cstdint>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct _GUID;
import llvc.Project;
import llvc.Timeline;

namespace winrt::llvc::implementation{

using namespace ::std;
using namespace ::winrt;

struct Ratio final{
    uint32_t num{};
    uint32_t den{};
    constexpr operator double() const noexcept{ return den != 0 ? 1.0 * num / den : 0; }
};

struct MediaInspectionResult{
    bool isValid{false};
    wstring errorMessage{};
    wstring container{};
    wstring videoCodec{};
    wstring audioCodec{};
    wstring duration{};
    wstring fileSize{};
    wstring sourceCreated{};
    wstring sourceModified{};
    wstring sourceEncodedBy{};
    wstring sourceComment{};
    wstring resolution{};
    Ratio frameRate{};
    wstring videoBitrate{};
    wstring audioBitrate{};
    wstring keyFrameSummary{};
    wstring keyFrameInterval{};
    wstring allSamplesIndependent{};
    wstring maxKeyFrameSpacing{};
    bool audioDisabledForThisSource{false};
    wstring audioDisabledReason{};
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
    using WAVArgs = winrt::Microsoft::UI::Xaml::WindowActivatedEventArgs;
    using MPSession = winrt::Windows::Media::Playback::MediaPlaybackSession;
    using MP = winrt::Windows::Media::Playback::MediaPlayer;
    using SFile = winrt::Windows::Storage::StorageFile;
    using DTS = winrt::Microsoft::UI::Xaml::DispatcherTimer;
    using IOpBool = winrt::Windows::Foundation::IAsyncOperation<bool>;
    using TS = winrt::Windows::Foundation::TimeSpan;
    using FState = winrt::Microsoft::UI::Xaml::FocusState;

    MainWindow(const hstring& launchArguments = {});

    void startButton_Click(const Control& sender, const REArgs& args);
    void pauseButton_Click(const Control& sender, const REArgs& args);
    void stopButton_Click(const Control& sender, const REArgs& args);
    void reevaluateClearCutMarkersButton_Click(const Control& sender, const REArgs& args);
    void timelineZoomSlider_ValueChanged(const Control& sender, const RBVArgs& args);
    void timelineZoomSlider_PointerWheelChanged(const Control& sender, const PREArgs& args);
    void keepAudioCheckBox_Changed(const Control& sender, const REArgs& args);
    void audioCrossfadeComboBox_SelectionChanged(const Control& sender, const Control& args);
    void audioVolumeSlider_ValueChanged(const Control& sender, const RBVArgs& args);
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
    void videoDetailsOpenMarker_Click(const Control& sender, const REArgs& args);
    void videoDetailsCollapseMarker_Click(const Control& sender, const REArgs& args);
    void cheatSheetOpenMarker_Click(const Control& sender, const REArgs& args);
    void cheatSheetCollapseMarker_Click(const Control& sender, const REArgs& args);
    AAction newProjectMenuItem_Click(const Control& sender, const REArgs& args);
    AAction undoMenuItem_Click(const Control& sender, const REArgs& args);
    AAction redoMenuItem_Click(const Control& sender, const REArgs& args);
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
    AAction manualMenuItem_Click(const Control& sender, const REArgs& args);
    AAction aboutMenuItem_Click(const Control& sender, const REArgs& args);
    AAction optionsMenuItem_Click(const Control& sender, const REArgs& args);
    void separatePreviewWindowMenuItem_Click(const Control& sender, const REArgs& args);
    void toggleSeparatePreviewFullscreenMenuItem_Click(const Control& sender, const REArgs& args);
    void zoomInTimelineMenuItem_Click(const Control& sender, const REArgs& args);
    void zoomOutTimelineMenuItem_Click(const Control& sender, const REArgs& args);
    AAction toggleCutMarkerAtCursorMenuItem_Click(const Control& sender, const REArgs& args);
    AAction markSceneCutAtCursorMenuItem_Click(const Control& sender, const REArgs& args);
    AAction markSceneKeptAtCursorMenuItem_Click(const Control& sender, const REArgs& args);
    void window_DragOver(const Control& sender, const DEArgs& args);
    AAction window_Drop(const Control& sender, const DEArgs& args);
    void onClosed(const Control& sender, const WEArgs& args);
    void onWindowActivated(const Control& sender, const WAVArgs& args);
    void onNaturalDurationChanged(const MPSession& sender, const Control& args);
    void onPositionTimerTick(const Control& sender, const Control& args);
    void cancelExportButton_Click(const Control& sender, const REArgs& args);
    bool isExportInProgressForClosePrompt() const;
    void requestExportCancel();

private:
    MP m_player{nullptr};
    MPSession::NaturalDurationChanged_revoker m_naturalDurationChangedRevoker{};
    DTS m_positionTimer{nullptr};
    double m_timelineDurationSeconds{0};
    uint64_t m_timelineRenderVersion{0};
    MediaInspectionResult m_mediaInfo{};
    vector<hstring> m_recentVideos{};
    vector<hstring> m_recentProjects{};
    uint32_t m_maxRecentVideos{5};
    uint32_t m_maxRecentProjects{5};
    hstring m_projectPath{};
    bool m_isClosing{false};
    bool m_isExportInProgress{false};
    std::atomic_bool m_cancelExportRequested{false};
    bool m_resumeTimelineRenderAfterExport{false};
    bool m_isTimelineDragging{false};
    bool m_timelineDragMoved{false};
    uint32_t m_timelineDragPointerId{0};
    double m_timelineDragStartX{0};
    double m_timelineDragStartOffset{0};
    bool m_isApplyingUndoRedoState{false};
    winrt::Microsoft::UI::Xaml::Window::Activated_revoker m_mainWindowActivatedRevoker{};
    bool m_isSeparatePreviewWindowOpen{false};
    bool m_isSeparatePreviewFullscreen{false};
    bool m_restorePreviewDetachedOnStartup{false};
    bool m_hasSeparatePreviewPlacement{false};
    bool m_restorePreviewFullscreenOnStartup{false};
    int32_t m_separatePreviewLeft{0};
    int32_t m_separatePreviewTop{0};
    int32_t m_separatePreviewWidthDips{960};
    int32_t m_separatePreviewHeightDips{540};
    int32_t m_separatePreviewDpi{96};
    RECT m_separatePreviewRestoreRect{0, 0, 0, 0};
    LONG_PTR m_separatePreviewRestoreStyle{0};
    LONG_PTR m_separatePreviewRestoreExStyle{0};
    winrt::Microsoft::UI::Xaml::Window m_separatePreviewWindow{nullptr};
    winrt::Microsoft::UI::Xaml::Window::Closed_revoker m_separatePreviewClosedRevoker{};
    ::llvc::Project m_prj{};
    ::llvc::Timeline m_tl{};

    struct UndoRedoState final{
        vector<::llvc::IndexedFrameSample> frameIndex{};
        vector<uint32_t> cutScenes{};
        bool keepAudio{true};
        int32_t audioCrossfadeMs{0};
        int32_t audioVolumePct{100};
    };
    vector<UndoRedoState> m_undoStack{};
    vector<UndoRedoState> m_redoStack{};

private:
    HWND getWindowHandle() const;
    void restoreWindowPlacement();
    void saveWindowPlacement() const;
    AAction pickAndLoadVideoAsync();
    void loadAppSettings();
    void saveAppSettings() const;
    void refreshRecentVideosMenu();
    void refreshRecentProjectsMenu();
    void addRecentVideo(const hstring& path);
    void addRecentProject(const hstring& path);
    AAction showInfoDialogAsync(const hstring& title, const hstring& message);
    AAction openFromLaunchArgumentsAsync(const hstring& arguments);
    AAction showPropertiesDialogAsync();
    AAction showOptionsDialogAsync();
    AAction openProjectFileAsync(const SFile& file);
    AAction saveProjectFileAsync(const SFile& file);
    void resetProjectState();
    void updateWindowTitle();
    IOpBool ensureProjectSavedBeforeContinuingAsync();
    static MediaInspectionResult inspectMediaFile(const wstring& filePath);
    static bool isSupportedVideoSubtype(const _GUID& subtype);
    static wstring guidToCodecName(const _GUID& subtype, bool isVideo);
    AAction loadVideoFileAsync(const SFile& file);
    fire_and_forget renderTimelineAsync();
    void updateTimelineCursorFromPlayback();
    void syncTimelineHorizontalScrollBar();
    void renderTimelineTicks();
    void seekTimelineToCanvasX(double pointerX);
    void renderKeyframeTicks();
    void renderCutOverlays();
    std::optional<int64_t> timelinePointToTime100ns(double pointerX, double width) const;
    bool toggleSelectedKeyframeAtTime100ns(int64_t time100ns);
    bool toggleCutBlockAtTime100ns(int64_t time100ns);
    bool setCutBlockAtTime100ns(int64_t time100ns, bool cutScene);
    bool toggleCutMarkerAtCursor();
    bool markSceneAtCursor(bool cutScene);
    bool nudgeCurrentSceneBoundaryToNearestRap(bool expandScene);
    bool trySkipCurrentCutDuringPlayback();
    void stepByFrame(int delta);
    bool moveCursorToMarker(int direction);
    void ensureTimelineCursorVisible(double cursorLeft);
    void ensureCurrentTimelineCursorVisible();
    void tryFocusTimelineCanvas(const FState focusState);
    bool handleStorylineKeyDown(const KRArgs& args);
    UndoRedoState captureUndoRedoState() const;
    bool isSameUndoRedoState(const UndoRedoState& a, const UndoRedoState& b) const;
    void clearUndoRedoHistory();
    bool pushUndoStateIfChanged();
    bool applyUndoRedoState(const UndoRedoState& state, bool fromUndo);
    bool undoLastEdit();
    bool redoLastEdit();
    void updateAudioUiAndPlaybackState();
    void setVideoDetailsPanelExpanded(bool expanded);
    void refreshVideoDetailsPanel();
    wstring buildSourcePropertiesText() const;
    void applyAudioSettingsToPlayer();
    void syncAudioCrossfadeComboSelection();
    void setStatusMessage(const wstring& message);
    void setErrorMessage(const wstring& message);
    void clearErrorMessage();
    void refreshStatusInfoSection();
    void setOperationInProgress(bool active, bool indeterminate = false);
    void setOperationProgress(double percent);
    static wstring formatTimelineDurationText(int64_t duration100ns);
    static wstring formatDateTimeText(const winrt::Windows::Foundation::DateTime& value);
    bool sourceHasAudio() const;
    bool setSeparatePreviewWindowOpen(bool open);
    void onSeparatePreviewWindowClosed(const Control& sender, const WEArgs& args);
    void onSeparatePreviewWindowKeyDown(const KRArgs& args);
    bool toggleSeparatePreviewFullscreen();
    void adjustTimelineZoomBy(int delta);
    void saveSeparatePreviewPlacement(HWND previewHwnd);
    void restoreSeparatePreviewPlacement(HWND previewHwnd);
    static TS secondsToTimeSpan(double seconds);
};

}

namespace winrt::llvc::factory_implementation{

struct MainWindow: MainWindowT<MainWindow, implementation::MainWindow>{};

}
