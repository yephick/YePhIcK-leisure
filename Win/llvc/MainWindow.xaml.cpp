#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"
#include "MainWindow.Helpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <functional>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

#include <microsoft.ui.xaml.window.h>
#include <shobjidl_core.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Editing.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>

#pragma comment(lib, "Shell32.lib")

using namespace std;
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Input;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Input;
using namespace Windows::Foundation;
using namespace Windows::Media::Playback;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;

namespace winrt::llvc::implementation{

using Control = MainWindow::Control;
using REArgs = MainWindow::REArgs;
using PREArgs = MainWindow::PREArgs;
using AAction = MainWindow::AAction;
using RBVArgs = MainWindow::RBVArgs;
using SVVCArgs = MainWindow::SVVCArgs;
using SCArgs = MainWindow::SCArgs;
using KRArgs = MainWindow::KRArgs;
using DEArgs = MainWindow::DEArgs;
using WEArgs = MainWindow::WEArgs;
using MPSession = MainWindow::MPSession;
using SFile = MainWindow::SFile;
using FState = MainWindow::FState;
using IOpBool = MainWindow::IOpBool;
using TS = MainWindow::TS;

constexpr auto W_POS_L{L"WindowLeft"};
constexpr auto W_POS_T{L"WindowTop"};
constexpr auto W_POS_W{L"WindowWidth"};
constexpr auto W_POS_H{L"WindowHeight"};
constexpr auto W_POS_DPI{L"WindowDpi"};
constexpr auto S_RECENT_VIDEOS{L"RecentVideos"};
constexpr auto S_RECENT_PROJECTS{L"RecentProjects"};
constexpr auto S_MAX_RECENT_VIDEOS{L"MaxRecentVideos"};
constexpr auto S_MAX_RECENT_PROJECTS{L"MaxRecentProjects"};
constexpr auto S_DEFAULT_MAX_RECENT{5};

constexpr std::array<int32_t, 8> AUDIO_CROSSFADE_PRESETS_MS{{0, 10, 50, 100, 250, 500, 750, 1000}};

bool isControlModifierActive(const Windows::System::VirtualKeyModifiers modifiers){
    if((modifiers & Windows::System::VirtualKeyModifiers::Control) == Windows::System::VirtualKeyModifiers::Control){
        return true;
    }

    const auto ctrlState{Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(Windows::System::VirtualKey::Control)};
    return (ctrlState & Windows::UI::Core::CoreVirtualKeyStates::Down) == Windows::UI::Core::CoreVirtualKeyStates::Down;
}

std::uint32_t estimateFrameNumberFromTime100ns(const std::wstring& frameRateText, const std::int64_t time100ns){
    try{
        const auto fps{std::stod(frameRateText)};
        if(fps > 0){
            return static_cast<std::uint32_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(std::llround((time100ns / 10'000'000.0) * fps))));
        }
    }catch(...){
    }
    return 0;
}

