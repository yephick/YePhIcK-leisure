module;

#include "pch.h"

export module llvc.Timeline;

import std;
import llvc.Project;

export namespace llvc{

using namespace ::std;

struct TimelineMajorTick final{
    double x{};
    wstring label{};
};

struct TimelineCutOverlay final{
    double left{};
    double width{};
};

struct Timeline final{
    static constexpr int64_t HnsPerSecond{10'000'000LL};

    std::optional<int64_t> pointToTime100ns(double pointerX, double width, int64_t duration100ns) const;
    double timeToCanvasX(int64_t time100ns, int64_t duration100ns, double width) const;
    double dragTargetOffset(double dragStartOffset, double pointerDeltaX, double canvasWidth, double viewportWidth) const;
    std::optional<double> cursorOffsetToEnsureVisible(double cursorLeft, double currentOffset, double viewportWidth, double canvasWidth, double padding = 48.0) const;
    vector<TimelineMajorTick> buildMajorTicks(double width, double durationSeconds) const;
    vector<double> buildKeyframeTickPositions(const vector<IndexedFrameSample>& frameIndex, double width, double durationSeconds) const;
    vector<TimelineCutOverlay> buildCutOverlays(const vector<pair<int64_t, int64_t>>& cutRanges100ns, double width, double durationSeconds) const;
    bool isTimeInsideRanges(int64_t time100ns, const vector<pair<int64_t, int64_t>>& ranges) const;
    int64_t frameStep100ns(uint32_t fpsNum, uint32_t fpsDen) const;
    int64_t applyFrameStep(int64_t current100ns, int delta, int64_t duration100ns, int64_t frameStep100ns) const;
    std::optional<int64_t> markerNavigationTarget100ns(const vector<IndexedFrameSample>& markers, int64_t current100ns, int direction) const;
};

}

