export module llvc.Timeline;

import std;
import llvc.Project;

export namespace llvc{

using namespace ::std;

vector<pair<uint32_t, uint32_t>> normalizeAndMergeIndexIntervals(
    vector<pair<uint32_t, uint32_t>> intervals,
    size_t keyframeCount);

struct Timeline;

struct TimelineMajorTick final{
    double x{};
    wstring label{};
};

struct TimelineScrollbarAnchor final{
    double ratio{};

    static std::optional<TimelineScrollbarAnchor> capture(double currentOffset, double maximumOffset);
    double restoreOffset(double maximumOffset) const;
};

struct TimelinePointerZoomAnchor final{
    int64_t time100ns{};
    double viewportPointerX{};

    double restoreOffset(const Timeline& timeline, int64_t duration100ns, double totalWidth, double viewportWidth) const;
};

struct TimelineThumbnailStripPlan final{
    double totalWidth{0.0};
    int thumbnailCount{0};
    double thumbnailWidth{0.0};
};

struct TimelineThumbnailIndexRange final{
    int first{};
    int last{};
};

struct TimelineRenderPostActions final{
    bool shouldQueueRapLookup{false};
    wstring readyStatus{};
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
    vector<TimelineMajorTick> buildMajorTicks(double width, double durationSeconds, int desiredTickCount = 0) const;
    vector<double> buildKeyframeTickPositions(const vector<IndexedFrameSample>& frameIndex, double width, double durationSeconds) const;
    vector<TimelineCutOverlay> buildCutOverlays(const vector<pair<int64_t, int64_t>>& cutRanges100ns, double width, double durationSeconds) const;
    bool isTimeInsideRanges(int64_t time100ns, const vector<pair<int64_t, int64_t>>& ranges) const;
    int64_t frameStep100ns(uint32_t fpsNum, uint32_t fpsDen) const;
    int64_t applyFrameStep(int64_t current100ns, int delta, int64_t duration100ns, int64_t frameStep100ns) const;
    std::optional<int64_t> markerNavigationTarget100ns(const vector<IndexedFrameSample>& markers, int64_t current100ns, int direction) const;
    TimelineThumbnailStripPlan buildThumbnailStripPlan(double durationSeconds, double zoomSetting) const;
    TimelineThumbnailIndexRange visibleThumbnailRange(double viewportLeft, double viewportWidth, double thumbnailWidth, int thumbnailCount) const;
    TimelineThumbnailIndexRange expandThumbnailRange(TimelineThumbnailIndexRange range, int padding, int thumbnailCount) const;
    int chooseNextThumbnailIndex(const vector<bool>& thumbnailBuilt, TimelineThumbnailIndexRange visibleRange, bool allowOffscreenExpansion) const;
    TimelineRenderPostActions buildRenderPostActions(const wstring& videoName, bool hasRequestedCuts, bool cachedRapLookupAttempted, bool isRapLookupInProgress) const;
};

}