std::wstring formatDurationFileTag(const std::int64_t duration100ns){
    const auto totalMs{std::max<std::int64_t>(0, (duration100ns + 5'000) / 10'000)};
    const auto minutes{totalMs / 60'000};
    const auto seconds{(totalMs / 1'000) % 60};
    const auto millis{totalMs % 1'000};
    return std::format(L"{:02}{:02}{:03}", minutes, seconds, millis);
}

std::vector<std::int64_t> buildSceneBoundaries100ns(const std::vector<IndexedFrameSample>& markers, const std::int64_t totalDuration100ns){
    std::vector<std::int64_t> boundaries;
    boundaries.reserve(markers.size() + 2);
    boundaries.push_back(0);
    for(const auto& marker : markers){
        boundaries.push_back(std::clamp(marker.time100ns, static_cast<std::int64_t>(0), totalDuration100ns));
    }
    boundaries.push_back(totalDuration100ns);
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    if(boundaries.empty() || boundaries.front() != 0){
        boundaries.insert(boundaries.begin(), 0);
    }
    if(boundaries.back() != totalDuration100ns){
        boundaries.push_back(totalDuration100ns);
    }
    return boundaries;
}

std::vector<std::pair<std::int64_t, std::int64_t>> buildCutRanges100ns(
    const std::vector<std::uint32_t>& cutScenes,
    const std::vector<IndexedFrameSample>& markers,
    const std::int64_t totalDuration100ns){

    const auto boundaries{buildSceneBoundaries100ns(markers, totalDuration100ns)};
    if(boundaries.size() < 2){
        return {};
    }

    const auto sceneCount{boundaries.size() - 1};
    std::vector<std::pair<std::int64_t, std::int64_t>> ranges;
    for(const auto sceneIndex : cutScenes){
        if(sceneIndex >= sceneCount){
            continue;
        }
        const auto start{boundaries[sceneIndex]};
        const auto end{boundaries[sceneIndex + 1]};
        if(end > start){
            ranges.emplace_back(start, end);
        }
    }

    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<std::int64_t, std::int64_t>> merged;
    for(const auto& range : ranges){
        if(merged.empty() || range.first > merged.back().second){
            merged.push_back(range);
        }else{
            merged.back().second = std::max(merged.back().second, range.second);
        }
    }
    return merged;
}

std::vector<std::pair<std::int64_t, std::int64_t>> buildEffectiveCutRangesWithRapPreroll(
    const std::vector<std::uint32_t>& cutScenes,
    const std::vector<IndexedFrameSample>& markers,
    const std::int64_t totalDuration100ns,
    const std::vector<std::int64_t>& rapTimes100ns){

    const auto boundaries{buildSceneBoundaries100ns(markers, totalDuration100ns)};
    if(boundaries.size() < 2){
        return {};
    }

    const auto sceneCount{boundaries.size() - 1};
    std::vector<bool> isCut(sceneCount, false);
    for(const auto scene : cutScenes){
        if(scene < sceneCount){
            isCut[scene] = true;
        }
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> keepRanges;
    for(size_t i = 0; i < sceneCount; ++i){
        if(isCut[i]){
            continue;
        }
        auto start{boundaries[i]};
        const auto end{boundaries[i + 1]};
        const auto rapIt{std::upper_bound(rapTimes100ns.begin(), rapTimes100ns.end(), start)};
        if(rapIt != rapTimes100ns.begin()){
            start = *(rapIt - 1);
        }
        if(end > start){
            keepRanges.emplace_back(start, end);
        }
    }

    if(keepRanges.empty()){
        return {{0, totalDuration100ns}};
    }

    std::sort(keepRanges.begin(), keepRanges.end());
    std::vector<std::pair<std::int64_t, std::int64_t>> mergedKeep;
    for(const auto& range : keepRanges){
        if(mergedKeep.empty() || range.first > mergedKeep.back().second){
            mergedKeep.push_back(range);
        }else{
            mergedKeep.back().second = std::max(mergedKeep.back().second, range.second);
        }
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> cutRanges;
    std::int64_t cursor{};
    for(const auto& [start, end] : mergedKeep){
        if(start > cursor){
            cutRanges.emplace_back(cursor, start);
        }
        cursor = std::max(cursor, end);
    }
    if(cursor < totalDuration100ns){
        cutRanges.emplace_back(cursor, totalDuration100ns);
    }

    return cutRanges;
}

std::int64_t removedDurationBefore(const std::vector<std::pair<std::int64_t, std::int64_t>>& ranges, const std::int64_t time100ns){
    std::int64_t removed{};
    for(const auto& [start, end] : ranges){
        if(time100ns <= start){
            break;
        }
        removed += std::min(time100ns, end) - start;
        if(time100ns < end){
            break;
        }
    }
    return removed;
}

std::uint32_t getNalLengthFieldSize(const com_ptr<IMFMediaType>& mediaType, const GUID& subtype){
    UINT8* configData{};
    UINT32 configSize{};
    if(FAILED(mediaType->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &configData, &configSize)) || !configData || configSize == 0){
        return 4;
    }

    if(subtype == MFVideoFormat_H264){
        if(configSize >= 5){
            const auto result{static_cast<std::uint32_t>((configData[4] & 0x03) + 1)};
            CoTaskMemFree(configData);
            return result;
        }
    }else if(subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265){
        if(configSize >= 22){
            const auto result{static_cast<std::uint32_t>((configData[21] & 0x03) + 1)};
            CoTaskMemFree(configData);
            return result;
        }
    }

    CoTaskMemFree(configData);
    return 4;
}

bool isTrueRandomAccessPointSample(const com_ptr<IMFSample>& sample, const GUID& subtype, const std::uint32_t nalLengthFieldSize, const bool allowInconclusive){
    if(subtype != MFVideoFormat_H264 && subtype != MFVideoFormat_HEVC && subtype != MFVideoFormat_H265){
        return true;
    }

    com_ptr<IMFMediaBuffer> contiguousBuffer;
    if(FAILED(sample->ConvertToContiguousBuffer(contiguousBuffer.put())) || !contiguousBuffer){
        return allowInconclusive; // parsing unavailable
    }

    BYTE* data{};
    DWORD maxLength{};
    DWORD currentLength{};
    if(FAILED(contiguousBuffer->Lock(&data, &maxLength, &currentLength)) || !data || currentLength == 0){
        return allowInconclusive;
    }

    auto classifyNalType = [&](const std::uint8_t nalHeader) -> std::optional<bool>{
        if(subtype == MFVideoFormat_H264){
            const auto nalType = static_cast<std::uint8_t>(nalHeader & 0x1F);
            if(nalType >= 1 && nalType <= 5){
                return nalType == 5; // IDR only
            }
        }else{
            const auto nalType = static_cast<std::uint8_t>((nalHeader >> 1) & 0x3F);
            if(nalType <= 31){
                return nalType == 19 || nalType == 20 || nalType == 21; // HEVC IDR/CRA
            }
        }
        return std::nullopt;
    };

    // Try MP4/MOV length-prefixed NAL units first.
    {
        const auto nalSizeField = std::clamp<std::uint32_t>(nalLengthFieldSize, 1, 4);
        size_t offset{};
        while(offset + nalSizeField <= currentLength){
            std::uint32_t nalLength{};
            for(std::uint32_t i = 0; i < nalSizeField; ++i){
                nalLength = (nalLength << 8) | data[offset + i];
            }
            offset += nalSizeField;

            if(nalLength == 0 || offset + nalLength > currentLength){
                break;
            }

            if(const auto maybeRap = classifyNalType(data[offset]); maybeRap.has_value()){
                const auto result{maybeRap.value()};
                contiguousBuffer->Unlock();
                return result;
            }

            offset += nalLength;
        }
    }

    // Fallback: try Annex B start-code format.
    for(size_t i = 0; i + 4 < currentLength; ++i){
        size_t nalStart{};
        if(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1){
            nalStart = i + 3;
        }else if(i + 4 < currentLength && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1){
            nalStart = i + 4;
        }else{
            continue;
        }

        if(nalStart >= currentLength){
            continue;
        }

        if(const auto maybeRap = classifyNalType(data[nalStart]); maybeRap.has_value()){
            const auto result{maybeRap.value()};
            contiguousBuffer->Unlock();
            return result;
        }
    }

    contiguousBuffer->Unlock();
    return allowInconclusive; // inconclusive parsing
}

bool isContainerSyncSample(const com_ptr<IMFSample>& sample){
    UINT32 cleanPoint{};
    return SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0;
}

MainWindow::MainWindow(){
    InitializeComponent();

    m_player = MediaPlayer();
    PreviewPlayer().SetMediaPlayer(m_player);

    m_naturalDurationChangedRevoker = m_player.PlaybackSession().NaturalDurationChanged(auto_revoke, {this, &MainWindow::onNaturalDurationChanged});

    m_positionTimer = DispatcherTimer();
    m_positionTimer.Interval(chrono::milliseconds(80));
    m_positionTimer.Tick({this, &MainWindow::onPositionTimerTick});
    m_positionTimer.Start();

    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();

    restoreWindowPlacement();
    loadAppSettings();
    Closed({this, &MainWindow::onClosed});
    refreshRecentVideosMenu();
    refreshRecentProjectsMenu();
    m_lastSavedProjectSnapshot = buildProjectSnapshot();
    updateWindowTitle();
}

HWND MainWindow::getWindowHandle() const{
    HWND hwnd{};
    const auto projected{const_cast<MainWindow*>(this)->get_strong()};
    check_hresult(projected.as<::IWindowNative>()->get_WindowHandle(&hwnd));
    return hwnd;
}


auto pixelsToDips(const int32_t pixelValue, const uint32_t dpi){
    return static_cast<int32_t>(lround((pixelValue * 96.0) / (dpi == 0 ? 96 : dpi)));
}

auto dipsToPixels(const int32_t dipValue, const uint32_t dpi){
    return static_cast<int32_t>(lround((dipValue * (dpi == 0 ? 96U : dpi)) / 96));
}

bool MainWindow::isRectVisibleOnAnyMonitor(const RECT& rect){
    const auto monitor{::MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)};
    if(!monitor){
        return false;
    }

    MONITORINFO monitorInfo{.cbSize = sizeof(monitorInfo)};
    if(!::GetMonitorInfoW(monitor, &monitorInfo)){
        return false;
    }

    RECT intersection{};
    return ::IntersectRect(&intersection, &rect, &monitorInfo.rcWork) != FALSE;
}

void MainWindow::restoreWindowPlacement(){
    const auto localSettings{ApplicationData::Current().LocalSettings()};
    const auto values{localSettings.Values()};

    if(!values.HasKey(W_POS_L) || !values.HasKey(W_POS_T) || !values.HasKey(W_POS_W) || !values.HasKey(W_POS_H)){
        return;
    }

    const auto left{unbox_value<int32_t>(values.Lookup(W_POS_L))};
    const auto top{unbox_value<int32_t>(values.Lookup(W_POS_T))};

    const auto hwnd{getWindowHandle()};

    SetWindowPos(hwnd, nullptr, left, top, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);

    const auto currentDpi{::GetDpiForWindow(hwnd)};
    const auto hasDpiData{values.HasKey(W_POS_DPI)};
    const auto storedWidth{unbox_value<int32_t>(values.Lookup(W_POS_W))};
    const auto storedHeight{unbox_value<int32_t>(values.Lookup(W_POS_H))};
    const auto width{hasDpiData ? dipsToPixels(storedWidth, currentDpi) : storedWidth};
    const auto height{hasDpiData ? dipsToPixels(storedHeight, currentDpi) : storedHeight};

    if(!isRectVisibleOnAnyMonitor(RECT{left, top, left + width, top + height})){
        values.Remove(W_POS_L);
        values.Remove(W_POS_T);
        values.Remove(W_POS_W);
        values.Remove(W_POS_H);
        values.Remove(W_POS_DPI);
        return;
    }

    SetWindowPos(hwnd, nullptr, left, top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
}

void MainWindow::loadAppSettings(){
    const auto values{ApplicationData::Current().LocalSettings().Values()};

    m_maxRecentVideos = S_DEFAULT_MAX_RECENT;
    m_maxRecentProjects = S_DEFAULT_MAX_RECENT;

    if(values.HasKey(S_MAX_RECENT_VIDEOS)){
        const auto parsed{unbox_value<int32_t>(values.Lookup(S_MAX_RECENT_VIDEOS))};
        m_maxRecentVideos = static_cast<uint32_t>(clamp(parsed, 1, 20));
    }
    if(values.HasKey(S_MAX_RECENT_PROJECTS)){
        const auto parsed{unbox_value<int32_t>(values.Lookup(S_MAX_RECENT_PROJECTS))};
        m_maxRecentProjects = static_cast<uint32_t>(clamp(parsed, 1, 20));
    }

    if(values.HasKey(S_RECENT_VIDEOS)){
        m_recentVideos = splitRecentItems(unbox_value<hstring>(values.Lookup(S_RECENT_VIDEOS)).c_str());
    }
    if(values.HasKey(S_RECENT_PROJECTS)){
        m_recentProjects = splitRecentItems(unbox_value<hstring>(values.Lookup(S_RECENT_PROJECTS)).c_str());
    }
    if(m_recentVideos.size() > m_maxRecentVideos){
        m_recentVideos.resize(m_maxRecentVideos);
    }
    if(m_recentProjects.size() > m_maxRecentProjects){
        m_recentProjects.resize(m_maxRecentProjects);
    }
}

void MainWindow::saveAppSettings() const{
    const auto values{ApplicationData::Current().LocalSettings().Values()};
    values.Insert(S_MAX_RECENT_VIDEOS, box_value(static_cast<int32_t>(m_maxRecentVideos)));
    values.Insert(S_MAX_RECENT_PROJECTS, box_value(static_cast<int32_t>(m_maxRecentProjects)));
    values.Insert(S_RECENT_VIDEOS, box_value(hstring(joinRecentItems(m_recentVideos))));
    values.Insert(S_RECENT_PROJECTS, box_value(hstring(joinRecentItems(m_recentProjects))));
}

void MainWindow::saveWindowPlacement() const{
    const auto hwnd{getWindowHandle()};

    RECT bounds{};
    if(!GetWindowRect(hwnd, &bounds)){
        return;
    }

    WINDOWPLACEMENT placement{.length = sizeof(placement)};
    if(GetWindowPlacement(hwnd, &placement) && placement.showCmd == SW_SHOWMAXIMIZED){
        bounds = placement.rcNormalPosition;
    }

    const auto localSettings{ApplicationData::Current().LocalSettings()};
    const auto values{localSettings.Values()};
    const auto dpi{::GetDpiForWindow(hwnd)};
    values.Insert(W_POS_L, box_value(static_cast<int32_t>(bounds.left)));
    values.Insert(W_POS_T, box_value(static_cast<int32_t>(bounds.top)));
    values.Insert(W_POS_W, box_value(pixelsToDips(static_cast<int32_t>(bounds.right - bounds.left), dpi)));
    values.Insert(W_POS_H, box_value(pixelsToDips(static_cast<int32_t>(bounds.bottom - bounds.top), dpi)));
    values.Insert(W_POS_DPI, box_value(static_cast<int32_t>(dpi)));
}

void MainWindow::onClosed(const Control&, const WEArgs&){
    m_isClosing = true;

    if(m_positionTimer){
        m_positionTimer.Stop();
    }

    m_naturalDurationChangedRevoker.revoke();
    saveWindowPlacement();
    saveAppSettings();
}

void MainWindow::startButton_Click(const Control&, const REArgs&){
    if(m_player){
        applyAudioSettingsToPlayer();
        m_player.Play();
    }
}

void MainWindow::pauseButton_Click(const Control&, const REArgs&){
    if(m_player){
        m_player.Pause();
    }
}

void MainWindow::stopButton_Click(const Control&, const REArgs&){
    if(m_player){
        m_player.Pause();
        m_player.PlaybackSession().Position(chrono::seconds(0));
        updateTimelineCursorFromPlayback();
    }
}

void MainWindow::timelineZoomSlider_ValueChanged(const Control&, const RBVArgs&){
    if(m_loadedFile && m_timelineDurationSeconds > 0){
        renderTimelineAsync();
    }
    updateWindowTitle();
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

void MainWindow::keepAudioCheckBox_Changed(const Control&, const REArgs&){
    m_keepAudio = KeepAudioCheckBox().IsChecked().GetBoolean();
    updateAudioUiAndPlaybackState();
    updateWindowTitle();
}

void MainWindow::audioCrossfadeComboBox_SelectionChanged(const Control&, const Control&){
    const auto selected{AudioCrossfadeComboBox().SelectedItem().try_as<Controls::ComboBoxItem>()};
    if(!selected){
        return;
    }

    if(!selected.Tag()){
        return;
    }

    try{
        const auto tag{unbox_value<hstring>(selected.Tag())};
        m_audioCrossfadeMs = normalizeAudioCrossfadeMs(stoi(wstring(tag.c_str())));
    }catch(...){
        m_audioCrossfadeMs = 0;
    }

    syncAudioCrossfadeComboSelection();
    updateWindowTitle();
}

void MainWindow::timelineHorizontalScrollBar_ValueChanged(const Control&, const RBVArgs& args){
    if(m_isClosing){
        return;
    }

    const auto scrollViewer{TimelineScrollViewer()};
    const auto offset{scrollViewer.HorizontalOffset()};
    if(fabs(offset - args.NewValue()) > 0.5){
        const auto targetOffset{box_value(args.NewValue()).as<IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }
}

void MainWindow::timelineScrollViewer_ViewChanged(const Control&, const SVVCArgs&){
    syncTimelineHorizontalScrollBar();
}

void MainWindow::timelineScrollViewer_SizeChanged(const Control&, const SCArgs&){
    syncTimelineHorizontalScrollBar();
}

void MainWindow::timelineCanvas_PointerPressed(const Control&, const PREArgs& e){
    if(m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    if(!point.Properties().IsLeftButtonPressed()){
        return;
    }

    m_isTimelineDragging = true;
    m_timelineDragMoved = false;
    m_timelineDragPointerId = e.Pointer().PointerId();
    m_timelineDragStartX = static_cast<double>(point.Position().X);
    m_timelineDragStartOffset = TimelineScrollViewer().HorizontalOffset();

    tryFocusTimelineCanvas(FocusState::Programmatic);
    TimelineCanvas().CapturePointer(e.Pointer());
    e.Handled(true);
}

void MainWindow::timelineCanvas_PointerMoved(const Control&, const PREArgs& e){
    if(!m_isTimelineDragging || e.Pointer().PointerId() != m_timelineDragPointerId){
        return;
    }

    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    const auto deltaX{point.Position().X - m_timelineDragStartX};

    if(fabs(deltaX) > 4.0){
        m_timelineDragMoved = true;
    }

    const auto scrollViewer{TimelineScrollViewer()};
    const auto viewportWidth{scrollViewer.ViewportWidth()};
    const auto maxOffset{max(0.0, TimelineCanvas().Width() - viewportWidth)};
    const auto target{clamp(m_timelineDragStartOffset - deltaX, 0.0, maxOffset)};
    const auto targetOffset{box_value(target).as<IReference<double>>()};
    scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    e.Handled(true);
}

bool MainWindow::toggleSelectedKeyframeAtCanvasX(const double pointerX){
    if(m_timelineDurationSeconds <= 0 || TimelineTickCanvas().Width() <= 0){
        return false;
    }

    constexpr auto hitTolerancePx{4.0};
    const auto width{TimelineTickCanvas().Width()};
    const auto clicked100ns{static_cast<int64_t>(clamp(pointerX, 0.0, width) / width * (m_timelineDurationSeconds * 10'000'000.0))};

    auto nearestIndex{m_frameIndex.size()};
    double nearestDistance{hitTolerancePx + 1.0};
    for(size_t i = 0; i < m_frameIndex.size(); ++i){
        const auto x{clamp((static_cast<double>(m_frameIndex[i].time100ns) / (m_timelineDurationSeconds * 10'000'000.0)) * width, 0.0, width)};
        const auto distance{fabs(pointerX - x)};
        if(distance <= hitTolerancePx && distance < nearestDistance){
            nearestDistance = distance;
            nearestIndex = i;
        }
    }

    if(nearestIndex != m_frameIndex.size()){
        const auto existing{static_cast<uint32_t>(nearestIndex)};
        const auto it{find(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end(), existing)};
        if(it == m_selectedKeyFrames.end()){
            m_selectedKeyFrames.push_back(existing);
        }else{
            m_selectedKeyFrames.erase(it);
        }
    }else{
        const auto insertIt{lower_bound(m_frameIndex.begin(), m_frameIndex.end(), clicked100ns, [](const IndexedFrameSample& a, int64_t t){
            return a.time100ns < t;
        })};
        const auto insertPos{static_cast<uint32_t>(distance(m_frameIndex.begin(), insertIt))};

        for(auto& selected : m_selectedKeyFrames){
            if(selected >= insertPos){
                ++selected;
            }
        }

        std::vector<uint32_t> updatedCuts;
        updatedCuts.reserve(m_cutScenes.size() + 1);
        const auto splitScene{insertPos};
        for(const auto sceneIndex : m_cutScenes){
            if(sceneIndex < splitScene){
                updatedCuts.push_back(sceneIndex);
            }else if(sceneIndex == splitScene){
                updatedCuts.push_back(sceneIndex);
                updatedCuts.push_back(sceneIndex + 1);
            }else{
                updatedCuts.push_back(sceneIndex + 1);
            }
        }
        sort(updatedCuts.begin(), updatedCuts.end());
        updatedCuts.erase(unique(updatedCuts.begin(), updatedCuts.end()), updatedCuts.end());
        m_cutScenes = std::move(updatedCuts);

        const auto frameNumber{estimateFrameNumberFromTime100ns(m_mediaInfo.frameRate, clicked100ns)};
        m_frameIndex.insert(insertIt, IndexedFrameSample{.time100ns=clicked100ns, .duration100ns=0, .cleanPoint=true, .sampleIndex=frameNumber});
        m_selectedKeyFrames.push_back(insertPos);
    }

    sort(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end());
    m_selectedKeyFrames.erase(unique(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end()), m_selectedKeyFrames.end());

    renderTimelineTicks();
    renderKeyframeTicks();
    renderCutOverlays();
    updateWindowTitle();
    return true;
}


void MainWindow::timelineCanvas_PointerReleased(const Control&, const PREArgs& e){
    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    if(point.Properties().PointerUpdateKind() == PointerUpdateKind::RightButtonReleased){
        if(toggleSelectedKeyframeAtCanvasX(point.Position().X)){
            e.Handled(true);
            return;
        }
    }

    if(!m_isTimelineDragging || e.Pointer().PointerId() != m_timelineDragPointerId){
        return;
    }
    const auto dragged{m_timelineDragMoved};

    m_isTimelineDragging = false;
    m_timelineDragMoved = false;
    TimelineCanvas().ReleasePointerCapture(e.Pointer());

    if(!dragged){
        const auto modifiers{e.KeyModifiers()};
        const auto ctrlPressed{isControlModifierActive(modifiers)};
        if(ctrlPressed){
            toggleCutBlockAtCanvasX(point.Position().X);
        }else{
            seekTimelineToCanvasX(point.Position().X, (modifiers & Windows::System::VirtualKeyModifiers::Shift) == Windows::System::VirtualKeyModifiers::Shift);
        }
    }

    e.Handled(true);
}

void MainWindow::timelineCanvas_PointerCanceled(const Control&, const PREArgs& e){
    if(m_isTimelineDragging && e.Pointer().PointerId() == m_timelineDragPointerId){
        m_isTimelineDragging = false;
        m_timelineDragMoved = false;
        TimelineCanvas().ReleasePointerCapture(e.Pointer());
        e.Handled(true);
    }
}

void MainWindow::timelineCanvas_PointerCaptureLost(const Control&, const PREArgs&){
    m_isTimelineDragging = false;
    m_timelineDragMoved = false;
}

void MainWindow::timelineCanvas_Loaded(const Control&, const REArgs&){
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

void MainWindow::timelineTickCanvas_PointerReleased(const Control&, const PREArgs& e){
    const auto point{e.GetCurrentPoint(TimelineTickCanvas())};
    if(point.Properties().PointerUpdateKind() == PointerUpdateKind::RightButtonReleased && toggleSelectedKeyframeAtCanvasX(point.Position().X)){
        tryFocusTimelineCanvas(FocusState::Programmatic);
        e.Handled(true);
        return;
    }

    if(point.Properties().PointerUpdateKind() == PointerUpdateKind::LeftButtonReleased){
        if(isControlModifierActive(e.KeyModifiers())){
            if(toggleCutBlockAtCanvasX(point.Position().X)){
                tryFocusTimelineCanvas(FocusState::Programmatic);
                e.Handled(true);
                return;
            }
        }
    }
}


void MainWindow::onNaturalDurationChanged(const MPSession& sender, const Control&){
    const auto duration{sender.NaturalDuration()};
    m_timelineDurationSeconds = max(0.0, duration.count() / 10'000'000.0);

    if(m_loadedFile && m_timelineDurationSeconds > 0){
        const auto weak{get_weak()};
        if(DispatcherQueue().HasThreadAccess()){
            renderTimelineAsync();
            return;
        }

        DispatcherQueue().TryEnqueue([weak](){
            if(const auto self{weak.get()}){
                self->renderTimelineAsync();
            }
        });
    }
}

void MainWindow::onPositionTimerTick(const Control&, const Control&){
    if(m_isClosing){
        return;
    }

    (void)trySkipCurrentCutDuringPlayback();
    updateTimelineCursorFromPlayback();
}

void MainWindow::updateTimelineCursorFromPlayback(){
    if(m_isClosing || !m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto current{m_player.PlaybackSession().Position()};
    const auto seconds{max(0.0, current.count() / 10'000'000.0)};
    const auto ratio{clamp(seconds / m_timelineDurationSeconds, 0.0, 1.0)};
    const auto left{ratio * TimelineCanvas().Width()};
    Controls::Canvas::SetLeft(TimelineCursor(), left);

    if(m_player.PlaybackSession().PlaybackState() == MediaPlaybackState::Playing){
        ensureTimelineCursorVisible(left);
    }

    syncTimelineHorizontalScrollBar();
}

void MainWindow::syncTimelineHorizontalScrollBar(){
    const auto scrollViewer{TimelineScrollViewer()};
    scrollViewer.UpdateLayout();

    const auto viewportWidth{max(1.0, scrollViewer.ViewportWidth())};
    const auto extentWidth{max(viewportWidth, scrollViewer.ExtentWidth())};
    const auto scrollableWidth{max(0.0, extentWidth - viewportWidth)};

    auto bar{TimelineHorizontalScrollBar()};
    bar.Minimum(0.0);
    bar.Maximum(max(1.0, scrollableWidth));
    bar.LargeChange(max(32.0, viewportWidth * 0.8));
    bar.SmallChange(24.0);
    bar.IsEnabled(scrollableWidth > 0.0);
    bar.Visibility(Visibility::Visible);

    const auto currentValue{bar.Value()};
    const auto offset{clamp(scrollViewer.HorizontalOffset(), 0.0, bar.Maximum())};
    if(fabs(currentValue - offset) > 0.5){
        bar.Value(offset);
    }
}

void MainWindow::seekTimelineToCanvasX(const double pointerX, const bool){
    if(!m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto x{clamp(pointerX, 0.0, TimelineCanvas().Width())};
    const auto ratio{x / TimelineCanvas().Width()};
    const auto target100ns{static_cast<int64_t>(ratio * (m_timelineDurationSeconds * 10'000'000.0))};

    m_player.PlaybackSession().Position(TimeSpan{target100ns});
    updateTimelineCursorFromPlayback();
}

void MainWindow::ensureTimelineCursorVisible(const double cursorLeft){
    const auto scrollViewer{TimelineScrollViewer()};
    const auto currentOffset{scrollViewer.HorizontalOffset()};
    const auto viewportWidth{scrollViewer.ViewportWidth()};

    if(viewportWidth <= 0){
        return;
    }

    constexpr auto cursorPadding{48.0};
    const auto minVisible{currentOffset + cursorPadding};
    const auto maxVisible{currentOffset + viewportWidth - cursorPadding};
    const auto maxOffset{max(0.0, TimelineCanvas().Width() - viewportWidth)};

    if(cursorLeft < minVisible){
        const auto targetOffset{box_value(clamp(cursorLeft - cursorPadding, 0.0, maxOffset)).as<IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }else if(cursorLeft > maxVisible){
        const auto targetOffset{box_value(clamp(cursorLeft + cursorPadding - viewportWidth, 0.0, maxOffset)).as<IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }
}

void MainWindow::renderTimelineTicks(){
    TimelineTickCanvas().Children().Clear();

    const auto width{TimelineCanvas().Width()};
    TimelineTickCanvas().Width(width);
    if(m_timelineDurationSeconds <= 0 || width <= 0){
        return;
    }

    const auto majorTickCount{clamp(static_cast<int>(ceil(width / 120.0)), 6, 36)};

    for(int i = 0; i <= majorTickCount; ++i){
        const auto ratio{static_cast<double>(i) / majorTickCount};
        const auto x{ratio * width};

        Shapes::Line majorTick{};
        majorTick.X1(x);
        majorTick.X2(x);
        majorTick.Y1(7);
        majorTick.Y2(23);
        majorTick.Stroke(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 180, 180, 180)));
        majorTick.StrokeThickness(1.0);
        TimelineTickCanvas().Children().Append(majorTick);

        Controls::TextBlock label{};
        const auto totalSeconds{static_cast<int>(round(ratio * m_timelineDurationSeconds))};
        const auto minutes{totalSeconds / 60};
        const auto seconds{totalSeconds % 60};
        auto text{to_wstring(minutes)};
        text += L":";
        if(seconds < 10){
            text += L"0";
        }
        text += to_wstring(seconds);
        label.Text(text);
        label.FontSize(11);
        label.Foreground(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 200, 200, 200)));
        Controls::Canvas::SetLeft(label, max(0.0, x + 3.0));
        Controls::Canvas::SetTop(label, 0);
        TimelineTickCanvas().Children().Append(label);
    }
}

void MainWindow::renderKeyframeTicks(){
    if(m_frameIndex.empty() || m_timelineDurationSeconds <= 0 || TimelineTickCanvas().Width() <= 0){
        return;
    }

    const auto width {TimelineTickCanvas().Width()};
    const auto total100ns {static_cast<double>(m_timelineDurationSeconds * 10'000'000.0)};
    uint32_t cleanOrdinal{};
    for(const auto& frame: m_frameIndex){
        if(!frame.cleanPoint){
            continue;
        }

        const auto x {clamp((frame.time100ns / total100ns) * width, 0.0, width)};
        const auto isSelected{find(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end(), cleanOrdinal) != m_selectedKeyFrames.end()};

        Shapes::Line tick{};
        tick.X1(x);
        tick.X2(x);
        tick.Y1(0);
        tick.Y2(isSelected ? 8.0 : 5.0);
        tick.Stroke(Media::SolidColorBrush(isSelected
            ? Windows::UI::ColorHelper::FromArgb(255, 255, 80, 80)
            : Windows::UI::ColorHelper::FromArgb(255, 80, 200, 255)));
        tick.StrokeThickness(isSelected ? 2.0 : 1.0);
        TimelineTickCanvas().Children().Append(tick);
        ++cleanOrdinal;
    }
}

void MainWindow::renderCutOverlays(){
    CutOverlayLayer().Children().Clear();

    const auto width{TimelineCanvas().Width()};
    CutOverlayLayer().Width(width);
    if(m_timelineDurationSeconds <= 0 || width <= 0){
        return;
    }

    const auto duration100ns{static_cast<int64_t>(m_timelineDurationSeconds * 10'000'000.0)};
    const auto cutRanges100ns{buildCutRanges100ns(m_cutScenes, m_frameIndex, duration100ns)};
    const auto overlayColor {Windows::UI::ColorHelper::FromArgb(180, 0, 0, 0)};
    for(const auto& [startTime100ns, endTime100ns] : cutRanges100ns){
        const auto start{clamp((static_cast<double>(startTime100ns) / 10'000'000.0) / m_timelineDurationSeconds, 0.0, 1.0)};
        const auto end{clamp((static_cast<double>(endTime100ns) / 10'000'000.0) / m_timelineDurationSeconds, 0.0, 1.0)};
        if(end <= start){
            continue;
        }

        Shapes::Rectangle block{};
        const auto left{start * width};
        block.Width(max(1.0, (end - start) * width));
        block.Height(86.0);
        block.Fill(Media::SolidColorBrush(overlayColor));
        block.IsHitTestVisible(false);
        Controls::Canvas::SetLeft(block, left);
        Controls::Canvas::SetTop(block, 0.0);
        CutOverlayLayer().Children().Append(block);
    }
}


bool MainWindow::toggleCutBlockAtCanvasX(const double pointerX){
    if(m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return false;
    }

    const auto width{TimelineCanvas().Width()};
    const auto clicked100ns{static_cast<int64_t>(clamp(pointerX, 0.0, width) / width * (m_timelineDurationSeconds * 10'000'000.0))};
    const auto boundaries{buildSceneBoundaries100ns(m_frameIndex, static_cast<int64_t>(m_timelineDurationSeconds * 10'000'000.0))};
    if(boundaries.size() < 2){
        return false;
    }

    auto sceneIndex{static_cast<uint32_t>(boundaries.size() - 2)};
    for(size_t i = 0; i + 1 < boundaries.size(); ++i){
        if(clicked100ns < boundaries[i + 1]){
            sceneIndex = static_cast<uint32_t>(i);
            break;
        }
    }

    const auto it{find(m_cutScenes.begin(), m_cutScenes.end(), sceneIndex)};
    if(it == m_cutScenes.end()){
        m_cutScenes.push_back(sceneIndex);
        sort(m_cutScenes.begin(), m_cutScenes.end());
    }else{
        m_cutScenes.erase(it);
    }

    renderCutOverlays();
    updateWindowTitle();
    return true;
}


bool MainWindow::trySkipCurrentCutDuringPlayback(){
    if(!m_player || m_cutScenes.empty() || m_timelineDurationSeconds <= 0){
        return false;
    }

    const auto state{m_player.PlaybackSession().PlaybackState()};
    if(state != MediaPlaybackState::Playing){
        return false;
    }

    const auto duration100ns{static_cast<int64_t>(m_timelineDurationSeconds * 10'000'000.0)};
    const auto cutRanges100ns{buildCutRanges100ns(m_cutScenes, m_frameIndex, duration100ns)};
    const auto now100ns{max<int64_t>(0, m_player.PlaybackSession().Position().count())};
    for(const auto& [start100ns, end100ns] : cutRanges100ns){
        if(now100ns >= start100ns && now100ns < end100ns){
            m_player.PlaybackSession().Position(TimeSpan{end100ns});
            return true;
        }
    }

    return false;
}


void MainWindow::stepByFrame(const int delta){
    if(!m_player || m_frameIndex.empty() || delta == 0){
        return;
    }

    const auto current {m_player.PlaybackSession().Position().count()};
    constexpr auto fallbackFrameDuration100ns{333'667LL}; // ~29.97 fps

    vector<int64_t> frameDurations;
    frameDurations.reserve(m_frameIndex.size());
    for(const auto& sample: m_frameIndex){
        if(sample.duration100ns > 0){
            frameDurations.push_back(sample.duration100ns);
        }
    }

    if(frameDurations.empty()){
        for(size_t i = 1; i < m_frameIndex.size(); ++i){
            const auto sampleCount {static_cast<int64_t>(m_frameIndex[i].sampleIndex) - static_cast<int64_t>(m_frameIndex[i - 1].sampleIndex)};
            const auto timeDelta {m_frameIndex[i].time100ns - m_frameIndex[i - 1].time100ns};
            if(sampleCount > 0 && timeDelta > 0){
                frameDurations.push_back(timeDelta / sampleCount);
            }
        }
    }

    int64_t frameStep100ns{fallbackFrameDuration100ns};
    if(!frameDurations.empty()){
        nth_element(frameDurations.begin(), frameDurations.begin() + static_cast<ptrdiff_t>(frameDurations.size() / 2), frameDurations.end());
        frameStep100ns = max<int64_t>(1, frameDurations[frameDurations.size() / 2]);
    }

    const auto direction {delta < 0 ? static_cast<int64_t>(-1) : static_cast<int64_t>(1)};
    const auto duration100ns {static_cast<int64_t>(max(0.0, m_timelineDurationSeconds) * 10'000'000.0)};
    const auto target {clamp(current + (direction * frameStep100ns), static_cast<int64_t>(0), duration100ns)};
    m_player.PlaybackSession().Position(TimeSpan{target});
    updateTimelineCursorFromPlayback();
}

void MainWindow::tryFocusTimelineCanvas(const FState focusState){
    const auto canvas{TimelineCanvas()};
    if(canvas && canvas.XamlRoot()){
        canvas.Focus(focusState);
    }
}

bool MainWindow::handleStorylineKeyDown(const KRArgs& args){
    const auto focused{Input::FocusManager::GetFocusedElement(Content().XamlRoot()).try_as<DependencyObject>()};
    const auto focusOnMenu{focused && isInMenuSubtree(focused)};
    const auto focusInDialog{focused && isInDialogSubtree(focused)};

    if(args.Key() == Windows::System::VirtualKey::Menu || args.Key() == Windows::System::VirtualKey::LeftMenu || args.Key() == Windows::System::VirtualKey::RightMenu){
        MainMenuBar().Focus(FocusState::Keyboard);
        args.Handled(true);
        return true;
    }

    if(args.Key() == Windows::System::VirtualKey::Tab && !focusOnMenu && !focusInDialog){
        tryFocusTimelineCanvas(FocusState::Programmatic);
        args.Handled(true);
        return true;
    }

    if(focusOnMenu || focusInDialog){
        return false;
    }

    tryFocusTimelineCanvas(FocusState::Programmatic);

    const auto ctrlState{Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(Windows::System::VirtualKey::Control)};
    const auto ctrlDown{(ctrlState & Windows::UI::Core::CoreVirtualKeyStates::Down) == Windows::UI::Core::CoreVirtualKeyStates::Down};

    if(ctrlDown){
        if(args.Key() == Windows::System::VirtualKey::O){
            (void)openProjectMenuItem_Click(nullptr, RoutedEventArgs{});
            args.Handled(true);
            return true;
        }
        if(args.Key() == Windows::System::VirtualKey::S){
            (void)saveProjectMenuItem_Click(nullptr, RoutedEventArgs{});
            args.Handled(true);
            return true;
        }
        if(args.Key() == Windows::System::VirtualKey::N){
            (void)newProjectMenuItem_Click(nullptr, RoutedEventArgs{});
            args.Handled(true);
            return true;
        }
    }

    switch(args.Key()){
    case Windows::System::VirtualKey::Space:
        if(m_player){
            const auto state {m_player.PlaybackSession().PlaybackState()};
            if(state == MediaPlaybackState::Playing){
                m_player.Pause();
            }else{
                m_player.Play();
            }
        }
        args.Handled(true);
        return true;
    case Windows::System::VirtualKey::Left:
        stepByFrame(-1);
        args.Handled(true);
        return true;
    case Windows::System::VirtualKey::Right:
        stepByFrame(1);
        args.Handled(true);
        return true;
    default:
        return false;
    }
}

void MainWindow::window_PreviewKeyDown(const Control&, const KRArgs& args){
    (void)handleStorylineKeyDown(args);
}

void MainWindow::window_KeyDown(const Control&, const KRArgs& args){
    (void)handleStorylineKeyDown(args);
}

void MainWindow::rootGrid_PointerReleased(const Control&, const PREArgs& args){
    const auto source {args.OriginalSource().try_as<DependencyObject>()};
    if(!source){
        return;
    }
    if(isInMenuSubtree(source) || isInDialogSubtree(source)){
        return;
    }
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

TS MainWindow::secondsToTimeSpan(const double seconds){
    return chrono::duration_cast<TimeSpan>(chrono::duration<double>(seconds));
}

AAction MainWindow::newProjectMenuItem_Click(const Control&, const REArgs&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    resetProjectState(true);
    StatusText().Text(L"New project created");
}

AAction MainWindow::openProjectMenuItem_Click(const Control&, const REArgs&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    FileOpenPicker picker{};
    picker.FileTypeFilter().Append(L".llvc");
    picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);

    auto initWithWindow{picker.as<IInitializeWithWindow>()};
    check_hresult(initWithWindow->Initialize(getWindowHandle()));

    if(const auto file{co_await picker.PickSingleFileAsync()}){
        co_await openProjectFileAsync(file);
    }
}

AAction MainWindow::saveProjectMenuItem_Click(const Control&, const REArgs&){
    StorageFile target{nullptr};

    if(!m_projectPath.empty()){
        try{
            target = co_await StorageFile::GetFileFromPathAsync(m_projectPath);
        }catch(...){
            target = nullptr;
        }
    }

    if(!target){
        FileSavePicker picker{};
        picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
        picker.FileTypeChoices().Insert(L"llvc project", single_threaded_vector<hstring>({L".llvc"}));
        picker.SuggestedFileName(L"project");

        auto initWithWindow{picker.as<IInitializeWithWindow>()};
        check_hresult(initWithWindow->Initialize(getWindowHandle()));

        target = co_await picker.PickSaveFileAsync();
        if(!target){
            co_return;
        }
    }

    co_await saveProjectFileAsync(target);
}

AAction MainWindow::saveProjectAsMenuItem_Click(const Control&, const REArgs&){
    FileSavePicker picker{};
    picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
    picker.FileTypeChoices().Insert(L"llvc project", single_threaded_vector<hstring>({L".llvc"}));
    picker.SuggestedFileName(L"project");

    auto initWithWindow{picker.as<IInitializeWithWindow>()};
    check_hresult(initWithWindow->Initialize(getWindowHandle()));

    const auto target{co_await picker.PickSaveFileAsync()};
    if(!target){
        co_return;
    }

    co_await saveProjectFileAsync(target);
}

AAction MainWindow::closeProjectMenuItem_Click(const Control&, const REArgs&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    resetProjectState(true);
    StatusText().Text(L"Project closed");
}

AAction MainWindow::loadVideoMenuItem_Click(const Control&, const REArgs&){
    co_await pickAndLoadVideoAsync();
}

AAction MainWindow::exportVideoMenuItem_Click(const Control&, const REArgs&){
    if(!m_loadedFile){
        co_await showInfoDialogAsync(L"Export video", L"Load a video before exporting.");
        co_return;
    }

    MFLifetime mf{};

    const std::wstring sourcePath{m_loadedFile.Path().c_str()};
    const auto sourceDuration100ns{std::max<std::int64_t>(0, static_cast<std::int64_t>(std::llround(std::max(0.0, m_timelineDurationSeconds) * 10'000'000.0)))};
    const auto cutRanges100ns{buildCutRanges100ns(m_cutScenes, m_frameIndex, sourceDuration100ns)};

    std::int64_t removedTotal100ns{};
    for(const auto& [start, end] : cutRanges100ns){
        removedTotal100ns += (end - start);
    }
    const auto outputDuration100ns{std::max<std::int64_t>(0, sourceDuration100ns - removedTotal100ns)};

    const filesystem::path sourceFsPath{sourcePath};
    const auto sourceExt{sourceFsPath.extension().wstring()};
    const auto defaultExt{_wcsicmp(sourceExt.c_str(), L".mov") == 0 ? L".mov" : L".mp4"};

    std::wstring outputPath{};

    {
        com_ptr<IFileSaveDialog> saveDialog;
        check_hresult(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(saveDialog.put())));

        DWORD options{};
        check_hresult(saveDialog->GetOptions(&options));
        check_hresult(saveDialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT));

        const COMDLG_FILTERSPEC fileTypes[]{
            {L"MP4 video", L"*.mp4"},
            {L"MOV video", L"*.mov"},
        };
        check_hresult(saveDialog->SetFileTypes(static_cast<UINT>(std::size(fileTypes)), fileTypes));

        const auto isMovDefault{_wcsicmp(defaultExt, L".mov") == 0};
        check_hresult(saveDialog->SetFileTypeIndex(isMovDefault ? 2U : 1U));
        check_hresult(saveDialog->SetDefaultExtension(isMovDefault ? L"mov" : L"mp4"));

        const auto suggestedName{sourceFsPath.stem().wstring() + L" - " + formatDurationFileTag(outputDuration100ns)};
        check_hresult(saveDialog->SetFileName(suggestedName.c_str()));

        const auto sourceFolder{sourceFsPath.parent_path().wstring()};
        if(!sourceFolder.empty()){
            com_ptr<IShellItem> folderItem;
            if(SUCCEEDED(SHCreateItemFromParsingName(sourceFolder.c_str(), nullptr, IID_PPV_ARGS(folderItem.put())))){
                (void)saveDialog->SetDefaultFolder(folderItem.get());
                (void)saveDialog->SetFolder(folderItem.get());
            }
        }

        const auto showHr{saveDialog->Show(getWindowHandle())};
        if(showHr == HRESULT_FROM_WIN32(ERROR_CANCELLED)){
            co_return;
        }
        check_hresult(showHr);

        com_ptr<IShellItem> resultItem;
        check_hresult(saveDialog->GetResult(resultItem.put()));
        PWSTR selectedPath{};
        check_hresult(resultItem->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath));
        outputPath = selectedPath ? selectedPath : L"";
        if(selectedPath){
            ::CoTaskMemFree(selectedPath);
        }
    }

    winrt::hstring exportErrorMessage{};

    try{
        StatusText().Text(L"Exporting...");

        com_ptr<IMFAttributes> readerAttributes;
        check_hresult(MFCreateAttributes(readerAttributes.put(), 1));
        check_hresult(readerAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE));

        com_ptr<IMFSourceReader> reader;
        check_hresult(MFCreateSourceReaderFromURL(sourcePath.c_str(), readerAttributes.get(), reader.put()));

        constexpr auto invalidStream{numeric_limits<DWORD>::max()};
        auto videoStreamIndex{invalidStream};
        for(DWORD streamIndex = 0;; ++streamIndex){
            com_ptr<IMFMediaType> type;
            const auto hr{reader->GetNativeMediaType(streamIndex, 0, type.put())};
            if(hr == MF_E_INVALIDSTREAMNUMBER){
                break;
            }
            check_hresult(hr);

            GUID major{GUID_NULL};
            check_hresult(type->GetGUID(MF_MT_MAJOR_TYPE, &major));
            if(major == MFMediaType_Video){
                videoStreamIndex = streamIndex;
                break;
            }
        }

        if(videoStreamIndex == invalidStream){
            throw hresult_error(MF_E_INVALIDMEDIATYPE, L"No video stream found");
        }

        check_hresult(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
        check_hresult(reader->SetStreamSelection(videoStreamIndex, TRUE));

        com_ptr<IMFMediaType> sourceVideoType;
        check_hresult(reader->GetNativeMediaType(videoStreamIndex, 0, sourceVideoType.put()));
        check_hresult(reader->SetCurrentMediaType(videoStreamIndex, nullptr, sourceVideoType.get()));

        GUID videoSubtype{GUID_NULL};
        check_hresult(sourceVideoType->GetGUID(MF_MT_SUBTYPE, &videoSubtype));
        const auto nalLengthFieldSize{getNalLengthFieldSize(sourceVideoType, videoSubtype)};

        std::vector<std::int64_t> rapTimes100ns;
        rapTimes100ns.reserve(2048);
        for(;;){
            DWORD actualStream{};
            DWORD flags{};
            LONGLONG timestamp{};
            com_ptr<IMFSample> sample;
            const auto hr{reader->ReadSample(videoStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put())};
            if(FAILED(hr)){
                check_hresult(hr);
            }
            if(flags & MF_SOURCE_READERF_ENDOFSTREAM){
                break;
            }
            if(!sample){
                continue;
            }

            LONGLONG sampleTime100ns{};
            if(FAILED(sample->GetSampleTime(&sampleTime100ns))){
                sampleTime100ns = timestamp;
            }

            if(isContainerSyncSample(sample) && isTrueRandomAccessPointSample(sample, videoSubtype, nalLengthFieldSize, false)){
                rapTimes100ns.push_back(std::max<std::int64_t>(0, sampleTime100ns));
            }
        }
        std::sort(rapTimes100ns.begin(), rapTimes100ns.end());
        rapTimes100ns.erase(std::unique(rapTimes100ns.begin(), rapTimes100ns.end()), rapTimes100ns.end());

        const auto effectiveCutRanges100ns{buildEffectiveCutRangesWithRapPreroll(m_cutScenes, m_frameIndex, sourceDuration100ns, rapTimes100ns)};

        PROPVARIANT startPos{};
        startPos.vt = VT_I8;
        startPos.hVal.QuadPart = 0;
        check_hresult(reader->SetCurrentPosition(GUID_NULL, startPos));
        PropVariantClear(&startPos);

        com_ptr<IMFSinkWriter> writer;
        check_hresult(MFCreateSinkWriterFromURL(outputPath.c_str(), nullptr, nullptr, writer.put()));

        DWORD writerVideoStreamIndex{};
        check_hresult(writer->AddStream(sourceVideoType.get(), &writerVideoStreamIndex));
        check_hresult(writer->SetInputMediaType(writerVideoStreamIndex, sourceVideoType.get(), nullptr));
        check_hresult(writer->BeginWriting());

        auto waitingForCleanPoint{false};
        auto markDiscontinuityOnNextWrittenSample{false};

        for(;;){
            DWORD actualStream{};
            DWORD flags{};
            LONGLONG timestamp{};
            com_ptr<IMFSample> sample;
            const auto hr{reader->ReadSample(videoStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put())};
            if(FAILED(hr)){
                check_hresult(hr);
            }
            if(flags & MF_SOURCE_READERF_ENDOFSTREAM){
                break;
            }
            if(!sample){
                continue;
            }

            LONGLONG sampleTime100ns{};
            if(FAILED(sample->GetSampleTime(&sampleTime100ns))){
                sampleTime100ns = timestamp;
            }
            const auto inTime100ns{std::max<std::int64_t>(0, sampleTime100ns)};
            auto dropped{false};
            for(const auto& [start, end] : effectiveCutRanges100ns){
                if(inTime100ns < start){
                    break;
                }
                if(inTime100ns < end){
                    dropped = true;
                    break;
                }
            }
            if(dropped){
                waitingForCleanPoint = true;
                markDiscontinuityOnNextWrittenSample = true;
                continue;
            }

            if(waitingForCleanPoint){
                const auto isContainerSync{isContainerSyncSample(sample)};
                const auto isBitstreamRap{isTrueRandomAccessPointSample(sample, videoSubtype, nalLengthFieldSize, false)};
                if(!(isContainerSync && isBitstreamRap)){
                    continue;
                }
                waitingForCleanPoint = false;
            }

            const auto outTime100ns{inTime100ns - removedDurationBefore(effectiveCutRanges100ns, inTime100ns)};
            check_hresult(sample->SetSampleTime(outTime100ns));

            UINT64 decodeTimestamp100ns{};
            if(SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &decodeTimestamp100ns))){
                const auto decodeTimeSigned{static_cast<std::int64_t>(decodeTimestamp100ns)};
                const auto outDecodeTime100ns{decodeTimeSigned - removedDurationBefore(effectiveCutRanges100ns, decodeTimeSigned)};
                check_hresult(sample->SetUINT64(MFSampleExtension_DecodeTimestamp, static_cast<UINT64>(std::max<std::int64_t>(0, outDecodeTime100ns))));
            }

            LONGLONG duration100ns{};
            if(SUCCEEDED(sample->GetSampleDuration(&duration100ns)) && duration100ns > 0){
                check_hresult(sample->SetSampleDuration(duration100ns));
            }

            if(markDiscontinuityOnNextWrittenSample){
                check_hresult(sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE));
                markDiscontinuityOnNextWrittenSample = false;
            }

            check_hresult(writer->WriteSample(writerVideoStreamIndex, sample.get()));
        }

        check_hresult(writer->Finalize());
        StatusText().Text(L"Export completed");
    }catch(const hresult_error& ex){
        StatusText().Text(L"Export failed");
        exportErrorMessage = ex.message();
    }

    if(!exportErrorMessage.empty()){
        co_await showInfoDialogAsync(L"Export failed", exportErrorMessage);
    }
}

AAction MainWindow::recentVideoMenuItem_Click(const Control& sender, const REArgs&){
    const auto item{sender.try_as<Controls::MenuFlyoutItem>()};
    if(!item || !item.Tag()){
        co_return;
    }

    auto openFailed{false};
    const auto path{unbox_value<hstring>(item.Tag())};
    try{
        const auto file{co_await StorageFile::GetFileFromPathAsync(path)};
        co_await loadVideoFileAsync(file);
    }catch(const winrt::hresult_error&){
        openFailed = true;
    }

    if(openFailed){
        co_await showInfoDialogAsync(L"Open failed", L"Could not open selected recent video.");
    }
}

AAction MainWindow::recentProjectMenuItem_Click(const Control& sender, const REArgs&){
    const auto item{sender.try_as<Controls::MenuFlyoutItem>()};
    if(!item || !item.Tag()){
        co_return;
    }

    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    auto openFailed{false};
    const auto path{unbox_value<hstring>(item.Tag())};
    try{
        const auto file{co_await StorageFile::GetFileFromPathAsync(path)};
        co_await openProjectFileAsync(file);
    }catch(...){
        openFailed = true;
    }

    if(openFailed){
        co_await showInfoDialogAsync(L"Open failed", L"Could not open selected recent project.");
    }
}

AAction MainWindow::propertiesMenuItem_Click(const Control&, const REArgs&){
    co_await showPropertiesDialogAsync();
}

AAction MainWindow::exitMenuItem_Click(const Control&, const REArgs&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    Close();
}

AAction MainWindow::aboutMenuItem_Click(const Control&, const REArgs&){
    co_await showInfoDialogAsync(L"About llvc", L"llvc - Lossless Video Cut\nPreview and timeline exploration tool.");
}

AAction MainWindow::optionsMenuItem_Click(const Control&, const REArgs&){
    co_await showOptionsDialogAsync();
}

AAction MainWindow::pickAndLoadVideoAsync(){
    FileOpenPicker picker{};
    picker.SuggestedStartLocation(PickerLocationId::VideosLibrary);
    picker.FileTypeFilter().Append(L".mp4");
    picker.FileTypeFilter().Append(L".mov");

    const auto hwnd{getWindowHandle()};
    auto initWithWindow{picker.as<IInitializeWithWindow>()};
    check_hresult(initWithWindow->Initialize(hwnd));

    if(const auto file{co_await picker.PickSingleFileAsync()}){
        co_await loadVideoFileAsync(file);
    }
}

void MainWindow::refreshRecentVideosMenu(){
    auto menu{RecentVideosMenu()};
    menu.Items().Clear();

    if(m_recentVideos.empty()){
        Controls::MenuFlyoutItem empty{};
        empty.Text(L"(none)");
        empty.IsEnabled(false);
        menu.Items().Append(empty);
        return;
    }

    for(const auto& path : m_recentVideos){
        Controls::MenuFlyoutItem item{};
        item.Text(path);
        item.Tag(box_value(path));
        item.Click({this, &MainWindow::recentVideoMenuItem_Click});
        menu.Items().Append(item);
    }
}

void MainWindow::refreshRecentProjectsMenu(){
    auto menu{RecentProjectsMenu()};
    menu.Items().Clear();

    if(m_recentProjects.empty()){
        Controls::MenuFlyoutItem empty{};
        empty.Text(L"(none)");
        empty.IsEnabled(false);
        menu.Items().Append(empty);
        return;
    }

    for(const auto& path : m_recentProjects){
        Controls::MenuFlyoutItem item{};
        item.Text(path);
        item.Tag(box_value(path));
        item.Click({this, &MainWindow::recentProjectMenuItem_Click});
        menu.Items().Append(item);
    }
}

void MainWindow::addRecentVideo(const hstring& path){
    if(path.empty()){
        return;
    }

    m_recentVideos.erase(remove(m_recentVideos.begin(), m_recentVideos.end(), path), m_recentVideos.end());
    m_recentVideos.insert(m_recentVideos.begin(), path);
    if(m_recentVideos.size() > m_maxRecentVideos){
        m_recentVideos.resize(m_maxRecentVideos);
    }
    refreshRecentVideosMenu();
    saveAppSettings();
}

void MainWindow::addRecentProject(const hstring& path){
    if(path.empty()){
        return;
    }

    m_recentProjects.erase(remove(m_recentProjects.begin(), m_recentProjects.end(), path), m_recentProjects.end());
    m_recentProjects.insert(m_recentProjects.begin(), path);
    if(m_recentProjects.size() > m_maxRecentProjects){
        m_recentProjects.resize(m_maxRecentProjects);
    }
    refreshRecentProjectsMenu();
    saveAppSettings();
}

void MainWindow::resetProjectState(const bool clearLoadedVideo){
    ++m_timelineRenderVersion;
    m_projectPath.clear();
    m_projectUnknownLines.clear();
    m_selectedKeyFrames.clear();
    m_cutScenes.clear();
    m_frameIndex.clear();
    m_mediaInfo = MediaInspectionResult{};
    m_timelineDurationSeconds = 0;
    m_keepAudio = true;
    m_audioCrossfadeMs = 0;
    TimelineZoomSlider().Value(3);
    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();

    ThumbnailLayer().Children().Clear();
    CutOverlayLayer().Children().Clear();
    TimelineTickCanvas().Children().Clear();
    KeyframeProgressBar().Value(0);
    KeyframeProgressBar().Visibility(Visibility::Collapsed);
    TimelineCanvas().Width(640.0);
    TimelineTickCanvas().Width(640.0);
    Controls::Canvas::SetLeft(TimelineCursor(), 0);
    syncTimelineHorizontalScrollBar();

    if(clearLoadedVideo){
        if(m_player){
            m_player.Pause();
        }
        m_loadedFile = nullptr;
        m_player.Source(nullptr);
    }

    StatusText().Text(L"Load or drag-and-drop a .mp4/.mov file to preview.");
    m_lastSavedProjectSnapshot = buildProjectSnapshot();
    updateWindowTitle();
}

wstring MainWindow::buildProjectSnapshot(){
    auto snapshot{std::format(
        L"file_path={}\nstoryline_zoom={:.15g}\nkeep_audio={}\naudio_crossfade_ms={}\nmarker_indices={}\ncuts_scenes={}\nmarker_points={}\n",
        (m_loadedFile ? m_loadedFile.Path().c_str() : L""),
        TimelineZoomSlider().Value(),
        m_keepAudio ? 1 : 0,
        m_audioCrossfadeMs,
        serializeIndexList(m_selectedKeyFrames),
        serializeIndexList(m_cutScenes),
        serializeKeyframeVector(m_frameIndex))};
    return snapshot;
}

bool MainWindow::isProjectDirty(){
    return buildProjectSnapshot() != m_lastSavedProjectSnapshot;
}

void MainWindow::updateWindowTitle(){
    wstring projectName{L"Untitled"};
    if(!m_projectPath.empty()){
        const auto projectPath{filesystem::path(m_projectPath.c_str())};
        const auto stem{projectPath.stem().wstring()};
        projectName = stem.empty() ? projectPath.filename().wstring() : stem;
        if(projectName.empty()){
            projectName = L"Untitled";
        }
    }

    if(isProjectDirty()){
        projectName += L"*";
    }

    const wstring loadedFile{m_loadedFile ? m_loadedFile.Path().c_str() : L"No file"};
    Title(hstring(std::format(L"llvc - Lossless Video Cut - {} - {}", projectName, loadedFile)));
}

IOpBool MainWindow::ensureProjectSavedBeforeContinuingAsync(){
    if(!isProjectDirty()){
        co_return true;
    }

    Controls::ContentDialog dialog{};
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(L"Unsaved changes"));
    dialog.Content(box_value(L"Current project has unsaved changes. Save before continuing?"));
    dialog.PrimaryButtonText(L"Save");
    dialog.SecondaryButtonText(L"Don't save");
    dialog.CloseButtonText(L"Cancel");

    const auto choice{co_await dialog.ShowAsync()};
    if(choice == Controls::ContentDialogResult::Primary){
        co_await saveProjectMenuItem_Click(nullptr, RoutedEventArgs{});
        co_return !isProjectDirty();
    }
    if(choice == Controls::ContentDialogResult::Secondary){
        co_return true;
    }

    co_return false;
}

AAction MainWindow::openProjectFileAsync(const SFile& file){
    resetProjectState(true);

    const auto lines{co_await FileIO::ReadLinesAsync(file)};

    vector<uint32_t> selectedMarkerIndices;
    vector<uint32_t> cutScenes;
    wstring loadedFilePath;
    auto zoomLevel{TimelineZoomSlider().Value()};
    auto keepAudioSetting{m_keepAudio};
    auto audioCrossfadeSetting{m_audioCrossfadeMs};
    vector<IndexedFrameSample> loadedMarkers;

    for(const auto& lineH: lines){
        const wstring line{lineH.c_str()};
        const auto trimmed{trim(line)};
        if(trimmed.empty()){
            continue;
        }

        if(trimmed[0] == L'#'){
            continue;
        }

        const auto eqPos{line.find(L'=')};
        if(eqPos == wstring::npos){
            continue;
        }

        const auto key{trim(line.substr(0, eqPos))};
        const auto value{trim(line.substr(eqPos + 1))};

        if(key == L"file_path"){
            loadedFilePath = value;
        }else if(key == L"storyline_zoom"){
            try{ zoomLevel = stod(value); }catch(...){ }
        }else if(key == L"marker_indices"){
            selectedMarkerIndices = parseIndexList(value);
        }else if(key == L"cuts_scenes"){
            cutScenes = parseIndexList(value);
        }else if(key == L"keep_audio"){
            keepAudioSetting = !(value == L"0" || value == L"false" || value == L"False");
        }else if(key == L"audio_crossfade_ms"){
            try{ audioCrossfadeSetting = normalizeAudioCrossfadeMs(stoi(value)); }catch(...){ }
        }else if(key == L"marker_points"){
            loadedMarkers = parseKeyframeVector(value);
        }else{
            continue;
        }
    }

    if(!loadedFilePath.empty()){
        try{
            const auto videoFile{co_await StorageFile::GetFileFromPathAsync(loadedFilePath)};
            co_await loadVideoFileAsync(videoFile);
        }catch(...){
            StatusText().Text(L"Project opened, but referenced video could not be loaded");
        }
    }

    zoomLevel = clamp(zoomLevel, TimelineZoomSlider().Minimum(), TimelineZoomSlider().Maximum());
    TimelineZoomSlider().Value(zoomLevel);
    m_keepAudio = keepAudioSetting;
    m_audioCrossfadeMs = normalizeAudioCrossfadeMs(audioCrossfadeSetting);
    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();

    m_frameIndex = loadedMarkers;
    sort(m_frameIndex.begin(), m_frameIndex.end(), [](const IndexedFrameSample& a, const IndexedFrameSample& b){
        return a.time100ns < b.time100ns;
    });
    const auto markerCount{m_frameIndex.size()};
    m_selectedKeyFrames.clear();
    m_selectedKeyFrames.reserve(selectedMarkerIndices.size());
    for(const auto ordinal : selectedMarkerIndices){
        if(ordinal < markerCount){
            m_selectedKeyFrames.push_back(ordinal);
        }
    }
    sort(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end());
    m_selectedKeyFrames.erase(unique(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end()), m_selectedKeyFrames.end());

    m_cutScenes.clear();
    const auto sceneCount{static_cast<uint32_t>(markerCount + 1)};
    for(const auto sceneIndex : cutScenes){
        if(sceneIndex < sceneCount){
            m_cutScenes.push_back(sceneIndex);
        }
    }
    sort(m_cutScenes.begin(), m_cutScenes.end());
    m_cutScenes.erase(unique(m_cutScenes.begin(), m_cutScenes.end()), m_cutScenes.end());
    m_projectUnknownLines.clear();
    m_projectPath = file.Path();

    addRecentProject(file.Path());
    m_lastSavedProjectSnapshot = buildProjectSnapshot();
    StatusText().Text(L"Project loaded");
    updateWindowTitle();
}

AAction MainWindow::saveProjectFileAsync(const SFile& file){
    vector<hstring> lines;
    lines.emplace_back(L"# llvc project file");
    lines.emplace_back(L"file_path=" + wstring(m_loadedFile ? m_loadedFile.Path().c_str() : L""));
    lines.emplace_back(L"storyline_zoom=" + to_wstring(TimelineZoomSlider().Value()));
    lines.emplace_back(L"keep_audio=" + wstring(m_keepAudio ? L"1" : L"0"));
    lines.emplace_back(L"audio_crossfade_ms=" + to_wstring(m_audioCrossfadeMs));
    lines.emplace_back(L"marker_indices=" + serializeIndexList(m_selectedKeyFrames));
    lines.emplace_back(L"cuts_scenes=" + serializeIndexList(m_cutScenes));
    lines.emplace_back(L"marker_points=" + serializeKeyframeVector(m_frameIndex));

    co_await FileIO::WriteLinesAsync(file, single_threaded_vector<hstring>(move(lines)));
    m_projectPath = file.Path();
    addRecentProject(file.Path());
    m_lastSavedProjectSnapshot = buildProjectSnapshot();
    StatusText().Text(L"Project saved");
    updateWindowTitle();
}

AAction MainWindow::showInfoDialogAsync(const hstring& title, const hstring& message){
    Controls::ContentDialog dialog{};
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(title));
    dialog.Content(box_value(message));
    dialog.CloseButtonText(L"OK");
    co_await dialog.ShowAsync();
}

AAction MainWindow::showPropertiesDialogAsync(){
    if(!m_loadedFile || !m_mediaInfo.isValid){
        co_await showInfoDialogAsync(L"Properties", L"No video is currently loaded.");
        co_return;
    }

    wstring content;
    content += L"File: "; content += m_loadedFile.Path().c_str(); content += L"\n";
    content += L"Container: "; content += m_mediaInfo.container; content += L"\n";
    content += L"Duration: "; content += m_mediaInfo.duration; content += L"\n";
    content += L"Size: "; content += m_mediaInfo.fileSize; content += L"\n";
    content += L"Video codec: "; content += m_mediaInfo.videoCodec; content += L"\n";
    content += L"Resolution: "; content += m_mediaInfo.resolution; content += L"\n";
    content += L"FPS: "; content += m_mediaInfo.frameRate; content += L"\n";
    content += L"Video bitrate: "; content += m_mediaInfo.videoBitrate; content += L"\n";
    content += L"Random access points: "; content += m_mediaInfo.keyFrameSummary; content += L"\n";
    content += L"Random access interval: "; content += m_mediaInfo.keyFrameInterval; content += L"\n";
    content += L"All samples independent: "; content += m_mediaInfo.allSamplesIndependent; content += L"\n";
    content += L"Max random access spacing: "; content += m_mediaInfo.maxKeyFrameSpacing; content += L"\n";
    content += L"Audio codec: "; content += m_mediaInfo.audioCodec; content += L"\n";
    content += L"Audio bitrate: "; content += m_mediaInfo.audioBitrate;

    co_await showInfoDialogAsync(L"Properties", hstring(content));
}

AAction MainWindow::showOptionsDialogAsync(){
    Controls::StackPanel panel{};
    panel.Spacing(10);

    Controls::TextBlock videosLabel{};
    videosLabel.Text(L"Recent videos to keep (1-20)");
    Controls::NumberBox videosCount{};
    videosCount.Minimum(1);
    videosCount.Maximum(20);
    videosCount.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
    videosCount.Value(static_cast<double>(m_maxRecentVideos));

    Controls::TextBlock projectsLabel{};
    projectsLabel.Text(L"Recent projects to keep (1-20)");
    Controls::NumberBox projectsCount{};
    projectsCount.Minimum(1);
    projectsCount.Maximum(20);
    projectsCount.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
    projectsCount.Value(static_cast<double>(m_maxRecentProjects));

    panel.Children().Append(videosLabel);
    panel.Children().Append(videosCount);
    panel.Children().Append(projectsLabel);
    panel.Children().Append(projectsCount);

    Controls::ContentDialog dialog{};
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(L"Options"));
    dialog.Content(panel);
    dialog.PrimaryButtonText(L"Save");
    dialog.CloseButtonText(L"Cancel");

    const auto dialogResult{co_await dialog.ShowAsync()};
    if(dialogResult != Controls::ContentDialogResult::Primary){
        co_return;
    }

    m_maxRecentVideos = static_cast<uint32_t>(clamp(static_cast<int>(lround(videosCount.Value())), 1, 20));
    m_maxRecentProjects = static_cast<uint32_t>(clamp(static_cast<int>(lround(projectsCount.Value())), 1, 20));

    if(m_recentVideos.size() > m_maxRecentVideos){
        m_recentVideos.resize(m_maxRecentVideos);
    }
    if(m_recentProjects.size() > m_maxRecentProjects){
        m_recentProjects.resize(m_maxRecentProjects);
    }

    refreshRecentVideosMenu();
    refreshRecentProjectsMenu();
    saveAppSettings();
}

bool MainWindow::isSupportedVideoSubtype(const GUID& subtype){
    return subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265;
}

wstring MainWindow::guidToCodecName(const GUID& subtype, bool isVideo){
    if(isVideo){
        if(subtype == MFVideoFormat_H264){
            return L"H.264";
        }
        if(subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265){
            return L"HEVC";
        }
    }else{
        if(subtype == MFAudioFormat_AAC){
            return L"AAC";
        }
        if(subtype == MFAudioFormat_MP3){
            return L"MP3";
        }
        if(subtype == MFAudioFormat_PCM){
            return L"PCM";
        }
    }

    return formatGuid(subtype);
}

MediaInspectionResult MainWindow::inspectMediaFile(const wstring& filePath){
    MediaInspectionResult result{};
    result.keyFrameSummary = L"unknown";
    result.keyFrameInterval = L"unknown";
    result.allSamplesIndependent = L"unknown";
    result.maxKeyFrameSpacing = L"unknown";
    MFLifetime mf{};

    com_ptr<IMFSourceReader> reader;
    check_hresult(MFCreateSourceReaderFromURL(filePath.c_str(), nullptr, reader.put()));

    uint32_t videoCount{};
    uint32_t audioCount{};
    bool hasText{};
    GUID videoSubtype{GUID_NULL};
    GUID audioSubtype{GUID_NULL};
    uint32_t width{};
    uint32_t height{};
    uint32_t fpsNum{};
    uint32_t fpsDen{};
    uint32_t videoBitrate{};
    uint32_t audioBitrate{};
    constexpr auto invalidStreamIndex{numeric_limits<DWORD>::max()};
    auto videoStreamIndex{invalidStreamIndex};
    uint32_t allSamplesIndependent{};
    uint32_t maxKeyFrameSpacing{};

    for(DWORD streamIndex = 0;; ++streamIndex){
        com_ptr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(streamIndex, 0, type.put());
        if(hr == MF_E_INVALIDSTREAMNUMBER){
            break;
        }
        check_hresult(hr);

        GUID major{GUID_NULL};
        GUID subtype{GUID_NULL};
        check_hresult(type->GetGUID(MF_MT_MAJOR_TYPE, &major));
        check_hresult(type->GetGUID(MF_MT_SUBTYPE, &subtype));

        if(major == MFMediaType_Video){
            ++videoCount;
            videoSubtype = subtype;
            videoStreamIndex = streamIndex;
            MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &width, &height);
            MFGetAttributeRatio(type.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
            (void)type->GetUINT32(MF_MT_AVG_BITRATE, &videoBitrate);
            if(SUCCEEDED(type->GetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, &allSamplesIndependent))){
                result.allSamplesIndependent = allSamplesIndependent != 0 ? L"yes" : L"no";
            }
            if(SUCCEEDED(type->GetUINT32(MF_MT_MAX_KEYFRAME_SPACING, &maxKeyFrameSpacing))){
                result.maxKeyFrameSpacing = to_wstring(maxKeyFrameSpacing) + L" frames";
            }
        }else if(major == MFMediaType_Audio){
            ++audioCount;
            audioSubtype = subtype;
            (void)type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &audioBitrate);
        }else{
            hasText = true;
        }
    }

    if(videoCount != 1){
        result.errorMessage = L"Expected exactly one video stream.";
        return result;
    }
    if(audioCount > 1){
        result.errorMessage = L"Multiple audio streams are not supported.";
        return result;
    }
    if(hasText){
        result.errorMessage = L"Subtitle/text streams are not supported.";
        return result;
    }
    if(!isSupportedVideoSubtype(videoSubtype)){
        result.errorMessage = L"Video codec not supported. Only H.264 and HEVC are allowed.";
        return result;
    }

    wstring lowerPath(filePath);
    transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::towlower);
    if(lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == L".mp4"){
        result.container = L"MP4";
    }else if(lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == L".mov"){
        result.container = L"MOV";
    }else{
        result.errorMessage = L"Container not supported. Only MP4 and MOV are allowed.";
        return result;
    }

    if(videoSubtype == MFVideoFormat_HEVC || videoSubtype == MFVideoFormat_H265){
        if(!hasDecoderForSubtype(videoSubtype)){
            result.errorMessage = L"HEVC support missing (install HEVC Video Extensions)";
            return result;
        }
    }else if(!hasDecoderForSubtype(videoSubtype)){
        result.errorMessage = L"No decoder available";
        return result;
    }

    PROPVARIANT duration{};
    PropVariantInit(&duration);
    if(SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) && duration.vt == VT_UI8){
        const auto seconds {duration.uhVal.QuadPart / 10'000'000.0};
        result.duration = std::format(L"{:.3f} s", seconds);
    }
    PropVariantClear(&duration);

    result.videoCodec = guidToCodecName(videoSubtype, true);
    result.audioCodec = audioCount == 0 ? L"none" : guidToCodecName(audioSubtype, false);
    result.resolution = width > 0 ? (to_wstring(width) + L"x" + to_wstring(height)) : L"-";
    result.frameRate = (fpsNum > 0 && fpsDen > 0) ? (formatRatio(fpsNum, fpsDen) + L" fps") : L"-";
    result.videoBitrate = videoBitrate > 0 ? (to_wstring(videoBitrate / 1000) + L" kbps") : L"-";
    result.audioBitrate = audioBitrate > 0 ? (to_wstring((audioBitrate * 8) / 1000) + L" kbps") : L"none";
    if(videoStreamIndex != invalidStreamIndex){
        analyzeKeyFrameCadence(reader.get(), videoStreamIndex, fpsNum, fpsDen, result);
    }
    result.isValid = true;
    return result;
}

