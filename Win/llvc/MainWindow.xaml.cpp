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
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Editing.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>

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
    Closed({this, &MainWindow::OnClosed});
    RefreshRecentVideosMenu();
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
        SeekTimelineToCanvasX(point.Position().X);
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

void MainWindow::SeekTimelineToCanvasX(const double pointerX){
    if(!m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto x{std::clamp(pointerX, 0.0, TimelineCanvas().Width())};
    const auto ratio{x / TimelineCanvas().Width()};
    const auto targetSeconds{ratio * m_timelineDurationSeconds};

    m_player.PlaybackSession().Position(SecondsToTimeSpan(targetSeconds));
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

Windows::Foundation::TimeSpan MainWindow::SecondsToTimeSpan(const double seconds){
    return std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::duration<double>(seconds));
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

Windows::Foundation::IAsyncAction MainWindow::PropertiesMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    co_await ShowPropertiesDialogAsync();
}

void MainWindow::ExitMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    Close();
}

Windows::Foundation::IAsyncAction MainWindow::AboutMenuItem_Click(IInspectable const&, RoutedEventArgs const&){
    co_await ShowInfoDialogAsync(L"About llvc", L"llvc - Lossless Video Cut\nPreview and timeline exploration tool.");
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

void MainWindow::AddRecentVideo(hstring const& path){
    if(path.empty()){
        return;
    }

    m_recentVideos.erase(std::remove(m_recentVideos.begin(), m_recentVideos.end(), path), m_recentVideos.end());
    m_recentVideos.insert(m_recentVideos.begin(), path);
    constexpr size_t maxRecent = 10;
    if(m_recentVideos.size() > maxRecent){
        m_recentVideos.resize(maxRecent);
    }
    RefreshRecentVideosMenu();
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

    const auto source{Windows::Media::Core::MediaSource::CreateFromStorageFile(file)};
    m_player.Source(source);
    m_loadedFile = file;
    AddRecentVideo(file.Path());

    ThumbnailLayer().Children().Clear();
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
        Controls::Canvas::SetLeft(TimelineCursor(), 0);
        RenderTimelineTicks();
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
