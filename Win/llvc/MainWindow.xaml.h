#pragma once

#include "MainWindow.g.h"

#include <cstdint>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <memory>

struct _GUID;
import llvc.Project;
import llvc.Timeline;
import llvc.TimelineRenderer;
import llvc.Media;
import llvc.Utils;
import llvc.EditorController;
import llvc.EditorCommands;
import llvc.Dialogs;
import llvc.ExportCoordinator;

namespace winrt::llvc::implementation{

using namespace ::std;
using namespace ::winrt;

struct Ratio final{
    uint32_t num{};
    uint32_t den{};
    constexpr operator double() const noexcept{ return den != 0 ? 1.0 * num / den : 0; }
};

using CapabilityState = ::llvc::CapabilityState;
using MediaInspectionResult = ::llvc::VideoSource::InspectionResult;

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
    void timelineScrollViewer_PointerWheelChanged(const Control& sender, const PREArgs& args);
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
    AAction shrinkSceneToRapMenuItem_Click(const Control& sender, const REArgs& args);
    AAction expandSceneToRapMenuItem_Click(const Control& sender, const REArgs& args);
    void window_DragOver(const Control& sender, const DEArgs& args);
    AAction window_Drop(const Control& sender, const DEArgs& args);
    void onClosed(const Control& sender, const WEArgs& args);
    void onWindowActivated(const Control& sender, const WAVArgs& args);
    void onNaturalDurationChanged(const MPSession& sender, const Control& args);
    void onPositionTimerTick(const Control& sender, const Control& args);
    void cancelExportButton_Click(const Control& sender, const REArgs& args);
    void exportOverlayCloseButton_Click(const Control& sender, const REArgs& args);
    AAction exportOverlayOpenExportButton_Click(const Control& sender, const REArgs& args);
    AAction exportOverlayDeleteSourceAndProjectButton_Click(const Control& sender, const REArgs& args);
    AAction exportOverlayDeleteProjectButton_Click(const Control& sender, const REArgs& args);
    bool isExportInProgressForClosePrompt() const;
    void requestExportCancel();

private:
    struct TimelineInteractionState final{
        bool isDragging{false};
        bool dragMoved{false};
        uint32_t dragPointerId{0};
        double dragStartX{0};
        double dragStartOffset{0};
        std::optional<::llvc::TimelineScrollbarAnchor> pendingScrollbarAnchor{};
        std::optional<::llvc::TimelinePointerZoomAnchor> pendingWheelZoomAnchor{};
    };

    struct SeparatePreviewState final{
        bool isOpen{false};
        bool isFullscreen{false};
        RECT restoreRect{0, 0, 0, 0};
        LONG_PTR restoreStyle{0};
        LONG_PTR restoreExStyle{0};
        winrt::Microsoft::UI::Xaml::Window window{nullptr};
        winrt::Microsoft::UI::Xaml::Window::Closed_revoker closedRevoker{};
        winrt::Microsoft::UI::Xaml::Controls::MediaPlayerElement player{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Image splashImage{nullptr};
    };