void MainWindow::window_DragOver(const Control&, const DEArgs& e){
    e.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
}

AAction MainWindow::window_Drop(const Control&, const DEArgs& e){
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

    const auto file{items.GetAt(0).try_as<StorageFile>()};
    if(!file){
        StatusText().Text(L"Dropped content is not a file");
        __debugbreak(); // this should have been caught earlier in this function!
        co_return;
    }

    {
        const auto ext{file.FileType()};
        wstring lower{ext.c_str()};
        transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        if(lower != L".mp4" && lower != L".mov"){
            StatusText().Text(L"Only .mp4 and .mov files are supported");
            co_return;
        }
    }

    co_await loadVideoFileAsync(file);
}

AAction MainWindow::loadVideoFileAsync(const SFile& file){
    MediaInspectionResult inspected{};
    try{
        inspected = inspectMediaFile(file.Path().c_str());
    }catch(const winrt::hresult_error& ex){
        inspected.errorMessage = L"No decoder available";
        if(ex.code() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)){
            inspected.errorMessage = L"File not found";
        }
    }

    if(!inspected.isValid){
        wstring status{L"Open rejected: "};
        status += inspected.errorMessage;
        StatusText().Text(status);
        co_await showInfoDialogAsync(L"Unsupported media", hstring(status));
        co_return;
    }

    const auto basicProperties{co_await file.GetBasicPropertiesAsync()};
    inspected.fileSize = formatFileSize(basicProperties.Size());
    m_mediaInfo = inspected;

    KeyframeProgressBar().Value(0);
    KeyframeProgressBar().Visibility(Visibility::Collapsed);
    wstring status{L"Loaded: "};
    status += file.Name().c_str();
    status += L" (loading story line...)";
    StatusText().Text(status);

    m_frameIndex.clear();

    const auto source{Windows::Media::Core::MediaSource::CreateFromStorageFile(file)};
    m_player.Source(source);
    m_player.IsMuted(false);
    m_loadedFile = file;
    if(m_projectPath.empty()){
        m_selectedKeyFrames.clear();
        m_cutScenes.clear();
    }
    addRecentVideo(file.Path());

    ThumbnailLayer().Children().Clear();
    CutOverlayLayer().Children().Clear();
    TimelineCanvas().Width(640.0);
    TimelineTickCanvas().Width(640.0);
    TimelineTickCanvas().Children().Clear();
    m_timelineDurationSeconds = 0;
    Controls::Canvas::SetLeft(TimelineCursor(), 0);
    syncTimelineHorizontalScrollBar();

    KeyframeProgressBar().Visibility(Visibility::Collapsed);

    updateAudioUiAndPlaybackState();
    updateWindowTitle();
}

