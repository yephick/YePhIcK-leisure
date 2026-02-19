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

using namespace winrt;
using namespace Microsoft::UI::Xaml;

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

std::wstring FormatGuid(GUID const& guid){
    OLECHAR raw[40]{};
    StringFromGUID2(guid, raw, static_cast<int>(std::size(raw)));
    return std::wstring(raw);
}

std::wstring FormatFileSize(uint64_t bytes){
    std::wstringstream ss;
    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;
    ss << bytes << L" bytes";
    if(bytes >= static_cast<uint64_t>(GB)){
        ss << L" (" << std::fixed << std::setprecision(2) << (bytes / GB) << L" GB)";
    }else if(bytes >= static_cast<uint64_t>(MB)){
        ss << L" (" << std::fixed << std::setprecision(2) << (bytes / MB) << L" MB)";
    }
    return ss.str();
}

std::wstring FormatRatio(uint32_t num, uint32_t den){
    if(den == 0){
        return L"-";
    }
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(3) << (static_cast<double>(num) / den);
    return ss.str();
}

std::wstring JoinRecentItems(std::vector<hstring> const& values){
    std::wstring out;
    for(size_t i = 0; i < values.size(); ++i){
        if(i > 0){
            out.push_back(RECENT_DELIMITER);
        }
        out += values[i].c_str();
    }
    return out;
}

std::vector<hstring> SplitRecentItems(std::wstring const& source){
    std::vector<hstring> items;
    size_t start{};
    while(start <= source.size()){
        const auto pos = source.find(RECENT_DELIMITER, start);
        const auto len = (pos == std::wstring::npos) ? (source.size() - start) : (pos - start);
        if(len > 0){
            items.emplace_back(source.substr(start, len));
        }
        if(pos == std::wstring::npos){
            break;
        }
        start = pos + 1;
    }
    return items;
}

bool IsInMenuSubtree(DependencyObject const& object){
    DependencyObject current{object};
    while(current){
        if(current.try_as<Controls::MenuBar>() || current.try_as<Controls::MenuFlyoutItem>() || current.try_as<Controls::MenuFlyoutSubItem>() || current.try_as<Controls::MenuFlyoutPresenter>()){
            return true;
        }
        current = Media::VisualTreeHelper::GetParent(current);
    }
    return false;
}

bool IsInDialogSubtree(DependencyObject const& object){
    DependencyObject current{object};
    while(current){
        if(current.try_as<Controls::ContentDialog>() || current.try_as<Controls::Primitives::Popup>()){
            return true;
        }
        current = Media::VisualTreeHelper::GetParent(current);
    }
    return false;
}