    MP m_player{nullptr};
    MPSession::NaturalDurationChanged_revoker m_naturalDurationChangedRevoker{};
    DTS m_positionTimer{nullptr};
    winrt::event_token m_positionTimerTickToken{};
    double m_timelineDurationSeconds{0};
    uint64_t m_timelineRenderVersion{0};
    MediaInspectionResult m_mediaInfo{};
    std::unique_ptr<::llvc::VideoSource> m_media{};
    hstring m_cachedRapSourcePath{};
    vector<int64_t> m_cachedRapTimes100ns{};
    bool m_cachedRapLookupAttempted{false};
    bool m_cachedRapLookupSucceeded{false};
    bool m_isRapLookupInProgress{false};
    bool m_hasTimelineRenderCompleted{false};
    bool m_isUiReadyForEvents{false};
    bool m_isSyncingTimelineScrollBar{false};
    bool m_pendingSeparatePreviewRestoreOnStartup{false};
    bool m_pendingReevaluateAfterRapLookup{false};
    bool m_pendingReevaluateWithoutUndoAfterRapLookup{false};
    std::vector<int64_t> m_pendingAutoEvaluateMarkerTimes100ns{};
    int m_pendingNudgeDirectionAfterRapLookup{0};
    ::llvc::AppSettingsState m_appSettings{};
    hstring m_projectPath{};
    bool m_isClosing{false};
    bool m_isExportInProgress{false};
    uint64_t m_currentExportSourceSizeBytes{0};
    int64_t m_currentExportSourceDuration100ns{0};
    int64_t m_currentExportOutputDuration100ns{0};
    bool m_currentExportPlanAdjusted{false};
    size_t m_currentExportCutBlockCount{0};
    bool m_exportOverlayHasFinalState{false};
    bool m_lastExportSucceeded{false};
    hstring m_currentExportSourcePath{};
    hstring m_currentExportProjectPath{};
    hstring m_currentExportOutputPath{};
    std::atomic_bool m_cancelExportRequested{false};
    std::chrono::steady_clock::time_point m_currentExportStartedAt{};
    std::chrono::steady_clock::time_point m_lastExportEtaRefreshAt{};
    std::optional<double> m_lastExportEtaProgress{};
    std::wstring m_exportEtaText{};
    TimelineInteractionState m_timelineInteraction{};
    winrt::Microsoft::UI::Xaml::Window::Activated_revoker m_mainWindowActivatedRevoker{};
    SeparatePreviewState m_separatePreview{};
    ::llvc::Project m_prj{};
    ::llvc::Timeline m_tl{};