namespace llvc{

vector<pair<uint32_t, uint32_t>> normalizeAndMergeIndexIntervals(vector<pair<uint32_t, uint32_t>> intervals, size_t keyframeCount){
    constexpr auto timelineEdgeSentinel{numeric_limits<uint32_t>::max()};

    using RankedInterval = pair<int64_t, int64_t>;
    vector<RankedInterval> normalized;
    normalized.reserve(intervals.size());

    for(const auto& interval: intervals){
        if((interval.first == timelineEdgeSentinel && interval.second == timelineEdgeSentinel)
            || (interval.first == timelineEdgeSentinel && interval.second >= keyframeCount)
            || (interval.second == timelineEdgeSentinel && interval.first >= keyframeCount)
            || (interval.first != timelineEdgeSentinel && interval.second != timelineEdgeSentinel && (interval.first >= keyframeCount || interval.second >= keyframeCount))){
            continue;
        }

        const auto startRank{interval.first == timelineEdgeSentinel ? static_cast<int64_t>(-1) : static_cast<int64_t>(interval.first)};
        const auto endRank{interval.second == timelineEdgeSentinel ? static_cast<int64_t>(keyframeCount) : static_cast<int64_t>(interval.second)};
        if(startRank >= endRank){
            continue;
        }
        normalized.emplace_back(startRank, endRank);
    }

    sort(normalized.begin(), normalized.end());

    vector<RankedInterval> mergedRanks;
    for(const auto& interval: normalized){
        if(mergedRanks.empty() || interval.first > mergedRanks.back().second){
            mergedRanks.push_back(interval);
        }else{
            mergedRanks.back().second = max(mergedRanks.back().second, interval.second);
        }
    }

    vector<pair<uint32_t, uint32_t>> merged;
    merged.reserve(mergedRanks.size());
    for(const auto& interval: mergedRanks){
        const auto start{interval.first < 0 ? timelineEdgeSentinel : static_cast<uint32_t>(interval.first)};
        const auto end{interval.second >= static_cast<int64_t>(keyframeCount) ? timelineEdgeSentinel : static_cast<uint32_t>(interval.second)};
        merged.emplace_back(start, end);
    }

    return merged;
}

std::optional<TimelineScrollbarAnchor> TimelineScrollbarAnchor::capture(double currentOffset, double maximumOffset){
    if(maximumOffset < 0.0){
        return std::nullopt;
    }

    if(maximumOffset == 0.0){
        return TimelineScrollbarAnchor{.ratio = 0.0};
    }

    return TimelineScrollbarAnchor{.ratio = clamp(currentOffset / maximumOffset, 0.0, 1.0)};
}

double TimelineScrollbarAnchor::restoreOffset(double maximumOffset) const{
    return clamp(ratio * max(0.0, maximumOffset), 0.0, max(0.0, maximumOffset));
}

double TimelinePointerZoomAnchor::restoreOffset(const Timeline& timeline, int64_t duration100ns, double totalWidth, double viewportWidth) const{
    if(duration100ns <= 0 || totalWidth <= 0.0 || !isfinite(totalWidth) || !isfinite(viewportWidth) || !isfinite(viewportPointerX)){
        return 0.0;
    }

    const auto safeViewportWidth{max(0.0, viewportWidth)};
    const auto safeMaxOffset{max(0.0, totalWidth - safeViewportWidth)};
    const auto safePointerX{clamp(viewportPointerX, 0.0, safeViewportWidth)};
    const auto safeTime100ns{clamp<int64_t>(time100ns, 0, duration100ns)};
    const auto anchorCanvasX{clamp(timeline.timeToCanvasX(safeTime100ns, duration100ns, totalWidth), 0.0, totalWidth)};
    return clamp(anchorCanvasX - safePointerX, 0.0, safeMaxOffset);
}

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

vector<TimelineMajorTick> Timeline::buildMajorTicks(double width, double durationSeconds, int desiredTickCount) const{
    vector<TimelineMajorTick> ticks;
    if(width <= 0 || durationSeconds <= 0){
        return ticks;
    }

    const auto majorTickCount{desiredTickCount > 0
        ? max(1, desiredTickCount)
        : clamp(static_cast<int>(ceil(width / 120.0)), 6, 36)};
    ticks.reserve(static_cast<size_t>(majorTickCount + 1));

    for(int i{}; i <= majorTickCount; ++i){
        const auto ratio{static_cast<double>(i) / majorTickCount};
        const auto totalSeconds{static_cast<int>(ratio * durationSeconds + 0.5)};
        const auto hours{totalSeconds / 3600};
        const auto minutes{(totalSeconds % 3600) / 60};
        const auto seconds{totalSeconds % 60};

        auto label{wstring{}};
        if(hours > 0){
            label = to_wstring(hours);
            label += L":";
            if(minutes < 10){
                label += L"0";
            }
            label += to_wstring(minutes);
        }else{
            label = to_wstring(minutes);
        }

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

TimelineThumbnailStripPlan Timeline::buildThumbnailStripPlan(double durationSeconds, double zoomSetting) const{
    constexpr auto minTimelineWidth{800.0};
    constexpr auto pixelsPerSecondAtBaseZoom{14.0};
    constexpr auto baseZoom{4.0};
    constexpr auto targetThumbnailWidth{153.0};

    if(durationSeconds <= 0){
        return TimelineThumbnailStripPlan{
            .totalWidth = minTimelineWidth,
            .thumbnailCount = 1,
            .thumbnailWidth = minTimelineWidth,
        };
    }

    const auto zoomScale{zoomSetting / baseZoom};
    const auto totalWidth{max(minTimelineWidth, durationSeconds * pixelsPerSecondAtBaseZoom * zoomScale)};
    const auto thumbnailCount{max(1, static_cast<int>(ceil(totalWidth / targetThumbnailWidth)))};
    return TimelineThumbnailStripPlan{
        .totalWidth = totalWidth,
        .thumbnailCount = thumbnailCount,
        .thumbnailWidth = totalWidth / thumbnailCount,
    };
}

TimelineThumbnailIndexRange Timeline::visibleThumbnailRange(double viewportLeft, double viewportWidth, double thumbnailWidth, int thumbnailCount) const{
    if(thumbnailCount <= 0 || thumbnailWidth <= 0.0){
        return TimelineThumbnailIndexRange{};
    }

    const auto safeViewportLeft{max(0.0, viewportLeft)};
    const auto viewportRight{safeViewportLeft + max(0.0, viewportWidth)};
    return TimelineThumbnailIndexRange{
        .first = clamp(static_cast<int>(floor(safeViewportLeft / thumbnailWidth)), 0, thumbnailCount - 1),
        .last = clamp(static_cast<int>(floor(max(safeViewportLeft, viewportRight - 1.0) / thumbnailWidth)), 0, thumbnailCount - 1),
    };
}

TimelineThumbnailIndexRange Timeline::expandThumbnailRange(TimelineThumbnailIndexRange range, int padding, int thumbnailCount) const{
    if(thumbnailCount <= 0){
        return TimelineThumbnailIndexRange{};
    }

    const auto safePadding{max(0, padding)};
    return TimelineThumbnailIndexRange{
        .first = clamp(range.first - safePadding, 0, thumbnailCount - 1),
        .last = clamp(range.last + safePadding, 0, thumbnailCount - 1),
    };
}

int Timeline::chooseNextThumbnailIndex(const vector<bool>& thumbnailBuilt, TimelineThumbnailIndexRange visibleRange, bool allowOffscreenExpansion) const{
    const auto thumbnailCount{static_cast<int>(thumbnailBuilt.size())};
    if(thumbnailCount <= 0){
        return -1;
    }

    for(auto index{visibleRange.first}; index <= visibleRange.last; ++index){
        if(index >= 0 && index < thumbnailCount && !thumbnailBuilt[index]){
            return index;
        }
    }

    if(!allowOffscreenExpansion){
        return -1;
    }

    auto left{visibleRange.first - 1};
    auto right{visibleRange.last + 1};
    while(left >= 0 || right < thumbnailCount){
        if(right < thumbnailCount && !thumbnailBuilt[right]){
            return right;
        }
        ++right;

        if(left >= 0 && !thumbnailBuilt[left]){
            return left;
        }
        --left;
    }

    return -1;
}

TimelineRenderPostActions Timeline::buildRenderPostActions(const wstring& videoName, bool hasRequestedCuts, bool cachedRapLookupAttempted, bool isRapLookupInProgress) const{
    return TimelineRenderPostActions{
        .shouldQueueRapLookup = hasRequestedCuts && !cachedRapLookupAttempted && !isRapLookupInProgress,
        .readyStatus = std::format(L"Loaded: {} (story line ready)", videoName),
    };
}

}
