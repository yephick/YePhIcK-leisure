module;

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>

export module llvc.TimelineRenderer;

import std;
import llvc.Project;
import llvc.Timeline;

export namespace llvc{

using namespace ::std;

void renderTimelineMajorTicks(
    const Timeline& timeline,
    const winrt::Microsoft::UI::Xaml::Controls::Canvas& tickCanvas,
    double width,
    double durationSeconds);

void renderTimelineKeyframeTicks(
    const winrt::Microsoft::UI::Xaml::Controls::Canvas& tickCanvas,
    const vector<IndexedFrameSample>& frameIndex,
    int64_t duration100ns);

void renderTimelineCutOverlays(
    const Timeline& timeline,
    const winrt::Microsoft::UI::Xaml::Controls::Canvas& overlayCanvas,
    const vector<pair<int64_t, int64_t>>& cutRanges100ns,
    double width,
    double durationSeconds);

}

namespace llvc{

using namespace ::winrt;
namespace Controls = ::winrt::Microsoft::UI::Xaml::Controls;
namespace Media = ::winrt::Microsoft::UI::Xaml::Media;
namespace Shapes = ::winrt::Microsoft::UI::Xaml::Shapes;

void renderTimelineMajorTicks(const Timeline& timeline, const Controls::Canvas& tickCanvas, double width, double durationSeconds){
    tickCanvas.Children().Clear();
    tickCanvas.Width(width);
    if(durationSeconds <= 0 || width <= 0){
        return;
    }

    for(const auto& tickData: timeline.buildMajorTicks(width, durationSeconds)){
        Shapes::Line majorTick{};
        majorTick.X1(tickData.x);
        majorTick.X2(tickData.x);
        majorTick.Y1(7);
        majorTick.Y2(23);
        majorTick.Stroke(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 180, 180, 180)));
        majorTick.StrokeThickness(1.0);
        tickCanvas.Children().Append(majorTick);

        Controls::TextBlock label{};
        label.Text(tickData.label);
        label.FontSize(11);
        label.Foreground(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 200, 200, 200)));
        Controls::Canvas::SetLeft(label, max(0.0, tickData.x + 3.0));
        Controls::Canvas::SetTop(label, 0);
        tickCanvas.Children().Append(label);
    }
}

void renderTimelineKeyframeTicks(const Controls::Canvas& tickCanvas, const vector<IndexedFrameSample>& frameIndex, int64_t duration100ns){
    if(frameIndex.empty()){
        return;
    }

    const auto width{tickCanvas.Width()};
    const auto total100ns{static_cast<double>(duration100ns)};
    if(width <= 0 || total100ns <= 0){
        return;
    }

    for(const auto& marker: frameIndex){
        const auto x{clamp((marker.time100ns / total100ns) * width, 0.0, width)};
        Shapes::Line tick{};
        tick.X1(x);
        tick.X2(x);
        tick.Y1(0);
        tick.Y2(8.0);
        const auto color{marker.cleanPoint
            ? Windows::UI::ColorHelper::FromArgb(255, 72, 214, 104)
            : Windows::UI::ColorHelper::FromArgb(255, 255, 80, 80)};
        tick.Stroke(Media::SolidColorBrush(color));
        tick.StrokeThickness(2.0);
        tickCanvas.Children().Append(tick);
    }
}

void renderTimelineCutOverlays(const Timeline& timeline, const Controls::Canvas& overlayCanvas, const vector<pair<int64_t, int64_t>>& cutRanges100ns, double width, double durationSeconds){
    overlayCanvas.Children().Clear();
    overlayCanvas.Width(width);
    if(durationSeconds <= 0 || width <= 0){
        return;
    }

    const auto overlayColor{Windows::UI::ColorHelper::FromArgb(180, 0, 0, 0)};
    const auto crossColor{Windows::UI::ColorHelper::FromArgb(220, 255, 48, 48)};
    for(const auto& overlay: timeline.buildCutOverlays(cutRanges100ns, width, durationSeconds)){
        Shapes::Rectangle block{};
        block.Width(overlay.width);
        block.Height(86.0);
        block.Fill(Media::SolidColorBrush(overlayColor));
        block.IsHitTestVisible(false);
        Controls::Canvas::SetLeft(block, overlay.left);
        Controls::Canvas::SetTop(block, 0.0);
        overlayCanvas.Children().Append(block);

        Shapes::Line diagonalOne{};
        diagonalOne.X1(overlay.left);
        diagonalOne.Y1(0.0);
        diagonalOne.X2(overlay.left + overlay.width);
        diagonalOne.Y2(86.0);
        diagonalOne.Stroke(Media::SolidColorBrush(crossColor));
        diagonalOne.StrokeThickness(2.0);
        diagonalOne.IsHitTestVisible(false);
        overlayCanvas.Children().Append(diagonalOne);

        Shapes::Line diagonalTwo{};
        diagonalTwo.X1(overlay.left + overlay.width);
        diagonalTwo.Y1(0.0);
        diagonalTwo.X2(overlay.left);
        diagonalTwo.Y2(86.0);
        diagonalTwo.Stroke(Media::SolidColorBrush(crossColor));
        diagonalTwo.StrokeThickness(2.0);
        diagonalTwo.IsHitTestVisible(false);
        overlayCanvas.Children().Append(diagonalTwo);
    }
}

}
