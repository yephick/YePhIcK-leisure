#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>

import std;
import llvc.AudioWaveform;
import llvc.Utils;
import llvc.Project;
import llvc.Timeline;
import llvc.EditorController;
import llvc.EditorCommands;
import llvc.Export;
import llvc.VideoStream;
import llvc.VideoContainer;
import llvc.Media;
import llvc.ExportCoordinator;

using namespace std;
namespace{

struct TestFailure: runtime_error{
    explicit TestFailure(const string& message): runtime_error(message) {}
};

void expect(bool condition, const string& message){
    if(!condition){
        throw TestFailure(message);
    }
}

using RecentItem = decltype(llvc::AppSettingsState{}.recentVideos)::value_type;

template<typename T>
struct is_vector: false_type{};

template<typename TValue, typename TAllocator>
struct is_vector<vector<TValue, TAllocator>>: true_type{};

template<typename T>
struct is_pair: false_type{};

template<typename TFirst, typename TSecond>
struct is_pair<pair<TFirst, TSecond>>: true_type{};

template<typename T>
bool areEqual(const T& actual, const T& expected){
    if constexpr (is_same_v<T, RecentItem>){
        return wstring_view{actual.c_str()} == wstring_view{expected.c_str()};
    }else if constexpr (is_vector<T>::value){
        if(actual.size() != expected.size()){
            return false;
        }
        for(size_t i{}; i < actual.size(); ++i){
            if(!areEqual(actual[i], expected[i])){
                return false;
            }
        }
        return true;
    }else if constexpr (is_pair<T>::value){
        return areEqual(actual.first, expected.first) && areEqual(actual.second, expected.second);
    }else{
        return actual == expected;
    }
}

template<typename T>
void expectEqual(const T& actual, const T& expected, const string& message){
    if(!areEqual(actual, expected)){
        throw TestFailure(message);
    }
}

void expectNear(double actual, double expected, double tolerance, const string& message){
    if(abs(actual - expected) > tolerance){
        throw TestFailure(message);
    }
}

template<typename Fn>
void expectHresult(Fn&& fn, HRESULT expected, const string& message){
    try{
        fn();
    }catch(const winrt::hresult_error& ex){
        if(ex.code() != expected){
            throw TestFailure(message + " (unexpected HRESULT)");
        }
        return;
    }
    throw TestFailure(message);
}

[[noreturn]] void fail(const string& message){
    throw TestFailure(message);
}

winrt::com_ptr<IMFMediaType> makeVideoTypeWithSequenceHeader(const GUID& subtype, const vector<uint8_t>& sequenceHeader){
    winrt::com_ptr<IMFMediaType> mediaType;
    winrt::check_hresult(MFCreateMediaType(mediaType.put()));
    winrt::check_hresult(mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    winrt::check_hresult(mediaType->SetGUID(MF_MT_SUBTYPE, subtype));
    if(!sequenceHeader.empty()){
        winrt::check_hresult(mediaType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader.data(), static_cast<UINT32>(sequenceHeader.size())));
    }
    return mediaType;
}

winrt::com_ptr<IMFSample> makeSampleWithBytes(const vector<uint8_t>& bytes){
    winrt::com_ptr<IMFSample> sample;
    winrt::check_hresult(MFCreateSample(sample.put()));

    winrt::com_ptr<IMFMediaBuffer> buffer;
    winrt::check_hresult(MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), buffer.put()));
    if(!bytes.empty()){
        BYTE* data{};
        DWORD maxLength{};
        DWORD currentLength{};
        winrt::check_hresult(buffer->Lock(&data, &maxLength, &currentLength));
        memcpy(data, bytes.data(), bytes.size());
        winrt::check_hresult(buffer->Unlock());
    }
    winrt::check_hresult(buffer->SetCurrentLength(static_cast<DWORD>(bytes.size())));
    winrt::check_hresult(sample->AddBuffer(buffer.get()));
    return sample;
}

vector<int64_t> parseSemicolonSeparatedInt64(const string& text){
    vector<int64_t> values;
    size_t start{};
    while(start <= text.size()){
        const auto end{text.find(';', start)};
        const auto token{text.substr(start, end == string::npos ? string::npos : end - start)};
        if(!token.empty()){
            values.push_back(stoll(token));
        }
        if(end == string::npos){
            break;
        }
        start = end + 1;
    }
    return values;
}

vector<uint32_t> parseSemicolonSeparatedUInt32(const string& text){
    vector<uint32_t> values;
    size_t start{};
    while(start <= text.size()){
        const auto end{text.find(',', start)};
        const auto token{text.substr(start, end == string::npos ? string::npos : end - start)};
        if(!token.empty()){
            values.push_back(static_cast<uint32_t>(stoul(token)));
        }
        if(end == string::npos){
            break;
        }
        start = end + 1;
    }
    return values;
}

llvc::IndexedFrameSample marker(int64_t time100ns, bool evaluated = false, bool cleanPoint = false, uint32_t sampleIndex = 0){
    return llvc::IndexedFrameSample{
        .time100ns = time100ns,
        .duration100ns = 0,
        .cleanPoint = cleanPoint,
        .evaluated = evaluated,
        .sampleIndex = sampleIndex,
    };
}

struct FakeExportMediaSource final: llvc::VideoSource{
    using llvc::VideoSource::VideoSource;

    InspectionResult inspect() const override{
        return {};
    }

    vector<int64_t> collectRapTimes100ns(const vector<int64_t>& = {}, const function<void(double)>& = {}, const function<bool()>& = {}) const override{
        return {};
    }

    void exportLossless(const ExportRequest& request) const override{
        ofstream output(filesystem::path{request.temporaryOutputPath}, ios::binary | ios::trunc);
        output << "fake export";
        output.close();
        if(request.onVideoProgress){
            request.onVideoProgress(100.0);
        }
    }
};

void testUtilsParsing(){
    const auto parsed{llvc::parseInt64List(L" 30,10,abc,10, 20 ,-5 ")};
    expectEqual(parsed, vector<int64_t>{-5, 10, 20, 30}, "parseInt64List should sort, dedup, and ignore invalid tokens");

    const auto pairs{llvc::parseIndexPairs(L" 1,2 ; bad ; 3, 4 ; 7,x ; 5,6 ")};
    expectEqual(pairs, vector<pair<uint32_t, uint32_t>>{{1, 2}, {3, 4}, {5, 6}}, "parseIndexPairs should keep only valid pairs");
    expectEqual(llvc::serializeIndexPairs(pairs), wstring(L"1,2;3,4;5,6"), "serializeIndexPairs should round-trip valid pairs");

    expectEqual(llvc::trim(L" \t hello world \r\n"), wstring(L"hello world"), "trim should remove surrounding whitespace");
}

void testUtilsWorkflowPolicies(){
    llvc::AppSettingsState settings{};
    llvc::applySeparatePreviewOpened(settings);
    expect(settings.restorePreviewDetachedOnStartup, "applySeparatePreviewOpened should mark detached preview on open");
    llvc::applySeparatePreviewFullscreen(settings, true);
    expect(settings.restorePreviewFullscreenOnStartup, "applySeparatePreviewFullscreen should remember fullscreen state");
    llvc::applySeparatePreviewClosed(settings);
    expect(!settings.restorePreviewDetachedOnStartup && !settings.restorePreviewFullscreenOnStartup, "applySeparatePreviewClosed should clear detached/fullscreen startup flags on close");

    expectEqual(llvc::buildProjectLoadedStatus(false, false), wstring(L"Project loaded"), "buildProjectLoadedStatus should return the default project-loaded status");
    expectEqual(llvc::buildProjectLoadedStatus(false, true), wstring(L""), "buildProjectLoadedStatus should stay quiet when the referenced video opened successfully");
    expectEqual(llvc::buildProjectLoadedStatus(true, true), wstring(L"Project opened, but referenced video could not be loaded"), "buildProjectLoadedStatus should report missing referenced video");
    expectEqual(llvc::buildTimelineLoadingStatus(L"demo.mp4"), wstring(L"Loaded: demo.mp4 (loading story line...)"), "buildTimelineLoadingStatus should build loading text");
    expectEqual(llvc::buildTimelineReadyStatus(L"demo.mp4"), wstring(L"Loaded: demo.mp4 (story line ready)"), "buildTimelineReadyStatus should build ready text");

    vector<winrt::hstring> recent{L"b.mp4", L"a.mp4"};
    llvc::pushRecentItemFront(recent, 2, L"c.mp4");
    expectEqual(recent, vector<winrt::hstring>{L"c.mp4", L"b.mp4"}, "pushRecentItemFront should push new items to the front and trim to the max size");
    llvc::pushRecentItemFront(recent, 2, L"b.mp4");
    expectEqual(recent, vector<winrt::hstring>{L"b.mp4", L"c.mp4"}, "pushRecentItemFront should move existing items to the front without duplicates");
    llvc::removeRecentItem(recent, L"c.mp4");
    expectEqual(recent, vector<winrt::hstring>{L"b.mp4"}, "removeRecentItem should remove matching items");

    vector<winrt::hstring> zeroCapacityRecent{L"keep.mp4"};
    llvc::pushRecentItemFront(zeroCapacityRecent, 0, L"drop.mp4");
    expect(zeroCapacityRecent.empty(), "pushRecentItemFront should trim everything when maxCount is zero");
}

void testMediaMkvInputIsRecognized(){
    const auto lower{llvc::createVideoSource(L"sample.mkv")};
    expect(static_cast<bool>(lower), "createVideoSource should recognize .mkv inputs");
    expectEqual(lower->sourcePath(), wstring(L"sample.mkv"), "MKV source should preserve its path");

    const auto upper{llvc::createVideoSource(L"sample.MKV")};
    expect(static_cast<bool>(upper), "createVideoSource should recognize uppercase .MKV inputs");
}

void testTimelineIntervalNormalization(){
    constexpr auto sentinel{numeric_limits<uint32_t>::max()};
    const auto normalized{llvc::normalizeAndMergeIndexIntervals({
        {2, 4},
        {0, 1},
        {1, 3},
        {sentinel, 1},
        {4, sentinel},
        {9, 10},
        {sentinel, sentinel},
    }, 5)};

    expectEqual(normalized, vector<pair<uint32_t, uint32_t>>{{sentinel, sentinel}}, "normalizeAndMergeIndexIntervals should merge touching intervals across timeline edges");
}