std::wstring Trim(std::wstring value){
    const auto first = value.find_first_not_of(L" \t\r\n");
    if(first == std::wstring::npos){
        return L"";
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<double> ParseNumberList(std::wstring const& text){
    std::vector<double> values;
    size_t start{};
    while(start <= text.size()){
        const auto pos = text.find(L',', start);
        auto token = Trim(text.substr(start, pos == std::wstring::npos ? std::wstring::npos : pos - start));
        if(!token.empty()){
            try{ values.push_back(std::stod(token)); }catch(...){ }
        }
        if(pos == std::wstring::npos){ break; }
        start = pos + 1;
    }
    return values;
}

std::vector<std::pair<double, double>> ParseNumberPairs(std::wstring const& text){
    std::vector<std::pair<double, double>> pairs;
    size_t start{};
    while(start <= text.size()){
        const auto sep = text.find(L';', start);
        const auto chunk = Trim(text.substr(start, sep == std::wstring::npos ? std::wstring::npos : sep - start));
        if(!chunk.empty()){
            const auto comma = chunk.find(L',');
            if(comma != std::wstring::npos){
                try{
                    const auto a = std::stod(Trim(chunk.substr(0, comma)));
                    const auto b = std::stod(Trim(chunk.substr(comma + 1)));
                    pairs.emplace_back(a, b);
                }catch(...){ }
            }
        }
        if(sep == std::wstring::npos){ break; }
        start = sep + 1;
    }
    return pairs;
}

std::wstring SerializeNumberList(std::vector<double> const& values){
    std::wstringstream ss;
    for(size_t i = 0; i < values.size(); ++i){
        if(i > 0){ ss << L","; }
        ss << std::setprecision(15) << values[i];
    }
    return ss.str();
}

std::wstring SerializeNumberPairs(std::vector<std::pair<double, double>> const& values){
    std::wstringstream ss;
    for(size_t i = 0; i < values.size(); ++i){
        if(i > 0){ ss << L";"; }
        ss << std::setprecision(15) << values[i].first << L"," << values[i].second;
    }
    return ss.str();
}


std::pair<double, double> NormalizeInterval(double a, double b){
    if(a > b){
        std::swap(a, b);
    }
    return {a, b};
}

std::vector<std::pair<double, double>> NormalizeAndMergeIntervals(std::vector<std::pair<double, double>> intervals, double maxSeconds){
    std::vector<std::pair<double, double>> normalized;
    normalized.reserve(intervals.size());
    for(auto const& interval : intervals){
        auto [start, end] = NormalizeInterval(interval.first, interval.second);
        start = std::clamp(start, 0.0, maxSeconds);
        end = std::clamp(end, 0.0, maxSeconds);
        if(end - start > 0.000001){
            normalized.emplace_back(start, end);
        }
    }

    std::sort(normalized.begin(), normalized.end(), [](auto const& a, auto const& b){
        return a.first < b.first;
    });

    std::vector<std::pair<double, double>> merged;
    for(auto const& interval : normalized){
        if(merged.empty() || interval.first > merged.back().second + 0.000001){
            merged.push_back(interval);
            continue;
        }
        merged.back().second = std::max(merged.back().second, interval.second);
    }

    return merged;
}

std::vector<IndexedFrameSample> ParseKeyframeVector(std::wstring const& text){
    std::vector<IndexedFrameSample> out;
    size_t start{};
    while(start <= text.size()){
        const auto sep = text.find(L';', start);
        const auto item = Trim(text.substr(start, sep == std::wstring::npos ? std::wstring::npos : sep - start));
        if(!item.empty()){
            const auto at = item.find(L'@');
            if(at != std::wstring::npos){
                try{
                    const auto t = static_cast<std::int64_t>(std::stoll(Trim(item.substr(0, at))));
                    const auto i = static_cast<std::uint32_t>(std::stoul(Trim(item.substr(at + 1))));
                    out.push_back(IndexedFrameSample{.time100ns=t, .duration100ns=0, .cleanPoint=true, .sampleIndex=i});
                }catch(...){ }
            }
        }
        if(sep == std::wstring::npos){ break; }
        start = sep + 1;
    }
    return out;
}

std::wstring SerializeKeyframeVector(std::vector<IndexedFrameSample> const& index){
    std::wstringstream ss;
    bool first{true};
    for(auto const& k : index){
        if(!k.cleanPoint){ continue; }
        if(!first){ ss << L";"; }
        first = false;
        ss << k.time100ns << L"@" << k.sampleIndex;
    }
    return ss.str();
}

bool HasDecoderForSubtype(GUID const& subtype){
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

void AnalyzeKeyFrameCadence(IMFSourceReader* reader, DWORD videoStreamIndex, uint32_t fpsNum, uint32_t fpsDen, MediaInspectionResult& result){
    constexpr uint32_t maxSamplesToInspect{1500};
    constexpr LONGLONG maxSpan100ns{120LL * 10'000'000LL};

    uint32_t sampledFrames{};
    uint32_t keyFrames{};
    bool cleanPointSeen{};
    LONGLONG firstTimestamp{-1};
    LONGLONG previousKeyTimestamp{-1};
    std::vector<double> keyIntervalsSec{};

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
        std::wstringstream ss;
        ss << keyFrames << L" key frames / " << sampledFrames << L" sampled frames (" << std::fixed << std::setprecision(2) << (ratio * 100.0) << L"%)";
        result.keyFrameSummary = ss.str();
    }

    if(keyIntervalsSec.empty()){
        result.keyFrameInterval = L"unknown (insufficient key frames sampled)";
        return;
    }

    const auto sum = std::accumulate(keyIntervalsSec.begin(), keyIntervalsSec.end(), 0.0);
    const auto avg = sum / static_cast<double>(keyIntervalsSec.size());
    const auto minIt = std::min_element(keyIntervalsSec.begin(), keyIntervalsSec.end());
    const auto maxIt = std::max_element(keyIntervalsSec.begin(), keyIntervalsSec.end());

    std::wstringstream ss;
    ss << std::fixed << std::setprecision(3)
       << L"avg " << avg << L" s, min " << *minIt << L" s, max " << *maxIt << L" s";

    if(fpsNum > 0 && fpsDen > 0){
        const double fps = static_cast<double>(fpsNum) / fpsDen;
        ss << L" (~" << std::setprecision(1) << (avg * fps) << L" frames avg)";
    }

    result.keyFrameInterval = ss.str();
}

MainWindow::MainWindow(){
    InitializeComponent();

    m_player = Windows::Media::Playback::MediaPlayer();
    PreviewPlayer().SetMediaPlayer(m_player);

    m_naturalDurationChangedRevoker = m_player.PlaybackSession().NaturalDurationChanged(auto_revoke, {this, &MainWindow::OnNaturalDurationChanged});

    m_positionTimer = DispatcherTimer();
    m_positionTimer.Interval(std::chrono::milliseconds(80));
    m_positionTimer.Tick({this, &MainWindow::OnPositionTimerTick});
    m_positionTimer.Start();

    RestoreWindowPlacement();
    LoadAppSettings();
    Closed({this, &MainWindow::OnClosed});
    RefreshRecentVideosMenu();
    RefreshRecentProjectsMenu();
    m_lastSavedProjectSnapshot = BuildProjectSnapshot();
}

HWND MainWindow::GetWindowHandle() const{
    HWND hwnd{};
    const auto projected{const_cast<MainWindow*>(this)->get_strong()};
    check_hresult(projected.as<::IWindowNative>()->get_WindowHandle(&hwnd));
    return hwnd;
}


int32_t PixelsToDips(const int32_t pixelValue, const uint32_t dpi){
    return static_cast<int32_t>(std::lround((pixelValue * 96.0) / (dpi == 0 ? 96 : dpi)));
}

int32_t DipsToPixels(const int32_t dipValue, const uint32_t dpi){
    return static_cast<int32_t>(std::lround((dipValue * (dpi == 0 ? 96U : dpi)) / 96.0));
}

bool MainWindow::IsRectVisibleOnAnyMonitor(RECT const& rect){
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

void MainWindow::RestoreWindowPlacement(){
    const auto localSettings{Windows::Storage::ApplicationData::Current().LocalSettings()};
    const auto values{localSettings.Values()};

    if(!values.HasKey(W_POS_L) || !values.HasKey(W_POS_T) || !values.HasKey(W_POS_W) || !values.HasKey(W_POS_H)){
        return;
    }

    const auto left{unbox_value<int32_t>(values.Lookup(W_POS_L))};
    const auto top{unbox_value<int32_t>(values.Lookup(W_POS_T))};

    const auto hwnd{GetWindowHandle()};

    SetWindowPos(hwnd, nullptr, left, top, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);

    const auto currentDpi{::GetDpiForWindow(hwnd)};
    const bool hasDpiData{values.HasKey(W_POS_DPI)};
    const auto storedWidth{unbox_value<int32_t>(values.Lookup(W_POS_W))};
    const auto storedHeight{unbox_value<int32_t>(values.Lookup(W_POS_H))};
    const auto width{hasDpiData ? DipsToPixels(storedWidth, currentDpi) : storedWidth};
    const auto height{hasDpiData ? DipsToPixels(storedHeight, currentDpi) : storedHeight};

    if(!IsRectVisibleOnAnyMonitor(RECT{left, top, left + width, top + height})){
        values.Remove(W_POS_L);
        values.Remove(W_POS_T);
        values.Remove(W_POS_W);
        values.Remove(W_POS_H);
        values.Remove(W_POS_DPI);
        return;
    }

    SetWindowPos(hwnd, nullptr, left, top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
}

void MainWindow::LoadAppSettings(){
    const auto values{Windows::Storage::ApplicationData::Current().LocalSettings().Values()};

    m_maxRecentVideos = S_DEFAULT_MAX_RECENT;
    m_maxRecentProjects = S_DEFAULT_MAX_RECENT;

    if(values.HasKey(S_MAX_RECENT_VIDEOS)){
        const auto parsed{unbox_value<int32_t>(values.Lookup(S_MAX_RECENT_VIDEOS))};
        m_maxRecentVideos = static_cast<std::uint32_t>(std::clamp(parsed, 1, 20));
    }
    if(values.HasKey(S_MAX_RECENT_PROJECTS)){
        const auto parsed{unbox_value<int32_t>(values.Lookup(S_MAX_RECENT_PROJECTS))};
        m_maxRecentProjects = static_cast<std::uint32_t>(std::clamp(parsed, 1, 20));
    }

    if(values.HasKey(S_RECENT_VIDEOS)){
        m_recentVideos = SplitRecentItems(unbox_value<hstring>(values.Lookup(S_RECENT_VIDEOS)).c_str());
    }
    if(values.HasKey(S_RECENT_PROJECTS)){
        m_recentProjects = SplitRecentItems(unbox_value<hstring>(values.Lookup(S_RECENT_PROJECTS)).c_str());
    }

    if(m_recentVideos.size() > m_maxRecentVideos){
        m_recentVideos.resize(m_maxRecentVideos);
    }
    if(m_recentProjects.size() > m_maxRecentProjects){
        m_recentProjects.resize(m_maxRecentProjects);
    }
}

void MainWindow::SaveAppSettings() const{
    const auto values{Windows::Storage::ApplicationData::Current().LocalSettings().Values()};
    values.Insert(S_MAX_RECENT_VIDEOS, box_value(static_cast<int32_t>(m_maxRecentVideos)));
    values.Insert(S_MAX_RECENT_PROJECTS, box_value(static_cast<int32_t>(m_maxRecentProjects)));
    values.Insert(S_RECENT_VIDEOS, box_value(hstring(JoinRecentItems(m_recentVideos))));
    values.Insert(S_RECENT_PROJECTS, box_value(hstring(JoinRecentItems(m_recentProjects))));
}

void MainWindow::SaveWindowPlacement() const{
    const auto hwnd{GetWindowHandle()};

    RECT bounds{};
    if(!GetWindowRect(hwnd, &bounds)){
        return;
    }

    WINDOWPLACEMENT placement{.length = sizeof(placement)};
    if(GetWindowPlacement(hwnd, &placement) && placement.showCmd == SW_SHOWMAXIMIZED){
        bounds = placement.rcNormalPosition;
    }

    const auto localSettings{Windows::Storage::ApplicationData::Current().LocalSettings()};
    const auto values{localSettings.Values()};
    const auto dpi{::GetDpiForWindow(hwnd)};
    values.Insert(W_POS_L, box_value(static_cast<int32_t>(bounds.left)));
    values.Insert(W_POS_T, box_value(static_cast<int32_t>(bounds.top)));
    values.Insert(W_POS_W, box_value(PixelsToDips(static_cast<int32_t>(bounds.right - bounds.left), dpi)));
    values.Insert(W_POS_H, box_value(PixelsToDips(static_cast<int32_t>(bounds.bottom - bounds.top), dpi)));
    values.Insert(W_POS_DPI, box_value(static_cast<int32_t>(dpi)));
}

void MainWindow::OnClosed(IInspectable const&, WindowEventArgs const&){
    m_isClosing = true;

    if(m_positionTimer){
        m_positionTimer.Stop();
    }

    m_naturalDurationChangedRevoker.revoke();
    SaveWindowPlacement();
    SaveAppSettings();
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
        UpdateTimelineCursorFromPlayback();
    }
}

void MainWindow::TimelineZoomSlider_ValueChanged(IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const&){
    if(m_loadedFile && m_timelineDurationSeconds > 0){
        RenderTimelineAsync();
    }
    TryFocusTimelineCanvas(Microsoft::UI::Xaml::FocusState::Programmatic);
}

void MainWindow::TimelineHorizontalScrollBar_ValueChanged(IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const& args){
    if(m_isClosing){
        return;
    }

    const auto scrollViewer{TimelineScrollViewer()};
    const auto offset{scrollViewer.HorizontalOffset()};
    if(std::fabs(offset - args.NewValue()) > 0.5){
        const auto targetOffset{box_value(args.NewValue()).as<Windows::Foundation::IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }
}

void MainWindow::TimelineScrollViewer_ViewChanged(IInspectable const&, Controls::ScrollViewerViewChangedEventArgs const&){
    SyncTimelineHorizontalScrollBar();
}

void MainWindow::TimelineScrollViewer_SizeChanged(IInspectable const&, SizeChangedEventArgs const&){
    SyncTimelineHorizontalScrollBar();
}

void MainWindow::TimelineCanvas_PointerPressed(IInspectable const&, Input::PointerRoutedEventArgs const& e){
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

    TryFocusTimelineCanvas(Microsoft::UI::Xaml::FocusState::Programmatic);
    TimelineCanvas().CapturePointer(e.Pointer());
    e.Handled(true);
}

void MainWindow::TimelineCanvas_PointerMoved(IInspectable const&, Input::PointerRoutedEventArgs const& e){
    if(!m_isTimelineDragging || e.Pointer().PointerId() != m_timelineDragPointerId){
        return;
    }

    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    const auto deltaX{point.Position().X - m_timelineDragStartX};

    if(std::fabs(deltaX) > 4.0){
        m_timelineDragMoved = true;
    }

    const auto scrollViewer{TimelineScrollViewer()};
    const auto viewportWidth{scrollViewer.ViewportWidth()};
    const auto maxOffset{std::max(0.0, TimelineCanvas().Width() - viewportWidth)};
    const auto target{std::clamp(m_timelineDragStartOffset - deltaX, 0.0, maxOffset)};
    const auto targetOffset{box_value(target).as<Windows::Foundation::IReference<double>>()};
    scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    e.Handled(true);
}

bool MainWindow::ToggleSelectedKeyframeAtCanvasX(const double pointerX){
    if(m_frameIndex.empty() || m_timelineDurationSeconds <= 0 || TimelineTickCanvas().Width() <= 0){
        return false;
    }

    constexpr double hitTolerancePx{4.0};
    const auto width{TimelineTickCanvas().Width()};
    const auto total100ns{m_timelineDurationSeconds * 10'000'000.0};

    std::int64_t nearestKeyTime{-1};
    double nearestDistance{hitTolerancePx + 1.0};
    for(auto const& frame : m_frameIndex){
        if(!frame.cleanPoint){
            continue;
        }

        const auto x{std::clamp((static_cast<double>(frame.time100ns) / total100ns) * width, 0.0, width)};
        const auto distance{std::fabs(pointerX - x)};
        if(distance <= hitTolerancePx && distance < nearestDistance){
            nearestDistance = distance;
            nearestKeyTime = frame.time100ns;
        }
    }

    if(nearestKeyTime < 0){
        return false;
    }

    const auto keySeconds{static_cast<double>(nearestKeyTime) / 10'000'000.0};
    constexpr double duplicateToleranceSeconds{0.000001};
    const auto it = std::find_if(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end(), [keySeconds](double t){
        return std::fabs(t - keySeconds) <= duplicateToleranceSeconds;
    });

    if(it == m_selectedKeyFrames.end()){
        m_selectedKeyFrames.push_back(keySeconds);
    }else{
        m_selectedKeyFrames.erase(it);
    }

    std::sort(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end());
    RenderTimelineTicks();
    RenderKeyframeTicks();
    return true;
}

void MainWindow::TimelineCanvas_PointerReleased(IInspectable const&, Input::PointerRoutedEventArgs const& e){
    if(!m_isTimelineDragging || e.Pointer().PointerId() != m_timelineDragPointerId){
        return;
    }

    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    const bool dragged{m_timelineDragMoved};

    m_isTimelineDragging = false;
    m_timelineDragMoved = false;
    TimelineCanvas().ReleasePointerCapture(e.Pointer());

    if(!dragged){
        const auto modifiers{e.KeyModifiers()};
        const bool ctrlPressed{(modifiers & Windows::System::VirtualKeyModifiers::Control) == Windows::System::VirtualKeyModifiers::Control};
        if(ctrlPressed){
            ToggleCutBlockAtCanvasX(point.Position().X);
        }else if(!ToggleSelectedKeyframeAtCanvasX(point.Position().X)){
            SeekTimelineToCanvasX(point.Position().X, (modifiers & Windows::System::VirtualKeyModifiers::Shift) == Windows::System::VirtualKeyModifiers::Shift);
        }
    }

    e.Handled(true);
}

void MainWindow::TimelineCanvas_PointerCanceled(IInspectable const&, Input::PointerRoutedEventArgs const& e){
    if(m_isTimelineDragging && e.Pointer().PointerId() == m_timelineDragPointerId){
        m_isTimelineDragging = false;
        m_timelineDragMoved = false;
        TimelineCanvas().ReleasePointerCapture(e.Pointer());
        e.Handled(true);
    }
}

void MainWindow::TimelineCanvas_PointerCaptureLost(IInspectable const&, Input::PointerRoutedEventArgs const&){
    m_isTimelineDragging = false;
    m_timelineDragMoved = false;
}

void MainWindow::TimelineCanvas_Loaded(IInspectable const&, RoutedEventArgs const&){
    TryFocusTimelineCanvas(Microsoft::UI::Xaml::FocusState::Programmatic);
}

void MainWindow::TimelineTickCanvas_PointerReleased(IInspectable const&, Input::PointerRoutedEventArgs const& e){
    const auto point{e.GetCurrentPoint(TimelineTickCanvas())};
    if(ToggleSelectedKeyframeAtCanvasX(point.Position().X)){
        TryFocusTimelineCanvas(Microsoft::UI::Xaml::FocusState::Programmatic);
        e.Handled(true);
    }
}


void MainWindow::OnNaturalDurationChanged(Windows::Media::Playback::MediaPlaybackSession const& sender, IInspectable const&){
    const auto duration{sender.NaturalDuration()};
    m_timelineDurationSeconds = std::max(0.0, duration.count() / 10'000'000.0);

    if(m_loadedFile && m_timelineDurationSeconds > 0){
        const auto weak{get_weak()};
        if(DispatcherQueue().HasThreadAccess()){
            RenderTimelineAsync();
            return;
        }

        DispatcherQueue().TryEnqueue([weak](){
            if(const auto self{weak.get()}){
                self->RenderTimelineAsync();
            }
        });
    }
}

void MainWindow::OnPositionTimerTick(IInspectable const&, IInspectable const&){
    if(m_isClosing){
        return;
    }

    (void)TrySkipCurrentCutDuringPlayback();
    UpdateTimelineCursorFromPlayback();
}

void MainWindow::UpdateTimelineCursorFromPlayback(){
    if(m_isClosing || !m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto current{m_player.PlaybackSession().Position()};
    const auto seconds{std::max(0.0, current.count() / 10'000'000.0)};
    const auto ratio{std::clamp(seconds / m_timelineDurationSeconds, 0.0, 1.0)};
    const auto left{ratio * TimelineCanvas().Width()};
    Controls::Canvas::SetLeft(TimelineCursor(), left);

    if(m_player.PlaybackSession().PlaybackState() == Windows::Media::Playback::MediaPlaybackState::Playing){
        EnsureTimelineCursorVisible(left);
    }

    SyncTimelineHorizontalScrollBar();
}

void MainWindow::SyncTimelineHorizontalScrollBar(){
    const auto scrollViewer{TimelineScrollViewer()};
    const auto scrollableWidth{std::max(0.0, scrollViewer.ScrollableWidth())};
    const auto viewportWidth{std::max(1.0, scrollViewer.ViewportWidth())};

    auto bar{TimelineHorizontalScrollBar()};
    bar.Maximum(std::max(1.0, scrollableWidth));
    bar.LargeChange(std::max(32.0, viewportWidth * 0.8));
    bar.SmallChange(24.0);
    bar.IsEnabled(true);
    bar.Visibility(Visibility::Visible);

    const auto currentValue{bar.Value()};
    const auto offset{std::clamp(scrollViewer.HorizontalOffset(), 0.0, bar.Maximum())};
    if(std::fabs(currentValue - offset) > 0.5){
        bar.Value(offset);
    }
}

void MainWindow::SeekTimelineToCanvasX(const double pointerX, const bool bypassSnap){
    if(!m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto x{std::clamp(pointerX, 0.0, TimelineCanvas().Width())};
    const auto ratio{x / TimelineCanvas().Width()};
    auto target100ns{static_cast<std::int64_t>(ratio * (m_timelineDurationSeconds * 10'000'000.0))};

    if(!bypassSnap && !m_frameIndex.empty()){
        const auto toSeconds = [](std::int64_t v){ return static_cast<double>(v) / 10'000'000.0; };
        const auto it = std::lower_bound(m_frameIndex.begin(), m_frameIndex.end(), target100ns, [](IndexedFrameSample const& a, std::int64_t v){
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
            std::int64_t left = -1;
            std::int64_t right = -1;
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
                useTime = (std::llabs(target100ns - left) <= std::llabs(right - target100ns)) ? left : right;
            }else if(left >= 0){
                useTime = left;
            }else if(right >= 0){
                useTime = right;
            }
        }
        target100ns = useTime;
    }

    m_player.PlaybackSession().Position(Windows::Foundation::TimeSpan{target100ns});
    UpdateTimelineCursorFromPlayback();
}

void MainWindow::EnsureTimelineCursorVisible(const double cursorLeft){
    const auto scrollViewer{TimelineScrollViewer()};
    const auto currentOffset{scrollViewer.HorizontalOffset()};
    const auto viewportWidth{scrollViewer.ViewportWidth()};

    if(viewportWidth <= 0){
        return;
    }

    constexpr auto cursorPadding{48.0};
    const auto minVisible{currentOffset + cursorPadding};
    const auto maxVisible{currentOffset + viewportWidth - cursorPadding};
    const auto maxOffset{std::max(0.0, TimelineCanvas().Width() - viewportWidth)};

    if(cursorLeft < minVisible){
        const auto targetOffset{box_value(std::clamp(cursorLeft - cursorPadding, 0.0, maxOffset)).as<Windows::Foundation::IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }else if(cursorLeft > maxVisible){
        const auto targetOffset{box_value(std::clamp(cursorLeft + cursorPadding - viewportWidth, 0.0, maxOffset)).as<Windows::Foundation::IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }
}

void MainWindow::RenderTimelineTicks(){
    TimelineTickCanvas().Children().Clear();

    const double width{TimelineCanvas().Width()};
    TimelineTickCanvas().Width(width);
    if(m_timelineDurationSeconds <= 0 || width <= 0){
        return;
    }

    const int majorTickCount{std::clamp(static_cast<int>(std::ceil(width / 120.0)), 6, 36)};

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
        const auto totalSeconds{static_cast<int>(std::round(ratio * m_timelineDurationSeconds))};
        const auto minutes{totalSeconds / 60};
        const auto seconds{totalSeconds % 60};
        auto text{std::to_wstring(minutes)};
        text += L":";
        if(seconds < 10){
            text += L"0";
        }
        text += std::to_wstring(seconds);
        label.Text(text);
        label.FontSize(11);
        label.Foreground(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 200, 200, 200)));
        Controls::Canvas::SetLeft(label, std::max(0.0, x + 3.0));
        Controls::Canvas::SetTop(label, 0);
        TimelineTickCanvas().Children().Append(label);
    }
}

void MainWindow::RenderKeyframeTicks(){
    if(m_frameIndex.empty() || m_timelineDurationSeconds <= 0 || TimelineTickCanvas().Width() <= 0){
        return;
    }

    const auto width = TimelineTickCanvas().Width();
    const auto total100ns = static_cast<double>(m_timelineDurationSeconds * 10'000'000.0);
    for(auto const& frame : m_frameIndex){
        if(!frame.cleanPoint){
            continue;
        }

        const auto x = std::clamp((frame.time100ns / total100ns) * width, 0.0, width);
        const auto keySeconds = static_cast<double>(frame.time100ns) / 10'000'000.0;
        constexpr double selectedToleranceSeconds{0.000001};
        const bool isSelected = std::any_of(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end(), [keySeconds](double t){
            return std::fabs(t - keySeconds) <= selectedToleranceSeconds;
        });

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
    }
}

void MainWindow::RenderCutOverlays(){
    CutOverlayLayer().Children().Clear();

    const auto width{TimelineCanvas().Width()};
    CutOverlayLayer().Width(width);
    if(m_timelineDurationSeconds <= 0 || width <= 0){
        return;
    }

    const auto overlayColor = Windows::UI::ColorHelper::FromArgb(90, 180, 180, 180);
    for(auto const& interval : m_cutIntervals){
        const auto start{std::clamp(interval.first / m_timelineDurationSeconds, 0.0, 1.0)};
        const auto end{std::clamp(interval.second / m_timelineDurationSeconds, 0.0, 1.0)};
        if(end <= start){
            continue;
        }

        Shapes::Rectangle block{};
        const auto left{start * width};
        block.Width(std::max(1.0, (end - start) * width));
        block.Height(86.0);
        block.Fill(Media::SolidColorBrush(overlayColor));
        block.IsHitTestVisible(false);
        Controls::Canvas::SetLeft(block, left);
        Controls::Canvas::SetTop(block, 0.0);
        CutOverlayLayer().Children().Append(block);
    }
}

bool MainWindow::ToggleCutBlockAtCanvasX(const double pointerX){
    if(m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return false;
    }

    const auto width{TimelineCanvas().Width()};
    const auto clampedX{std::clamp(pointerX, 0.0, width)};
    const auto clickedSeconds{(clampedX / width) * m_timelineDurationSeconds};

    std::vector<double> selectedMarkers;
    selectedMarkers.reserve(m_selectedKeyFrames.size());
    for(auto const marker : m_selectedKeyFrames){
        if(marker >= 0.0 && marker <= m_timelineDurationSeconds){
            selectedMarkers.push_back(marker);
        }
    }

    if(selectedMarkers.size() < 2){
        return false;
    }

    std::sort(selectedMarkers.begin(), selectedMarkers.end());
    selectedMarkers.erase(std::unique(selectedMarkers.begin(), selectedMarkers.end(), [](double a, double b){
        return std::fabs(a - b) <= 0.000001;
    }), selectedMarkers.end());

    if(selectedMarkers.size() < 2){
        return false;
    }

    const auto rightIt = std::upper_bound(selectedMarkers.begin(), selectedMarkers.end(), clickedSeconds);
    if(rightIt == selectedMarkers.begin() || rightIt == selectedMarkers.end()){
        return false;
    }

    const auto blockStart = *(rightIt - 1);
    const auto blockEnd = *rightIt;
    if(blockEnd - blockStart <= 0.000001){
        return false;
    }

    bool removed{false};
    std::vector<std::pair<double, double>> updated;
    updated.reserve(m_cutIntervals.size() + 1);
    for(auto const& interval : m_cutIntervals){
        if(interval.second <= blockStart || interval.first >= blockEnd){
            updated.push_back(interval);
            continue;
        }

        removed = true;
        if(interval.first < blockStart){
            updated.emplace_back(interval.first, blockStart);
        }
        if(interval.second > blockEnd){
            updated.emplace_back(blockEnd, interval.second);
        }
    }

    if(!removed){
        updated.emplace_back(blockStart, blockEnd);
    }

    m_cutIntervals = NormalizeAndMergeIntervals(std::move(updated), m_timelineDurationSeconds);
    RenderCutOverlays();
    return true;
}

bool MainWindow::TrySkipCurrentCutDuringPlayback(){
    if(!m_player || m_cutIntervals.empty() || m_timelineDurationSeconds <= 0){
        return false;
    }

    const auto state{m_player.PlaybackSession().PlaybackState()};
    if(state != Windows::Media::Playback::MediaPlaybackState::Playing){
        return false;
    }

    const auto nowSeconds{std::max(0.0, m_player.PlaybackSession().Position().count() / 10'000'000.0)};
    for(auto const& interval : m_cutIntervals){
        if(nowSeconds >= interval.first && nowSeconds < interval.second){
            const auto jumpTo{std::min(m_timelineDurationSeconds, interval.second + 0.0005)};
            m_player.PlaybackSession().Position(SecondsToTimeSpan(jumpTo));
            return true;
        }
    }

    return false;
}

void MainWindow::StepByFrame(const int delta){
    if(!m_player || m_frameIndex.empty() || delta == 0){
        return;
    }

    const auto current = m_player.PlaybackSession().Position().count();
    auto nearest = std::lower_bound(m_frameIndex.begin(), m_frameIndex.end(), current, [](IndexedFrameSample const& a, std::int64_t t){ return a.time100ns < t; });

    std::ptrdiff_t currentIndex{};
    if(nearest == m_frameIndex.end()){
        currentIndex = static_cast<std::ptrdiff_t>(m_frameIndex.size()) - 1;
    }else if(nearest == m_frameIndex.begin()){
        currentIndex = 0;
    }else{
        const auto prev = nearest - 1;
        currentIndex = (std::llabs(nearest->time100ns - current) < std::llabs(current - prev->time100ns))
            ? static_cast<std::ptrdiff_t>(nearest - m_frameIndex.begin())
            : static_cast<std::ptrdiff_t>(prev - m_frameIndex.begin());
    }

    const auto targetIndex = std::clamp<std::ptrdiff_t>(
        currentIndex + (delta < 0 ? -1 : 1),
        0,
        static_cast<std::ptrdiff_t>(m_frameIndex.size()) - 1);

    m_player.PlaybackSession().Position(Windows::Foundation::TimeSpan{m_frameIndex[static_cast<size_t>(targetIndex)].time100ns});
    UpdateTimelineCursorFromPlayback();
}

void MainWindow::StepByKeyframe(const int delta){
    if(!m_player || m_frameIndex.empty() || delta == 0){
        return;
    }

    std::vector<std::int64_t> keys;
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
    std::int64_t target = keys.front();

    if(delta > 0){
        const auto it = std::upper_bound(keys.begin(), keys.end(), current);
        target = (it != keys.end()) ? *it : keys.back();
    }else{
        const auto it = std::lower_bound(keys.begin(), keys.end(), current);
        if(it == keys.begin()){
            target = keys.front();
        }else{
            target = *(it - 1);
        }
    }

    m_player.PlaybackSession().Position(Windows::Foundation::TimeSpan{target});
    UpdateTimelineCursorFromPlayback();
}

void MainWindow::TryFocusTimelineCanvas(Microsoft::UI::Xaml::FocusState const focusState){
    const auto canvas{TimelineCanvas()};
    if(canvas && canvas.XamlRoot()){
        canvas.Focus(focusState);
    }
}

bool MainWindow::HandleStorylineKeyDown(Input::KeyRoutedEventArgs const& args){
    const auto focused{Input::FocusManager::GetFocusedElement(Content().XamlRoot()).try_as<DependencyObject>()};
    const bool focusOnMenu = focused && IsInMenuSubtree(focused);
    const bool focusInDialog = focused && IsInDialogSubtree(focused);

    if(args.Key() == Windows::System::VirtualKey::Menu || args.Key() == Windows::System::VirtualKey::LeftMenu || args.Key() == Windows::System::VirtualKey::RightMenu){
        MainMenuBar().Focus(Microsoft::UI::Xaml::FocusState::Keyboard);
        args.Handled(true);
        return true;
    }

    if(args.Key() == Windows::System::VirtualKey::Tab && !focusOnMenu && !focusInDialog){
        TryFocusTimelineCanvas(Microsoft::UI::Xaml::FocusState::Programmatic);
        args.Handled(true);
        return true;
    }

    if(focusOnMenu || focusInDialog){
        return false;
    }

    TryFocusTimelineCanvas(Microsoft::UI::Xaml::FocusState::Programmatic);

    const auto ctrlState{Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(Windows::System::VirtualKey::Control)};
    const bool ctrlDown{(ctrlState & Windows::UI::Core::CoreVirtualKeyStates::Down) == Windows::UI::Core::CoreVirtualKeyStates::Down};

    if(ctrlDown){
        if(args.Key() == Windows::System::VirtualKey::O){
            (void)OpenProjectMenuItem_Click(nullptr, RoutedEventArgs{});
            args.Handled(true);
            return true;
        }
        if(args.Key() == Windows::System::VirtualKey::S){
            (void)SaveProjectMenuItem_Click(nullptr, RoutedEventArgs{});
            args.Handled(true);
            return true;
        }
        if(args.Key() == Windows::System::VirtualKey::N){
            (void)NewProjectMenuItem_Click(nullptr, RoutedEventArgs{});
            args.Handled(true);
            return true;
        }
    }

    switch(args.Key()){
    case Windows::System::VirtualKey::Space:
        if(m_player){
            const auto state = m_player.PlaybackSession().PlaybackState();
            if(state == Windows::Media::Playback::MediaPlaybackState::Playing){
                m_player.Pause();
            }else{
                m_player.Play();
            }
        }
        args.Handled(true);
        return true;
    case Windows::System::VirtualKey::Left:
        StepByFrame(-1);
        args.Handled(true);
        return true;
    case Windows::System::VirtualKey::Right:
        StepByFrame(1);
        args.Handled(true);
        return true;
    case Windows::System::VirtualKey::Up:
        StepByKeyframe(-1);
        args.Handled(true);
        return true;
    case Windows::System::VirtualKey::Down:
        StepByKeyframe(1);
        args.Handled(true);
        return true;
    default:
        return false;
    }
}

void MainWindow::Window_PreviewKeyDown(IInspectable const&, Input::KeyRoutedEventArgs const& args){
    (void)HandleStorylineKeyDown(args);
}

void MainWindow::Window_KeyDown(IInspectable const&, Input::KeyRoutedEventArgs const& args){
    (void)HandleStorylineKeyDown(args);
}

void MainWindow::RootGrid_PointerReleased(IInspectable const&, Input::PointerRoutedEventArgs const& args){
    const auto source = args.OriginalSource().try_as<DependencyObject>();
    if(!source){
        return;
    }
    if(IsInMenuSubtree(source) || IsInDialogSubtree(source)){
        return;
    }
    TryFocusTimelineCanvas(Microsoft::UI::Xaml::FocusState::Programmatic);
}

Windows::Foundation::TimeSpan MainWindow::SecondsToTimeSpan(const double seconds){
    return std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::duration<double>(seconds));
}

void MainWindow::KeyFrameSnapMode_Checked(IInspectable const& sender, RoutedEventArgs const&){
    const auto radio{sender.try_as<Controls::RadioButton>()};
    if(!radio || !radio.Tag()){
        return;
    }

    m_keyFrameSnapMode = unbox_value<hstring>(radio.Tag()).c_str();
    TryFocusTimelineCanvas(Microsoft::UI::Xaml::FocusState::Programmatic);
}

Windows::Foundation::IAsyncAction MainWindow::NewProjectMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    if(!co_await EnsureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    ResetProjectState(true);
    StatusText().Text(L"New project created");
}

Windows::Foundation::IAsyncAction MainWindow::OpenProjectMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    if(!co_await EnsureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    Windows::Storage::Pickers::FileOpenPicker picker{};
    picker.FileTypeFilter().Append(L".llvc");
    picker.SuggestedStartLocation(Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary);

    auto initWithWindow{picker.as<IInitializeWithWindow>()};
    check_hresult(initWithWindow->Initialize(GetWindowHandle()));

    if(const auto file{co_await picker.PickSingleFileAsync()}){
        co_await OpenProjectFileAsync(file);
    }
}

Windows::Foundation::IAsyncAction MainWindow::SaveProjectMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    Windows::Storage::StorageFile target{nullptr};

    if(!m_projectPath.empty()){
        try{
            target = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(m_projectPath);
        }catch(...){
            target = nullptr;
        }
    }

    if(!target){
        Windows::Storage::Pickers::FileSavePicker picker{};
        picker.SuggestedStartLocation(Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary);
        picker.FileTypeChoices().Insert(L"llvc project", single_threaded_vector<hstring>({L".llvc"}));
        picker.SuggestedFileName(L"project");

        auto initWithWindow{picker.as<IInitializeWithWindow>()};
        check_hresult(initWithWindow->Initialize(GetWindowHandle()));

        target = co_await picker.PickSaveFileAsync();
        if(!target){
            co_return;
        }
    }

    co_await SaveProjectFileAsync(target);
}

Windows::Foundation::IAsyncAction MainWindow::CloseProjectMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    if(!co_await EnsureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    ResetProjectState(true);
    StatusText().Text(L"Project closed");
}

Windows::Foundation::IAsyncAction MainWindow::LoadVideoMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    co_await PickAndLoadVideoAsync();
}

Windows::Foundation::IAsyncAction MainWindow::RecentVideoMenuItem_Click(IInspectable const& sender, RoutedEventArgs const&){
    const auto item{sender.try_as<Controls::MenuFlyoutItem>()};
    if(!item || !item.Tag()){
        co_return;
    }

    bool openFailed{false};
    const auto path{unbox_value<hstring>(item.Tag())};
    try{
        const auto file{co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path)};
        co_await LoadVideoFileAsync(file);
    }catch(winrt::hresult_error const&){
        openFailed = true;
    }

    if(openFailed){
        co_await ShowInfoDialogAsync(L"Open failed", L"Could not open selected recent video.");
    }
}

Windows::Foundation::IAsyncAction MainWindow::RecentProjectMenuItem_Click(IInspectable const& sender, RoutedEventArgs const&){
    const auto item{sender.try_as<Controls::MenuFlyoutItem>()};
    if(!item || !item.Tag()){
        co_return;
    }

    if(!co_await EnsureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    bool openFailed{false};
    const auto path{unbox_value<hstring>(item.Tag())};
    try{
        const auto file{co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path)};
        co_await OpenProjectFileAsync(file);
    }catch(...){
        openFailed = true;
    }

    if(openFailed){
        co_await ShowInfoDialogAsync(L"Open failed", L"Could not open selected recent project.");
    }
}

Windows::Foundation::IAsyncAction MainWindow::PropertiesMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    co_await ShowPropertiesDialogAsync();
}