bool MainWindow::sourceHasAudio() const{
    return m_mediaInfo.isValid && m_mediaInfo.audioCodec != L"none";
}

int32_t MainWindow::normalizeAudioCrossfadeMs(const int32_t valueMs){
    const auto nearest = min_element(AUDIO_CROSSFADE_PRESETS_MS.begin(), AUDIO_CROSSFADE_PRESETS_MS.end(), [valueMs](const auto a, const auto b){
        return abs(a - valueMs) < abs(b - valueMs);
    });
    return nearest == AUDIO_CROSSFADE_PRESETS_MS.end() ? 0 : *nearest;
}

void MainWindow::syncAudioCrossfadeComboSelection(){
    const auto target{normalizeAudioCrossfadeMs(m_audioCrossfadeMs)};
    m_audioCrossfadeMs = target;

    const auto combo{AudioCrossfadeComboBox()};
    const auto items{combo.Items()};
    for(uint32_t i = 0; i < items.Size(); ++i){
        const auto item{items.GetAt(i).try_as<Controls::ComboBoxItem>()};
        if(!item){
            continue;
        }

        if(!item.Tag()){
            continue;
        }

        try{
            const auto tag{unbox_value<hstring>(item.Tag())};
            if(stoi(wstring(tag.c_str())) == target){
                combo.SelectedIndex(static_cast<int32_t>(i));
                return;
            }
        }catch(...){ }
    }

    combo.SelectedIndex(0);
}