void testTimelineMath(){
    llvc::Timeline timeline{};

    const auto maybeTime{timeline.pointToTime100ns(50.0, 100.0, 1'000)};
    expect(maybeTime.has_value(), "pointToTime100ns should produce a value for valid input");
    expectEqual(*maybeTime, int64_t{500}, "pointToTime100ns should map proportionally");
    expectEqual(timeline.timeToCanvasX(250, 1'000, 200.0), 50.0, "timeToCanvasX should map proportionally");

    const auto leftOffset{timeline.cursorOffsetToEnsureVisible(20.0, 100.0, 200.0, 500.0, 30.0)};
    expect(leftOffset.has_value(), "cursorOffsetToEnsureVisible should request left scroll when cursor is clipped");
    expectEqual(*leftOffset, 0.0, "cursorOffsetToEnsureVisible should clamp left scroll to zero");

    const auto rightOffset{timeline.cursorOffsetToEnsureVisible(380.0, 100.0, 200.0, 500.0, 30.0)};
    expect(rightOffset.has_value(), "cursorOffsetToEnsureVisible should request right scroll when cursor is clipped");
    expectEqual(*rightOffset, 210.0, "cursorOffsetToEnsureVisible should preserve requested padding on the right");

    const auto visibleOffset{timeline.cursorOffsetToEnsureVisible(180.0, 100.0, 200.0, 500.0, 30.0)};
    expect(!visibleOffset.has_value(), "cursorOffsetToEnsureVisible should do nothing when cursor is already visible");
}

void testTimelineDerivedHelpers(){
    llvc::Timeline timeline{};

    expectEqual(timeline.dragTargetOffset(100.0, 25.0, 500.0, 200.0), 75.0, "dragTargetOffset should move opposite the drag delta");
    expectEqual(timeline.dragTargetOffset(10.0, 100.0, 500.0, 200.0), 0.0, "dragTargetOffset should clamp at the left edge");
    expectEqual(timeline.dragTargetOffset(250.0, -100.0, 500.0, 200.0), 300.0, "dragTargetOffset should clamp at the right edge");

    expectEqual(timeline.frameStep100ns(30000, 1001), int64_t{333666}, "frameStep100ns should compute integer frame duration");
    expectEqual(timeline.frameStep100ns(0, 1), int64_t{333667}, "frameStep100ns should fall back for invalid fps");
    expectEqual(timeline.applyFrameStep(1000, 1, 5000, 400), int64_t{1400}, "applyFrameStep should move forward by one frame");
    expectEqual(timeline.applyFrameStep(1000, -1, 5000, 400), int64_t{600}, "applyFrameStep should move backward by one frame");
    expectEqual(timeline.applyFrameStep(4900, 1, 5000, 400), int64_t{5000}, "applyFrameStep should clamp to duration");

    const vector<llvc::IndexedFrameSample> markers{
        marker(1LL * llvc::Timeline::HnsPerSecond, true, true),
        marker(2LL * llvc::Timeline::HnsPerSecond, false, false),
        marker(4LL * llvc::Timeline::HnsPerSecond, true, true),
    };
    const auto previousMarker{timeline.markerNavigationTarget100ns(markers, 26000000, -1)};
    if(!previousMarker.has_value()){
        fail("markerNavigationTarget100ns should find previous marker");
    }
    if(*previousMarker != 2LL * llvc::Timeline::HnsPerSecond){
        fail("markerNavigationTarget100ns should find previous marker (actual=" + to_string(*previousMarker) + ")");
    }

    const auto nextMarker{timeline.markerNavigationTarget100ns(markers, 26000000, 1)};
    if(!nextMarker.has_value()){
        fail("markerNavigationTarget100ns should find next marker");
    }
    if(*nextMarker != 4LL * llvc::Timeline::HnsPerSecond){
        fail("markerNavigationTarget100ns should find next marker (actual=" + to_string(*nextMarker) + ")");
    }
    expectEqual(timeline.markerNavigationTarget100ns(markers, 5000000, -1), optional<int64_t>{}, "markerNavigationTarget100ns should return nullopt before first marker");

    const auto keyframePositions{timeline.buildKeyframeTickPositions(markers, 500.0, 5.0)};
    expectEqual(keyframePositions.size(), size_t{2}, "buildKeyframeTickPositions should include only clean points");
    expectNear(keyframePositions[0], 100.0, 0.01, "buildKeyframeTickPositions should place the first clean point proportionally");
    expectNear(keyframePositions[1], 400.0, 0.01, "buildKeyframeTickPositions should place the second clean point proportionally");

    const auto overlays{timeline.buildCutOverlays({{100, 300}, {450, 450}, {700, 900}}, 500.0, 10.0)};
    expectEqual(overlays.size(), size_t{2}, "buildCutOverlays should ignore empty ranges");
    expect(timeline.isTimeInsideRanges(150, {{100, 300}, {400, 500}}), "isTimeInsideRanges should detect contained times");
    expect(!timeline.isTimeInsideRanges(350, {{100, 300}, {400, 500}}), "isTimeInsideRanges should reject times between ranges");
}

void testTimelineTickDensityAndLabels(){
    llvc::Timeline timeline{};

    const auto defaultTicks{timeline.buildMajorTicks(1200.0, 600.0)};
    expect(defaultTicks.size() >= size_t{7}, "buildMajorTicks should provide a reasonable default density");
    expectEqual(defaultTicks.front().x, 0.0, "buildMajorTicks should start at the left edge");
    expectEqual(defaultTicks.front().label, wstring(L"0:00"), "buildMajorTicks should label the origin");
    expectNear(defaultTicks.back().x, 1200.0, 0.01, "buildMajorTicks should end at the right edge");
    expectEqual(defaultTicks.back().label, wstring(L"10:00"), "buildMajorTicks should label the full duration");

    const auto requestedTicks{timeline.buildMajorTicks(500.0, 95.0, 10)};
    expectEqual(requestedTicks.size(), size_t{11}, "buildMajorTicks should honor an explicit desired tick count");
    expectEqual(requestedTicks[5].label, wstring(L"0:48"), "buildMajorTicks should round labels to the nearest second");
}

void testTimelineZoomAnchorsAndThumbnailPlanning(){
    llvc::Timeline timeline{};

    const auto scrollbarAnchor{llvc::TimelineScrollbarAnchor::capture(25.0, 100.0)};
    expect(scrollbarAnchor.has_value(), "TimelineScrollbarAnchor should capture a valid scrollbar ratio");
    expectNear(scrollbarAnchor->restoreOffset(300.0), 75.0, 0.001, "TimelineScrollbarAnchor should restore proportionally");
    expectEqual(llvc::TimelineScrollbarAnchor::capture(0.0, 0.0)->restoreOffset(250.0), 0.0, "TimelineScrollbarAnchor should restore zero offset when the original extent was not scrollable");

    const llvc::TimelinePointerZoomAnchor pointerAnchor{
        .time100ns = 5LL * llvc::Timeline::HnsPerSecond,
        .viewportPointerX = 120.0,
    };
    expectNear(pointerAnchor.restoreOffset(timeline, 10LL * llvc::Timeline::HnsPerSecond, 1000.0, 300.0), 380.0, 0.001, "TimelinePointerZoomAnchor should keep the hovered time under the same pointer position");

    const auto stripPlan{timeline.buildThumbnailStripPlan(120.0, 8.0)};
    expect(stripPlan.totalWidth > 0.0, "buildThumbnailStripPlan should produce a positive width");
    expect(stripPlan.thumbnailCount >= 1, "buildThumbnailStripPlan should produce at least one thumbnail");
    expect(stripPlan.thumbnailWidth > 0.0, "buildThumbnailStripPlan should produce a positive thumbnail width");

    const auto visibleRange{timeline.visibleThumbnailRange(320.0, 210.0, 100.0, 12)};
    expectEqual(visibleRange.first, 3, "visibleThumbnailRange should locate the first visible thumbnail");
    expectEqual(visibleRange.last, 5, "visibleThumbnailRange should locate the last visible thumbnail");

    vector<bool> built(8, false);
    built[2] = true;
    built[3] = true;
    const llvc::TimelineThumbnailIndexRange buildRange{.first = 2, .last = 4};
    expectEqual(timeline.chooseNextThumbnailIndex(built, buildRange, false), 4, "chooseNextThumbnailIndex should prefer unfinished visible thumbnails");
    built[4] = true;
    expectEqual(timeline.chooseNextThumbnailIndex(built, buildRange, true), 5, "chooseNextThumbnailIndex should expand outward after visible thumbnails are built");

    const auto postActions{timeline.buildRenderPostActions(L"sample.mp4", true, false, false)};
    expect(postActions.shouldQueueRapLookup, "buildRenderPostActions should request RAP lookup when cuts exist and no lookup has started");
    expectEqual(postActions.readyStatus, wstring(L"Loaded: sample.mp4 (story line ready)"), "buildRenderPostActions should build the ready status line");
}

void testTimelineZoomOutAtEndOfVideo(){
    llvc::Timeline timeline{};

    const llvc::TimelinePointerZoomAnchor endPointerAnchor{
        .time100ns = 10LL * llvc::Timeline::HnsPerSecond,
        .viewportPointerX = 280.0,
    };

    expectNear(endPointerAnchor.restoreOffset(timeline, 10LL * llvc::Timeline::HnsPerSecond, 1000.0, 300.0), 700.0, 0.001, "TimelinePointerZoomAnchor should clamp end-of-video anchors to the final scroll extent");
    expectNear(endPointerAnchor.restoreOffset(timeline, 10LL * llvc::Timeline::HnsPerSecond, 240.0, 300.0), 0.0, 0.001, "TimelinePointerZoomAnchor should stay stable when the zoomed-out strip is narrower than the viewport");

    const auto scrollbarAnchorAtEnd{llvc::TimelineScrollbarAnchor::capture(700.0, 700.0)};
    expect(scrollbarAnchorAtEnd.has_value(), "TimelineScrollbarAnchor should capture a valid far-right anchor at the video end");
    expectNear(scrollbarAnchorAtEnd->restoreOffset(0.0), 0.0, 0.001, "TimelineScrollbarAnchor should restore a far-right anchor to zero when zooming out removes all scrollable extent");
}

void testAudioWaveformScalingAndThresholdColor(){
    expectEqual(llvc::formatWaveformThresholdDb(-18.2), wstring(L"-18 dB"), "formatWaveformThresholdDb should round to whole dB labels");
    expectEqual(llvc::clampAudioPeak(-0.25f), 0.0f, "clampAudioPeak should clamp negative peaks");
    expectEqual(llvc::clampAudioPeak(1.25f), 1.0f, "clampAudioPeak should clamp peaks above full scale");

    const auto thresholdAmplitude{llvc::audioWaveformThresholdAmplitude(-6.0)};
    expectNear(llvc::audioWaveformHeightRatio(static_cast<float>(thresholdAmplitude), -6.0), llvc::AudioWaveformThresholdLineRatio, 0.001, "audioWaveformHeightRatio should place the threshold at the guide line");
    expectNear(llvc::audioWaveformHeightRatio(1.0f, -6.0), 1.0, 0.001, "audioWaveformHeightRatio should allow clipped/full-scale bars to reach full height");
    expect(llvc::audioWaveformHeightRatio(static_cast<float>(thresholdAmplitude / 2.0), -6.0) < llvc::AudioWaveformThresholdLineRatio, "audioWaveformHeightRatio should keep quieter peaks below the guide line");

    expect(!llvc::audioWaveformPeakIsHot(static_cast<float>(thresholdAmplitude * 0.99), -6.0), "audioWaveformPeakIsHot should keep just-below-threshold bars cool");
    expect(llvc::audioWaveformPeakIsHot(static_cast<float>(thresholdAmplitude), -6.0), "audioWaveformPeakIsHot should turn the whole bar hot at the threshold");
    expect(llvc::audioWaveformPeakIsHot(1.0f, -6.0), "audioWaveformPeakIsHot should keep loud clipped bars hot");
}

void testAudioWaveformChunkPlanning(){
    const auto visibleRange{llvc::visibleAudioWaveformChunkRange(320.0, 210.0, 100.0, 12)};
    expectEqual(visibleRange.first, 3, "visibleAudioWaveformChunkRange should locate the first visible audio chunk");
    expectEqual(visibleRange.last, 5, "visibleAudioWaveformChunkRange should locate the last visible audio chunk");

    vector<bool> built(8, false);
    built[2] = true;
    built[3] = true;
    const llvc::AudioWaveformChunkRange buildRange{.first = 2, .last = 4};
    expectEqual(llvc::chooseNextAudioWaveformChunkIndex(built, buildRange, false), 4, "chooseNextAudioWaveformChunkIndex should prefer unfinished visible chunks");
    built[4] = true;
    expectEqual(llvc::chooseNextAudioWaveformChunkIndex(built, buildRange, true), 5, "chooseNextAudioWaveformChunkIndex should expand outward after visible chunks are built");
    expectEqual(llvc::chooseNextAudioWaveformChunkIndex({}, {.first = 0, .last = 0}, true), -1, "chooseNextAudioWaveformChunkIndex should report no work when there are no chunks");

    expectEqual(llvc::audioWaveformChunkBucketStart(2, 8, 4096), size_t{1024}, "audioWaveformChunkBucketStart should map chunks to proportional bucket ranges");
    expectEqual(llvc::audioWaveformChunkBucketEnd(2, 8, 4096), size_t{1536}, "audioWaveformChunkBucketEnd should map chunks to proportional bucket ranges");
    expectEqual(llvc::audioWaveformChunkBucketEnd(7, 8, 4096), size_t{4096}, "audioWaveformChunkBucketEnd should include all remaining buckets in the last chunk");
}

void testTimelinePlanningEdgeCases(){
    llvc::Timeline timeline{};

    const auto fallbackStripPlan{timeline.buildThumbnailStripPlan(0.0, 8.0)};
    expectEqual(fallbackStripPlan.totalWidth, 800.0, "buildThumbnailStripPlan should fall back to the minimum width when duration is empty");
    expectEqual(fallbackStripPlan.thumbnailCount, 1, "buildThumbnailStripPlan should fall back to a single thumbnail when duration is empty");
    expectEqual(fallbackStripPlan.thumbnailWidth, 800.0, "buildThumbnailStripPlan should make the fallback thumbnail span the full strip");

    const auto invalidVisibleRange{timeline.visibleThumbnailRange(-25.0, 0.0, 0.0, 0)};
    expectEqual(invalidVisibleRange.first, 0, "visibleThumbnailRange should default to zero for invalid input");
    expectEqual(invalidVisibleRange.last, 0, "visibleThumbnailRange should default to zero for invalid input");

    expectEqual(timeline.chooseNextThumbnailIndex({}, {.first = 0, .last = 0}, true), -1, "chooseNextThumbnailIndex should report no work when there are no thumbnails");

    const auto quietPostActions{timeline.buildRenderPostActions(L"sample.mp4", true, true, false)};
    expect(!quietPostActions.shouldQueueRapLookup, "buildRenderPostActions should stay quiet after RAP lookup was already attempted");
}

void testProjectCutRangesAndBoundaries(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100), marker(300), marker(600)});
    project.refreshSelectedMarkers();
    project.cutScenes({1, 2});

    expectEqual(project.buildSceneBoundaries100ns(), vector<int64_t>{0, 100, 300, 600, 1'000}, "buildSceneBoundaries100ns should include edges and sorted markers");
    expectEqual(project.buildCutRanges100ns(), vector<pair<int64_t, int64_t>>{{100, 600}}, "buildCutRanges100ns should merge adjacent cut scenes");
}

void testProjectSceneBoundariesClampAndIgnoreInvalidCuts(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(-200), marker(200), marker(1'500), marker(200), marker(800)});
    project.refreshSelectedMarkers();
    project.cutScenes({0, 2, 4, 9});

    expectEqual(project.buildSceneBoundaries100ns(), vector<int64_t>{0, 200, 800, 1'000}, "buildSceneBoundaries100ns should clamp markers into the timeline and deduplicate equal boundaries");
    expectEqual(project.buildCutRanges100ns(), vector<pair<int64_t, int64_t>>{{0, 200}, {800, 1'000}}, "buildCutRanges100ns should ignore invalid scene indices while preserving valid cuts");
}

void testProjectNoCutPlanAndEmptyAlignment(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100), marker(300)});
    project.refreshSelectedMarkers();

    const auto noCutPlan{project.buildEffectiveExportPlanWithRapPreroll(1'000, {100, 300, 700})};
    expect(!noCutPlan.hasRequestedCuts, "plan should report no requested cuts");
    expectEqual(noCutPlan.effectiveOutputDuration100ns, int64_t{1'000}, "no-cut plan should preserve full duration");

    project.cutScenes({1});
    const auto emptyPlan{project.buildEffectiveExportPlanWithRapPreroll(1'000, {50})};
    expect(emptyPlan.hasRequestedCuts, "plan should report requested cuts when cut scenes exist");
    expect(emptyPlan.emptyAfterAlignment, "plan should flag empty alignment when no safe RAP block survives");
    expectEqual(emptyPlan.effectiveOutputDuration100ns, int64_t{1'000}, "empty alignment should fall back to full source duration");
}

void testProjectEffectiveExportAlignment(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100), marker(300), marker(600)});
    project.refreshSelectedMarkers();
    project.cutScenes({1});

    const auto plan{project.buildEffectiveExportPlanWithRapPreroll(1'000, {90, 120, 250, 310, 800})};
    expectEqual(plan.requestedCutRanges100ns, vector<pair<int64_t, int64_t>>{{100, 300}}, "requested cut range should match cut scene boundaries");
    expectEqual(plan.effectiveCutRanges100ns, vector<pair<int64_t, int64_t>>{{120, 250}}, "effective cut range should align inward to safe RAPs");
    expectEqual(plan.effectiveOutputDuration100ns, int64_t{870}, "effective output duration should reflect aligned cuts");
    expect(plan.materiallyDifferent, "effective plan should report when RAP alignment changed the cuts");
}

void testProjectTailCutPreservesLastMarker(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100), marker(300), marker(600)});
    project.refreshSelectedMarkers();
    project.cutScenes({3});

    const auto plan{project.buildEffectiveExportPlanWithRapPreroll(1'000, {650, 700, 800})};
    expectEqual(plan.effectiveCutRanges100ns, vector<pair<int64_t, int64_t>>{{600, 1'000}}, "tail cut should preserve the last marker instead of nudging it to the next RAP");
}

void testProjectDurationFormattingSupportsHours(){
    expectEqual(llvc::formatDuration100ns(0), wstring(L"00:00.000"), "formatDuration100ns should format zero duration");
    expectEqual(llvc::formatDuration100ns(17LL * 60LL * 10'000'000LL + 25LL * 10'000'000LL + 5470000LL), wstring(L"17:25.547"), "formatDuration100ns should keep short durations in mm:ss.mmm form");
    expectEqual(llvc::formatDuration100ns(
        14LL * 60LL * 60LL * 10'000'000LL
        + 17LL * 60LL * 10'000'000LL
        + 29LL * 10'000'000LL
        + 8210000LL), wstring(L"14:17:29.821"), "formatDuration100ns should render long durations as h:mm:ss.mmm instead of total minutes");
}

void testProjectRapLookupTargetsUseCutBoundaries(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100, true, true), marker(300, false, false), marker(600, false, false), marker(1'000, false, false)});
    project.refreshSelectedMarkers();
    project.cutScenes({1, 2});

    const auto targets{llvc::buildRapLookupTimesForExportAlignment(project, 1'000)};
    expectEqual(targets, vector<int64_t>{100, 300, 600}, "buildRapLookupTimesForExportAlignment should include cut boundaries and markers once, excluding the timeline edge");
}

void testProjectRapLookupTargetsNormalizeAndClamp(){
    llvc::Project project{};
    project.timelineDuration100ns(2'000);
    project.frameIndex({marker(0), marker(500), marker(500), marker(2'000), marker(2'500)});
    project.refreshSelectedMarkers();
    project.cutScenes({0, 1, 2, 3});

    const auto targets{llvc::buildRapLookupTimesForExportAlignment(project, 2'000)};
    expectEqual(targets, vector<int64_t>{500}, "buildRapLookupTimesForExportAlignment should sort, dedup, and skip source edges/out-of-range times");
}

void testProjectMarkerToggleAndSceneRemap(){
    llvc::Project project{};
    project.timelineDuration100ns(100'000'000);
    project.frameIndex({marker(10'000'000), marker(30'000'000), marker(60'000'000)});
    project.refreshSelectedMarkers();
    project.cutScenes({1});

    expect(project.toggleSelectedKeyframeAtTime100ns(30'000'000, 30.0), "toggleSelectedKeyframeAtTime100ns should remove an existing marker");
    expectEqual(project.buildSceneBoundaries100ns(), vector<int64_t>{0, 10'000'000, 60'000'000, 100'000'000}, "removing a marker should rebuild scene boundaries");
    expectEqual(project.cutScenes(), vector<uint32_t>{1}, "removing a marker should remap cut scenes onto the merged scene");

    expect(project.toggleSelectedKeyframeAtTime100ns(20'000'000, 30.0), "toggleSelectedKeyframeAtTime100ns should insert a new marker");
    expectEqual(project.buildSceneBoundaries100ns(), vector<int64_t>{0, 10'000'000, 20'000'000, 60'000'000, 100'000'000}, "inserting a marker should add a new boundary");
    expectEqual(project.cutScenes(), vector<uint32_t>{1, 2}, "inserting inside a cut scene should split that scene into two cut scenes");

    expect(project.toggleCutBlockAtTime100ns(5'000'000), "toggleCutBlockAtTime100ns should toggle the matching scene");
    expectEqual(project.cutScenes(), vector<uint32_t>{0, 1, 2}, "toggleCutBlockAtTime100ns should add a newly cut scene");
    expect(project.setCutBlockAtTime100ns(5'000'000, false), "setCutBlockAtTime100ns should clear a cut scene");
    expectEqual(project.cutScenes(), vector<uint32_t>{1, 2}, "setCutBlockAtTime100ns should remove the requested cut scene");
}

void testProjectAudioNormalization(){
    llvc::Project project{};
    project.audioXfadeMs(620);
    expectEqual(project.audioXfadeMs(), int32_t{500}, "audioXfadeMs should snap to the nearest preset");

    project.audioVolumePct(203);
    expectEqual(project.audioVolumePct(), int32_t{200}, "audioVolumePct should clamp to 5 percent steps");
}

void testProjectRapMarkersSelectionNormalization(){
    llvc::Project project{};
    project.frameIndex({
        marker(30'000'000, true, true, 9),
        marker(10'000'000, false, false, 7),
        marker(20'000'000, true, true, 8),
        marker(20'000'000, false, false, 10),
    });

    const auto rapMarkers{project.buildRapMarkersFromSelection()};
    expectEqual(rapMarkers.size(), size_t{3}, "buildRapMarkersFromSelection should sort and dedup marker times");
    expectEqual(rapMarkers[0].time100ns, int64_t{10'000'000}, "buildRapMarkersFromSelection should sort ascending");
    expectEqual(rapMarkers[1].time100ns, int64_t{20'000'000}, "buildRapMarkersFromSelection should retain one marker per timestamp");
    expectEqual(rapMarkers[2].time100ns, int64_t{30'000'000}, "buildRapMarkersFromSelection should sort ascending");
}

void testProjectEffectiveExportProgressAndStablePlan(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100, true, true), marker(300, false, false), marker(700, true, true)});
    project.refreshSelectedMarkers();
    project.cutScenes({0});

    vector<double> progressValues;
    const auto plan{project.buildEffectiveExportPlanWithRapPreroll(1'000, {100, 700}, [&](double pct){
        progressValues.push_back(pct);
    })};

    expectEqual(plan.requestedCutRanges100ns, vector<pair<int64_t, int64_t>>{{0, 100}}, "safe edge cuts should preserve the original range");
    expectEqual(plan.effectiveCutRanges100ns, vector<pair<int64_t, int64_t>>{{0, 100}}, "already-RAP-safe cuts should not shift");
    expect(!plan.materiallyDifferent, "already-safe plans should not report material differences");
    expectEqual(progressValues.size(), size_t{4}, "progress callback should report per-marker progress plus final completion");
    expectNear(progressValues.back(), 100.0, 0.001, "progress callback should finish at 100 percent");
}

void testProjectEffectiveExportMergesAlignedBlocks(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100), marker(250), marker(400), marker(550), marker(800)});
    project.refreshSelectedMarkers();
    project.cutScenes({0, 2});

    const auto plan{project.buildEffectiveExportPlanWithRapPreroll(1'000, {50, 90, 320, 580, 790})};
    expectEqual(plan.requestedCutRanges100ns, vector<pair<int64_t, int64_t>>{{0, 100}, {250, 400}}, "requested cuts should preserve separate cut scenes");
    expectEqual(plan.effectiveCutRanges100ns, vector<pair<int64_t, int64_t>>{{0, 90}}, "aligned cut ranges should follow the current inward RAP alignment rules");
    expectEqual(plan.effectiveOutputDuration100ns, int64_t{910}, "effective output duration should reflect the surviving aligned cut span");
    expect(plan.materiallyDifferent, "RAP-adjusted blocks should be marked as materially different");
}

void testProjectOutputDurationTracksMergedRanges(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100), marker(300), marker(600)});
    project.refreshSelectedMarkers();
    project.cutScenes({0, 1, 2});

    expectEqual(project.buildCutRanges100ns(), vector<pair<int64_t, int64_t>>{{0, 600}}, "adjacent cut scenes should merge into one output range");
    expectEqual(project.outputDuration100ns(), int64_t{400}, "outputDuration100ns should subtract merged cut ranges once");
}

void testProjectEffectiveExportPreservesAlreadySafeTailCut(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(200, true, true), marker(600, false, false)});
    project.refreshSelectedMarkers();
    project.cutScenes({2});

    const auto plan{project.buildEffectiveExportPlanWithRapPreroll(1'000, {150, 220, 700, 900})};
    expectEqual(plan.requestedCutRanges100ns, vector<pair<int64_t, int64_t>>{{600, 1'000}}, "tail cut request should start at the last marker");
    expectEqual(plan.effectiveCutRanges100ns, vector<pair<int64_t, int64_t>>{{600, 1'000}}, "tail cut should preserve the user marker even when it is off-RAP");
    expectEqual(plan.effectiveOutputDuration100ns, int64_t{600}, "tail cut should remove the trailing duration only once");
}

void testProjectEstimateCoordinatorHelpers(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(200, true, true), marker(600, false, false)});
    project.refreshSelectedMarkers();

    expect(!llvc::projectHasRequestedCuts(project), "projectHasRequestedCuts should report false when there are no cut scenes");
    expect(!llvc::cutPlanUsesUnevaluatedSceneEdgeMarkers(project), "cutPlanUsesUnevaluatedSceneEdgeMarkers should ignore red markers that are not scene edges");
    expect(!llvc::effectiveExportPlanNeedsRapAlignment(project), "effectiveExportPlanNeedsRapAlignment should stay false without requested cuts");

    project.cutScenes({1});
    expect(llvc::projectHasRequestedCuts(project), "projectHasRequestedCuts should report true for cut scenes");
    expect(llvc::cutPlanUsesUnevaluatedSceneEdgeMarkers(project), "cutPlanUsesUnevaluatedSceneEdgeMarkers should detect red scene-edge markers");
    expect(llvc::effectiveExportPlanNeedsRapAlignment(project), "effectiveExportPlanNeedsRapAlignment should require RAP alignment for red scene-edge markers");

    project.frameIndex({marker(200, true, true), marker(600, true, true)});
    project.refreshSelectedMarkers();
    expect(!llvc::cutPlanUsesUnevaluatedSceneEdgeMarkers(project), "cutPlanUsesUnevaluatedSceneEdgeMarkers should accept fully evaluated scene edges");
    expect(!llvc::effectiveExportPlanNeedsRapAlignment(project), "effectiveExportPlanNeedsRapAlignment should skip reevaluation when cut edges are already green");

    const auto directPlan{llvc::buildDirectEffectiveExportPlan(project, 1'000)};
    expectEqual(directPlan.requestedCutRanges100ns, vector<pair<int64_t, int64_t>>{{200, 600}}, "buildDirectEffectiveExportPlan should preserve the current cut ranges");
    expectEqual(directPlan.effectiveCutRanges100ns, vector<pair<int64_t, int64_t>>{{200, 600}}, "buildDirectEffectiveExportPlan should match effective ranges to requested ranges");
    expectEqual(directPlan.effectiveOutputDuration100ns, int64_t{600}, "buildDirectEffectiveExportPlan should preserve the current output duration");
    expect(!directPlan.materiallyDifferent, "buildDirectEffectiveExportPlan should report no material changes");
}

void testExportPreflightAndPlanSummary(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.videoFilePath(L"");
    const auto missingVideo{llvc::buildExportPreflightState(project, true, true, true, false, true)};
    expect(!missingVideo.canExport, "buildExportPreflightState should block when no video is loaded");

    const auto tempPath{filesystem::temp_directory_path() / L"llvc-tests-export-preflight.tmp"};
    {
        ofstream tempFile(tempPath, ios::binary | ios::trunc);
        tempFile << "x";
    }
    project.videoFilePath(winrt::hstring{tempPath.wstring()});
    project.frameIndex({marker(200, true, true), marker(600, false, false)});
    project.refreshSelectedMarkers();
    project.cutScenes({1});
    const auto preflight{llvc::buildExportPreflightState(project, true, true, true, false, true)};
    expect(preflight.canExport, "buildExportPreflightState should allow supported exports");
    expect(preflight.needsRapReevaluation, "buildExportPreflightState should report RAP reevaluation need for red scene-edge markers");
    expectEqual(preflight.requestedCutBlockCount, size_t{1}, "buildExportPreflightState should report requested cut blocks");

    const llvc::EffectiveExportPlan plan{
        .requestedCutRanges100ns = {{100, 300}},
        .effectiveCutRanges100ns = {{120, 250}},
        .sourceDuration100ns = 1'000,
        .requestedOutputDuration100ns = 800,
        .effectiveOutputDuration100ns = 870,
        .hasRequestedCuts = true,
        .emptyAfterAlignment = false,
        .materiallyDifferent = true,
    };
    const auto summary{llvc::summarizeEffectiveExportPlan(plan)};
    expectEqual(summary.repositionedMarkers, size_t{2}, "summarizeEffectiveExportPlan should count moved cut boundaries");
    expectEqual(summary.shrunkCutScenes, size_t{1}, "summarizeEffectiveExportPlan should count shrunk cut scenes");
    expectEqual(summary.shrunkTotal100ns, int64_t{70}, "summarizeEffectiveExportPlan should sum shrink duration");

    const auto overlayEstimates{llvc::buildExportOverlayEstimates(1000, 10'000'000, 5'000'000, false, true, 20)};
    expectEqual(overlayEstimates.estimatedTargetBytes, uint64_t{500}, "buildExportOverlayEstimates should scale target size by output duration");
    expectEqual(overlayEstimates.estimatedSavingsBytes, uint64_t{500}, "buildExportOverlayEstimates should compute estimated savings");
    expectEqual(overlayEstimates.estimatedDroppedAudioBytes, uint64_t{10}, "buildExportOverlayEstimates should estimate dropped audio bytes when audio is removed");

    filesystem::remove(tempPath);
}

void testExportCoordinatorDoesNotRequireFullRapReevaluationForExport(){
    const auto tempDir{filesystem::temp_directory_path()};
    const auto sourcePath{tempDir / L"llvc-tests-coordinator-source.mp4"};
    const auto tempOutputPath{tempDir / L"llvc-tests-coordinator-output.tmp"};
    const auto outputPath{tempDir / L"llvc-tests-coordinator-output.mp4"};
    {
        ofstream source(sourcePath, ios::binary | ios::trunc);
        source << "source";
    }
    filesystem::remove(tempOutputPath);
    filesystem::remove(outputPath);

    llvc::Project project{};
    project.videoFilePath(winrt::hstring{sourcePath.wstring()});
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(200, false, false), marker(600, false, false)});
    project.refreshSelectedMarkers();
    project.cutScenes({1});

    FakeExportMediaSource media{sourcePath.wstring()};
    llvc::VideoSource::InspectionResult mediaInfo{};
    mediaInfo.audioExportSupport = llvc::CapabilityState::NotApplicable;

    bool ensureRapCalled{};
    bool reevaluateCalled{};
    bool buildPlanCalled{};
    llvc::ExportCoordinatorResult result{};
    llvc::runExportAsync(llvc::ExportCoordinatorRequest{
        .project = &project,
        .media = &media,
        .mediaInfo = &mediaInfo,
        .sourcePath = sourcePath.wstring(),
        .outputPath = outputPath.wstring(),
        .temporaryOutputPath = tempOutputPath.wstring(),
        .sourceDuration100ns = 1'000,
        .sourceHasAudio = false,
        .needsRapReevaluation = true,
        .ensureRapMarkersAvailableAsync = [&ensureRapCalled](const wstring&, const function<void(double)>& progressCallback) -> winrt::Windows::Foundation::IAsyncOperation<bool>{
            ensureRapCalled = true;
            if(progressCallback){
                progressCallback(100.0);
            }
            co_return true;
        },
        .reevaluateCutMarkers = [&reevaluateCalled](bool){
            reevaluateCalled = true;
            return false;
        },
        .buildEffectiveExportPlan = [&buildPlanCalled](const function<void(double)>&) -> optional<llvc::EffectiveExportPlan>{
            buildPlanCalled = true;
            return llvc::EffectiveExportPlan{
                .requestedCutRanges100ns = {{200, 600}},
                .effectiveCutRanges100ns = {{220, 550}},
                .sourceDuration100ns = 1'000,
                .requestedOutputDuration100ns = 600,
                .effectiveOutputDuration100ns = 670,
                .hasRequestedCuts = true,
                .emptyAfterAlignment = false,
                .materiallyDifferent = true,
            };
        },
    }, result).get();

    expect(ensureRapCalled, "runExportAsync should request RAP data when reevaluation is needed");
    expect(!reevaluateCalled, "runExportAsync should not require full marker reevaluation after boundary-only export RAP lookup");
    expect(buildPlanCalled, "runExportAsync should build the non-mutating effective export plan");
    expect(result.succeeded, "runExportAsync should continue when the effective export plan is ready");
    expect(filesystem::exists(outputPath), "runExportAsync should move the temporary output into place");

    filesystem::remove(sourcePath);
    filesystem::remove(tempOutputPath);
    filesystem::remove(outputPath);
}

void testExportCutTimelineMapping(){
    const vector<pair<int64_t, int64_t>> cutRanges{
        {0, 100},
        {300, 500},
        {500, 600},
        {900, 1'000},
    };

    const auto keepRanges{llvc::invertCutRanges100ns(cutRanges, 1'000)};
    expectEqual(keepRanges, vector<pair<int64_t, int64_t>>{{100, 300}, {600, 900}}, "invertCutRanges100ns should produce keep ranges between normalized cuts");

    expectEqual(llvc::removedDurationBefore(cutRanges, 0), int64_t{0}, "removedDurationBefore should report no removed duration at the start");
    expectEqual(llvc::removedDurationBefore(cutRanges, 50), int64_t{50}, "removedDurationBefore should count partial removal inside the first cut");
    expectEqual(llvc::removedDurationBefore(cutRanges, 100), int64_t{100}, "removedDurationBefore should include cuts ending exactly at the sample time");
    expectEqual(llvc::removedDurationBefore(cutRanges, 250), int64_t{100}, "removedDurationBefore should not count kept timeline spans");
    expectEqual(llvc::removedDurationBefore(cutRanges, 550), int64_t{350}, "removedDurationBefore should count partial removal inside a later cut");
    expectEqual(llvc::removedDurationBefore(cutRanges, 1'000), int64_t{500}, "removedDurationBefore should count all removed duration at the end");

    expectEqual(250 - llvc::removedDurationBefore(cutRanges, 250), int64_t{150}, "timeline mapping should preserve samples before later cuts");
    expectEqual(700 - llvc::removedDurationBefore(cutRanges, 700), int64_t{300}, "timeline mapping should collapse removed spans before later samples");
}

void testExportVideoReadRangesSkipCutHeavySourceSpans(){
    const vector<pair<int64_t, int64_t>> effectiveCutRanges{
        {0, 183'451'427'043},
        {194'133'346'597, 305'542'854'597},
        {313'756'180'422, 514'498'210'000},
    };

    const auto readRanges{llvc::buildVideoExportReadRanges100ns(effectiveCutRanges, 514'498'210'000)};
    expectEqual(readRanges, vector<pair<int64_t, int64_t>>{
        {183'451'427'043, 194'133'346'597},
        {305'542'854'597, 313'756'180'422},
    }, "buildVideoExportReadRanges100ns should seek directly to kept spans for cut-heavy exports");
    expect(readRanges.front().first > 0, "cut-heavy export should not start reading video samples at the beginning of the source");
}

void testExportAudioCrossfade(){
    vector<float> firstSegment{1.0f, 1.0f, 1.0f, 1.0f};
    const vector<float> secondSegment{0.0f, 0.0f, 2.0f, 2.0f};
    llvc::appendCrossfadedAudioSegment(firstSegment, secondSegment, 2, 2);

    expectEqual(firstSegment.size(), size_t{4}, "appendCrossfadedAudioSegment should overlap, not duplicate, faded frames");
    expectNear(firstSegment[0], 1.0, 0.0001, "appendCrossfadedAudioSegment should keep the start of a two-frame equal-power fade on the first segment");
    expectNear(firstSegment[1], 1.0, 0.0001, "appendCrossfadedAudioSegment should apply the fade to every channel");
    expectNear(firstSegment[2], 2.0, 0.0001, "appendCrossfadedAudioSegment should end a two-frame equal-power fade on the second segment");
    expectNear(firstSegment[3], 2.0, 0.0001, "appendCrossfadedAudioSegment should apply the fade-in to every channel");

    vector<float> oneFrameFade{4.0f, 4.0f};
    llvc::appendCrossfadedAudioSegment(oneFrameFade, vector<float>{7.0f, 8.0f, 9.0f, 10.0f}, 2, 1);
    expectEqual(oneFrameFade, vector<float>{7.0f, 8.0f, 9.0f, 10.0f}, "appendCrossfadedAudioSegment should replace the overlapped frame for a one-frame fade and append the remainder");

    vector<float> noFade{1.0f, 2.0f};
    llvc::appendCrossfadedAudioSegment(noFade, vector<float>{3.0f, 4.0f}, 2, 0);
    expectEqual(noFade, vector<float>{1.0f, 2.0f, 3.0f, 4.0f}, "appendCrossfadedAudioSegment should append directly when fade duration is zero");
}

void testExportH264SampleNormalization(){
    const vector<uint8_t> annexBWithLeadingBytes{0x47, 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
    const auto normalizedAnnexB{llvc::normalizeH264SampleForMpegTs(annexBWithLeadingBytes.data(), annexBWithLeadingBytes.size(), 4)};
    expectEqual(normalizedAnnexB, vector<uint8_t>{0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84}, "normalizeH264SampleForMpegTs should skip leading bytes before an Annex B start code");

    const vector<uint8_t> lengthPrefixedWithPadding{0x00, 0x00, 0x00, 0x03, 0x65, 0x88, 0x84, 0x00, 0x00, 0x00};
    const auto normalizedLengthPrefixed{llvc::normalizeH264SampleForMpegTs(lengthPrefixedWithPadding.data(), lengthPrefixedWithPadding.size(), 4)};
    expectEqual(normalizedLengthPrefixed, vector<uint8_t>{0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84}, "normalizeH264SampleForMpegTs should tolerate trailing zero padding in length-prefixed samples");

    const vector<uint8_t> malformedLengthPrefixed{0x00, 0x00, 0x00, 0x04, 0x65, 0x88, 0x84};
    const auto normalizedMalformed{llvc::normalizeH264SampleForMpegTs(malformedLengthPrefixed.data(), malformedLengthPrefixed.size(), 4)};
    expect(normalizedMalformed.empty(), "normalizeH264SampleForMpegTs should reject malformed length-prefixed samples");

    const vector<uint8_t> chunkedAccessUnit{
        0x00, 0x00, 0x00, 0x02, 0x09, 0xF0,
        0x00, 0x00, 0x00, 0x03, 0x06, 0x01, 0x80,
        0x00, 0x00, 0x00, 0x04, 0x65, 0x88, 0x84, 0x21};
    const auto normalizedChunkedAccessUnit{llvc::normalizeH264SampleForMpegTs(chunkedAccessUnit.data(), chunkedAccessUnit.size(), 4)};
    expectEqual(normalizedChunkedAccessUnit, vector<uint8_t>{
        0x00, 0x00, 0x00, 0x01, 0x09, 0xF0,
        0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x80,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21}, "normalizeH264SampleForMpegTs should preserve all NAL units from a reassembled chunked access unit");
}

void testExportH264ParameterSetExtraction(){
    const vector<uint8_t> avccSequenceHeader{
        0x01, 0x64, 0x00, 0x1F, 0xFF,
        0xE1,
        0x00, 0x04, 0x67, 0x64, 0x00, 0x1F,
        0x01,
        0x00, 0x03, 0x68, 0xEE, 0x06};
    const auto parameterSets{llvc::extractH264ParameterSetsAnnexBForMpegTs(avccSequenceHeader)};
    expectEqual(parameterSets, vector<uint8_t>{0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x06}, "extractH264ParameterSetsAnnexBForMpegTs should convert avcC SPS/PPS data into Annex B parameter sets");

    const vector<uint8_t> annexBSequenceHeader{0x12, 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x06};
    const auto normalizedAnnexB{llvc::extractH264ParameterSetsAnnexBForMpegTs(annexBSequenceHeader)};
    expectEqual(normalizedAnnexB, vector<uint8_t>{0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x06}, "extractH264ParameterSetsAnnexBForMpegTs should also accept Annex B sequence header data");

    const vector<uint8_t> zeroSpsCount{0x01, 0x64, 0x00, 0x1F, 0xFF, 0xE0};
    expect(llvc::extractH264ParameterSetsAnnexBForMpegTs(zeroSpsCount).empty(), "extractH264ParameterSetsAnnexBForMpegTs should reject avcC data without SPS entries");

    const vector<uint8_t> truncatedSps{0x01, 0x64, 0x00, 0x1F, 0xFF, 0xE1, 0x00, 0x04, 0x67, 0x64};
    expect(llvc::extractH264ParameterSetsAnnexBForMpegTs(truncatedSps).empty(), "extractH264ParameterSetsAnnexBForMpegTs should reject truncated SPS data");

    const vector<uint8_t> trailingJunk{
        0x01, 0x64, 0x00, 0x1F, 0xFF,
        0xE1,
        0x00, 0x04, 0x67, 0x64, 0x00, 0x1F,
        0x01,
        0x00, 0x03, 0x68, 0xEE, 0x06,
        0x7F};
    expect(llvc::extractH264ParameterSetsAnnexBForMpegTs(trailingJunk).empty(), "extractH264ParameterSetsAnnexBForMpegTs should reject non-zero trailing avcC data");
}

void testVideoStreamNalLengthFieldDetection(){
    llvc::MFLifetime mf{};

    expectEqual(llvc::getNalLengthFieldSize(nullptr, MFVideoFormat_H264), uint32_t{4}, "getNalLengthFieldSize should default null media types to four-byte NAL lengths");
    expectEqual(llvc::getNalLengthFieldSize(makeVideoTypeWithSequenceHeader(MFVideoFormat_H264, {}), MFVideoFormat_H264), uint32_t{4}, "getNalLengthFieldSize should default missing codec headers to four-byte NAL lengths");

    auto h264OneByte{makeVideoTypeWithSequenceHeader(MFVideoFormat_H264, vector<uint8_t>{0x01, 0x64, 0x00, 0x1F, 0xFC})};
    expectEqual(llvc::getNalLengthFieldSize(h264OneByte, MFVideoFormat_H264), uint32_t{1}, "getNalLengthFieldSize should read one-byte H.264 NAL length fields from avcC");

    auto h264FourByte{makeVideoTypeWithSequenceHeader(MFVideoFormat_H264, vector<uint8_t>{0x01, 0x64, 0x00, 0x1F, 0xFF})};
    expectEqual(llvc::getNalLengthFieldSize(h264FourByte, MFVideoFormat_H264), uint32_t{4}, "getNalLengthFieldSize should read four-byte H.264 NAL length fields from avcC");

    vector<uint8_t> hevcConfig(22, 0);
    hevcConfig[21] = 0xFD;
    auto hevcTwoByte{makeVideoTypeWithSequenceHeader(MFVideoFormat_HEVC, hevcConfig)};
    expectEqual(llvc::getNalLengthFieldSize(hevcTwoByte, MFVideoFormat_HEVC), uint32_t{2}, "getNalLengthFieldSize should read HEVC NAL length fields from hvcC");

    hevcConfig[21] = 0xFF;
    auto hevcFourByte{makeVideoTypeWithSequenceHeader(MFVideoFormat_HEVC, hevcConfig)};
    expectEqual(llvc::getNalLengthFieldSize(hevcFourByte, MFVideoFormat_HEVC), uint32_t{4}, "getNalLengthFieldSize should read four-byte HEVC NAL length fields from hvcC");
}

void testVideoStreamContainerSyncSampleDetection(){
    llvc::MFLifetime mf{};

    auto sample{makeSampleWithBytes(vector<uint8_t>{0x01})};
    expect(!llvc::isContainerSyncSample(sample), "isContainerSyncSample should be false when the clean-point flag is absent");

    winrt::check_hresult(sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE));
    expect(llvc::isContainerSyncSample(sample), "isContainerSyncSample should be true when the clean-point flag is set");
}

void testVideoStreamSampleRapDetection(){
    llvc::MFLifetime mf{};

    const vector<uint8_t> h264IdrAccessUnit{0x00, 0x00, 0x00, 0x03, 0x65, 0x88, 0x84};
    expect(llvc::isTrueRandomAccessPointSample(makeSampleWithBytes(h264IdrAccessUnit), MFVideoFormat_H264, 4, false), "isTrueRandomAccessPointSample should recognize H.264 IDR samples");

    const vector<uint8_t> h264NonRapAccessUnit{0x00, 0x00, 0x00, 0x03, 0x61, 0x88, 0x84};
    expect(!llvc::isTrueRandomAccessPointSample(makeSampleWithBytes(h264NonRapAccessUnit), MFVideoFormat_H264, 4, false), "isTrueRandomAccessPointSample should reject H.264 non-IDR samples");

    const vector<uint8_t> hevcRapAccessUnit{0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0x88, 0x84};
    expect(llvc::isTrueRandomAccessPointSample(makeSampleWithBytes(hevcRapAccessUnit), MFVideoFormat_HEVC, 0, false), "isTrueRandomAccessPointSample should recognize HEVC RAP samples");

    const vector<uint8_t> h264InconclusiveAccessUnit{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1F,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x06};
    expect(llvc::isTrueRandomAccessPointSample(makeSampleWithBytes(h264InconclusiveAccessUnit), MFVideoFormat_H264, 0, true), "isTrueRandomAccessPointSample should honor allowInconclusive when no slice NAL is present");
    expect(!llvc::isTrueRandomAccessPointSample(makeSampleWithBytes(h264InconclusiveAccessUnit), MFVideoFormat_H264, 0, false), "isTrueRandomAccessPointSample should reject inconclusive samples when strict RAP proof is required");
}

void testVideoStreamH264ParameterSetDetection(){
    const vector<uint8_t> spsAndPps{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1F,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x06};
    expect(llvc::h264AnnexBSampleContainsParameterSets(spsAndPps.data(), spsAndPps.size()), "h264AnnexBSampleContainsParameterSets should require both SPS and PPS");

    const vector<uint8_t> spsOnly{0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1F};
    expect(!llvc::h264AnnexBSampleContainsParameterSets(spsOnly.data(), spsOnly.size()), "h264AnnexBSampleContainsParameterSets should reject SPS-only samples");

    const vector<uint8_t> ppsOnly{0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x06};
    expect(!llvc::h264AnnexBSampleContainsParameterSets(ppsOnly.data(), ppsOnly.size()), "h264AnnexBSampleContainsParameterSets should reject PPS-only samples");

    const vector<uint8_t> noParameterSets{0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
    expect(!llvc::h264AnnexBSampleContainsParameterSets(noParameterSets.data(), noParameterSets.size()), "h264AnnexBSampleContainsParameterSets should reject samples without parameter sets");
}

void testExportReaderSampleTimeResolution(){
    expectEqual(llvc::resolveReaderSampleTime100ns(1'234, 5'678), int64_t{1'234}, "resolveReaderSampleTime100ns should keep a positive sample time");
    expectEqual(llvc::resolveReaderSampleTime100ns(0, 5'678), int64_t{5'678}, "resolveReaderSampleTime100ns should fall back to the ReadSample timestamp when the sample time is zero");
    expectEqual(llvc::resolveReaderSampleTime100ns(-1, 5'678), int64_t{5'678}, "resolveReaderSampleTime100ns should fall back to the ReadSample timestamp when the sample time is negative");
    expectEqual(llvc::resolveReaderSampleTime100ns(0, 0), int64_t{0}, "resolveReaderSampleTime100ns should clamp missing timestamps to zero");
}

void testMpegTsKeyframeClassification(){
    expect(llvc::shouldTreatMpegTsSampleAsKeyframe(false, true), "shouldTreatMpegTsSampleAsKeyframe should trust a bitstream RAP even when the container clean-point flag is not set");
    expect(llvc::shouldTreatMpegTsSampleAsKeyframe(true, true), "shouldTreatMpegTsSampleAsKeyframe should accept a sample when both sources report RAP");
    expect(!llvc::shouldTreatMpegTsSampleAsKeyframe(true, false), "shouldTreatMpegTsSampleAsKeyframe should reject non-RAP samples");
}

void testMpegTsBitstreamRapDetection(){
    const vector<uint8_t> annexBIdrAccessUnit{
        0x00, 0x00, 0x00, 0x01, 0x09, 0xF0,
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1F,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x06,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
    expect(llvc::bitstreamLooksLikeRandomAccessPoint(annexBIdrAccessUnit.data(), annexBIdrAccessUnit.size(), MFVideoFormat_H264, 0, false), "bitstreamLooksLikeRandomAccessPoint should recognize Annex B IDR access units as RAP");

    const vector<uint8_t> lengthPrefixedNonRapAccessUnit{
        0x00, 0x00, 0x00, 0x02, 0x09, 0xF0,
        0x00, 0x00, 0x00, 0x03, 0x06, 0x01, 0x80,
        0x00, 0x00, 0x00, 0x03, 0x61, 0x88, 0x84};
    expect(!llvc::bitstreamLooksLikeRandomAccessPoint(lengthPrefixedNonRapAccessUnit.data(), lengthPrefixedNonRapAccessUnit.size(), MFVideoFormat_H264, 4, false), "bitstreamLooksLikeRandomAccessPoint should reject non-IDR H.264 access units");

    const vector<uint8_t> annexBHevcRapAccessUnit{
        0x00, 0x00, 0x00, 0x01, 0x40, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0x88, 0x84};
    expect(llvc::bitstreamLooksLikeRandomAccessPoint(annexBHevcRapAccessUnit.data(), annexBHevcRapAccessUnit.size(), MFVideoFormat_HEVC, 0, false), "bitstreamLooksLikeRandomAccessPoint should recognize HEVC IDR access units as RAP");

    const vector<uint8_t> inconclusiveAccessUnit{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1F,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x06};
    expect(llvc::bitstreamLooksLikeRandomAccessPoint(inconclusiveAccessUnit.data(), inconclusiveAccessUnit.size(), MFVideoFormat_H264, 0, true), "bitstreamLooksLikeRandomAccessPoint should honor allowInconclusive when no slice NAL is present");
    expect(!llvc::bitstreamLooksLikeRandomAccessPoint(inconclusiveAccessUnit.data(), inconclusiveAccessUnit.size(), MFVideoFormat_H264, 0, false), "bitstreamLooksLikeRandomAccessPoint should reject inconclusive access units when strict RAP proof is required");
}

void testMpegTsDecodeTimeInference(){
    optional<int64_t> nextInferredDecodeTime100ns{};
    expectEqual(llvc::resolveMpegTsDecodeTime100ns(nextInferredDecodeTime100ns, 500'000, 333'333), optional<int64_t>{0}, "resolveMpegTsDecodeTime100ns should start inferred DTS at zero when the stream begins without decode timestamps");
    expectEqual(nextInferredDecodeTime100ns, optional<int64_t>{500'000}, "resolveMpegTsDecodeTime100ns should prefer the access-unit duration when advancing inferred DTS");
    expectEqual(llvc::resolveMpegTsDecodeTime100ns(nextInferredDecodeTime100ns, 0, 333'333), optional<int64_t>{500'000}, "resolveMpegTsDecodeTime100ns should fall back to the nominal frame duration when a sample duration is unavailable");
    expectEqual(nextInferredDecodeTime100ns, optional<int64_t>{833'333}, "resolveMpegTsDecodeTime100ns should keep the next inferred DTS monotonic when falling back to the nominal cadence");
    nextInferredDecodeTime100ns.reset();
    expectEqual(llvc::resolveMpegTsDecodeTime100ns(nextInferredDecodeTime100ns, 0, 0), optional<int64_t>{}, "resolveMpegTsDecodeTime100ns should return nullopt when neither the sample duration nor the nominal frame duration is available");
}

void testMpegTsDecodeTimeSanitization(){
    expectEqual(llvc::sanitizeMpegTsVideoDecodeTime100ns(optional<int64_t>{}, 10'000'000), optional<int64_t>{}, "sanitizeMpegTsVideoDecodeTime100ns should omit DTS when no decode time is available");
    expectEqual(llvc::sanitizeMpegTsVideoDecodeTime100ns(optional<int64_t>{0}, 10'000'000), optional<int64_t>{}, "sanitizeMpegTsVideoDecodeTime100ns should omit zero DTS values");
    expectEqual(llvc::sanitizeMpegTsVideoDecodeTime100ns(optional<int64_t>{12'000'000}, 10'000'000), optional<int64_t>{}, "sanitizeMpegTsVideoDecodeTime100ns should reject DTS values that lead presentation time");
    expectEqual(llvc::sanitizeMpegTsVideoDecodeTime100ns(optional<int64_t>{5'000'000}, 30'000'001), optional<int64_t>{}, "sanitizeMpegTsVideoDecodeTime100ns should reject DTS values that lag presentation time by more than the TS reorder tolerance");
    expectEqual(llvc::sanitizeMpegTsVideoDecodeTime100ns(optional<int64_t>{8'000'000}, 10'000'000), optional<int64_t>{8'000'000}, "sanitizeMpegTsVideoDecodeTime100ns should preserve plausible reordered DTS values");
}

void testMpegTsPesHeaderSanitization(){
    const auto appendPtsForTest{[](vector<uint8_t>& out, uint8_t prefix, int64_t time100ns){
        const auto pts{static_cast<uint64_t>(max<int64_t>(0, time100ns) * 9 / 1000) & 0x1FFFFFFFFULL};
        out.push_back(static_cast<uint8_t>((prefix << 4) | (((pts >> 30) & 0x07) << 1) | 1));
        out.push_back(static_cast<uint8_t>((pts >> 22) & 0xFF));
        out.push_back(static_cast<uint8_t>((((pts >> 15) & 0x7F) << 1) | 1));
        out.push_back(static_cast<uint8_t>((pts >> 7) & 0xFF));
        out.push_back(static_cast<uint8_t>(((pts & 0x7F) << 1) | 1));
    }};
    const auto buildVideoPesHeaderForTest{[&](int64_t presentationTime100ns, optional<int64_t> decodeTime100ns){
        vector<uint8_t> pes;
        pes.reserve(19);
        pes.insert(pes.end(), {0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x80});
        const auto trustedDts100ns{llvc::sanitizeMpegTsVideoDecodeTime100ns(decodeTime100ns, presentationTime100ns)};
        if(trustedDts100ns.has_value() && *trustedDts100ns != presentationTime100ns){
            pes.push_back(0xC0);
            pes.push_back(10);
            appendPtsForTest(pes, 0x03, presentationTime100ns);
            appendPtsForTest(pes, 0x01, *trustedDts100ns);
        }else{
            pes.push_back(0x80);
            pes.push_back(5);
            appendPtsForTest(pes, 0x02, presentationTime100ns);
        }
        return pes;
    }};

    const auto ptsOnlyHeader{buildVideoPesHeaderForTest(10'000'000, optional<int64_t>{0})};
    expectEqual(ptsOnlyHeader[7], uint8_t{0x80}, "buildMpegTsVideoPesHeaderForTest should emit a PTS-only PES header when DTS is zero");
    expectEqual(ptsOnlyHeader[8], uint8_t{5}, "buildMpegTsVideoPesHeaderForTest should encode a five-byte timestamp payload when DTS is omitted");

    const auto invalidLagHeader{buildVideoPesHeaderForTest(30'000'001, optional<int64_t>{5'000'000})};
    expectEqual(invalidLagHeader[7], uint8_t{0x80}, "buildMpegTsVideoPesHeaderForTest should omit DTS when it lags far behind PTS");
    expectEqual(invalidLagHeader[8], uint8_t{5}, "buildMpegTsVideoPesHeaderForTest should keep invalid DTS out of the PES header");

    const auto reorderedHeader{buildVideoPesHeaderForTest(10'000'000, optional<int64_t>{8'000'000})};
    expectEqual(reorderedHeader[7], uint8_t{0xC0}, "buildMpegTsVideoPesHeaderForTest should emit both PTS and DTS when the decode timestamp is plausible");
    expectEqual(reorderedHeader[8], uint8_t{10}, "buildMpegTsVideoPesHeaderForTest should encode ten bytes of timestamp payload when DTS is preserved");
}

void testVideoContainerWriterCodecGuards(){
    llvc::MFLifetime mf{};

    expectHresult(
        []{
            llvc::writeWebmVp9VideoSamplesForExport(nullptr, 0, L"", {}, MFVideoFormat_H264, 4, 0, 0, 0, 0, 0, 0, {}, {});
        },
        MF_E_INVALIDMEDIATYPE,
        "writeWebmVp9VideoSamplesForExport should reject non-VP9 video before touching the reader");

    expectHresult(
        []{
            llvc::writeMpegTsH264VideoSamplesForExport(nullptr, 0, L"", {}, MFVideoFormat_VP90, 0, nullptr, 0, nullptr, 0, nullptr, false, {}, {}, {});
        },
        MF_E_INVALIDMEDIATYPE,
        "writeMpegTsH264VideoSamplesForExport should reject non-H.264 video before touching the reader");
}

void testVideoContainerDelegationCodecGuards(){
    llvc::MFLifetime mf{};
    auto vp9VideoType{makeVideoTypeWithSequenceHeader(MFVideoFormat_VP90, {})};
    auto h264VideoType{makeVideoTypeWithSequenceHeader(MFVideoFormat_H264, {})};
    const llvc::VideoContainerExportRequest request{
        .sourcePath = L"",
        .temporaryOutputPath = L"",
        .effectiveCutRanges100ns = {},
        .sourceDuration100ns = 0,
        .outputDuration100ns = 0,
        .keepAudio = false,
        .allowAudio = false,
    };

    expectHresult(
        [&]{
            llvc::writeVideoContainerForExport(llvc::VideoContainerExportKind::WebmVp9, nullptr, 0, h264VideoType, MFVideoFormat_H264, 4, request);
        },
        MF_E_INVALIDMEDIATYPE,
        "writeVideoContainerForExport should route WebM exports through the VP9 writer guard");

    expectHresult(
        [&]{
            llvc::writeVideoContainerForExport(llvc::VideoContainerExportKind::MpegTs, nullptr, 0, vp9VideoType, MFVideoFormat_VP90, 0, request);
        },
        MF_E_INVALIDMEDIATYPE,
        "writeVideoContainerForExport should route MPEG-TS exports through the H.264 writer guard");
}

void testEditorHistoryUndoRedo(){
    llvc::Project project{};
    project.timelineDuration100ns(100'000'000);
    llvc::EditorHistoryState history{};

    const auto toggleResult{llvc::toggleSelectedKeyframe(project, 30.0, history, 20'000'000)};
    expect(toggleResult.changed, "toggleSelectedKeyframe should add a marker");
    expect(toggleResult.markerCountIncreased, "toggleSelectedKeyframe should report marker insertion");
    expectEqual(project.frameIndex().size(), size_t{1}, "project should contain the inserted marker");
    expectEqual(history.undoStack.size(), size_t{1}, "history should capture a pre-change undo snapshot");

    const auto undoResult{llvc::undo(project, history)};
    expect(undoResult.changed && undoResult.wasUndo, "undo should restore the previous editor snapshot");
    expectEqual(project.frameIndex().size(), size_t{0}, "undo should remove the inserted marker");

    const auto redoResult{llvc::redo(project, history)};
    expect(redoResult.changed && !redoResult.wasUndo, "redo should restore the undone editor snapshot");
    expectEqual(project.frameIndex().size(), size_t{1}, "redo should restore the inserted marker");
}

void testEditorHistoryClearsRedoAfterNewEdit(){
    llvc::Project project{};
    project.timelineDuration100ns(100'000'000);
    llvc::EditorHistoryState history{};

    expect(llvc::toggleSelectedKeyframe(project, 30.0, history, 20'000'000).changed, "toggleSelectedKeyframe should create an initial edit");
    expect(llvc::undo(project, history).changed, "undo should populate the redo stack");
    expectEqual(history.redoStack.size(), size_t{1}, "undo should add one redo snapshot");

    expect(llvc::toggleSelectedKeyframe(project, 30.0, history, 40'000'000).changed, "a new edit after undo should still be accepted");
    expect(history.redoStack.empty(), "a new edit should clear stale redo history");
}

void testEditorHistorySnapshotGuards(){
    llvc::Project project{};
    project.timelineDuration100ns(10'000'000);
    llvc::EditorHistoryState history{};

    expect(llvc::pushUndoSnapshotIfChanged(project, history), "first snapshot should be added");
    expect(!llvc::pushUndoSnapshotIfChanged(project, history), "duplicate snapshot should not be added");

    history.isApplying = true;
    expect(!llvc::pushUndoSnapshotIfChanged(project, history), "history should ignore snapshots while applying");
    history.isApplying = false;
}

void testEditorCommands(){
    llvc::Project project{};
    project.timelineDuration100ns(100'000'000);
    project.frameIndex({marker(20'000'000), marker(50'000'000), marker(80'000'000)});
    project.refreshSelectedMarkers();
    project.cutScenes({1});
    llvc::EditorHistoryState history{};
    llvc::Timeline timeline{};

    const auto cutToggle{llvc::executeToggleCutBlockCommand(project, history, 10'000'000)};
    expect(cutToggle.changed && cutToggle.refreshTimelineTicks && cutToggle.refreshCutOverlays, "toggle cut command should request timeline refresh");

    const auto setCut{llvc::executeSetCutBlockCommand(project, history, 10'000'000, false)};
    expect(setCut.changed && setCut.refreshWindowTitle, "set cut command should refresh dependent UI");

    const auto toggleMarker{llvc::executeToggleMarkerCommand(project, 30.0, history, 35'000'000)};
    expect(toggleMarker.changed && toggleMarker.markerCountIncreased, "toggle marker command should report marker insertion");

    const auto rapNudge{llvc::executeRapNudgeCommand(project, timeline, {15'000'000, 25'000'000, 45'000'000, 55'000'000, 75'000'000, 85'000'000}, history, 30'000'000, false)};
    expect(rapNudge.changed, "RAP nudge command should adjust scene boundaries when surrounding RAPs exist");
    expect(rapNudge.refreshTimelineTicks && rapNudge.refreshKeyframeTicks && rapNudge.refreshCutOverlays, "RAP nudge command should refresh all timeline layers");
}

void testEditorCommandsNoOpCases(){
    llvc::Project project{};
    project.timelineDuration100ns(100'000'000);
    project.frameIndex({marker(20'000'000, true, true), marker(50'000'000, true, true)});
    project.refreshSelectedMarkers();
    llvc::EditorHistoryState history{};
    llvc::Timeline timeline{};

    const auto unchangedCut{llvc::executeSetCutBlockCommand(project, history, 10'000'000, false)};
    expect(unchangedCut.changed, "set cut command currently reports success for addressable scenes even when the state is unchanged");

    const auto noRapNudge{llvc::executeRapNudgeCommand(project, timeline, {20'000'000, 50'000'000}, history, 10'000'000, false)};
    expect(!noRapNudge.changed, "RAP nudge command should report no change when boundaries are already on RAPs");
}

void testEditorReevaluationAddsBracketMarkersAndAdjustsScenes(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(300, false, false)});
    project.refreshSelectedMarkers();
    project.cutScenes({1});
    llvc::EditorHistoryState history{};
    llvc::Timeline timeline{};

    const auto result{llvc::reevaluateClearCutMarkers(project, timeline, {250, 400}, history, true)};
    expect(result.changed, "reevaluateClearCutMarkers should change the project when RAP brackets are available");
    expect(result.hadMarkers, "reevaluateClearCutMarkers should report existing markers");
    expect(result.undoSnapshotCreated, "reevaluateClearCutMarkers should capture undo state when requested");
    expectEqual(result.replacedCount, uint32_t{1}, "reevaluateClearCutMarkers should count off-RAP user markers as replaced");

    const auto& markers{project.frameIndex()};
    expectEqual(markers.size(), size_t{3}, "reevaluateClearCutMarkers should keep the red marker and add two green RAP markers");
    expectEqual(markers[0].time100ns, int64_t{250}, "reevaluateClearCutMarkers should add the left RAP bracket");
    expect(markers[0].evaluated && markers[0].cleanPoint, "left RAP bracket should be green and clean");
    expectEqual(markers[1].time100ns, int64_t{300}, "reevaluateClearCutMarkers should preserve the original red marker");
    expect(!markers[1].evaluated && !markers[1].cleanPoint, "original off-RAP marker should stay red");
    expectEqual(markers[2].time100ns, int64_t{400}, "reevaluateClearCutMarkers should add the right RAP bracket");
    expect(markers[2].evaluated && markers[2].cleanPoint, "right RAP bracket should be green and clean");
    expectEqual(project.cutScenes(), vector<uint32_t>{2, 3}, "reevaluateClearCutMarkers should remap cut scenes onto the aligned tail-preserving span");
    expectEqual(history.undoStack.size(), size_t{1}, "reevaluateClearCutMarkers should push one undo snapshot");
}

void testEditorReevaluationPreservesTailCutBoundary(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(600, false, false)});
    project.refreshSelectedMarkers();
    project.cutScenes({1});
    llvc::EditorHistoryState history{};
    llvc::Timeline timeline{};

    const auto result{llvc::reevaluateClearCutMarkers(project, timeline, {550, 700, 900}, history, false)};
    expect(result.changed, "reevaluateClearCutMarkers should still update tail-cut markers");
    expect(!result.undoSnapshotCreated, "reevaluateClearCutMarkers should skip undo snapshots when not requested");

    const auto& markers{project.frameIndex()};
    expectEqual(markers.size(), size_t{3}, "tail-cut reevaluation should preserve the original marker and add RAP brackets");
    expectEqual(markers[1].time100ns, int64_t{600}, "tail-cut reevaluation should keep the last user marker in place");
    expectEqual(project.cutScenes(), vector<uint32_t>{2, 3}, "tail-cut reevaluation should preserve the tail cut while remapping across the inserted RAP brackets");
}

void testEvaluatePlacedMarkerAffectsOnlyNewMarker(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(200, false, false), marker(600, false, false)});
    project.refreshSelectedMarkers();
    project.cutScenes({1});
    const auto beforeCutRanges{project.buildCutRanges100ns()};

    const auto result{llvc::evaluatePlacedMarkerAgainstRap(project, {550, 650}, 600)};
    expect(result.changed, "evaluatePlacedMarkerAgainstRap should update the just-placed marker when RAP data is available");
    expectEqual(result.addedRapMarkerCount, uint32_t{2}, "evaluatePlacedMarkerAgainstRap should add surrounding RAP markers for the placed marker");

    const auto& markers{project.frameIndex()};
    expectEqual(markers.size(), size_t{4}, "evaluatePlacedMarkerAgainstRap should only add RAP markers around the targeted marker");
    expectEqual(markers[0].time100ns, int64_t{200}, "evaluatePlacedMarkerAgainstRap should preserve earlier red markers");
    expect(!markers[0].evaluated && !markers[0].cleanPoint, "evaluatePlacedMarkerAgainstRap should leave earlier red markers untouched");
    expectEqual(markers[1].time100ns, int64_t{550}, "evaluatePlacedMarkerAgainstRap should add the left RAP marker for the targeted marker");
    expect(markers[1].evaluated && markers[1].cleanPoint, "left RAP marker should be green");
    expectEqual(markers[2].time100ns, int64_t{600}, "evaluatePlacedMarkerAgainstRap should preserve the targeted user marker");
    expect(!markers[2].evaluated && !markers[2].cleanPoint, "targeted off-RAP marker should stay red");
    expectEqual(markers[3].time100ns, int64_t{650}, "evaluatePlacedMarkerAgainstRap should add the right RAP marker for the targeted marker");
    expectEqual(project.buildCutRanges100ns(), beforeCutRanges, "evaluatePlacedMarkerAgainstRap should preserve the original cut time ranges while adding helper markers");
}

void testEvaluatePlacedMarkerPreservesExistingCutRangesWhenInsertedIntoKeptScene(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(200, true, true), marker(400, false, false), marker(800, true, true)});
    project.refreshSelectedMarkers();
    project.cutScenes({2});

    const auto beforeCutRanges{project.buildCutRanges100ns()};
    const auto result{llvc::evaluatePlacedMarkerAgainstRap(project, {350, 450}, 400)};
    expect(result.changed, "evaluatePlacedMarkerAgainstRap should add RAP helper markers around an off-RAP inserted marker");
    expectEqual(result.addedRapMarkerCount, uint32_t{2}, "evaluatePlacedMarkerAgainstRap should report both added RAP helper markers");

    const auto& markers{project.frameIndex()};
    expectEqual(markers.size(), size_t{5}, "evaluatePlacedMarkerAgainstRap should preserve existing markers and add only the bracket markers");
    expectEqual(markers[0].time100ns, int64_t{200}, "evaluatePlacedMarkerAgainstRap should keep the earlier evaluated marker");
    expectEqual(markers[1].time100ns, int64_t{350}, "evaluatePlacedMarkerAgainstRap should add the left helper RAP marker");
    expectEqual(markers[2].time100ns, int64_t{400}, "evaluatePlacedMarkerAgainstRap should preserve the inserted user marker");
    expectEqual(markers[3].time100ns, int64_t{450}, "evaluatePlacedMarkerAgainstRap should add the right helper RAP marker");
    expectEqual(markers[4].time100ns, int64_t{800}, "evaluatePlacedMarkerAgainstRap should keep the later evaluated marker");
    expectEqual(project.buildCutRanges100ns(), beforeCutRanges, "evaluatePlacedMarkerAgainstRap should preserve the existing cut time ranges when adding helper markers inside a kept scene");
}

void testEditorReevaluationOnlyShrinksExistingCutScenes(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(300, false, false), marker(700, false, false)});
    project.refreshSelectedMarkers();
    project.cutScenes({1});
    llvc::EditorHistoryState history{};
    llvc::Timeline timeline{};

    const auto result{llvc::reevaluateClearCutMarkers(project, timeline, {250, 400, 650, 750}, history, true)};
    expect(result.changed, "reevaluateClearCutMarkers should adjust cut scene boundaries when RAP markers are available");
    expectEqual(project.buildCutRanges100ns(), vector<pair<int64_t, int64_t>>{{400, 650}}, "reevaluateClearCutMarkers should only shrink the existing cut range inward to RAP-safe bounds");
    expectEqual(project.cutScenes().size(), size_t{1}, "reevaluateClearCutMarkers should keep exactly one cut scene after inserting helper markers around both boundaries");
}

void testEditorReevaluationNoOpWithoutMarkers(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    llvc::EditorHistoryState history{};
    llvc::Timeline timeline{};

    const auto result{llvc::reevaluateClearCutMarkers(project, timeline, {100, 200}, history, true)};
    expect(!result.changed, "reevaluateClearCutMarkers should stay unchanged when there are no markers");
    expect(!result.hadMarkers, "reevaluateClearCutMarkers should report the missing marker set");
    expect(!result.undoSnapshotCreated, "reevaluateClearCutMarkers should not create undo history when there is nothing to do");
    expect(history.undoStack.empty(), "reevaluateClearCutMarkers should leave undo history untouched when there are no markers");
}

void testEditorCommandsRapNudgeExpandScene(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(300, false, false), marker(700, false, false)});
    project.refreshSelectedMarkers();
    project.cutScenes({1});
    llvc::EditorHistoryState history{};
    llvc::Timeline timeline{};

    const auto result{llvc::executeRapNudgeCommand(project, timeline, {200, 250, 350, 650, 750, 900}, history, 500, true)};
    expect(result.changed, "executeRapNudgeCommand should expand the selected scene block when surrounding RAPs are available");
    expectEqual(project.buildCutRanges100ns(), vector<pair<int64_t, int64_t>>{{250, 750}}, "executeRapNudgeCommand should preserve the cut block while expanding both boundaries outward");

    const auto& markers{project.frameIndex()};
    expectEqual(markers.size(), size_t{2}, "executeRapNudgeCommand should reuse the existing boundaries instead of creating new markers");
    expectEqual(markers[0].time100ns, int64_t{250}, "executeRapNudgeCommand should move the left boundary to the previous RAP");
    expectEqual(markers[1].time100ns, int64_t{750}, "executeRapNudgeCommand should move the right boundary to the next RAP");
}

struct NamedTest{
    const char* name;
    void (*fn)();
};

}

int wmain(int argc, wchar_t* argv[]){
    const vector<NamedTest> tests{
        {"UtilsParsing", &testUtilsParsing},
        {"UtilsWorkflowPolicies", &testUtilsWorkflowPolicies},
        {"MediaMkvInputIsRecognized", &testMediaMkvInputIsRecognized},
        {"TimelineIntervalNormalization", &testTimelineIntervalNormalization},
        {"TimelineMath", &testTimelineMath},
        {"TimelineDerivedHelpers", &testTimelineDerivedHelpers},
        {"TimelineTickDensityAndLabels", &testTimelineTickDensityAndLabels},
        {"TimelineZoomAnchorsAndThumbnailPlanning", &testTimelineZoomAnchorsAndThumbnailPlanning},
        {"TimelineZoomOutAtEndOfVideo", &testTimelineZoomOutAtEndOfVideo},
        {"AudioWaveformScalingAndThresholdColor", &testAudioWaveformScalingAndThresholdColor},
        {"AudioWaveformChunkPlanning", &testAudioWaveformChunkPlanning},
        {"TimelinePlanningEdgeCases", &testTimelinePlanningEdgeCases},
        {"ProjectCutRangesAndBoundaries", &testProjectCutRangesAndBoundaries},
        {"ProjectSceneBoundariesClampAndIgnoreInvalidCuts", &testProjectSceneBoundariesClampAndIgnoreInvalidCuts},
        {"ProjectNoCutPlanAndEmptyAlignment", &testProjectNoCutPlanAndEmptyAlignment},
        {"ProjectEffectiveExportAlignment", &testProjectEffectiveExportAlignment},
        {"ProjectTailCutPreservesLastMarker", &testProjectTailCutPreservesLastMarker},
        {"ProjectDurationFormattingSupportsHours", &testProjectDurationFormattingSupportsHours},
        {"ProjectRapLookupTargetsUseCutBoundaries", &testProjectRapLookupTargetsUseCutBoundaries},
        {"ProjectRapLookupTargetsNormalizeAndClamp", &testProjectRapLookupTargetsNormalizeAndClamp},
        {"ProjectMarkerToggleAndSceneRemap", &testProjectMarkerToggleAndSceneRemap},
        {"ProjectAudioNormalization", &testProjectAudioNormalization},
        {"ProjectRapMarkersSelectionNormalization", &testProjectRapMarkersSelectionNormalization},
        {"ProjectEffectiveExportProgressAndStablePlan", &testProjectEffectiveExportProgressAndStablePlan},
        {"ProjectEffectiveExportMergesAlignedBlocks", &testProjectEffectiveExportMergesAlignedBlocks},
        {"ProjectOutputDurationTracksMergedRanges", &testProjectOutputDurationTracksMergedRanges},
        {"ProjectEffectiveExportPreservesAlreadySafeTailCut", &testProjectEffectiveExportPreservesAlreadySafeTailCut},
        {"ProjectEstimateCoordinatorHelpers", &testProjectEstimateCoordinatorHelpers},
        {"ExportPreflightAndPlanSummary", &testExportPreflightAndPlanSummary},
        {"ExportCoordinatorDoesNotRequireFullRapReevaluationForExport", &testExportCoordinatorDoesNotRequireFullRapReevaluationForExport},
        {"ExportCutTimelineMapping", &testExportCutTimelineMapping},
        {"ExportVideoReadRangesSkipCutHeavySourceSpans", &testExportVideoReadRangesSkipCutHeavySourceSpans},
        {"ExportAudioCrossfade", &testExportAudioCrossfade},
        {"ExportH264SampleNormalization", &testExportH264SampleNormalization},
        {"ExportH264ParameterSetExtraction", &testExportH264ParameterSetExtraction},
        {"VideoStreamNalLengthFieldDetection", &testVideoStreamNalLengthFieldDetection},
        {"VideoStreamContainerSyncSampleDetection", &testVideoStreamContainerSyncSampleDetection},
        {"VideoStreamSampleRapDetection", &testVideoStreamSampleRapDetection},
        {"VideoStreamH264ParameterSetDetection", &testVideoStreamH264ParameterSetDetection},
        {"ExportReaderSampleTimeResolution", &testExportReaderSampleTimeResolution},
        {"MpegTsKeyframeClassification", &testMpegTsKeyframeClassification},
        {"MpegTsBitstreamRapDetection", &testMpegTsBitstreamRapDetection},
        {"MpegTsDecodeTimeInference", &testMpegTsDecodeTimeInference},
        {"MpegTsDecodeTimeSanitization", &testMpegTsDecodeTimeSanitization},
        {"MpegTsPesHeaderSanitization", &testMpegTsPesHeaderSanitization},
        {"VideoContainerWriterCodecGuards", &testVideoContainerWriterCodecGuards},
        {"VideoContainerDelegationCodecGuards", &testVideoContainerDelegationCodecGuards},
        {"EditorHistoryUndoRedo", &testEditorHistoryUndoRedo},
        {"EditorHistoryClearsRedoAfterNewEdit", &testEditorHistoryClearsRedoAfterNewEdit},
        {"EditorHistorySnapshotGuards", &testEditorHistorySnapshotGuards},
        {"EditorCommands", &testEditorCommands},
        {"EditorCommandsNoOpCases", &testEditorCommandsNoOpCases},
        {"EditorReevaluationAddsBracketMarkersAndAdjustsScenes", &testEditorReevaluationAddsBracketMarkersAndAdjustsScenes},
        {"EditorReevaluationPreservesTailCutBoundary", &testEditorReevaluationPreservesTailCutBoundary},
        {"EvaluatePlacedMarkerAffectsOnlyNewMarker", &testEvaluatePlacedMarkerAffectsOnlyNewMarker},
        {"EvaluatePlacedMarkerPreservesExistingCutRangesWhenInsertedIntoKeptScene", &testEvaluatePlacedMarkerPreservesExistingCutRangesWhenInsertedIntoKeptScene},
        {"EditorReevaluationOnlyShrinksExistingCutScenes", &testEditorReevaluationOnlyShrinksExistingCutScenes},
        {"EditorReevaluationNoOpWithoutMarkers", &testEditorReevaluationNoOpWithoutMarkers},
        {"EditorCommandsRapNudgeExpandScene", &testEditorCommandsRapNudgeExpandScene},
    };

    size_t failures{};
    for(const auto& test: tests){
        try{
            test.fn();
            wcout << L"[PASS] " << test.name << L"\n";
        }catch(const exception& ex){
            ++failures;
            wcout << L"[FAIL] " << test.name << L": " << ex.what() << L"\n";
        }
    }

    if(failures != 0){
        wcout << failures << L" test(s) failed\n";
        return 1;
    }

    wcout << tests.size() << L" test(s) passed\n";
    return 0;
}