Windows::Foundation::IAsyncAction MainWindow::ExitMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    if(!co_await EnsureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    Close();
}

Windows::Foundation::IAsyncAction MainWindow::AboutMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    co_await ShowInfoDialogAsync(L"About llvc", L"llvc - Lossless Video Cut\nPreview and timeline exploration tool.");
}

Windows::Foundation::IAsyncAction MainWindow::OptionsMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    co_await ShowOptionsDialogAsync();
}

Windows::Foundation::IAsyncAction MainWindow::PickAndLoadVideoAsync(){
    Windows::Storage::Pickers::FileOpenPicker picker{};
    picker.SuggestedStartLocation(Windows::Storage::Pickers::PickerLocationId::VideosLibrary);
    picker.FileTypeFilter().Append(L".mp4");
    picker.FileTypeFilter().Append(L".mov");

    const auto hwnd{GetWindowHandle()};
    auto initWithWindow{picker.as<IInitializeWithWindow>()};
    check_hresult(initWithWindow->Initialize(hwnd));

    if(const auto file{co_await picker.PickSingleFileAsync()}){
        co_await LoadVideoFileAsync(file);
    }
}

void MainWindow::RefreshRecentVideosMenu(){
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
        item.Click({this, &MainWindow::RecentVideoMenuItem_Click});
        menu.Items().Append(item);
    }
}