namespace llvc{

std::optional<int64_t> Timeline::pointToTime100ns(double pointerX, double width, int64_t duration100ns) const{
    if(duration100ns <= 0 || width <= 0){
        return std::nullopt;
    }

    const auto clampedX{clamp(pointerX, 0.0, width)};
    return static_cast<int64_t>((clampedX / width) * duration100ns);
}

double Timeline::timeToCanvasX(int64_t time100ns, int64_t duration100ns, double width) const{
    if(duration100ns <= 0 || width <= 0){
        return 0.0;
    }

    const auto ratio{clamp(static_cast<double>(time100ns) / static_cast<double>(duration100ns), 0.0, 1.0)};
    return ratio * width;
}

double Timeline::dragTargetOffset(double dragStartOffset, double pointerDeltaX, double canvasWidth, double viewportWidth) const{
    const auto maxOffset{max(0.0, canvasWidth - viewportWidth)};
    return clamp(dragStartOffset - pointerDeltaX, 0.0, maxOffset);
}

std::optional<double> Timeline::cursorOffsetToEnsureVisible(double cursorLeft, double currentOffset, double viewportWidth, double canvasWidth, double padding) const{
    if(viewportWidth <= 0 || canvasWidth <= 0){
        return std::nullopt;
    }

    const auto minVisible{currentOffset + padding};
    const auto maxVisible{currentOffset + viewportWidth - padding};
    const auto maxOffset{max(0.0, canvasWidth - viewportWidth)};

    if(cursorLeft < minVisible){
        return clamp(cursorLeft - padding, 0.0, maxOffset);
    }

    if(cursorLeft > maxVisible){
        return clamp(cursorLeft + padding - viewportWidth, 0.0, maxOffset);
    }

    return std::nullopt;
}

vector<TimelineMajorTick> Timeline::buildMajorTicks(double width, double durationSeconds) const{
    vector<TimelineMajorTick> ticks;
    if(width <= 0 || durationSeconds <= 0){
        return ticks;
    }

    const auto majorTickCount{clamp(static_cast<int>(ceil(width / 120.0)), 6, 36)};
    ticks.reserve(static_cast<size_t>(majorTickCount + 1));

    for(int i{}; i <= majorTickCount; ++i){
        const auto ratio{static_cast<double>(i) / majorTickCount};
        const auto totalSeconds{static_cast<int>(ratio * durationSeconds + 0.5)};
        const auto minutes{totalSeconds / 60};
        const auto seconds{totalSeconds % 60};

        auto label{to_wstring(minutes)};
        label += L":";
        if(seconds < 10){
            label += L"0";
        }
        label += to_wstring(seconds);

        ticks.push_back(TimelineMajorTick{.x = ratio * width, .label = std::move(label)});
    }

    return ticks;
}

vector<double> Timeline::buildKeyframeTickPositions(const vector<IndexedFrameSample>& frameIndex, double width, double durationSeconds) const{
    vector<double> positions;
    if(frameIndex.empty() || width <= 0 || durationSeconds <= 0){
        return positions;
    }

    const auto total100ns{durationSeconds * HnsPerSecond};
    for(const auto& frame: frameIndex){
        if(!frame.cleanPoint){
            continue;
        }

        positions.push_back(clamp((frame.time100ns / total100ns) * width, 0.0, width));
    }

    return positions;
}

vector<TimelineCutOverlay> Timeline::buildCutOverlays(const vector<pair<int64_t, int64_t>>& cutRanges100ns, double width, double durationSeconds) const{
    vector<TimelineCutOverlay> overlays;
    if(width <= 0 || durationSeconds <= 0){
        return overlays;
    }

    overlays.reserve(cutRanges100ns.size());
    for(const auto& [startTime100ns, endTime100ns]: cutRanges100ns){
        const auto start{clamp((static_cast<double>(startTime100ns) / HnsPerSecond) / durationSeconds, 0.0, 1.0)};
        const auto end{clamp((static_cast<double>(endTime100ns) / HnsPerSecond) / durationSeconds, 0.0, 1.0)};
        if(end <= start){
            continue;
        }

        const auto left{start * width};
        const auto blockWidth{max(1.0, (end - start) * width)};
        overlays.push_back(TimelineCutOverlay{.left = left, .width = blockWidth});
    }

    return overlays;
}

bool Timeline::isTimeInsideRanges(int64_t time100ns, const vector<pair<int64_t, int64_t>>& ranges) const{
    for(const auto& [start100ns, end100ns]: ranges){
        if(time100ns < start100ns){
            return false;
        }
        if(time100ns < end100ns){
            return true;
        }
    }

    return false;
}

int64_t Timeline::frameStep100ns(uint32_t fpsNum, uint32_t fpsDen) const{
    constexpr auto fallbackFrameDuration100ns{333'667LL};
    if(fpsNum == 0 || fpsDen == 0){
        return fallbackFrameDuration100ns;
    }

    const auto exactDuration100ns{(HnsPerSecond * static_cast<int64_t>(fpsDen)) / static_cast<int64_t>(fpsNum)};
    return exactDuration100ns > 0 ? exactDuration100ns : fallbackFrameDuration100ns;
}

int64_t Timeline::applyFrameStep(int64_t current100ns, int delta, int64_t duration100ns, int64_t frameStep100ns) const{
    if(delta == 0 || duration100ns <= 0 || frameStep100ns <= 0){
        return clamp(current100ns, 0LL, max(0LL, duration100ns));
    }

    const auto direction{delta < 0 ? -1 : 1};
    return clamp(current100ns + (direction * frameStep100ns), 0LL, duration100ns);
}

std::optional<int64_t> Timeline::markerNavigationTarget100ns(const vector<IndexedFrameSample>& markers, int64_t current100ns, int direction) const{
    if(direction == 0 || markers.empty()){
        return std::nullopt;
    }

    if(direction < 0){
        for(auto it{markers.rbegin()}; it != markers.rend(); ++it){
            if(it->time100ns < current100ns){
                return it->time100ns;
            }
        }
        return std::nullopt;
    }

    const auto it{find_if(markers.begin(), markers.end(), [current100ns](const auto& marker){ return marker.time100ns > current100ns; })};
    if(it == markers.end()){
        return std::nullopt;
    }

    return it->time100ns;
}

}