void MainWindow::applyAudioSettingsToPlayer(){
    if(!m_player){
        return;
    }

    const auto allowAudio{sourceHasAudio() && m_keepAudio};
    m_player.IsMuted(!allowAudio);
}

void MainWindow::updateAudioUiAndPlaybackState(){
    const auto hasAudio{sourceHasAudio()};
    if(!hasAudio){
        m_keepAudio = false;
    }

    KeepAudioCheckBox().IsEnabled(hasAudio);
    KeepAudioCheckBox().IsChecked(box_value(hasAudio && m_keepAudio).as<IReference<bool>>());
    AudioCrossfadeComboBox().IsEnabled(hasAudio && m_keepAudio);
    applyAudioSettingsToPlayer();
}

winrt::fire_and_forget MainWindow::renderTimelineAsync(){
    const auto lifetime{get_strong()};

    if(m_isClosing || !m_loadedFile || m_timelineDurationSeconds <= 0){
        co_return;
    }

    if(!DispatcherQueue().HasThreadAccess()){
        const auto weak{get_weak()};
        DispatcherQueue().TryEnqueue([weak](){
            if(const auto self{weak.get()}){
                self->renderTimelineAsync();
            }
        });
        co_return;
    }

    try{
        const auto renderVersion{++m_timelineRenderVersion};
        const auto zoom{TimelineZoomSlider().Value()};
        const auto totalWidth{max(800.0, m_timelineDurationSeconds * 14.0 * zoom)};
        const auto thumbnailCount{clamp(static_cast<int>(totalWidth / 150.0), 8, 96)};
        const auto thumbnailWidth{totalWidth / thumbnailCount};

        TimelineCanvas().Width(totalWidth);
        ThumbnailLayer().Children().Clear();
        CutOverlayLayer().Width(totalWidth);
        renderTimelineTicks();
        renderKeyframeTicks();
        renderCutOverlays();
        syncTimelineHorizontalScrollBar();

        const auto clip{co_await Windows::Media::Editing::MediaClip::CreateFromFileAsync(m_loadedFile)};
        Windows::Media::Editing::MediaComposition composition{};
        composition.Clips().Append(clip);

        if(renderVersion != m_timelineRenderVersion){
            co_return;
        }

        vector<bool> thumbnailBuilt(thumbnailCount, false);

        for(int builtCount = 0; builtCount < thumbnailCount; ++builtCount){
            if(renderVersion != m_timelineRenderVersion || m_isClosing){
                co_return;
            }

            const auto scrollViewer{TimelineScrollViewer()};
            const auto viewportWidth{max(0.0, scrollViewer.ViewportWidth())};
            const auto viewportLeft{scrollViewer.HorizontalOffset()};
            const auto viewportRight{viewportLeft + viewportWidth};
            const auto firstVisibleIndex{clamp(static_cast<int>(floor(viewportLeft / thumbnailWidth)), 0, thumbnailCount - 1)};
            const auto lastVisibleIndex{clamp(static_cast<int>(floor(max(viewportLeft, viewportRight - 1.0) / thumbnailWidth)), 0, thumbnailCount - 1)};

            auto nextIndex{-1};
            for(auto i{firstVisibleIndex}; i <= lastVisibleIndex; ++i){
                if(!thumbnailBuilt[i]){
                    nextIndex = i;
                    break;
                }
            }

            if(nextIndex < 0){
                auto left{firstVisibleIndex - 1};
                auto right{lastVisibleIndex + 1};
                while(nextIndex < 0 && (left >= 0 || right < thumbnailCount)){
                    if(right < thumbnailCount && !thumbnailBuilt[right]){
                        nextIndex = right;
                        break;
                    }
                    ++right;

                    if(left >= 0 && !thumbnailBuilt[left]){
                        nextIndex = left;
                        break;
                    }
                    --left;
                }
            }

            if(nextIndex < 0){
                break;
            }

            const auto t{(nextIndex + 0.5) / thumbnailCount};
            const auto stream{co_await composition.GetThumbnailAsync(secondsToTimeSpan(t * m_timelineDurationSeconds), 180, 96, Windows::Media::Editing::VideoFramePrecision::NearestFrame)};
            if(renderVersion != m_timelineRenderVersion || m_isClosing){
                co_return;
            }

            Controls::Image image{};
            image.Width(max(8.0, thumbnailWidth - 2.0));
            image.Height(86);
            image.Stretch(Media::Stretch::UniformToFill);

            Media::Imaging::BitmapImage bitmap{};
            co_await bitmap.SetSourceAsync(stream);
            image.Source(bitmap);

            Controls::Canvas::SetLeft(image, nextIndex * thumbnailWidth);
            ThumbnailLayer().Children().Append(image);
            thumbnailBuilt[static_cast<size_t>(nextIndex)] = true;
            renderCutOverlays();
        }

        updateTimelineCursorFromPlayback();
        ensureTimelineCursorVisible(Controls::Canvas::GetLeft(TimelineCursor()));
        syncTimelineHorizontalScrollBar();

        wstring status{L"Loaded: "};
        status += m_loadedFile.Name().c_str();
        status += L" (story line ready)";
        StatusText().Text(status);
    }catch(const winrt::hresult_error& ex){
        wstring status{L"Failed to render story line: "};
        status += ex.message().c_str();
        StatusText().Text(status);
    }
}

}