void MainWindow::RefreshRecentProjectsMenu(){
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
        item.Click({this, &MainWindow::RecentProjectMenuItem_Click});
        menu.Items().Append(item);
    }
}

void MainWindow::AddRecentVideo(hstring const& path){
    if(path.empty()){
        return;
    }

    m_recentVideos.erase(std::remove(m_recentVideos.begin(), m_recentVideos.end(), path), m_recentVideos.end());
    m_recentVideos.insert(m_recentVideos.begin(), path);
    if(m_recentVideos.size() > m_maxRecentVideos){
        m_recentVideos.resize(m_maxRecentVideos);
    }
    RefreshRecentVideosMenu();
    SaveAppSettings();
}

void MainWindow::AddRecentProject(hstring const& path){
    if(path.empty()){
        return;
    }

    m_recentProjects.erase(std::remove(m_recentProjects.begin(), m_recentProjects.end(), path), m_recentProjects.end());
    m_recentProjects.insert(m_recentProjects.begin(), path);
    if(m_recentProjects.size() > m_maxRecentProjects){
        m_recentProjects.resize(m_maxRecentProjects);
    }
    RefreshRecentProjectsMenu();
    SaveAppSettings();
}

void MainWindow::ResetProjectState(const bool clearLoadedVideo){
    m_projectPath.clear();
    m_projectUnknownLines.clear();
    m_selectedKeyFrames.clear();
    m_cutIntervals.clear();
    m_frameIndex.clear();
    m_keyFrameSnapMode = L"Nearest";
    NearestSnapRadio().IsChecked(true);
    TimelineZoomSlider().Value(3);

    if(clearLoadedVideo){
        m_loadedFile = nullptr;
        m_player.Source(nullptr);
    }

    m_lastSavedProjectSnapshot = BuildProjectSnapshot();
}