    ::llvc::EditorHistoryState m_editorHistory{};
    std::optional<::llvc::EditorSnapshot> m_lastReevaluatedEditorSnapshot{};
    hstring m_lastReevaluatedRapSourcePath{};
    mutable std::optional<::llvc::EditorSnapshot> m_cachedEffectiveExportPlanSnapshot{};
    mutable hstring m_cachedEffectiveExportPlanSourcePath{};
    mutable std::optional<::llvc::EffectiveExportPlan> m_cachedEffectiveExportPlan{};
    winrt::Microsoft::UI::Xaml::Window::Closed_revoker m_mainWindowClosedRevoker{};

private:
    enum class ExportOverlayStage{
        Rap,
        Video,
        Audio,
        Finalize,
    };
    DTS m_exportEtaTimer{nullptr};
    std::optional<ExportOverlayStage> m_activeExportStage{};
    std::optional<double> m_activeExportStageProgress{};
    std::chrono::steady_clock::time_point m_activeExportStageStartedAt{};
    std::array<bool, 4> m_exportStageActive{};
    std::array<std::optional<double>, 4> m_exportStageProgress{};

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
    void removeRecentPath(const hstring& path);
    AAction openFromLaunchArgumentsAsync(const hstring& arguments);
    AAction openProjectFileAsync(const SFile& file);
    AAction saveProjectFileAsync(const SFile& file);
    void resetProjectState();
    std::wstring currentProjectDisplayName() const;
    void updateWindowTitle();
    IOpBool ensureProjectSavedBeforeContinuingAsync();
    static MediaInspectionResult inspectMediaFile(const wstring& filePath);
    static wstring guidToCodecName(const _GUID& subtype, bool isVideo);
    AAction loadVideoFileAsync(const SFile& file);
    AAction showOptionsDialogAsync();
    fire_and_forget renderTimelineAsync();
    void updateTimelineCursorFromPlayback();
    void updateTimelineCursorFromPosition(int64_t position100ns);
    int64_t currentNavigationTime100ns();
    void syncTimelineHorizontalScrollBar();
    void updateTimelineCursorFromViewportOffset(double offset);
    void renderTimelineTicks();
    void seekTimelineToCanvasX(double pointerX);
    void renderKeyframeTicks();
    void renderCutOverlays();
    std::optional<int64_t> timelinePointToTime100ns(double pointerX, double width) const;
    bool toggleSelectedKeyframeAtTime100ns(int64_t time100ns);
    bool evaluatePlacedMarkerAtTime100ns(int64_t time100ns);
    fire_and_forget toggleSelectedKeyframeAtTime100nsAsync(int64_t time100ns);
    bool toggleCutBlockAtTime100ns(int64_t time100ns);
    bool setCutBlockAtTime100ns(int64_t time100ns, bool cutScene);
    bool toggleCutMarkerAtCursor();
    bool markSceneAtCursor(bool cutScene);
    bool nudgeCurrentSceneBoundaryToNearestRap(bool expandScene);
    bool trySkipCurrentCutDuringPlayback();
    void stepByFrame(int delta);
    bool seekBySeconds(int deltaSeconds);
    bool seekBySeconds(double deltaSeconds);
    bool jumpToTimelinePercent(uint32_t percent);
    bool moveCursorToMarker(int direction);
    void ensureTimelineCursorVisible(double cursorLeft);
    void ensureCurrentTimelineCursorVisible();
    void tryFocusTimelineCanvas(const FState focusState);
    bool handleStorylineKeyDown(const KRArgs& args);
    std::optional<::llvc::EffectiveExportPlan> tryBuildEffectiveExportPlan(const std::function<void(double)>& progressCallback = {}) const;
    bool tryGetRapTimes100ns(vector<int64_t>& rapTimes100ns) const;
    bool reevaluateClearCutMarkers(bool pushUndoState);
    bool reevaluateAll(bool pushUndoState);
    void queueRapLookup(bool queueReevaluate, int nudgeDirection);
    IOpBool ensureRapMarkersAvailableAsync(const wstring& statusMessage, const std::function<void(double)>& progressCallback = {});
    fire_and_forget runRapLookupAsync();
    void clearUndoRedoHistory();
    bool undoLastEdit();
    bool redoLastEdit();
    void applyEditorCommandResult(const ::llvc::EditorCommandResult& result);
    void refreshEditorUiState();
    void updateAudioUiAndPlaybackState();
    void setVideoDetailsPanelExpanded(bool expanded);
    void refreshVideoDetailsPanel();
    static wstring buildEstimatedOutputText(
        const ::llvc::Project& project,
        const MediaInspectionResult& mediaInfo,
        bool hasTimelineRenderCompleted,
        bool isRapLookupInProgress,
        bool cachedRapLookupAttempted,
        bool cachedRapLookupSucceeded,
        const std::optional<::llvc::EffectiveExportPlan>& effectivePlan);
    wstring buildSourcePropertiesText() const;
    void applyAudioSettingsToPlayer();
    void syncAudioCrossfadeComboSelection();
    void updatePreviewPlaceholderVisibility();
    void setStatusMessage(const wstring& message);
    void setErrorMessage(const wstring& message);
    void clearErrorMessage();
    void refreshStatusInfoSection();
    IOpBool confirmAdjustedExportPlanAsync(const ::llvc::EffectiveExportPlan& plan);
    void configureExportOverlay(const wstring& outputPath, int64_t sourceDuration100ns, int64_t outputDuration100ns, uint64_t sourceSizeBytes, bool adjustedPlan, size_t cutBlockCount, const ::llvc::EffectiveExportPlan* effectivePlan = nullptr);
    void setExportOverlayStageState(ExportOverlayStage stage, const wstring& label, std::optional<double> progressPercent, bool active);
    void setExportOverlayStagePending(ExportOverlayStage stage);
    void setExportOverlayStageSkipped(ExportOverlayStage stage, const wstring& reason);
    void setExportOverlayStageComplete(ExportOverlayStage stage, const wstring& label = L"Done");
    void updateExportOverlayStatus();
    void updateExportOverlayActionButtons();
    void ensureExportEtaTimer();
    void stopExportEtaTimer();
    void exportEtaTimer_Tick(const Control& sender, const Control& args);
    void updateExportStageEta();
    std::chrono::seconds estimateStageDuration(ExportOverlayStage stage) const;
    static const wchar_t* exportStageDisplayName(ExportOverlayStage stage) noexcept;
    AAction deleteExportArtifactsAsync(bool deleteSource, bool deleteProject);
    AAction promptDeleteSourceAndProjectAfterExportAsync(const std::wstring& exportedPath);
    void setOperationInProgress(bool active, bool indeterminate = false);
    void setOperationProgress(double percent);
    void refreshExportEta(double percent);
    static std::wstring formatRemainingDurationText(std::chrono::seconds remaining);
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
    void resetProjectStateImpl();
    bool handleStorylineKeyDownImpl(const KRArgs& args);
    wstring buildSourcePropertiesTextImpl() const;
    static TS secondsToTimeSpan(double seconds);
};

}

namespace winrt::llvc::factory_implementation{

struct MainWindow: MainWindowT<MainWindow, implementation::MainWindow>{};

}
