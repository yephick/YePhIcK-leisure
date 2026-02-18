#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <vector>

#include <microsoft.ui.xaml.window.h>
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

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::llvc::implementation{

constexpr auto W_POS_L{L"WindowLeft"};
constexpr auto W_POS_T{L"WindowTop"};
constexpr auto W_POS_W{L"WindowWidth"};
constexpr auto W_POS_H{L"WindowHeight"};
constexpr auto W_POS_DPI{L"WindowDpi"};

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
    const HMONITOR monitor{::MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)};
    if(!monitor){
        return false;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
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

    const uint32_t currentDpi{::GetDpiForWindow(hwnd)};
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

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if(GetWindowPlacement(hwnd, &placement) && placement.showCmd == SW_SHOWMAXIMIZED){
        bounds = placement.rcNormalPosition;
    }

    const auto localSettings{Windows::Storage::ApplicationData::Current().LocalSettings()};
    const auto values{localSettings.Values()};
    const uint32_t dpi{::GetDpiForWindow(hwnd)};
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
    const double offset{scrollViewer.HorizontalOffset()};
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
    const double pointerX{static_cast<double>(point.Position().X)};
    const double deltaX{pointerX - m_timelineDragStartX};

    if(std::fabs(deltaX) > 4.0){
        m_timelineDragMoved = true;
    }

    const auto scrollViewer{TimelineScrollViewer()};
    const double viewportWidth{scrollViewer.ViewportWidth()};
    const double maxOffset{std::max(0.0, TimelineCanvas().Width() - viewportWidth)};
    const double target{std::clamp(m_timelineDragStartOffset - deltaX, 0.0, maxOffset)};
    const auto targetOffset{box_value(target).as<Windows::Foundation::IReference<double>>()};
    scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    e.Handled(true);
}

void MainWindow::TimelineCanvas_PointerReleased(IInspectable const&, Input::PointerRoutedEventArgs const& e){
    if(!m_isTimelineDragging || e.Pointer().PointerId() != m_timelineDragPointerId){
        return;
    }

    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    const double pointerX{static_cast<double>(point.Position().X)};
    const bool dragged{m_timelineDragMoved};

    m_isTimelineDragging = false;
    m_timelineDragMoved = false;
    TimelineCanvas().ReleasePointerCapture(e.Pointer());

    if(!dragged){
        SeekTimelineToCanvasX(pointerX);
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
    const double seconds{std::max(0.0, current.count() / 10'000'000.0)};
    const double ratio{std::clamp(seconds / m_timelineDurationSeconds, 0.0, 1.0)};
    const double left{ratio * TimelineCanvas().Width()};
    Controls::Canvas::SetLeft(TimelineCursor(), left);

    if(m_player.PlaybackSession().PlaybackState() == Windows::Media::Playback::MediaPlaybackState::Playing){
        EnsureTimelineCursorVisible(left);
    }

    SyncTimelineHorizontalScrollBar();
}

void MainWindow::SyncTimelineHorizontalScrollBar(){
    const auto scrollViewer{TimelineScrollViewer()};
    const double scrollableWidth{std::max(0.0, scrollViewer.ScrollableWidth())};
    const double viewportWidth{std::max(1.0, scrollViewer.ViewportWidth())};

    auto bar{TimelineHorizontalScrollBar()};
    bar.Maximum(std::max(1.0, scrollableWidth));
    bar.LargeChange(std::max(32.0, viewportWidth * 0.8));
    bar.SmallChange(24.0);
    bar.IsEnabled(true);
    bar.Visibility(Visibility::Visible);

    const double currentValue{bar.Value()};
    const double offset{std::clamp(scrollViewer.HorizontalOffset(), 0.0, bar.Maximum())};
    if(std::fabs(currentValue - offset) > 0.5){
        bar.Value(offset);
    }
}

void MainWindow::SeekTimelineToCanvasX(const double pointerX){
    if(!m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const double x{std::clamp(pointerX, 0.0, TimelineCanvas().Width())};
    const double ratio{x / TimelineCanvas().Width()};
    const double targetSeconds{ratio * m_timelineDurationSeconds};

    m_player.PlaybackSession().Position(SecondsToTimeSpan(targetSeconds));
    UpdateTimelineCursorFromPlayback();
}

void MainWindow::EnsureTimelineCursorVisible(const double cursorLeft){
    const auto scrollViewer{TimelineScrollViewer()};
    const double currentOffset{scrollViewer.HorizontalOffset()};
    const double viewportWidth{scrollViewer.ViewportWidth()};

    if(viewportWidth <= 0){
        return;
    }

    constexpr double cursorPadding{48.0};
    const double minVisible{currentOffset + cursorPadding};
    const double maxVisible{currentOffset + viewportWidth - cursorPadding};
    const double maxOffset{std::max(0.0, TimelineCanvas().Width() - viewportWidth)};

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
        const double ratio{static_cast<double>(i) / static_cast<double>(majorTickCount)};
        const double x{ratio * width};

        Shapes::Line majorTick{};
        majorTick.X1(x);
        majorTick.X2(x);
        majorTick.Y1(7);
        majorTick.Y2(23);
        majorTick.Stroke(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 180, 180, 180)));
        majorTick.StrokeThickness(1.0);
        TimelineTickCanvas().Children().Append(majorTick);

        Controls::TextBlock label{};
        const int totalSeconds{static_cast<int>(std::round(ratio * m_timelineDurationSeconds))};
        const int minutes{totalSeconds / 60};
        const int seconds{totalSeconds % 60};
        std::wstring text{std::to_wstring(minutes)};
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
    const auto source{Windows::Media::Core::MediaSource::CreateFromStorageFile(file)};
    m_player.Source(source);
    m_loadedFile = file;

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

    co_return;
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
        const double zoom{TimelineZoomSlider().Value()};
        const double totalWidth{std::max(800.0, m_timelineDurationSeconds * 14.0 * zoom)};
        const int thumbnailCount{std::clamp(static_cast<int>(totalWidth / 150.0), 8, 96)};
        const double thumbnailWidth{totalWidth / static_cast<double>(thumbnailCount)};

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
            const double viewportWidth{std::max(0.0, scrollViewer.ViewportWidth())};
            const double viewportLeft{scrollViewer.HorizontalOffset()};
            const double viewportRight{viewportLeft + viewportWidth};
            const int firstVisibleIndex{std::clamp(static_cast<int>(std::floor(viewportLeft / thumbnailWidth)), 0, thumbnailCount - 1)};
            const int lastVisibleIndex{std::clamp(static_cast<int>(std::floor(std::max(viewportLeft, viewportRight - 1.0) / thumbnailWidth)), 0, thumbnailCount - 1)};

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

            const double t{(static_cast<double>(nextIndex) + 0.5) / static_cast<double>(thumbnailCount)};
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