std::wstring MainWindow::BuildProjectSnapshot(){
    std::wstringstream ss;
    ss << L"file_path=" << (m_loadedFile ? m_loadedFile.Path().c_str() : L"") << L"\n";
    ss << L"storyline_zoom=" << std::setprecision(15) << TimelineZoomSlider().Value() << L"\n";
    ss << L"keyframe_snap_mode=" << m_keyFrameSnapMode << L"\n";
    ss << L"selected_key_frames=" << SerializeNumberList(m_selectedKeyFrames) << L"\n";
    ss << L"cut_intervals=" << SerializeNumberPairs(m_cutIntervals) << L"\n";
    for(auto const& line : m_projectUnknownLines){
        ss << line << L"\n";
    }
    return ss.str();
}

bool MainWindow::IsProjectDirty(){
    return BuildProjectSnapshot() != m_lastSavedProjectSnapshot;
}

Windows::Foundation::IAsyncOperation<bool> MainWindow::EnsureProjectSavedBeforeContinuingAsync(){
    if(!IsProjectDirty()){
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
        co_await SaveProjectMenuItem_Click(nullptr, RoutedEventArgs{});
        co_return !IsProjectDirty();
    }
    if(choice == Controls::ContentDialogResult::Secondary){
        co_return true;
    }

    co_return false;
}

