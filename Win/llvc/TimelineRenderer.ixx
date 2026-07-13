module;

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>

export module llvc.TimelineRenderer;

import std;
import llvc.Project;
import llvc.Session;
import llvc.Timeline;

export namespace llvc{

using namespace ::std;

void renderTimelineMajorTicks(
    const Timeline& timeline,
    const winrt::Microsoft::UI::Xaml::Controls::Canvas& tickCanvas,
    double width,
    double durationSeconds,
    double visibleWidth);

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

void renderTimelineMarkers(const winrt::Microsoft::UI::Xaml::Controls::Canvas& tickCanvas, span<const TimelineMarkerRenderItem> markers);
void renderTimelineCutScenes(const winrt::Microsoft::UI::Xaml::Controls::Canvas& overlayCanvas, double width, span<const TimelineCutSceneRenderItem> cutScenes);

}

namespace llvc{

using namespace ::winrt;
namespace Controls = ::winrt::Microsoft::UI::Xaml::Controls;
namespace Media = ::winrt::Microsoft::UI::Xaml::Media;
namespace Shapes = ::winrt::Microsoft::UI::Xaml::Shapes;

void renderTimelineMajorTicks(const Timeline& timeline, const Controls::Canvas& tickCanvas, double width, double durationSeconds, double visibleWidth){
    tickCanvas.Children().Clear();
    tickCanvas.Width(width);
    if(durationSeconds <= 0 || width <= 0){
        return;
    }

    const auto effectiveVisibleWidth{max(visibleWidth, 1.0)};
    const auto minimumVisibleTickCount{10};
    const auto visibleWidthDrivenTickCount{static_cast<int>(ceil((width / effectiveVisibleWidth) * minimumVisibleTickCount))};
    const auto spacingDrivenTickCount{static_cast<int>(ceil(width / 120.0))};
    const auto derivedTickCount{max(minimumVisibleTickCount, max(visibleWidthDrivenTickCount, spacingDrivenTickCount))};
    const auto ticks{timeline.buildMajorTicks(width, durationSeconds, derivedTickCount)};
    for(const auto& tickData: ticks){
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

void renderTimelineMarkers(const Controls::Canvas& tickCanvas, span<const TimelineMarkerRenderItem> markers){
    for(const auto& marker: markers){
        Shapes::Line tick{};
        tick.X1(marker.x); tick.X2(marker.x); tick.Y1(0); tick.Y2(8.0);
        tick.Stroke(Media::SolidColorBrush(marker.isEvaluatedAgainstRap ? Windows::UI::ColorHelper::FromArgb(255, 72, 214, 104) : Windows::UI::ColorHelper::FromArgb(255, 255, 80, 80)));
        tick.StrokeThickness(2.0);
        tickCanvas.Children().Append(tick);
    }
}

void renderTimelineCutScenes(const Controls::Canvas& overlayCanvas, double width, span<const TimelineCutSceneRenderItem> cutScenes){
    overlayCanvas.Children().Clear(); overlayCanvas.Width(width);
    const auto overlayColor{Windows::UI::ColorHelper::FromArgb(180, 0, 0, 0)};
    const auto crossColor{Windows::UI::ColorHelper::FromArgb(220, 255, 48, 48)};
    for(const auto& scene: cutScenes){
        Controls::Canvas sceneCanvas{};
        sceneCanvas.Width(scene.width); sceneCanvas.Height(86.0); sceneCanvas.IsHitTestVisible(false);
        Controls::Canvas::SetLeft(sceneCanvas, scene.left);

        Shapes::Rectangle block{}; block.Width(scene.width); block.Height(86.0); block.Fill(Media::SolidColorBrush(overlayColor)); block.IsHitTestVisible(false);
        sceneCanvas.Children().Append(block);

        for(const auto [x1, x2]: {pair{0.0, scene.width}, pair{scene.width, 0.0}}){
            Shapes::Line diagonal{};
            diagonal.X1(x1); diagonal.Y1(0.0); diagonal.X2(x2); diagonal.Y2(86.0);
            diagonal.Stroke(Media::SolidColorBrush(crossColor)); diagonal.StrokeThickness(2.0); diagonal.IsHitTestVisible(false);
            sceneCanvas.Children().Append(diagonal);
        }
        overlayCanvas.Children().Append(sceneCanvas);
    }
}

}
