#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"

#include <algorithm>
#include <cmath>

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
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

void MainWindow::RestoreWindowPlacement(){
    const auto localSettings{Windows::Storage::ApplicationData::Current().LocalSettings()};
    const auto values{localSettings.Values()};

    if(!values.HasKey(W_POS_L) || !values.HasKey(W_POS_T) || !values.HasKey(W_POS_W) || !values.HasKey(W_POS_H)){
        return;
    }

    const auto left{unbox_value<int32_t>(values.Lookup(W_POS_L))};
    const auto top{unbox_value<int32_t>(values.Lookup(W_POS_T))};
    const auto width{unbox_value<int32_t>(values.Lookup(W_POS_W))};
    const auto height{unbox_value<int32_t>(values.Lookup(W_POS_H))};

    const auto hwnd{GetWindowHandle()};
    SetWindowPos(hwnd, nullptr, left, top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
}

void MainWindow::SaveWindowPlacement() const{
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if(!GetWindowPlacement(GetWindowHandle(), &placement)){
        return;
    }

    const auto normalBounds{placement.rcNormalPosition};

    const auto localSettings{Windows::Storage::ApplicationData::Current().LocalSettings()};
    const auto values{localSettings.Values()};
    values.Insert(W_POS_L, box_value(static_cast<int32_t>(normalBounds.left)));
    values.Insert(W_POS_T, box_value(static_cast<int32_t>(normalBounds.top)));
    values.Insert(W_POS_W, box_value(static_cast<int32_t>(normalBounds.right - normalBounds.left)));
    values.Insert(W_POS_H, box_value(static_cast<int32_t>(normalBounds.bottom - normalBounds.top)));
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

void MainWindow::TimelineCanvas_PointerPressed(IInspectable const&, Input::PointerRoutedEventArgs const& e){
    if(!m_player || m_timelineDurationSeconds <= 0){
        return;
    }

    if(TimelineCanvas().Width() <= 0){
        return;
    }

    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    const double pointerX{static_cast<double>(point.Position().X)};
    const double x{std::clamp(pointerX, 0.0, TimelineCanvas().Width())};
    const double ratio{x / TimelineCanvas().Width()};
    const double targetSeconds{ratio * m_timelineDurationSeconds};

    m_player.PlaybackSession().Position(SecondsToTimeSpan(targetSeconds));
    UpdateTimelineCursorFromPlayback();
}

void MainWindow::OnNaturalDurationChanged(Windows::Media::Playback::MediaPlaybackSession const& sender, IInspectable const&){
    const auto duration{sender.NaturalDuration()};
    m_timelineDurationSeconds = std::max(0.0, static_cast<double>(duration.count()) / 10'000'000.0);

    if(m_loadedFile && m_timelineDurationSeconds > 0){
        RenderTimelineAsync();
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
    const double seconds{std::max(0.0, static_cast<double>(current.count()) / 10'000'000.0)};
    const double ratio{std::clamp(seconds / m_timelineDurationSeconds, 0.0, 1.0)};
    const double left{ratio * TimelineCanvas().Width()};
    Controls::Canvas::SetLeft(TimelineCursor(), left);
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
    m_timelineDurationSeconds = 0;
    Controls::Canvas::SetLeft(TimelineCursor(), 0);

    std::wstring status{L"Loaded: "};
    status += file.Name().c_str();
    status += L" (loading story line...)";
    StatusText().Text(status);

    co_return;
}

winrt::fire_and_forget MainWindow::RenderTimelineAsync(){
    if(m_isClosing || !m_loadedFile || m_timelineDurationSeconds <= 0){
        co_return;
    }

    const auto renderVersion{++m_timelineRenderVersion};
    const double zoom{TimelineZoomSlider().Value()};
    const double totalWidth{std::max(800.0, m_timelineDurationSeconds * 14.0 * zoom)};
    const int thumbnailCount{std::clamp(static_cast<int>(totalWidth / 150.0), 8, 96)};
    const double thumbnailWidth{totalWidth / static_cast<double>(thumbnailCount)};

    TimelineCanvas().Width(totalWidth);
    ThumbnailLayer().Children().Clear();
    Controls::Canvas::SetLeft(TimelineCursor(), 0);

    const auto clip{co_await Windows::Media::Editing::MediaClip::CreateFromFileAsync(m_loadedFile)};
    Windows::Media::Editing::MediaComposition composition{};
    composition.Clips().Append(clip);

    if(renderVersion != m_timelineRenderVersion){
        co_return;
    }

    for(int i = 0; i < thumbnailCount; ++i){
        if(renderVersion != m_timelineRenderVersion){
            co_return;
        }

        const double t{(static_cast<double>(i) + 0.5) / static_cast<double>(thumbnailCount)};
        const auto stream{co_await composition.GetThumbnailAsync(SecondsToTimeSpan(t * m_timelineDurationSeconds), 180, 96, Windows::Media::Editing::VideoFramePrecision::NearestFrame)};
        if(renderVersion != m_timelineRenderVersion){
            co_return;
        }

        Controls::Image image{};
        image.Width(std::max(8.0, thumbnailWidth - 2.0));
        image.Height(86);
        image.Stretch(Media::Stretch::UniformToFill);

        Media::Imaging::BitmapImage bitmap{};
        co_await bitmap.SetSourceAsync(stream);
        image.Source(bitmap);

        Controls::Canvas::SetLeft(image, i * thumbnailWidth);
        ThumbnailLayer().Children().Append(image);
    }

    UpdateTimelineCursorFromPlayback();

    std::wstring status{L"Loaded: "};
    status += m_loadedFile.Name().c_str();
    status += L" (story line ready)";
    StatusText().Text(status);
}

}