Windows::Foundation::IAsyncAction MainWindow::OpenProjectFileAsync(Windows::Storage::StorageFile const& file){
    const auto lines{co_await Windows::Storage::FileIO::ReadLinesAsync(file)};

    std::vector<std::wstring> unknownLines;
    std::vector<double> selectedKeyFrames;
    std::vector<std::pair<double, double>> cutIntervals;
    std::wstring loadedFilePath;
    std::wstring snapMode{L"Nearest"};
    double zoomLevel{TimelineZoomSlider().Value()};
    std::vector<IndexedFrameSample> loadedKeyframeIndex;

    for(auto const& lineH : lines){
        const std::wstring line{lineH.c_str()};
        const auto trimmed{Trim(line)};
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
        if(eqPos == std::wstring::npos){
            unknownLines.push_back(line);
            continue;
        }

        const auto key{Trim(line.substr(0, eqPos))};
        const auto value{Trim(line.substr(eqPos + 1))};

        if(key == L"file_path"){
            loadedFilePath = value;
        }else if(key == L"storyline_zoom"){
            try{ zoomLevel = std::stod(value); }catch(...){ unknownLines.push_back(line); }
        }else if(key == L"keyframe_snap_mode"){
            if(value == L"Left" || value == L"Right" || value == L"Nearest"){
                snapMode = value;
            }else{
                unknownLines.push_back(line);
            }
        }else if(key == L"selected_key_frames"){
            selectedKeyFrames = ParseNumberList(value);
        }else if(key == L"cut_intervals"){
            cutIntervals = ParseNumberPairs(value);
        }else if(key == L"keyframe_index"){
            loadedKeyframeIndex = ParseKeyframeVector(value);
        }else{
            unknownLines.push_back(line);
        }
    }

    if(!loadedFilePath.empty()){
        try{
            const auto videoFile{co_await Windows::Storage::StorageFile::GetFileFromPathAsync(loadedFilePath)};
            co_await LoadVideoFileAsync(videoFile);
        }catch(...){
            StatusText().Text(L"Project opened, but referenced video could not be loaded");
        }
    }

    zoomLevel = std::clamp(zoomLevel, TimelineZoomSlider().Minimum(), TimelineZoomSlider().Maximum());
    TimelineZoomSlider().Value(zoomLevel);
    m_keyFrameSnapMode = snapMode;
    m_selectedKeyFrames = std::move(selectedKeyFrames);
    m_cutIntervals = NormalizeAndMergeIntervals(std::move(cutIntervals), m_timelineDurationSeconds > 0 ? m_timelineDurationSeconds : std::numeric_limits<double>::max());
    m_projectUnknownLines = std::move(unknownLines);
    m_projectPath = file.Path();
    if(!loadedKeyframeIndex.empty()){
        m_frameIndex = std::move(loadedKeyframeIndex);
    }

    if(snapMode == L"Left"){
        LeftSnapRadio().IsChecked(true);
    }else if(snapMode == L"Right"){
        RightSnapRadio().IsChecked(true);
    }else{
        NearestSnapRadio().IsChecked(true);
    }

    AddRecentProject(file.Path());
    m_lastSavedProjectSnapshot = BuildProjectSnapshot();
    StatusText().Text(L"Project loaded");
}

