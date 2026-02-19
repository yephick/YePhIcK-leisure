#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <numeric>
#include <limits>
#include <sstream>
#include <string_view>
#include <functional>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mftransform.h>
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

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

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
constexpr wchar_t RECENT_DELIMITER{0x1F};

struct MFLifetime{
    MFLifetime(){
        check_hresult(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    }
    ~MFLifetime(){
        MFShutdown();
    }
};

wstring formatGuid(GUID const& guid){
    OLECHAR raw[40]{};
    StringFromGUID2(guid, raw, static_cast<int>(size(raw)));
    return wstring(raw);
}

wstring formatFileSize(uint64_t bytes){
    wstringstream ss;
    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;
    ss << bytes << L" bytes";
    if(bytes >= static_cast<uint64_t>(GB)){
        ss << L" (" << fixed << setprecision(2) << (bytes / GB) << L" GB)";
    }else if(bytes >= static_cast<uint64_t>(MB)){
        ss << L" (" << fixed << setprecision(2) << (bytes / MB) << L" MB)";
    }
    return ss.str();
}

wstring formatRatio(uint32_t num, uint32_t den){
    if(den == 0){
        return L"-";
    }
    wstringstream ss;
    ss << fixed << setprecision(3) << (static_cast<double>(num) / den);
    return ss.str();
}

wstring joinRecentItems(vector<hstring> const& values){
    wstring out;
    for(size_t i = 0; i < values.size(); ++i){
        if(i > 0){
            out.push_back(RECENT_DELIMITER);
        }
        out += values[i].c_str();
    }
    return out;
}

vector<hstring> splitRecentItems(wstring const& source){
    vector<hstring> items;
    size_t start{};
    while(start <= source.size()){
        const auto pos = source.find(RECENT_DELIMITER, start);
        const auto len = (pos == wstring::npos) ? (source.size() - start) : (pos - start);
        if(len > 0){
            items.emplace_back(source.substr(start, len));
        }
        if(pos == wstring::npos){
            break;
        }
        start = pos + 1;
    }
    return items;
}

bool isInMenuSubtree(DependencyObject const& object){
    DependencyObject current{object};
    while(current){
        if(current.try_as<Controls::MenuBar>() || current.try_as<Controls::MenuFlyoutItem>() || current.try_as<Controls::MenuFlyoutSubItem>() || current.try_as<Controls::MenuFlyoutPresenter>()){
            return true;
        }
        current = Media::VisualTreeHelper::GetParent(current);
    }
    return false;
}

bool isInDialogSubtree(DependencyObject const& object){
    DependencyObject current{object};
    while(current){
        if(current.try_as<Controls::ContentDialog>() || current.try_as<Controls::Primitives::Popup>()){
            return true;
        }
        current = Media::VisualTreeHelper::GetParent(current);
    }
    return false;
}

wstring trim(wstring value){
    const auto first = value.find_first_not_of(L" \t\r\n");
    if(first == wstring::npos){
        return L"";
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

vector<uint32_t> parseIndexList(wstring const& text){
    vector<uint32_t> values;
    size_t start{};
    while(start <= text.size()){
        const auto pos = text.find(L',', start);
        auto token = trim(text.substr(start, pos == wstring::npos ? wstring::npos : pos - start));
        if(!token.empty()){
            try{ values.push_back(static_cast<uint32_t>(stoul(token))); }catch(...){ }
        }
        if(pos == wstring::npos){ break; }
        start = pos + 1;
    }
    return values;
}

vector<pair<uint32_t, uint32_t>> parseIndexPairs(wstring const& text){
    vector<pair<uint32_t, uint32_t>> pairs;
    size_t start{};
    while(start <= text.size()){
        const auto sep = text.find(L';', start);
        const auto chunk = trim(text.substr(start, sep == wstring::npos ? wstring::npos : sep - start));
        if(!chunk.empty()){
            const auto comma = chunk.find(L',');
            if(comma != wstring::npos){
                try{
                    const auto a = static_cast<uint32_t>(stoul(trim(chunk.substr(0, comma))));
                    const auto b = static_cast<uint32_t>(stoul(trim(chunk.substr(comma + 1))));
                    pairs.emplace_back(a, b);
                }catch(...){ }
            }
        }
        if(sep == wstring::npos){ break; }
        start = sep + 1;
    }
    return pairs;
}

wstring serializeIndexList(vector<uint32_t> const& values){
    wstringstream ss;
    for(size_t i = 0; i < values.size(); ++i){
        if(i > 0){ ss << L","; }
        ss << values[i];
    }
    return ss.str();
}

wstring serializeIndexPairs(vector<pair<uint32_t, uint32_t>> const& values){
    wstringstream ss;
    for(size_t i = 0; i < values.size(); ++i){
        if(i > 0){ ss << L";"; }
        ss << values[i].first << L"," << values[i].second;
    }
    return ss.str();
}

constexpr uint32_t TIMELINE_EDGE_SENTINEL = numeric_limits<uint32_t>::max();

vector<int64_t> buildCleanKeyframeTimes100ns(vector<IndexedFrameSample> const& index){
    vector<int64_t> times;
    times.reserve(index.size());
    for(auto const& sample : index){
        if(sample.cleanPoint){
            times.push_back(sample.time100ns);
        }
    }
    return times;
}

vector<pair<uint32_t, uint32_t>> normalizeAndMergeIndexIntervals(vector<pair<uint32_t, uint32_t>> intervals, size_t keyframeCount){
    using RankedInterval = pair<int64_t, int64_t>;
    vector<RankedInterval> normalized;
    normalized.reserve(intervals.size());

    for(auto const& interval : intervals){
        if((interval.first == TIMELINE_EDGE_SENTINEL && interval.second == TIMELINE_EDGE_SENTINEL)
            || (interval.first == TIMELINE_EDGE_SENTINEL && interval.second >= keyframeCount)
            || (interval.second == TIMELINE_EDGE_SENTINEL && interval.first >= keyframeCount)
            || (interval.first != TIMELINE_EDGE_SENTINEL && interval.second != TIMELINE_EDGE_SENTINEL && (interval.first >= keyframeCount || interval.second >= keyframeCount))){
            continue;
        }

        const auto startRank = interval.first == TIMELINE_EDGE_SENTINEL ? static_cast<int64_t>(-1) : static_cast<int64_t>(interval.first);
        const auto endRank = interval.second == TIMELINE_EDGE_SENTINEL ? static_cast<int64_t>(keyframeCount) : static_cast<int64_t>(interval.second);
        if(startRank >= endRank){
            continue;
        }
        normalized.emplace_back(startRank, endRank);
    }

    sort(normalized.begin(), normalized.end());

    vector<RankedInterval> mergedRanks;
    for(auto const& interval : normalized){
        if(mergedRanks.empty() || interval.first > mergedRanks.back().second){
            mergedRanks.push_back(interval);
        }else{
            mergedRanks.back().second = max(mergedRanks.back().second, interval.second);
        }
    }

    vector<pair<uint32_t, uint32_t>> merged;
    merged.reserve(mergedRanks.size());
    for(auto const& interval : mergedRanks){
        const auto start = interval.first < 0 ? TIMELINE_EDGE_SENTINEL : static_cast<uint32_t>(interval.first);
        const auto end = interval.second >= static_cast<int64_t>(keyframeCount) ? TIMELINE_EDGE_SENTINEL : static_cast<uint32_t>(interval.second);
        merged.emplace_back(start, end);
    }

    return merged;
}


vector<IndexedFrameSample> parseKeyframeVector(wstring const& text){
    vector<IndexedFrameSample> out;
    size_t start{};
    while(start <= text.size()){
        const auto sep = text.find(L';', start);
        const auto item = trim(text.substr(start, sep == wstring::npos ? wstring::npos : sep - start));
        if(!item.empty()){
            const auto at = item.find(L'@');
            if(at != wstring::npos){
                try{
                    const auto t = static_cast<int64_t>(stoll(trim(item.substr(0, at))));
                    const auto i = static_cast<uint32_t>(stoul(trim(item.substr(at + 1))));
                    out.push_back(IndexedFrameSample{.time100ns=t, .duration100ns=0, .cleanPoint=true, .sampleIndex=i});
                }catch(...){ }
            }
        }
        if(sep == wstring::npos){ break; }
        start = sep + 1;
    }
    return out;
}

wstring serializeKeyframeVector(vector<IndexedFrameSample> const& index){
    wstringstream ss;
    bool first{true};
    for(auto const& k : index){
        if(!k.cleanPoint){ continue; }
        if(!first){ ss << L";"; }
        first = false;
        ss << k.time100ns << L"@" << k.sampleIndex;
    }
    return ss.str();
}

bool hasDecoderForSubtype(GUID const& subtype){
    MFT_REGISTER_TYPE_INFO inType{};
    inType.guidMajorType = MFMediaType_Video;
    inType.guidSubtype = subtype;

    IMFActivate** activates{};
    UINT32 count{};
    const HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE,
        &inType,
        nullptr,
        &activates,
        &count);

    if(SUCCEEDED(hr) && activates){
        for(UINT32 i = 0; i < count; ++i){
            activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }

    return SUCCEEDED(hr) && count > 0;
}

void analyzeKeyFrameCadence(IMFSourceReader* reader, DWORD videoStreamIndex, uint32_t fpsNum, uint32_t fpsDen, MediaInspectionResult& result){
    constexpr uint32_t maxSamplesToInspect{1500};
    constexpr LONGLONG maxSpan100ns{120LL * 10'000'000LL};

    uint32_t sampledFrames{};
    uint32_t keyFrames{};
    bool cleanPointSeen{};
    LONGLONG firstTimestamp{-1};
    LONGLONG previousKeyTimestamp{-1};
    vector<double> keyIntervalsSec{};

    for(uint32_t i = 0; i < maxSamplesToInspect; ++i){
        DWORD actualStreamIndex{};
        DWORD flags{};
        LONGLONG timestamp{};
        com_ptr<IMFSample> sample{};

        const HRESULT hr = reader->ReadSample(videoStreamIndex, 0, &actualStreamIndex, &flags, &timestamp, sample.put());
        if(FAILED(hr)){
            break;
        }
        if(flags & MF_SOURCE_READERF_ENDOFSTREAM){
            break;
        }
        if(!sample){
            continue;
        }

        ++sampledFrames;
        if(firstTimestamp < 0){
            firstTimestamp = timestamp;
        }

        UINT32 cleanPoint{};
        if(SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0){
            cleanPointSeen = true;
            ++keyFrames;
            if(previousKeyTimestamp >= 0 && timestamp > previousKeyTimestamp){
                keyIntervalsSec.push_back((timestamp - previousKeyTimestamp) / 10'000'000.0);
            }
            previousKeyTimestamp = timestamp;
        }

        if(firstTimestamp >= 0 && timestamp - firstTimestamp >= maxSpan100ns){
            break;
        }
    }

    if(sampledFrames == 0){
        result.keyFrameSummary = L"unknown (no samples read)";
        result.keyFrameInterval = L"unknown";
        return;
    }

    if(!cleanPointSeen){
        result.keyFrameSummary = L"unknown (clean-point flags unavailable)";
        result.keyFrameInterval = L"unknown";
        return;
    }

    const auto ratio = static_cast<double>(keyFrames) / static_cast<double>(sampledFrames);
    {
        wstringstream ss;
        ss << keyFrames << L" key frames / " << sampledFrames << L" sampled frames (" << fixed << setprecision(2) << (ratio * 100.0) << L"%)";
        result.keyFrameSummary = ss.str();
    }

    if(keyIntervalsSec.empty()){
        result.keyFrameInterval = L"unknown (insufficient key frames sampled)";
        return;
    }

    const auto sum = accumulate(keyIntervalsSec.begin(), keyIntervalsSec.end(), 0.0);
    const auto avg = sum / static_cast<double>(keyIntervalsSec.size());
    const auto minIt = min_element(keyIntervalsSec.begin(), keyIntervalsSec.end());
    const auto maxIt = max_element(keyIntervalsSec.begin(), keyIntervalsSec.end());

    wstringstream ss;
    ss << fixed << setprecision(3)
       << L"avg " << avg << L" s, min " << *minIt << L" s, max " << *maxIt << L" s";

    if(fpsNum > 0 && fpsDen > 0){
        const double fps = static_cast<double>(fpsNum) / fpsDen;
        ss << L" (~" << setprecision(1) << (avg * fps) << L" frames avg)";
    }

    result.keyFrameInterval = ss.str();
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

    restoreWindowPlacement();
    loadAppSettings();
    Closed({this, &MainWindow::onClosed});
    refreshRecentVideosMenu();
    refreshRecentProjectsMenu();
    m_lastSavedProjectSnapshot = buildProjectSnapshot();
}

HWND MainWindow::getWindowHandle() const{
    HWND hwnd{};
    const auto projected{const_cast<MainWindow*>(this)->get_strong()};
    check_hresult(projected.as<::IWindowNative>()->get_WindowHandle(&hwnd));
    return hwnd;
}


int32_t PixelsToDips(const int32_t pixelValue, const uint32_t dpi){
    return static_cast<int32_t>(lround((pixelValue * 96.0) / (dpi == 0 ? 96 : dpi)));
}

int32_t DipsToPixels(const int32_t dipValue, const uint32_t dpi){
    return static_cast<int32_t>(lround((dipValue * (dpi == 0 ? 96U : dpi)) / 96.0));
}

bool MainWindow::isRectVisibleOnAnyMonitor(RECT const& rect){
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
    const bool hasDpiData{values.HasKey(W_POS_DPI)};
    const auto storedWidth{unbox_value<int32_t>(values.Lookup(W_POS_W))};
    const auto storedHeight{unbox_value<int32_t>(values.Lookup(W_POS_H))};
    const auto width{hasDpiData ? DipsToPixels(storedWidth, currentDpi) : storedWidth};
    const auto height{hasDpiData ? DipsToPixels(storedHeight, currentDpi) : storedHeight};

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
    values.Insert(W_POS_W, box_value(PixelsToDips(static_cast<int32_t>(bounds.right - bounds.left), dpi)));
    values.Insert(W_POS_H, box_value(PixelsToDips(static_cast<int32_t>(bounds.bottom - bounds.top), dpi)));
    values.Insert(W_POS_DPI, box_value(static_cast<int32_t>(dpi)));
}

void MainWindow::onClosed(IInspectable const&, WindowEventArgs const&){
    m_isClosing = true;

    if(m_positionTimer){
        m_positionTimer.Stop();
    }

    m_naturalDurationChangedRevoker.revoke();
    saveWindowPlacement();
    saveAppSettings();
}

void MainWindow::startButton_Click(IInspectable const&, RoutedEventArgs const&){
    if(m_player){
        m_player.Play();
    }
}

void MainWindow::pauseButton_Click(IInspectable const&, RoutedEventArgs const&){
    if(m_player){
        m_player.Pause();
    }
}

void MainWindow::stopButton_Click(IInspectable const&, RoutedEventArgs const&){
    if(m_player){
        m_player.Pause();
        m_player.PlaybackSession().Position(chrono::seconds(0));
        updateTimelineCursorFromPlayback();
    }
}

void MainWindow::timelineZoomSlider_ValueChanged(IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const&){
    if(m_loadedFile && m_timelineDurationSeconds > 0){
        renderTimelineAsync();
    }
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

void MainWindow::timelineHorizontalScrollBar_ValueChanged(IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const& args){
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

void MainWindow::timelineScrollViewer_ViewChanged(IInspectable const&, Controls::ScrollViewerViewChangedEventArgs const&){
    syncTimelineHorizontalScrollBar();
}

void MainWindow::timelineScrollViewer_SizeChanged(IInspectable const&, SizeChangedEventArgs const&){
    syncTimelineHorizontalScrollBar();
}

void MainWindow::timelineCanvas_PointerPressed(IInspectable const&, Input::PointerRoutedEventArgs const& e){
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

void MainWindow::timelineCanvas_PointerMoved(IInspectable const&, Input::PointerRoutedEventArgs const& e){
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
    if(m_frameIndex.empty() || m_timelineDurationSeconds <= 0 || TimelineTickCanvas().Width() <= 0){
        return false;
    }

    constexpr double hitTolerancePx{4.0};
    const auto width{TimelineTickCanvas().Width()};
    const auto total100ns{m_timelineDurationSeconds * 10'000'000.0};

    uint32_t nearestOrdinal{};
    bool foundNearest{false};
    uint32_t cleanOrdinal{};
    double nearestDistance{hitTolerancePx + 1.0};
    for(auto const& frame : m_frameIndex){
        if(!frame.cleanPoint){
            continue;
        }

        const auto x{clamp((static_cast<double>(frame.time100ns) / total100ns) * width, 0.0, width)};
        const auto distance{fabs(pointerX - x)};
        if(distance <= hitTolerancePx && distance < nearestDistance){
            nearestDistance = distance;
            nearestOrdinal = cleanOrdinal;
            foundNearest = true;
        }
        ++cleanOrdinal;
    }

    if(!foundNearest){
        return false;
    }

    const auto it = find(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end(), nearestOrdinal);

    if(it == m_selectedKeyFrames.end()){
        m_selectedKeyFrames.push_back(nearestOrdinal);
    }else{
        m_selectedKeyFrames.erase(it);
    }

    sort(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end());
    renderTimelineTicks();
    renderKeyframeTicks();
    return true;
}

void MainWindow::timelineCanvas_PointerReleased(IInspectable const&, Input::PointerRoutedEventArgs const& e){
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
    const bool dragged{m_timelineDragMoved};

    m_isTimelineDragging = false;
    m_timelineDragMoved = false;
    TimelineCanvas().ReleasePointerCapture(e.Pointer());

    if(!dragged){
        const auto modifiers{e.KeyModifiers()};
        const bool ctrlPressed{(modifiers & Windows::System::VirtualKeyModifiers::Control) == Windows::System::VirtualKeyModifiers::Control};
        if(ctrlPressed){
            toggleCutBlockAtCanvasX(point.Position().X);
        }else{
            seekTimelineToCanvasX(point.Position().X, (modifiers & Windows::System::VirtualKeyModifiers::Shift) == Windows::System::VirtualKeyModifiers::Shift);
        }
    }

    e.Handled(true);
}

void MainWindow::timelineCanvas_PointerCanceled(IInspectable const&, Input::PointerRoutedEventArgs const& e){
    if(m_isTimelineDragging && e.Pointer().PointerId() == m_timelineDragPointerId){
        m_isTimelineDragging = false;
        m_timelineDragMoved = false;
        TimelineCanvas().ReleasePointerCapture(e.Pointer());
        e.Handled(true);
    }
}

void MainWindow::timelineCanvas_PointerCaptureLost(IInspectable const&, Input::PointerRoutedEventArgs const&){
    m_isTimelineDragging = false;
    m_timelineDragMoved = false;
}

void MainWindow::timelineCanvas_Loaded(IInspectable const&, RoutedEventArgs const&){
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

void MainWindow::timelineTickCanvas_PointerReleased(IInspectable const&, Input::PointerRoutedEventArgs const& e){
    const auto point{e.GetCurrentPoint(TimelineTickCanvas())};
    if(point.Properties().PointerUpdateKind() == PointerUpdateKind::RightButtonReleased && toggleSelectedKeyframeAtCanvasX(point.Position().X)){
        tryFocusTimelineCanvas(FocusState::Programmatic);
        e.Handled(true);
    }
}


void MainWindow::onNaturalDurationChanged(MediaPlaybackSession const& sender, IInspectable const&){
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

void MainWindow::onPositionTimerTick(IInspectable const&, IInspectable const&){
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

void MainWindow::seekTimelineToCanvasX(const double pointerX, const bool bypassSnap){
    if(!m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto x{clamp(pointerX, 0.0, TimelineCanvas().Width())};
    const auto ratio{x / TimelineCanvas().Width()};
    auto target100ns{static_cast<int64_t>(ratio * (m_timelineDurationSeconds * 10'000'000.0))};

    if(!bypassSnap && !m_frameIndex.empty()){
        const auto toSeconds = [](int64_t v){ return static_cast<double>(v) / 10'000'000.0; };
        const auto it = lower_bound(m_frameIndex.begin(), m_frameIndex.end(), target100ns, [](IndexedFrameSample const& a, int64_t v){
            return a.time100ns < v;
        });

        auto useTime = target100ns;
        if(m_keyFrameSnapMode == L"Left"){
            for(auto rit = (it == m_frameIndex.begin() ? m_frameIndex.begin() : it);;){
                if(rit == m_frameIndex.begin()){
                    if(rit->cleanPoint){ useTime = rit->time100ns; }
                    break;
                }
                --rit;
                if(rit->cleanPoint){ useTime = rit->time100ns; break; }
            }
        }else if(m_keyFrameSnapMode == L"Right"){
            for(auto fit = it; fit != m_frameIndex.end(); ++fit){
                if(fit->cleanPoint){ useTime = fit->time100ns; break; }
            }
        }else{
            int64_t left = -1;
            int64_t right = -1;
            if(it != m_frameIndex.begin()){
                for(auto rit = it;;){
                    --rit;
                    if(rit->cleanPoint){ left = rit->time100ns; break; }
                    if(rit == m_frameIndex.begin()){ break; }
                }
            }
            for(auto fit = it; fit != m_frameIndex.end(); ++fit){
                if(fit->cleanPoint){ right = fit->time100ns; break; }
            }
            if(left >= 0 && right >= 0){
                useTime = (llabs(target100ns - left) <= llabs(right - target100ns)) ? left : right;
            }else if(left >= 0){
                useTime = left;
            }else if(right >= 0){
                useTime = right;
            }
        }
        target100ns = useTime;
    }

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

    const double width{TimelineCanvas().Width()};
    TimelineTickCanvas().Width(width);
    if(m_timelineDurationSeconds <= 0 || width <= 0){
        return;
    }

    const int majorTickCount{clamp(static_cast<int>(ceil(width / 120.0)), 6, 36)};

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

    const auto width = TimelineTickCanvas().Width();
    const auto total100ns = static_cast<double>(m_timelineDurationSeconds * 10'000'000.0);
    uint32_t cleanOrdinal{};
    for(auto const& frame : m_frameIndex){
        if(!frame.cleanPoint){
            continue;
        }

        const auto x = clamp((frame.time100ns / total100ns) * width, 0.0, width);
        const bool isSelected = find(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end(), cleanOrdinal) != m_selectedKeyFrames.end();

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

    const auto cleanKeyTimes = buildCleanKeyframeTimes100ns(m_frameIndex);
    if(cleanKeyTimes.empty()){
        return;
    }

    const auto overlayColor = Windows::UI::ColorHelper::FromArgb(90, 180, 180, 180);
    for(auto const& interval : m_cutIntervals){
        const auto startTime100ns = interval.first == TIMELINE_EDGE_SENTINEL ? static_cast<int64_t>(0)
            : (interval.first < cleanKeyTimes.size() ? cleanKeyTimes[interval.first] : static_cast<int64_t>(-1));
        const auto endTime100ns = interval.second == TIMELINE_EDGE_SENTINEL ? static_cast<int64_t>(m_timelineDurationSeconds * 10'000'000.0)
            : (interval.second < cleanKeyTimes.size() ? cleanKeyTimes[interval.second] : static_cast<int64_t>(-1));
        if(startTime100ns < 0 || endTime100ns < 0){
            continue;
        }

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
    const auto clampedX{clamp(pointerX, 0.0, width)};
    const auto clickedSeconds{(clampedX / width) * m_timelineDurationSeconds};

    if(m_selectedKeyFrames.empty()){
        return false;
    }

    auto selectedMarkers{m_selectedKeyFrames};
    sort(selectedMarkers.begin(), selectedMarkers.end());
    selectedMarkers.erase(unique(selectedMarkers.begin(), selectedMarkers.end()), selectedMarkers.end());

    const auto cleanKeyTimes = buildCleanKeyframeTimes100ns(m_frameIndex);
    if(cleanKeyTimes.size() < 2){
        return false;
    }

    const auto clicked100ns = static_cast<int64_t>(clickedSeconds * 10'000'000.0);
    const auto rightIt = upper_bound(selectedMarkers.begin(), selectedMarkers.end(), clicked100ns, [&cleanKeyTimes](int64_t time100ns, uint32_t ordinal){
        if(ordinal >= cleanKeyTimes.size()){
            return true;
        }
        return time100ns < cleanKeyTimes[ordinal];
    });
    uint32_t blockStart{TIMELINE_EDGE_SENTINEL};
    uint32_t blockEnd{TIMELINE_EDGE_SENTINEL};
    if(rightIt == selectedMarkers.begin()){
        blockStart = TIMELINE_EDGE_SENTINEL;
        blockEnd = *rightIt;
    }else if(rightIt == selectedMarkers.end()){
        blockStart = *(rightIt - 1);
        blockEnd = TIMELINE_EDGE_SENTINEL;
    }else{
        blockStart = *(rightIt - 1);
        blockEnd = *rightIt;
    }

    const auto rankOfStart = [](uint32_t v){
        return v == TIMELINE_EDGE_SENTINEL ? static_cast<int64_t>(-1) : static_cast<int64_t>(v);
    };
    const auto rankOfEnd = [keyCount = cleanKeyTimes.size()](uint32_t v){
        return v == TIMELINE_EDGE_SENTINEL ? static_cast<int64_t>(keyCount) : static_cast<int64_t>(v);
    };
    const auto blockStartRank = rankOfStart(blockStart);
    const auto blockEndRank = rankOfEnd(blockEnd);
    if(blockStartRank >= blockEndRank){
        return false;
    }

    const auto encodeInterval = [keyCount = cleanKeyTimes.size()](int64_t startRank, int64_t endRank){
        const auto start = startRank < 0 ? TIMELINE_EDGE_SENTINEL : static_cast<uint32_t>(startRank);
        const auto end = endRank >= static_cast<int64_t>(keyCount) ? TIMELINE_EDGE_SENTINEL : static_cast<uint32_t>(endRank);
        return make_pair(start, end);
    };

    bool removed{false};
    vector<pair<uint32_t, uint32_t>> updated;
    updated.reserve(m_cutIntervals.size() + 1);
    for(auto const& interval : m_cutIntervals){
        const auto intervalStartRank = rankOfStart(interval.first);
        const auto intervalEndRank = rankOfEnd(interval.second);
        if(intervalEndRank <= blockStartRank || intervalStartRank >= blockEndRank){
            updated.push_back(interval);
            continue;
        }

        removed = true;
        if(intervalStartRank < blockStartRank){
            updated.push_back(encodeInterval(intervalStartRank, blockStartRank));
        }
        if(intervalEndRank > blockEndRank){
            updated.push_back(encodeInterval(blockEndRank, intervalEndRank));
        }
    }

    if(!removed){
        updated.emplace_back(blockStart, blockEnd);
    }

    m_cutIntervals = normalizeAndMergeIndexIntervals(move(updated), cleanKeyTimes.size());
    renderCutOverlays();
    return true;
}

bool MainWindow::trySkipCurrentCutDuringPlayback(){
    if(!m_player || m_cutIntervals.empty() || m_timelineDurationSeconds <= 0){
        return false;
    }

    const auto state{m_player.PlaybackSession().PlaybackState()};
    if(state != MediaPlaybackState::Playing){
        return false;
    }

    const auto cleanKeyTimes = buildCleanKeyframeTimes100ns(m_frameIndex);
    if(cleanKeyTimes.empty()){
        return false;
    }

    const auto now100ns{max<int64_t>(0, m_player.PlaybackSession().Position().count())};
    for(auto const& interval : m_cutIntervals){
        const auto start100ns = interval.first == TIMELINE_EDGE_SENTINEL ? static_cast<int64_t>(0)
            : (interval.first < cleanKeyTimes.size() ? cleanKeyTimes[interval.first] : static_cast<int64_t>(-1));
        const auto end100ns = interval.second == TIMELINE_EDGE_SENTINEL ? static_cast<int64_t>(m_timelineDurationSeconds * 10'000'000.0)
            : (interval.second < cleanKeyTimes.size() ? cleanKeyTimes[interval.second] : static_cast<int64_t>(-1));
        if(start100ns < 0 || end100ns < 0){
            continue;
        }

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

    const auto current = m_player.PlaybackSession().Position().count();
    constexpr int64_t fallbackFrameDuration100ns{333'667}; // ~29.97 fps

    vector<int64_t> frameDurations;
    frameDurations.reserve(m_frameIndex.size());
    for(auto const& sample : m_frameIndex){
        if(sample.duration100ns > 0){
            frameDurations.push_back(sample.duration100ns);
        }
    }

    if(frameDurations.empty()){
        for(size_t i = 1; i < m_frameIndex.size(); ++i){
            const auto sampleCount = static_cast<int64_t>(m_frameIndex[i].sampleIndex) - static_cast<int64_t>(m_frameIndex[i - 1].sampleIndex);
            const auto timeDelta = m_frameIndex[i].time100ns - m_frameIndex[i - 1].time100ns;
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

    const auto direction = delta < 0 ? static_cast<int64_t>(-1) : static_cast<int64_t>(1);
    const auto duration100ns = static_cast<int64_t>(max(0.0, m_timelineDurationSeconds) * 10'000'000.0);
    const auto target = clamp(current + (direction * frameStep100ns), static_cast<int64_t>(0), duration100ns);
    m_player.PlaybackSession().Position(TimeSpan{target});
    updateTimelineCursorFromPlayback();
}

void MainWindow::stepByKeyframe(const int delta){
    if(!m_player || m_frameIndex.empty() || delta == 0){
        return;
    }

    vector<int64_t> keys;
    keys.reserve(m_frameIndex.size());
    for(auto const& f : m_frameIndex){
        if(f.cleanPoint){
            keys.push_back(f.time100ns);
        }
    }
    if(keys.empty()){
        return;
    }

    const auto current = m_player.PlaybackSession().Position().count();
    int64_t target = keys.front();

    if(delta > 0){
        const auto it = upper_bound(keys.begin(), keys.end(), current);
        target = (it != keys.end()) ? *it : keys.back();
    }else{
        const auto it = lower_bound(keys.begin(), keys.end(), current);
        if(it == keys.begin()){
            target = keys.front();
        }else{
            target = *(it - 1);
        }
    }

    m_player.PlaybackSession().Position(TimeSpan{target});
    updateTimelineCursorFromPlayback();
}

void MainWindow::tryFocusTimelineCanvas(FocusState const focusState){
    const auto canvas{TimelineCanvas()};
    if(canvas && canvas.XamlRoot()){
        canvas.Focus(focusState);
    }
}

bool MainWindow::handleStorylineKeyDown(Input::KeyRoutedEventArgs const& args){
    const auto focused{Input::FocusManager::GetFocusedElement(Content().XamlRoot()).try_as<DependencyObject>()};
    const bool focusOnMenu = focused && isInMenuSubtree(focused);
    const bool focusInDialog = focused && isInDialogSubtree(focused);

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
    const bool ctrlDown{(ctrlState & Windows::UI::Core::CoreVirtualKeyStates::Down) == Windows::UI::Core::CoreVirtualKeyStates::Down};

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
            const auto state = m_player.PlaybackSession().PlaybackState();
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
    case Windows::System::VirtualKey::Up:
        stepByKeyframe(-1);
        args.Handled(true);
        return true;
    case Windows::System::VirtualKey::Down:
        stepByKeyframe(1);
        args.Handled(true);
        return true;
    default:
        return false;
    }
}

void MainWindow::window_PreviewKeyDown(IInspectable const&, Input::KeyRoutedEventArgs const& args){
    (void)handleStorylineKeyDown(args);
}

void MainWindow::window_KeyDown(IInspectable const&, Input::KeyRoutedEventArgs const& args){
    (void)handleStorylineKeyDown(args);
}

void MainWindow::rootGrid_PointerReleased(IInspectable const&, Input::PointerRoutedEventArgs const& args){
    const auto source = args.OriginalSource().try_as<DependencyObject>();
    if(!source){
        return;
    }
    if(isInMenuSubtree(source) || isInDialogSubtree(source)){
        return;
    }
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

TimeSpan MainWindow::secondsToTimeSpan(const double seconds){
    return chrono::duration_cast<TimeSpan>(chrono::duration<double>(seconds));
}

void MainWindow::keyFrameSnapMode_Checked(IInspectable const& sender, RoutedEventArgs const&){
    const auto radio{sender.try_as<Controls::RadioButton>()};
    if(!radio || !radio.Tag()){
        return;
    }

    m_keyFrameSnapMode = unbox_value<hstring>(radio.Tag()).c_str();
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

IAsyncAction MainWindow::newProjectMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    resetProjectState(true);
    StatusText().Text(L"New project created");
}

IAsyncAction MainWindow::openProjectMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
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

IAsyncAction MainWindow::saveProjectMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
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

IAsyncAction MainWindow::closeProjectMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    resetProjectState(true);
    StatusText().Text(L"Project closed");
}

IAsyncAction MainWindow::loadVideoMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    co_await pickAndLoadVideoAsync();
}

IAsyncAction MainWindow::recentVideoMenuItem_Click(IInspectable const& sender, RoutedEventArgs const&){
    const auto item{sender.try_as<Controls::MenuFlyoutItem>()};
    if(!item || !item.Tag()){
        co_return;
    }

    bool openFailed{false};
    const auto path{unbox_value<hstring>(item.Tag())};
    try{
        const auto file{co_await StorageFile::GetFileFromPathAsync(path)};
        co_await loadVideoFileAsync(file);
    }catch(winrt::hresult_error const&){
        openFailed = true;
    }

    if(openFailed){
        co_await showInfoDialogAsync(L"Open failed", L"Could not open selected recent video.");
    }
}

IAsyncAction MainWindow::recentProjectMenuItem_Click(IInspectable const& sender, RoutedEventArgs const&){
    const auto item{sender.try_as<Controls::MenuFlyoutItem>()};
    if(!item || !item.Tag()){
        co_return;
    }

    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    bool openFailed{false};
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

IAsyncAction MainWindow::propertiesMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    co_await showPropertiesDialogAsync();
}

IAsyncAction MainWindow::exitMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    Close();
}

IAsyncAction MainWindow::aboutMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    co_await showInfoDialogAsync(L"About llvc", L"llvc - Lossless Video Cut\nPreview and timeline exploration tool.");
}

IAsyncAction MainWindow::optionsMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    co_await showOptionsDialogAsync();
}

IAsyncAction MainWindow::pickAndLoadVideoAsync(){
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

    for(auto const& path : m_recentVideos){
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

    for(auto const& path : m_recentProjects){
        Controls::MenuFlyoutItem item{};
        item.Text(path);
        item.Tag(box_value(path));
        item.Click({this, &MainWindow::recentProjectMenuItem_Click});
        menu.Items().Append(item);
    }
}

void MainWindow::addRecentVideo(hstring const& path){
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

void MainWindow::addRecentProject(hstring const& path){
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
    m_cutIntervals.clear();
    m_frameIndex.clear();
    m_mediaInfo = MediaInspectionResult{};
    m_timelineDurationSeconds = 0;
    m_keyFrameSnapMode = L"Nearest";
    NearestSnapRadio().IsChecked(true);
    TimelineZoomSlider().Value(3);

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
}

wstring MainWindow::buildProjectSnapshot(){
    wstringstream ss;
    ss << L"file_path=" << (m_loadedFile ? m_loadedFile.Path().c_str() : L"") << L"\n";
    ss << L"storyline_zoom=" << setprecision(15) << TimelineZoomSlider().Value() << L"\n";
    ss << L"keyframe_snap_mode=" << m_keyFrameSnapMode << L"\n";
    ss << L"selected_keyframe_indices=" << serializeIndexList(m_selectedKeyFrames) << L"\n";
    ss << L"cut_interval_indices=" << serializeIndexPairs(m_cutIntervals) << L"\n";
    for(auto const& line : m_projectUnknownLines){
        ss << line << L"\n";
    }
    return ss.str();
}

bool MainWindow::isProjectDirty(){
    return buildProjectSnapshot() != m_lastSavedProjectSnapshot;
}

IAsyncOperation<bool> MainWindow::ensureProjectSavedBeforeContinuingAsync(){
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

IAsyncAction MainWindow::openProjectFileAsync(StorageFile const& file){
    resetProjectState(true);

    const auto lines{co_await FileIO::ReadLinesAsync(file)};

    vector<wstring> unknownLines;
    vector<uint32_t> selectedKeyframeIndices;
    vector<pair<uint32_t, uint32_t>> cutIntervalIndices;
    wstring loadedFilePath;
    wstring snapMode{L"Nearest"};
    double zoomLevel{TimelineZoomSlider().Value()};
    vector<IndexedFrameSample> loadedKeyframeIndex;

    for(auto const& lineH : lines){
        const wstring line{lineH.c_str()};
        const auto trimmed{trim(line)};
        if(trimmed.empty()){
            unknownLines.push_back(line);
            continue;
        }

        if(trimmed[0] == L'#'){
            if(trimmed != L"# llvc project file"){
                unknownLines.push_back(line);
            }
            continue;
        }

        const auto eqPos{line.find(L'=')};
        if(eqPos == wstring::npos){
            unknownLines.push_back(line);
            continue;
        }

        const auto key{trim(line.substr(0, eqPos))};
        const auto value{trim(line.substr(eqPos + 1))};

        if(key == L"file_path"){
            loadedFilePath = value;
        }else if(key == L"storyline_zoom"){
            try{ zoomLevel = stod(value); }catch(...){ unknownLines.push_back(line); }
        }else if(key == L"keyframe_snap_mode"){
            if(value == L"Left" || value == L"Right" || value == L"Nearest"){
                snapMode = value;
            }else{
                unknownLines.push_back(line);
            }
        }else if(key == L"selected_keyframe_indices"){
            selectedKeyframeIndices = parseIndexList(value);
        }else if(key == L"cut_interval_indices"){
            cutIntervalIndices = parseIndexPairs(value);
        }else if(key == L"keyframe_index"){
            loadedKeyframeIndex = parseKeyframeVector(value);
        }else{
            unknownLines.push_back(line);
        }
    }

    if(!loadedFilePath.empty()){
        try{
            const auto videoFile{co_await StorageFile::GetFileFromPathAsync(loadedFilePath)};
            if(!loadedKeyframeIndex.empty()){
                co_await loadVideoFileAsync(videoFile, &loadedKeyframeIndex);
            }else{
                co_await loadVideoFileAsync(videoFile);
            }
        }catch(...){
            StatusText().Text(L"Project opened, but referenced video could not be loaded");
        }
    }

    zoomLevel = clamp(zoomLevel, TimelineZoomSlider().Minimum(), TimelineZoomSlider().Maximum());
    TimelineZoomSlider().Value(zoomLevel);
    m_keyFrameSnapMode = snapMode;

    const auto cleanKeyTimes = buildCleanKeyframeTimes100ns(m_frameIndex);
    m_selectedKeyFrames.clear();
    m_selectedKeyFrames.reserve(selectedKeyframeIndices.size());
    for(const auto ordinal : selectedKeyframeIndices){
        if(ordinal < cleanKeyTimes.size()){
            m_selectedKeyFrames.push_back(ordinal);
        }
    }
    sort(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end());
    m_selectedKeyFrames.erase(unique(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end()), m_selectedKeyFrames.end());

    m_cutIntervals = normalizeAndMergeIndexIntervals(move(cutIntervalIndices), cleanKeyTimes.size());
    m_projectUnknownLines = move(unknownLines);
    m_projectPath = file.Path();

    if(snapMode == L"Left"){
        LeftSnapRadio().IsChecked(true);
    }else if(snapMode == L"Right"){
        RightSnapRadio().IsChecked(true);
    }else{
        NearestSnapRadio().IsChecked(true);
    }

    addRecentProject(file.Path());
    m_lastSavedProjectSnapshot = buildProjectSnapshot();
    StatusText().Text(L"Project loaded");
}

IAsyncAction MainWindow::saveProjectFileAsync(StorageFile const& file){
    vector<hstring> lines;
    lines.emplace_back(L"# llvc project file");
    lines.emplace_back(L"file_path=" + wstring(m_loadedFile ? m_loadedFile.Path().c_str() : L""));
    lines.emplace_back(L"storyline_zoom=" + to_wstring(TimelineZoomSlider().Value()));
    lines.emplace_back(L"keyframe_snap_mode=" + m_keyFrameSnapMode);
    lines.emplace_back(L"selected_keyframe_indices=" + serializeIndexList(m_selectedKeyFrames));
    lines.emplace_back(L"cut_interval_indices=" + serializeIndexPairs(m_cutIntervals));
    lines.emplace_back(L"keyframe_index=" + serializeKeyframeVector(m_frameIndex));

    for(auto const& unknown : m_projectUnknownLines){
        if(trim(unknown) == L"# llvc project file"){
            continue;
        }
        lines.emplace_back(unknown);
    }

    co_await FileIO::WriteLinesAsync(file, single_threaded_vector<hstring>(move(lines)));
    m_projectPath = file.Path();
    addRecentProject(file.Path());
    m_lastSavedProjectSnapshot = buildProjectSnapshot();
    StatusText().Text(L"Project saved");
}

IAsyncAction MainWindow::showInfoDialogAsync(hstring const& title, hstring const& message){
    Controls::ContentDialog dialog{};
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(title));
    dialog.Content(box_value(message));
    dialog.CloseButtonText(L"OK");
    co_await dialog.ShowAsync();
}

IAsyncAction MainWindow::showPropertiesDialogAsync(){
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
    content += L"Key frames: "; content += m_mediaInfo.keyFrameSummary; content += L"\n";
    content += L"Key frame interval: "; content += m_mediaInfo.keyFrameInterval; content += L"\n";
    content += L"All samples independent: "; content += m_mediaInfo.allSamplesIndependent; content += L"\n";
    content += L"Max key frame spacing: "; content += m_mediaInfo.maxKeyFrameSpacing; content += L"\n";
    content += L"Audio codec: "; content += m_mediaInfo.audioCodec; content += L"\n";
    content += L"Audio bitrate: "; content += m_mediaInfo.audioBitrate;

    co_await showInfoDialogAsync(L"Properties", hstring(content));
}

IAsyncAction MainWindow::showOptionsDialogAsync(){
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

bool MainWindow::isSupportedVideoSubtype(GUID const& subtype){
    return subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265;
}

wstring MainWindow::guidToCodecName(GUID const& subtype, bool isVideo){
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

MediaInspectionResult MainWindow::inspectMediaFile(wstring const& filePath){
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
    constexpr DWORD invalidStreamIndex{numeric_limits<DWORD>::max()};
    DWORD videoStreamIndex{invalidStreamIndex};
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
        const auto seconds = static_cast<double>(duration.uhVal.QuadPart) / 10'000'000.0;
        wstringstream ss;
        ss << fixed << setprecision(3) << seconds << L" s";
        result.duration = ss.str();
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

vector<IndexedFrameSample> MainWindow::buildKeyframeIndexForFile(wstring const& filePath, function<void(double)> const& onProgress){
    vector<IndexedFrameSample> index;
    MFLifetime mf{};

    com_ptr<IMFSourceReader> reader;
    check_hresult(MFCreateSourceReaderFromURL(filePath.c_str(), nullptr, reader.put()));

    constexpr DWORD invalidStream{numeric_limits<DWORD>::max()};
    DWORD videoStreamIndex{invalidStream};
    for(DWORD streamIndex = 0;; ++streamIndex){
        com_ptr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(streamIndex, 0, type.put());
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
        return index;
    }

    check_hresult(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
    check_hresult(reader->SetStreamSelection(videoStreamIndex, TRUE));

    PROPVARIANT duration{};
    PropVariantInit(&duration);
    LONGLONG totalDuration100ns{};
    if(SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) && duration.vt == VT_UI8){
        totalDuration100ns = duration.uhVal.QuadPart;
    }
    PropVariantClear(&duration);
    if(onProgress){
        onProgress(0.0);
    }

    double lastReportedProgress{-1.0};
    for(uint32_t sampleIndex = 0;; ++sampleIndex){
        DWORD actualStream{};
        DWORD flags{};
        LONGLONG timestamp{};
        com_ptr<IMFSample> sample;
        const HRESULT hr = reader->ReadSample(videoStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put());
        if(FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)){
            break;
        }
        if(!sample){
            continue;
        }

        LONGLONG sampleDuration{};
        (void)sample->GetSampleDuration(&sampleDuration);
        UINT32 clean{};
        const bool cleanPoint = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean != 0;

        index.push_back(IndexedFrameSample{
            .time100ns = timestamp,
            .duration100ns = sampleDuration,
            .cleanPoint = cleanPoint,
            .sampleIndex = sampleIndex,
        });

        if(onProgress && totalDuration100ns > 0){
            const auto ratio = clamp(static_cast<double>(timestamp) / static_cast<double>(totalDuration100ns), 0.0, 1.0);
            const auto percent = ratio * 100.0;
            if(percent - lastReportedProgress >= 1.0 || percent >= 100.0){
                onProgress(percent);
                lastReportedProgress = percent;
            }
        }
    }

    if(onProgress){
        onProgress(100.0);
    }

    return index;
}

void MainWindow::window_DragOver(IInspectable const&, DragEventArgs const& e){
    e.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
}

IAsyncAction MainWindow::window_Drop(IInspectable const&, DragEventArgs const& e){
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

IAsyncAction MainWindow::loadVideoFileAsync(StorageFile const& file, vector<IndexedFrameSample> const* preloadedKeyframeIndex){
    MediaInspectionResult inspected{};
    try{
        inspected = inspectMediaFile(file.Path().c_str());
    }catch(winrt::hresult_error const& ex){
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
    KeyframeProgressBar().Visibility(Visibility::Visible);
    wstring status{L"Loaded: "};
    status += file.Name().c_str();
    status += L" (building keyframe index...)";
    StatusText().Text(status);

    if(preloadedKeyframeIndex && !preloadedKeyframeIndex->empty()){
        m_frameIndex = *preloadedKeyframeIndex;
        KeyframeProgressBar().Value(100);
    }else{
        const auto weak{get_weak()};
        auto progress = [weak](double percent){
            if(const auto self = weak.get()){
                self->DispatcherQueue().TryEnqueue([weak, percent](){
                    if(const auto uiSelf = weak.get()){
                        uiSelf->KeyframeProgressBar().Value(clamp(percent, 0.0, 100.0));
                    }
                });
            }
        };

        winrt::apartment_context uiThread;
        co_await winrt::resume_background();
        auto indexed = buildKeyframeIndexForFile(file.Path().c_str(), progress);
        co_await uiThread;
        m_frameIndex = move(indexed);
    }

    const auto source{Windows::Media::Core::MediaSource::CreateFromStorageFile(file)};
    m_player.Source(source);
    m_loadedFile = file;
    if(m_projectPath.empty()){
        m_selectedKeyFrames.clear();
        m_cutIntervals.clear();
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

    status = L"Loaded: ";
    status += file.Name().c_str();
    status += L" (loading story line...)";
    StatusText().Text(status);
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

        vector<bool> thumbnailBuilt(static_cast<size_t>(thumbnailCount), false);

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

            int nextIndex{-1};
            for(int i = firstVisibleIndex; i <= lastVisibleIndex; ++i){
                if(!thumbnailBuilt[static_cast<size_t>(i)]){
                    nextIndex = i;
                    break;
                }
            }

            if(nextIndex < 0){
                int left{firstVisibleIndex - 1};
                int right{lastVisibleIndex + 1};
                while(nextIndex < 0 && (left >= 0 || right < thumbnailCount)){
                    if(right < thumbnailCount && !thumbnailBuilt[static_cast<size_t>(right)]){
                        nextIndex = right;
                        break;
                    }
                    ++right;

                    if(left >= 0 && !thumbnailBuilt[static_cast<size_t>(left)]){
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
    }catch(winrt::hresult_error const& ex){
        wstring status{L"Failed to render story line: "};
        status += ex.message().c_str();
        StatusText().Text(status);
    }
}

}