Windows::Foundation::IAsyncAction MainWindow::SaveProjectFileAsync(Windows::Storage::StorageFile const& file){
    std::vector<hstring> lines;
    lines.emplace_back(L"# llvc project file");
    lines.emplace_back(L"file_path=" + std::wstring(m_loadedFile ? m_loadedFile.Path().c_str() : L""));
    lines.emplace_back(L"storyline_zoom=" + std::to_wstring(TimelineZoomSlider().Value()));
    lines.emplace_back(L"keyframe_snap_mode=" + m_keyFrameSnapMode);
    lines.emplace_back(L"selected_key_frames=" + SerializeNumberList(m_selectedKeyFrames));
    lines.emplace_back(L"cut_intervals=" + SerializeNumberPairs(m_cutIntervals));
    lines.emplace_back(L"keyframe_index=" + SerializeKeyframeVector(m_frameIndex));

    for(auto const& unknown : m_projectUnknownLines){
        if(Trim(unknown) == L"# llvc project file"){
            continue;
        }
        lines.emplace_back(unknown);
    }

    co_await Windows::Storage::FileIO::WriteLinesAsync(file, single_threaded_vector<hstring>(std::move(lines)));
    m_projectPath = file.Path();
    AddRecentProject(file.Path());
    m_lastSavedProjectSnapshot = BuildProjectSnapshot();
    StatusText().Text(L"Project saved");
}

Windows::Foundation::IAsyncAction MainWindow::ShowInfoDialogAsync(hstring const& title, hstring const& message){
    Controls::ContentDialog dialog{};
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(title));
    dialog.Content(box_value(message));
    dialog.CloseButtonText(L"OK");
    co_await dialog.ShowAsync();
}

Windows::Foundation::IAsyncAction MainWindow::ShowPropertiesDialogAsync(){
    if(!m_loadedFile || !m_mediaInfo.isValid){
        co_await ShowInfoDialogAsync(L"Properties", L"No video is currently loaded.");
        co_return;
    }

    std::wstring content;
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

    co_await ShowInfoDialogAsync(L"Properties", hstring(content));
}

Windows::Foundation::IAsyncAction MainWindow::ShowOptionsDialogAsync(){
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

    m_maxRecentVideos = static_cast<std::uint32_t>(std::clamp(static_cast<int>(std::lround(videosCount.Value())), 1, 20));
    m_maxRecentProjects = static_cast<std::uint32_t>(std::clamp(static_cast<int>(std::lround(projectsCount.Value())), 1, 20));

    if(m_recentVideos.size() > m_maxRecentVideos){
        m_recentVideos.resize(m_maxRecentVideos);
    }
    if(m_recentProjects.size() > m_maxRecentProjects){
        m_recentProjects.resize(m_maxRecentProjects);
    }

    RefreshRecentVideosMenu();
    RefreshRecentProjectsMenu();
    SaveAppSettings();
}

bool MainWindow::IsSupportedVideoSubtype(GUID const& subtype){
    return subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265;
}

std::wstring MainWindow::GuidToCodecName(GUID const& subtype, bool isVideo){
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

    return FormatGuid(subtype);
}

MediaInspectionResult MainWindow::InspectMediaFile(std::wstring const& filePath){
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
    constexpr DWORD invalidStreamIndex{std::numeric_limits<DWORD>::max()};
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
                result.maxKeyFrameSpacing = std::to_wstring(maxKeyFrameSpacing) + L" frames";
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
    if(!IsSupportedVideoSubtype(videoSubtype)){
        result.errorMessage = L"Video codec not supported. Only H.264 and HEVC are allowed.";
        return result;
    }

    std::wstring lowerPath(filePath);
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::towlower);
    if(lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == L".mp4"){
        result.container = L"MP4";
    }else if(lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == L".mov"){
        result.container = L"MOV";
    }else{
        result.errorMessage = L"Container not supported. Only MP4 and MOV are allowed.";
        return result;
    }

    if(videoSubtype == MFVideoFormat_HEVC || videoSubtype == MFVideoFormat_H265){
        if(!HasDecoderForSubtype(videoSubtype)){
            result.errorMessage = L"HEVC support missing (install HEVC Video Extensions)";
            return result;
        }
    }else if(!HasDecoderForSubtype(videoSubtype)){
        result.errorMessage = L"No decoder available";
        return result;
    }

    PROPVARIANT duration{};
    PropVariantInit(&duration);
    if(SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) && duration.vt == VT_UI8){
        const auto seconds = static_cast<double>(duration.uhVal.QuadPart) / 10'000'000.0;
        std::wstringstream ss;
        ss << std::fixed << std::setprecision(3) << seconds << L" s";
        result.duration = ss.str();
    }
    PropVariantClear(&duration);

    result.videoCodec = GuidToCodecName(videoSubtype, true);
    result.audioCodec = audioCount == 0 ? L"none" : GuidToCodecName(audioSubtype, false);
    result.resolution = width > 0 ? (std::to_wstring(width) + L"x" + std::to_wstring(height)) : L"-";
    result.frameRate = (fpsNum > 0 && fpsDen > 0) ? (FormatRatio(fpsNum, fpsDen) + L" fps") : L"-";
    result.videoBitrate = videoBitrate > 0 ? (std::to_wstring(videoBitrate / 1000) + L" kbps") : L"-";
    result.audioBitrate = audioBitrate > 0 ? (std::to_wstring((audioBitrate * 8) / 1000) + L" kbps") : L"none";
    if(videoStreamIndex != invalidStreamIndex){
        AnalyzeKeyFrameCadence(reader.get(), videoStreamIndex, fpsNum, fpsDen, result);
    }
    result.isValid = true;
    return result;
}

std::vector<IndexedFrameSample> MainWindow::BuildKeyframeIndexForFile(std::wstring const& filePath){
    std::vector<IndexedFrameSample> index;
    MFLifetime mf{};

    com_ptr<IMFSourceReader> reader;
    check_hresult(MFCreateSourceReaderFromURL(filePath.c_str(), nullptr, reader.put()));

    constexpr DWORD invalidStream{std::numeric_limits<DWORD>::max()};
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

    for(std::uint32_t sampleIndex = 0;; ++sampleIndex){
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

        LONGLONG duration{};
        (void)sample->GetSampleDuration(&duration);
        UINT32 clean{};
        const bool cleanPoint = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean != 0;

        index.push_back(IndexedFrameSample{
            .time100ns = timestamp,
            .duration100ns = duration,
            .cleanPoint = cleanPoint,
            .sampleIndex = sampleIndex,
        });
    }

    return index;
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
        __debugbreak(); // this should have been caught earlier in this function!
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
    MediaInspectionResult inspected{};
    try{
        inspected = InspectMediaFile(file.Path().c_str());
    }catch(winrt::hresult_error const& ex){
        inspected.errorMessage = L"No decoder available";
        if(ex.code() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)){
            inspected.errorMessage = L"File not found";
        }
    }

    if(!inspected.isValid){
        std::wstring status{L"Open rejected: "};
        status += inspected.errorMessage;
        StatusText().Text(status);
        co_await ShowInfoDialogAsync(L"Unsupported media", hstring(status));
        co_return;
    }

    const auto basicProperties{co_await file.GetBasicPropertiesAsync()};
    inspected.fileSize = FormatFileSize(basicProperties.Size());
    m_mediaInfo = inspected;
    m_frameIndex = BuildKeyframeIndexForFile(file.Path().c_str());

    const auto source{Windows::Media::Core::MediaSource::CreateFromStorageFile(file)};
    m_player.Source(source);
    m_loadedFile = file;
    if(m_projectPath.empty()){
        m_selectedKeyFrames.clear();
        m_cutIntervals.clear();
    }
    AddRecentVideo(file.Path());

    ThumbnailLayer().Children().Clear();
    CutOverlayLayer().Children().Clear();
    TimelineCanvas().Width(640.0);
    TimelineTickCanvas().Width(640.0);
    TimelineTickCanvas().Children().Clear();
    m_timelineDurationSeconds = 0;
    Controls::Canvas::SetLeft(TimelineCursor(), 0);
    SyncTimelineHorizontalScrollBar();

    std::wstring status{L"Loaded: "};
    status += file.Name().c_str();
    status += L" (loading story line...)";
    StatusText().Text(status);
}

winrt::fire_and_forget MainWindow::RenderTimelineAsync(){
    const auto lifetime{get_strong()};

    if(m_isClosing || !m_loadedFile || m_timelineDurationSeconds <= 0){
        co_return;
    }

    if(!DispatcherQueue().HasThreadAccess()){
        const auto weak{get_weak()};
        DispatcherQueue().TryEnqueue([weak](){
            if(const auto self{weak.get()}){
                self->RenderTimelineAsync();
            }
        });
        co_return;
    }

    try{
        const auto renderVersion{++m_timelineRenderVersion};
        const auto zoom{TimelineZoomSlider().Value()};
        const auto totalWidth{std::max(800.0, m_timelineDurationSeconds * 14.0 * zoom)};
        const auto thumbnailCount{std::clamp(static_cast<int>(totalWidth / 150.0), 8, 96)};
        const auto thumbnailWidth{totalWidth / thumbnailCount};

        TimelineCanvas().Width(totalWidth);
        ThumbnailLayer().Children().Clear();
        CutOverlayLayer().Width(totalWidth);
        RenderTimelineTicks();
        RenderKeyframeTicks();
        RenderCutOverlays();
        SyncTimelineHorizontalScrollBar();

        const auto clip{co_await Windows::Media::Editing::MediaClip::CreateFromFileAsync(m_loadedFile)};
        Windows::Media::Editing::MediaComposition composition{};
        composition.Clips().Append(clip);

        if(renderVersion != m_timelineRenderVersion){
            co_return;
        }

        std::vector<bool> thumbnailBuilt(static_cast<size_t>(thumbnailCount), false);

        for(int builtCount = 0; builtCount < thumbnailCount; ++builtCount){
            if(renderVersion != m_timelineRenderVersion || m_isClosing){
                co_return;
            }

            const auto scrollViewer{TimelineScrollViewer()};
            const auto viewportWidth{std::max(0.0, scrollViewer.ViewportWidth())};
            const auto viewportLeft{scrollViewer.HorizontalOffset()};
            const auto viewportRight{viewportLeft + viewportWidth};
            const auto firstVisibleIndex{std::clamp(static_cast<int>(std::floor(viewportLeft / thumbnailWidth)), 0, thumbnailCount - 1)};
            const auto lastVisibleIndex{std::clamp(static_cast<int>(std::floor(std::max(viewportLeft, viewportRight - 1.0) / thumbnailWidth)), 0, thumbnailCount - 1)};

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
            const auto stream{co_await composition.GetThumbnailAsync(SecondsToTimeSpan(t * m_timelineDurationSeconds), 180, 96, Windows::Media::Editing::VideoFramePrecision::NearestFrame)};
            if(renderVersion != m_timelineRenderVersion || m_isClosing){
                co_return;
            }

            Controls::Image image{};
            image.Width(std::max(8.0, thumbnailWidth - 2.0));
            image.Height(86);
            image.Stretch(Media::Stretch::UniformToFill);

            Media::Imaging::BitmapImage bitmap{};
            co_await bitmap.SetSourceAsync(stream);
            image.Source(bitmap);

            Controls::Canvas::SetLeft(image, nextIndex * thumbnailWidth);
            ThumbnailLayer().Children().Append(image);
            thumbnailBuilt[static_cast<size_t>(nextIndex)] = true;
            RenderCutOverlays();
        }

        UpdateTimelineCursorFromPlayback();
        EnsureTimelineCursorVisible(Controls::Canvas::GetLeft(TimelineCursor()));
        SyncTimelineHorizontalScrollBar();

        std::wstring status{L"Loaded: "};
        status += m_loadedFile.Name().c_str();
        status += L" (story line ready)";
        StatusText().Text(status);
    }catch(winrt::hresult_error const& ex){
        std::wstring status{L"Failed to render story line: "};
        status += ex.message().c_str();
        StatusText().Text(status);
    }
}

}
