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
import llvc.CutPlanner;
import llvc.Project;
import llvc.Timeline;
import llvc.EditorController;
import llvc.EditorCommands;
import llvc.Export;
import llvc.VideoStream;
import llvc.VideoContainer;
import llvc.Media;
import llvc.ExportCoordinator;
import llvc.Session;

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

struct FakeRapMediaSource final: llvc::VideoSource{
    using llvc::VideoSource::VideoSource;

    InspectionResult inspect() const override{ return {}; }

    vector<int64_t> collectRapTimes100ns(const vector<int64_t>&, const function<void(double)>& progress, const function<bool()>& canceled) const override{
        if(canceled && canceled()){
            return {};
        }
        if(progress){ progress(100.0); }
        return {300, 100, 300};
    }

    void exportLossless(const ExportRequest&) const override{}
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

    const auto bufferedRange{timeline.expandThumbnailRange(visibleRange, 1, 12)};
    expectEqual(bufferedRange.first, 2, "expandThumbnailRange should include one thumbnail before the viewport");
    expectEqual(bufferedRange.last, 6, "expandThumbnailRange should include one thumbnail after the viewport");
    const auto leftEdgeRange{timeline.expandThumbnailRange({.first = 0, .last = 2}, 2, 12)};
    expectEqual(leftEdgeRange.first, 0, "expandThumbnailRange should clamp its left buffer");
    const auto rightEdgeRange{timeline.expandThumbnailRange({.first = 10, .last = 11}, 2, 12)};
    expectEqual(rightEdgeRange.last, 11, "expandThumbnailRange should clamp its right buffer");

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

void testTimelineOwnsNeutralPreviewStores(){
    llvc::Timeline timeline{};
    timeline.thumbnails().setup(llvc::ThumbnailPreviewSetup{
        .sourceFrameCount = 100,
        .sourceFrameRate = llvc::FrameRate{.num = 30, .den = 1},
        .zoomLevels = {10, 5, 10},
        .thumbnailHeight = 90,
        .sourceAspectRatio = llvc::Rational{.width = 16, .height = 9},
    });

    expectEqual(timeline.thumbnails().thumbnailWidth(), uint32_t{160}, "ThumbnailPreviewStore should derive thumbnail width from source aspect ratio");
    expectEqual(timeline.thumbnails().thumbnailHeight(), uint32_t{90}, "ThumbnailPreviewStore should keep configured thumbnail height");
    expectEqual(timeline.thumbnails().zoomTrackCount(), size_t{2}, "ThumbnailPreviewStore should normalize duplicate zoom levels");
    expectEqual(timeline.thumbnails().framesBetweenThumbnails(5), llvc::FrameIndex{5}, "ThumbnailPreviewStore should expose per-zoom frame spacing");
    expectEqual(timeline.thumbnails().slotCount(10), size_t{10}, "ThumbnailPreviewStore should create slots from source frame count and spacing");

    timeline.thumbnails().requestBuild(10, 20, 50);
    expect(!timeline.thumbnails().tryGet(10, 20), "ThumbnailPreviewStore should not return queued thumbnails before pixels are ready");

    timeline.thumbnails().putReadyFrame(20, 160, 90, vector<uint8_t>{1, 2, 3, 4});
    const auto ready{timeline.thumbnails().tryGet(10, 20)};
    expect(ready.has_value(), "ThumbnailPreviewStore should return ready neutral thumbnail pixels");
    expectEqual(ready->frameIndex, llvc::FrameIndex{20}, "ThumbnailPreviewStore should retrieve by source frame index");
    expectEqual(ready->bgraPixels.size(), size_t{4}, "ThumbnailPreviewStore should expose neutral BGRA pixel data");

    timeline.waveforms().setup(llvc::AudioWaveformSetup{
        .sourceFrameCount = 100,
        .sourceFrameRate = llvc::FrameRate{.num = 30, .den = 1},
        .zoomLevels = {1, 2, 2},
    });
    timeline.waveforms().replaceSamples({llvc::AudioWaveformSample{.firstFrame = 0, .endFrameExclusive = 10, .peak = 0.5f, .ready = true}});
    timeline.waveforms().analysisQueued(true);
    expectEqual(timeline.waveforms().zoomLevelCount(), size_t{2}, "AudioWaveformStore should normalize duplicate zoom levels");
    expectEqual(timeline.waveforms().samples().size(), size_t{1}, "AudioWaveformStore should own neutral waveform samples");
    timeline.waveforms().setupAnalysis(L"sample.mp4", 8, 2);
    expect(timeline.waveforms().beginAnalysis(), "AudioWaveformStore should own waveform activity state");
    timeline.waveforms().completeAnalysisChunk(0, vector<float>{0.1f, 0.2f, 0.3f, 0.4f});
    timeline.waveforms().completeAnalysisChunk(1, vector<float>{0.5f, 0.6f, 0.7f, 0.8f});
    timeline.waveforms().endAnalysis();
    const auto waveformView{timeline.waveforms().analysisView()};
    expect(waveformView.ready && !waveformView.inProgress, "AudioWaveformStore should retain completed neutral analysis data");
    expectEqual(waveformView.peaks.size(), size_t{8}, "AudioWaveformStore should retain bucket peaks");

    timeline.thumbnailRenderState().sourcePath = L"sample.mp4";
    timeline.thumbnailRenderState().built = {true, false};
    timeline.renderScheduling().renderVersion = 4;
    timeline.renderScheduling().renderCompleted = true;

    timeline.reset();
    expectEqual(timeline.thumbnails().sourceFrameCount(), llvc::FrameIndex{0}, "Timeline reset should clear thumbnail store setup");
    expectEqual(timeline.waveforms().sourceFrameCount(), llvc::FrameIndex{0}, "Timeline reset should clear waveform store setup");
    expectEqual(timeline.waveforms().samples().size(), size_t{0}, "Timeline reset should clear waveform samples");
    expect(!timeline.waveforms().analysisQueued(), "Timeline reset should clear waveform scheduling state");
    expect(timeline.thumbnailRenderState().sourcePath.empty() && timeline.thumbnailRenderState().built.empty(), "Timeline reset should clear thumbnail render scheduling state");
    expectEqual(timeline.renderScheduling().renderVersion, uint64_t{0}, "Timeline reset should clear render version state");
    expect(!timeline.renderScheduling().renderCompleted, "Timeline reset should clear render completion state");
}

void testTimelineSchedulingKeepsInitialPassAndCancelsByGeneration(){
    llvc::Timeline timeline{};
    timeline.thumbnails().setup({
        .sourceFrameCount = 100,
        .zoomLevels = {1},
        .thumbnailHeight = 10,
        .sourceAspectRatio = {.width = 16, .height = 9},
    });

    const auto initialGeneration{timeline.thumbnails().buildGeneration()};
    timeline.thumbnails().requestBuild(1, 0, 100);
    timeline.thumbnails().requestBuild(1, 40, 60);
    expect(timeline.thumbnails().hasPendingBuild(), "thumbnail scheduling should retain pending work");
    expect(timeline.thumbnails().initialFullPassPending(), "a delayed viewport request must not replace the initial full thumbnail pass");
    timeline.thumbnails().completeBuildPass(true);
    expect(!timeline.thumbnails().initialFullPassPending(), "completed full thumbnail pass should clear its pending state");
    timeline.thumbnails().cancelBuilds();
    expect(timeline.thumbnails().buildGeneration() > initialGeneration, "thumbnail cancellation should advance its generation");

    timeline.waveforms().setupAnalysis(L"sample.mp4", 8, 2);
    timeline.waveforms().analysisQueued(true);
    const auto waveformGeneration{timeline.waveforms().analysisGeneration()};
    expect(timeline.waveforms().hasPendingAnalysis(), "waveform scheduling should expose queued work");
    timeline.waveforms().cancelAnalysis();
    expect(!timeline.waveforms().hasPendingAnalysis(), "waveform cancellation should drain queued work");
    expect(timeline.waveforms().analysisGeneration() > waveformGeneration, "waveform cancellation should advance its generation");
}

void testTimelineFrameConversion(){
    llvc::Timeline timeline{};
    expectEqual(timeline.pointToFrame(0.0, 200.0, 100), llvc::FrameIndex{0}, "pointToFrame should map the left edge to frame zero");
    expectEqual(timeline.pointToFrame(100.0, 200.0, 100), llvc::FrameIndex{50}, "pointToFrame should map proportional canvas positions to frame indexes");
    expectEqual(timeline.pointToFrame(250.0, 200.0, 100), llvc::FrameIndex{99}, "pointToFrame should clamp beyond the right edge to the last frame");
    expectEqual(timeline.frameToPoint(50, 200.0, 100), 100.0, "frameToPoint should map frame indexes proportionally");
    expectEqual(timeline.frameToPoint(150, 200.0, 100), 200.0, "frameToPoint should clamp frame indexes beyond the source frame count");
}

void testSessionBuildsNeutralTimelineRenderModel(){
    llvc::Session session{};
    session.media().sourceFrameCount(100);
    session.project().frameIndex({
        marker(10'000'000, false, false, 10),
        marker(40'000'000, true, true, 40),
        marker(70'000'000, false, false, 70),
    });
    session.project().cutScenes({1});
    session.timeline().thumbnails().setup({
        .sourceFrameCount = 100,
        .sourceFrameRate = {.num = 30, .den = 1},
        .zoomLevels = {1},
        .thumbnailHeight = 10,
        .sourceAspectRatio = {.width = 1, .height = 1},
    });
    session.timeline().thumbnails().putReadyFrame(40, 10, 10, {1, 2, 3, 4});
    session.timeline().waveforms().replaceSamples({{.firstFrame = 10, .endFrameExclusive = 20, .peak = 0.5f, .ready = true}});

    const auto model{session.buildTimelineRenderModel(llvc::TimelineViewportRequest{
        .zoom = 1,
        .firstVisibleFrame = 0,
        .endVisibleFrameExclusive = 50,
        .viewportWidthPixels = 500.0,
        .viewportHeightPixels = 120.0,
    })};

    expectEqual(model.totalCanvasWidth, 1'000.0, "Session timeline render model should scale canvas width from visible frame span");
    expectEqual(model.markers.size(), size_t{3}, "Session timeline render model should expose planner markers");
    expectEqual(model.markers[1].frameIndex, llvc::FrameIndex{40}, "Session timeline render model should preserve marker frame indexes");
    expectEqual(model.markers[1].x, 400.0, "Session timeline render model should use Timeline math for marker positions");
    expect(model.markers[1].isEvaluatedAgainstRap, "Session timeline render model should carry marker RAP evaluation state");
    expectEqual(model.cutScenes.size(), size_t{1}, "Session timeline render model should expose planner cut scenes");
    expectEqual(model.cutScenes[0].firstFrame, llvc::FrameIndex{10}, "Session timeline render model should expose cut scene frame starts");
    expectEqual(model.cutScenes[0].endFrameExclusive, llvc::FrameIndex{40}, "Session timeline render model should expose exclusive cut scene frame ends");
    expectEqual(model.cutScenes[0].left, 100.0, "Session timeline render model should position cut scene left edges with Timeline math");
    expectEqual(model.cutScenes[0].width, 300.0, "Session timeline render model should position cut scene widths with Timeline math");
    expectEqual(model.thumbnails.size(), size_t{1}, "Session timeline render model should expose Timeline-owned neutral thumbnails");
    expectEqual(model.thumbnails[0].frameIndex, llvc::FrameIndex{40}, "Timeline render thumbnails should retain source frame identity");
    expectEqual(model.thumbnails[0].bgraPixels.size(), size_t{4}, "Timeline render thumbnails should contain neutral pixel data");
    expectEqual(model.waveform.size(), size_t{1}, "Session timeline render model should expose Timeline-owned waveform data");
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

void testCutPlannerFrameModelAndSceneRemap(){
    llvc::CutPlanner planner{};
    expect(planner.addMarker(70), "CutPlanner should add a frame-index marker");
    expect(planner.addMarker(30), "CutPlanner should keep markers sorted after insertion");
    expect(!planner.addMarker(30), "CutPlanner should reject duplicate marker frame indexes");
    expectEqual(planner.markers().size(), size_t{2}, "CutPlanner marker set should be deduplicated");
    expectEqual(planner.markers().markers()[0].frameIndex, llvc::FrameIndex{30}, "CutPlanner should sort markers by frame index");
    expectEqual(planner.markers().markers()[1].frameIndex, llvc::FrameIndex{70}, "CutPlanner should preserve later sorted markers");

    expect(planner.setCutSceneContainingFrame(40, true, 100), "CutPlanner should cut the scene containing a frame");
    expectEqual(planner.cutScenes().cutSceneIndexes()[0], uint32_t{1}, "CutPlanner should map frame 40 to the middle scene");

    expect(planner.addMarker(50), "CutPlanner should insert a marker inside the cut scene");
    expectEqual(vector<uint32_t>{planner.cutScenes().cutSceneIndexes().begin(), planner.cutScenes().cutSceneIndexes().end()}, vector<uint32_t>{1, 2}, "CutPlanner should split a cut scene when a marker is inserted inside it");

    expect(planner.removeMarker(50), "CutPlanner should remove an existing marker");
    expectEqual(vector<uint32_t>{planner.cutScenes().cutSceneIndexes().begin(), planner.cutScenes().cutSceneIndexes().end()}, vector<uint32_t>{1}, "CutPlanner should merge scene indexes when a marker is removed");

    const auto ranges{planner.buildSceneRanges(100)};
    expectEqual(ranges.size(), size_t{3}, "CutPlanner should build one scene per marker-delimited range");
    expectEqual(ranges[1].firstFrame, llvc::FrameIndex{30}, "CutPlanner should use frame indexes as scene range starts");
    expectEqual(ranges[1].endFrameExclusive, llvc::FrameIndex{70}, "CutPlanner should use exclusive frame-index scene ends");
    expect(ranges[1].cut, "CutPlanner should flag cut scenes in the render-independent range model");

    const auto directPlan{planner.buildDirectExportPlan(100)};
    expect(directPlan.hasRequestedCuts, "CutPlanner direct export plan should report requested cuts");
    expectEqual(directPlan.requestedCutRanges.size(), size_t{1}, "CutPlanner direct export plan should expose cut frame ranges");
    expectEqual(directPlan.requestedCutRanges[0].firstFrame, llvc::FrameIndex{30}, "CutPlanner export ranges should start on frame indexes");
    expectEqual(directPlan.requestedCutRanges[0].endFrameExclusive, llvc::FrameIndex{70}, "CutPlanner export ranges should end on exclusive frame indexes");
    expectEqual(directPlan.requestedOutputFrameCount, llvc::FrameIndex{60}, "CutPlanner should calculate output frame count after cuts");
}

void testProjectSynchronizesOwnedCutPlanner(){
    llvc::Project project{};
    project.cutPlanner().rapFrames().replaceAll({25});
    project.timelineDuration100ns(100'000'000);
    project.frameIndex({
        marker(10'000'000, false, false, 10),
        marker(40'000'000, true, true, 40),
    });
    project.cutScenes({1});

    const auto& planner{project.cutPlanner()};
    expectEqual(planner.markers().size(), size_t{2}, "Project should mirror legacy markers into its owned CutPlanner");
    expectEqual(planner.markers().markers()[0].frameIndex, llvc::FrameIndex{10}, "Project should use marker sampleIndex as the planner frame identity");
    expectEqual(planner.markers().markers()[1].frameIndex, llvc::FrameIndex{40}, "Project should keep planner markers sorted by frame index");
    expect(planner.rapFrames().contains(40), "Project should mirror evaluated legacy markers into planner RAP frames");
    expect(planner.rapFrames().contains(25), "Project legacy synchronization should preserve scanned planner RAP frames");
    expectEqual(planner.cutScenes().cutSceneIndexes()[0], uint32_t{1}, "Project should mirror legacy cut scenes into its owned CutPlanner");
}

void testProjectMapsLegacyMarkerTimesToPlannerFrames(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100), marker(900)});
    project.cutScenes({0, 2});
    project.synchronizeCutPlannerFrames(1'000);

    const auto ranges{project.cutPlanner().buildSceneRanges(1'000)};
    expectEqual(ranges.size(), size_t{3}, "legacy marker times should create three planner scenes");
    expect(ranges[0].cut && !ranges[1].cut && ranges[2].cut, "legacy edge cut scenes should preserve the middle scene");
    expectEqual(ranges[0].endFrameExclusive, llvc::FrameIndex{100}, "first legacy marker should map to its source frame");
    expectEqual(ranges[2].firstFrame, llvc::FrameIndex{900}, "second legacy marker should map to its source frame");
    expectEqual(project.cutPlanner().buildCutSceneRanges(1'000).size(), size_t{2}, "planner conversion should retain both legacy cut regions");
    expect(project.frameIndex().empty() && project.cutScenes().empty(), "legacy edit data must be discarded after one-way conversion to CutPlanner");
}

void testProjectSynchronizesPlannerMarkersForLegacyReevaluation(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.cutPlanner().addMarker(100);
    project.cutPlanner().addMarker(300);
    project.cutPlanner().cutScenes().replaceCutSceneIndexes({1});

    project.synchronizeLegacyEditModelFromCutPlanner(1'000);
    expectEqual(project.frameIndex().size(), size_t{2}, "planner markers should be available to legacy re-evaluation");
    expectEqual(project.frameIndex()[1].time100ns, int64_t{300}, "planner marker frame should map to legacy marker time");
    expectEqual(project.cutScenes(), vector<uint32_t>{1}, "planner cut scenes should be available to legacy re-evaluation");
}

void testProjectPreservesExactRapTimesAcrossLegacySynchronization(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(255, true, true, 0)});
    project.synchronizeCutPlannerFrames(1'000);
    project.synchronizeLegacyEditModelFromCutPlanner(1'000);

    expectEqual(project.frameIndex()[0].time100ns, int64_t{255}, "legacy synchronization should retain exact evaluated RAP time values");
}

void testSessionResetClearsOwnedState(){
    llvc::Session session{};
    session.media().inspection().frameRate = {.num = 30, .den = 1};
    session.media().sourceFrameCount(90);
    expectEqual(session.media().frameIndexForTime100ns(500'0000), llvc::FrameIndex{15}, "MediaSession should convert playback time to source frame indexes");
    session.project().timelineDuration100ns(100'000'000);
    session.project().frameIndex({marker(10'000'000, false, false, 10)});
    session.project().cutScenes({1});
    session.preview().play();
    session.preview().seekToFrame(50);
    session.exporter().beginExport();
    jthread exportWorker{[&]{
        while(!session.exporter().cancelRequested()){
            this_thread::yield();
        }
        session.exporter().endExport();
    }};
    session.editorHistory().undoStack.push_back(llvc::captureEditorSnapshot(session.project()));

    session.reset();

    expect(!session.project().hasVideoFile(), "Session reset should reset Project state");
    expectEqual(session.media().sourceFrameCount(), llvc::FrameIndex{0}, "Session reset should clear media frame metadata");
    expect(session.project().frameIndex().empty(), "Session reset should clear project markers");
    expect(session.project().cutPlanner().markers().empty(), "Session reset should clear planner markers through Project");
    expect(!session.preview().isPlaying(), "Session reset should stop preview playback state");
    expectEqual(session.preview().currentFrame(), llvc::FrameIndex{0}, "Session reset should reset preview frame position");
    expect(!session.exporter().isExportInProgress(), "Session reset should clear export operation state");
    expect(!session.exporter().cancelRequested(), "Session reset should clear export cancellation state");
    expect(session.editorHistory().undoStack.empty(), "Session reset should clear editor undo history");
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
    bool buildPlanCalled{};
    bool exportWasActiveDuringPreflight{};
    vector<int64_t> requestedRapTargets{};
    llvc::ExportCoordinatorResult result{};
    llvc::ExportController exporter{};
    exporter.run(llvc::ExportCoordinatorRequest{
        .project = &project,
        .media = &media,
        .mediaInfo = &mediaInfo,
        .sourcePath = sourcePath.wstring(),
        .outputPath = outputPath.wstring(),
        .temporaryOutputPath = tempOutputPath.wstring(),
        .sourceDuration100ns = 1'000,
        .sourceHasAudio = false,
        .needsRapReevaluation = true,
        .ensureRapMarkersAvailableAsync = [&ensureRapCalled, &requestedRapTargets, &exportWasActiveDuringPreflight, &exporter](const wstring&, const vector<int64_t>& targets, const function<void(double)>& progressCallback) -> winrt::Windows::Foundation::IAsyncOperation<bool>{
            ensureRapCalled = true;
            requestedRapTargets = targets;
            exportWasActiveDuringPreflight = exporter.isExportInProgress();
            if(progressCallback){
                progressCallback(100.0);
            }
            co_return true;
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
    expect(exportWasActiveDuringPreflight, "ExportController should own the export activity during RAP preflight");
    expect(!exporter.isExportInProgress(), "ExportController should end its activity after the export completes");
    expect(requestedRapTargets == vector<int64_t>{200, 600}, "runExportAsync should request RAP data for the export cut boundaries");
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

void testEditorFrameCommandsUseCutPlanner(){
    llvc::Project project{};
    llvc::EditorHistoryState history{};

    const auto addMarker{llvc::executeToggleMarkerFrameCommand(project, history, 30)};
    expect(addMarker.changed && addMarker.markerCountIncreased, "frame marker command should add a planner marker");
    expect(project.cutPlanner().markers().contains(30), "frame marker command should target Project-owned CutPlanner markers");
    expect(addMarker.refreshTimelineTicks && addMarker.refreshCutOverlays && addMarker.refreshWindowTitle, "frame marker command should request normal timeline refreshes");
    expect(project.isDirty(), "frame marker command should mark the project dirty");

    const auto toggleCut{llvc::executeToggleCutSceneFrameCommand(project, history, 40, 100)};
    expect(toggleCut.changed, "frame cut-scene command should toggle a planner scene");
    expect(project.cutPlanner().cutScenes().containsSceneIndex(1), "frame cut-scene command should target Project-owned CutPlanner cut scenes");

    const auto clearCut{llvc::executeSetCutSceneFrameCommand(project, history, 40, false, 100)};
    expect(clearCut.changed, "frame set-cut-scene command should change planner scene state");
    expect(!project.cutPlanner().cutScenes().containsSceneIndex(1), "frame set-cut-scene command should clear planner cut scenes");

    const auto removeMarker{llvc::executeToggleMarkerFrameCommand(project, history, 30)};
    expect(removeMarker.changed && !removeMarker.markerCountIncreased, "frame marker command should remove an existing planner marker");
    expect(!project.cutPlanner().markers().contains(30), "frame marker command should remove from Project-owned CutPlanner markers");
}

void testEditorHistoryUndoRedoPlannerFrameCommands(){
    llvc::Project project{};
    llvc::EditorHistoryState history{};
    project.cutPlanner().rapFrames().replaceAll({0, 30, 60, 90});

    expect(llvc::executeToggleMarkerFrameCommand(project, history, 30).changed, "frame marker command should create undo history");
    expect(llvc::executeToggleCutSceneFrameCommand(project, history, 40, 100).changed, "frame cut-scene command should create undo history");

    const auto undoCut{llvc::undo(project, history)};
    expect(undoCut.changed, "undo should restore the prior planner cut-scene state");
    expect(!project.cutPlanner().cutScenes().containsSceneIndex(1), "undo should clear the planner cut scene");
    expect(project.cutPlanner().markers().contains(30), "undoing a cut scene should retain the planner marker");
    expect(project.cutPlanner().rapFrames().contains(60), "undo should retain planner RAP data");

    const auto undoMarker{llvc::undo(project, history)};
    expect(undoMarker.changed, "undo should restore the prior planner marker state");
    expect(!project.cutPlanner().markers().contains(30), "undo should remove the planner marker");

    const auto redoMarker{llvc::redo(project, history)};
    expect(redoMarker.changed && project.cutPlanner().markers().contains(30), "redo should restore the planner marker");

    const auto redoCut{llvc::redo(project, history)};
    expect(redoCut.changed && project.cutPlanner().cutScenes().containsSceneIndex(1), "redo should restore the planner cut scene");
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

void testSessionResetDrainsTimelineActivity(){
    llvc::Session session{};
    atomic_bool started{};
    jthread worker{[&]{
        auto activity{session.timeline().beginActivity()};
        started.store(true, memory_order_release);
        while(!activity.canceled()){
            this_thread::yield();
        }
    }};
    while(!started.load(memory_order_acquire)){
        this_thread::yield();
    }

    session.reset();
    expect(session.timeline().activityCanceled(), "Session reset should cancel active timeline work before it clears the timeline");
}

void testTimelineActivityLifecycleAndRenderModelPayload(){
    llvc::Timeline timeline{};
    {
        auto activity{timeline.beginActivity()};
        expect(!activity.canceled(), "Timeline activity should begin active");
        timeline.cancelOutstandingWork();
        expect(activity.canceled(), "Timeline activity should observe cancellation before reset");
    }
    timeline.waitForIdle();

    timeline.waveforms().replaceSamples({{.firstFrame = 10, .endFrameExclusive = 20, .peak = 0.75f, .ready = true}});
    llvc::CutPlanner planner{};
    planner.addMarker(30);
    planner.cutScenes().replaceCutSceneIndexes({1});
    const auto model{timeline.buildRenderModel(planner, 100, {.zoom = 1, .firstVisibleFrame = 0, .endVisibleFrameExclusive = 100, .viewportWidthPixels = 500, .viewportHeightPixels = 80})};
    expectEqual(model.waveform.size(), size_t{1}, "Timeline render model should carry neutral waveform data");
    expectEqual(model.cutScenes.size(), size_t{1}, "Timeline render model should carry planner cut scenes");
}

void testPreviewControllerOwnsNavigationTargets(){
    llvc::PreviewController preview{};
    expectEqual(preview.seekTarget(150, 100), llvc::FrameIndex{99}, "Preview seek target should clamp to source bounds");
    expectEqual(preview.stepTarget(-200, 100), llvc::FrameIndex{0}, "Preview step target should clamp at the source start");
    expectEqual(preview.stepTarget(1, 100), llvc::FrameIndex{1}, "Preview step target should advance one frame after a backward clamp");
    expectEqual(preview.stepTarget(1, 100), llvc::FrameIndex{2}, "Preview step target should continue advancing frame by frame");
    expectEqual(preview.percentTarget(50, 101), llvc::FrameIndex{50}, "Preview percentage target should be frame based");
}

void testMediaSessionOwnsRapLookupTargets(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(200), marker(600)});
    project.cutScenes({1});
    llvc::MediaSession media{};
    expectEqual(media.rapLookupTargets(project, 1'000), vector<int64_t>{200, 600}, "MediaSession should provide the RAP lookup targets for the current project");
}

void testMediaSessionFrameTimeRoundTripsAtFractionalRate(){
    llvc::MediaSession media{};
    media.inspection().frameRate = {.num = 30'000, .den = 1'001};
    media.sourceFrameCount(100);
    const auto frameOneTime{media.time100nsForFrame(1)};
    expectEqual(media.frameIndexForTime100ns(frameOneTime), llvc::FrameIndex{1}, "fractional-rate frame timestamps should round-trip to the same frame");
    expectEqual(media.frameIndexForTime100ns(media.time100nsForFrame(2)), llvc::FrameIndex{2}, "fractional-rate stepping should not collapse the second frame onto the first");
}

void testMediaSessionOwnsCanonicalRapScanCommit(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.cutPlanner().addMarker(100);
    project.cutPlanner().addMarker(500);
    project.synchronizeCutPlannerFrames(1'000);
    llvc::MediaSession media{};
    media.inspection().frameRate = {.num = 10'000'000, .den = 1};
    media.sourceFrameCount(1'000);

    expectEqual(media.markerTimesForRapScan(project), vector<int64_t>{100, 500}, "MediaSession should derive RAP scan targets from canonical planner markers");
    media.commitRapLookup(project, true, true, {100, 500}, {100, 500});
    expectEqual(vector<llvc::FrameIndex>{project.cutPlanner().rapFrames().frames().begin(), project.cutPlanner().rapFrames().frames().end()}, vector<llvc::FrameIndex>{100, 500}, "MediaSession should commit scanned RAPs to the planner frame set");
}

void testMediaSessionQueuesRepeatedRapRequestsAndTracksRetry(){
    llvc::MediaSession media{};
    media.load(L"sample.mp4", make_unique<FakeRapMediaSource>(L"sample.mp4"), {});
    expect(media.queueRapLookup({500, 200, 500}), "MediaSession should accept a queued RAP request");
    expect(media.hasQueuedRapLookup(), "MediaSession should expose queued RAP work");
    expectEqual(media.takeQueuedRapLookupTargets(), vector<int64_t>{200, 500}, "MediaSession should coalesce duplicate queued RAP targets");
    expect(!media.hasQueuedRapLookup(), "taking queued RAP work should drain the request");
    media.requestRapRetry();
    expectEqual(media.rapLookup().retryCount, uint32_t{1}, "MediaSession should count explicit RAP retries");
    media.abandonRapLookup();
    expect(media.rapLookup().canceled, "abandoned RAP work should retain a canceled result state");
}

void testCanonicalFrameExportPlanAndMediaRapActivity(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100), marker(300), marker(600)});
    project.cutScenes({1});
    project.synchronizeCutPlannerFrames(1'000);
    project.cutPlanner().rapFrames().replaceAll({120, 250, 600});

    const auto directPlan{project.buildFrameExportPlan(false)};
    expectEqual(directPlan.requestedCutRanges, vector<llvc::ExportFrameRange>{{.firstFrame = 100, .endFrameExclusive = 300}}, "Project should expose the canonical frame-range export plan");
    const auto effectivePlan{project.buildEffectiveExportPlanWithRapPreroll(1'000, {})};
    expectEqual(effectivePlan.effectiveCutRanges, vector<llvc::ExportFrameRange>{{.firstFrame = 120, .endFrameExclusive = 250}}, "canonical export should shrink an unevaluated cut scene inward to its surrounding RAP frames");
    expectEqual(llvc::buildRapLookupTimesForExportAlignment(project, 1'000), vector<int64_t>{100, 300, 600}, "export RAP targets must come from unevaluated canonical planner frames after undo or redo");

    llvc::MediaSession media{};
    media.load(L"sample.mp4", make_unique<FakeRapMediaSource>(L"sample.mp4"), {});
    expect(media.scanAndCommitRapMarkersAsync(project, {200}, {}, {}).get(), "MediaSession should own the RAP scan transaction");
    expect(media.rapLookup().succeeded && media.rapLookup().partial, "MediaSession should commit RAP scan results and activity state");
    media.reset();
    expect(!media.hasSource() && !media.rapLookup().inProgress, "MediaSession reset should drain and clear RAP activity state");
}

void testCanonicalExportMergesCutFragmentsBeforeRapAlignment(){
    llvc::CutPlanner planner{};
    planner.addMarker(100);
    planner.addMarker(200);
    planner.addMarker(300);
    planner.cutScenes().replaceCutSceneIndexes({1, 2});
    planner.rapFrames().replaceAll({120, 180, 220, 280});

    const auto plan{planner.buildRapAlignedExportPlan(1'000)};
    expectEqual(plan.effectiveCutRanges, vector<llvc::ExportFrameRange>{{.firstFrame = 120, .endFrameExclusive = 280}}, "RAP alignment must preserve a cut scene split by a newly inserted marker");
}

void testCanonicalExportPlanDoesNotRealignReevaluatedTailCut(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(200), marker(600)});
    project.refreshSelectedMarkers();
    project.cutScenes({2});
    project.synchronizeCutPlannerFrames(1'000);
    project.cutPlanner().rapFrames().replaceAll({200, 700});

    const auto plan{project.buildEffectiveExportPlanWithRapPreroll(1'000, {})};
    expectEqual(plan.effectiveCutRanges, vector<llvc::ExportFrameRange>{{.firstFrame = 600, .endFrameExclusive = 1'000}}, "canonical export must preserve the reevaluated tail-cut boundary instead of snapping it again");
    expectEqual(plan.effectiveCutRanges100ns, vector<pair<int64_t, int64_t>>{{600, 1'000}}, "canonical export must preserve the tail cut when converting its frame plan to source times");
}

void testCanonicalProjectPlannerSurvivesMediaMetadataArrival(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.cutPlanner().markers().replaceAll({
        {.frameIndex = 200, .isEvaluatedAgainstRap = true},
        {.frameIndex = 600, .isEvaluatedAgainstRap = false},
    });
    project.cutPlanner().rapFrames().replaceAll({200});
    project.cutPlanner().cutScenes().replaceCutSceneIndexes({1});

    project.synchronizeCutPlannerFrames(1'000);

    expectEqual(project.cutPlanner().markers().markers().size(), size_t{2}, "media metadata must not replace canonical persisted planner markers with an empty legacy list");
    expectEqual(project.cutPlanner().markers().markers()[1].frameIndex, llvc::FrameIndex{600}, "canonical marker frame identity must survive metadata arrival");
    expectEqual(vector<uint32_t>{project.cutPlanner().cutScenes().cutSceneIndexes().begin(), project.cutPlanner().cutScenes().cutSceneIndexes().end()}, vector<uint32_t>{1}, "canonical cut scenes must survive metadata arrival");
}

void testExportControllerUsesCachedPlanBeforeRapLookup(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(200), marker(600)});
    project.refreshSelectedMarkers();
    project.cutScenes({1});

    llvc::ExportController exporter{};
    const auto snapshot{llvc::captureEditorSnapshot(project)};
    const wstring sourcePath{L"sample.mp4"};
    const llvc::EffectiveExportPlan expectedPlan{.effectiveCutRanges100ns = {{200, 600}}};
    exporter.cachePlan(expectedPlan, snapshot, sourcePath);

    const auto cachedPlan{exporter.tryGetCachedPlan(snapshot, sourcePath)};
    expect(cachedPlan.has_value(), "ExportController should return a plan cached by RAP reevaluation before requesting another RAP lookup");
    expectEqual(cachedPlan->effectiveCutRanges100ns, expectedPlan.effectiveCutRanges100ns, "cached reevaluated plan should remain the export authority while red helper markers are present");
}

void testSessionInvalidatesActivityGeneration(){
    llvc::Session session{};
    const auto initialGeneration{session.activityGeneration()};
    session.invalidateActivities();
    expect(session.activityGeneration() > initialGeneration, "Session activity invalidation should advance the stale-work generation");
    const auto invalidatedGeneration{session.activityGeneration()};
    session.reset();
    expect(session.activityGeneration() > invalidatedGeneration, "Session reset should invalidate outstanding activity work before clearing state");
}

void testPreviewSkipsPlannerCutAtPlaybackStart(){
    llvc::CutPlanner planner{};
    planner.addMarker(100);
    planner.addMarker(300);
    planner.cutScenes().replaceCutSceneIndexes({1});
    llvc::PreviewController preview{};
    preview.seekToFrame(150, 1'000);

    expect(preview.skipCurrentCutSceneDuringPlayback(planner, 1'000), "PreviewController should skip a cut scene when playback starts inside it");
    expectEqual(preview.currentFrame(), llvc::FrameIndex{300}, "PreviewController should seek to the cut scene's exclusive end frame");
}

void testPreviewSkipsContiguousPlannerCutScenesInOneSeek(){
    llvc::CutPlanner planner{};
    planner.addMarker(100);
    planner.addMarker(200);
    planner.addMarker(300);
    planner.addMarker(400);
    planner.cutScenes().replaceCutSceneIndexes({1, 2, 3});
    llvc::PreviewController preview{};
    preview.seekToFrame(150, 1'000);

    expect(preview.skipCurrentCutSceneDuringPlayback(planner, 1'000), "PreviewController should skip a contiguous deleted run");
    expectEqual(preview.currentFrame(), llvc::FrameIndex{400}, "PreviewController should skip all contiguous cut scenes in one seek");
}

void testPlannerNativeReevaluationPreservesCutIntent(){
    llvc::Project project{};
    project.cutPlanner().addMarker(300);
    project.cutPlanner().addMarker(700);
    project.cutPlanner().cutScenes().replaceCutSceneIndexes({1});
    project.cutPlanner().rapFrames().replaceAll({250, 400, 650, 750});
    llvc::EditorHistoryState history{};

    const auto result{llvc::reevaluateClearCutMarkers(project, llvc::FrameIndex{1'000}, history, true)};
    expect(result.changed, "planner-native reevaluation should update RAP-unsafe cut boundaries");
    expectEqual(project.cutPlanner().buildCutSceneRanges(1'000), vector<llvc::SceneFrameRange>{{.firstFrame = 400, .endFrameExclusive = 650, .sceneIndex = 3, .cut = true}}, "planner-native reevaluation should shrink the requested cut range to safe RAP boundaries");
    expectEqual(history.undoStack.size(), size_t{1}, "planner-native reevaluation should create one undo snapshot");

    (void)llvc::reevaluateClearCutMarkers(project, llvc::FrameIndex{1'000}, history, false);
    expectEqual(project.cutPlanner().buildCutSceneRanges(1'000), vector<llvc::SceneFrameRange>{{.firstFrame = 400, .endFrameExclusive = 650, .sceneIndex = 3, .cut = true}}, "repeated planner-native reevaluation should be stable");
}

void testPlannerNativeReevaluationPreservesTailCutBoundary(){
    llvc::Project project{};
    project.cutPlanner().addMarker(600);
    project.cutPlanner().cutScenes().replaceCutSceneIndexes({1});
    project.cutPlanner().rapFrames().replaceAll({550, 700, 900});
    llvc::EditorHistoryState history{};

    (void)llvc::reevaluateClearCutMarkers(project, llvc::FrameIndex{1'000}, history, false);
    (void)llvc::reevaluateClearCutMarkers(project, llvc::FrameIndex{1'000}, history, false);

    const auto cuts{project.cutPlanner().buildCutSceneRanges(1'000)};
    expect(!cuts.empty(), "planner-native reevaluation should retain the tail cut");
    expectEqual(cuts.front().firstFrame, llvc::FrameIndex{600}, "repeated planner-native reevaluation should preserve the original tail-cut boundary");
    expectEqual(cuts.back().endFrameExclusive, llvc::FrameIndex{1'000}, "planner-native reevaluation should preserve the tail cut through the source end");
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

    (void)llvc::reevaluateClearCutMarkers(project, timeline, {550, 700, 900}, history, false);
    expectEqual(project.buildCutRanges100ns(), vector<pair<int64_t, int64_t>>{{600, 1'000}}, "repeated reevaluation should preserve the tail-cut boundary");
}

void testEditorReevaluationPreservesPlannerKeptGapBetweenEdgeCuts(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100, false, false), marker(300, false, false)});
    project.refreshSelectedMarkers();
    project.cutScenes({0, 2});
    llvc::EditorHistoryState history{};
    llvc::Timeline timeline{};

    (void)llvc::reevaluateClearCutMarkers(project, timeline, {50, 150, 250, 350}, history, false);
    project.synchronizeCutPlannerFrames(1'000);

    expectEqual(project.cutPlanner().buildCutSceneRanges(1'000), vector<llvc::SceneFrameRange>{{.firstFrame = 0, .endFrameExclusive = 50, .sceneIndex = 0, .cut = true}, {.firstFrame = 300, .endFrameExclusive = 350, .sceneIndex = 5, .cut = true}, {.firstFrame = 350, .endFrameExclusive = 1'000, .sceneIndex = 6, .cut = true}}, "planner synchronization should preserve the RAP-aligned kept gap between edge cuts");
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

void testEvaluatePlacedMarkerUsesCanonicalFrames(){
    llvc::Project project{};
    project.cutPlanner().addMarker(300);
    project.cutPlanner().cutScenes().replaceCutSceneIndexes({1});

    const auto result{llvc::evaluatePlacedMarkerFrameAgainstRap(project, {250, 350}, 300)};
    expect(result.changed, "frame-native marker evaluation should update the requested planner marker");
    expectEqual(project.cutPlanner().buildRapAlignedExportPlan(1'000).effectiveCutRanges, vector<llvc::ExportFrameRange>{{.firstFrame = 300, .endFrameExclusive = 1'000}}, "frame-native marker evaluation should preserve the split cut scene state");
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
    project.synchronizeCutPlannerFrames(1'000);
    expectEqual(project.cutPlanner().buildCutSceneRanges(1'000).size(), size_t{1}, "planner synchronization should preserve the re-evaluated kept gap");
    expectEqual(project.cutPlanner().buildCutSceneRanges(1'000)[0].firstFrame, llvc::FrameIndex{400}, "planner synchronization should retain the RAP-aligned cut start");
    expectEqual(project.cutPlanner().buildCutSceneRanges(1'000)[0].endFrameExclusive, llvc::FrameIndex{650}, "planner synchronization should retain the RAP-aligned cut end");
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
        {"TimelineOwnsNeutralPreviewStores", &testTimelineOwnsNeutralPreviewStores},
        {"TimelineSchedulingKeepsInitialPassAndCancelsByGeneration", &testTimelineSchedulingKeepsInitialPassAndCancelsByGeneration},
        {"TimelineFrameConversion", &testTimelineFrameConversion},
        {"SessionBuildsNeutralTimelineRenderModel", &testSessionBuildsNeutralTimelineRenderModel},
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
        {"CutPlannerFrameModelAndSceneRemap", &testCutPlannerFrameModelAndSceneRemap},
        {"ProjectSynchronizesOwnedCutPlanner", &testProjectSynchronizesOwnedCutPlanner},
        {"ProjectMapsLegacyMarkerTimesToPlannerFrames", &testProjectMapsLegacyMarkerTimesToPlannerFrames},
        {"ProjectSynchronizesPlannerMarkersForLegacyReevaluation", &testProjectSynchronizesPlannerMarkersForLegacyReevaluation},
        {"ProjectPreservesExactRapTimesAcrossLegacySynchronization", &testProjectPreservesExactRapTimesAcrossLegacySynchronization},
        {"SessionResetClearsOwnedState", &testSessionResetClearsOwnedState},
        {"SessionResetDrainsTimelineActivity", &testSessionResetDrainsTimelineActivity},
        {"TimelineActivityLifecycleAndRenderModelPayload", &testTimelineActivityLifecycleAndRenderModelPayload},
        {"PreviewControllerOwnsNavigationTargets", &testPreviewControllerOwnsNavigationTargets},
        {"MediaSessionOwnsRapLookupTargets", &testMediaSessionOwnsRapLookupTargets},
        {"MediaSessionFrameTimeRoundTripsAtFractionalRate", &testMediaSessionFrameTimeRoundTripsAtFractionalRate},
        {"MediaSessionOwnsCanonicalRapScanCommit", &testMediaSessionOwnsCanonicalRapScanCommit},
        {"MediaSessionQueuesRepeatedRapRequestsAndTracksRetry", &testMediaSessionQueuesRepeatedRapRequestsAndTracksRetry},
        {"SessionInvalidatesActivityGeneration", &testSessionInvalidatesActivityGeneration},
        {"PreviewSkipsPlannerCutAtPlaybackStart", &testPreviewSkipsPlannerCutAtPlaybackStart},
        {"PreviewSkipsContiguousPlannerCutScenesInOneSeek", &testPreviewSkipsContiguousPlannerCutScenesInOneSeek},
        {"ProjectEstimateCoordinatorHelpers", &testProjectEstimateCoordinatorHelpers},
        {"CanonicalFrameExportPlanAndMediaRapActivity", &testCanonicalFrameExportPlanAndMediaRapActivity},
        {"CanonicalExportMergesCutFragmentsBeforeRapAlignment", &testCanonicalExportMergesCutFragmentsBeforeRapAlignment},
        {"CanonicalExportPlanDoesNotRealignReevaluatedTailCut", &testCanonicalExportPlanDoesNotRealignReevaluatedTailCut},
        {"CanonicalProjectPlannerSurvivesMediaMetadataArrival", &testCanonicalProjectPlannerSurvivesMediaMetadataArrival},
        {"ExportControllerUsesCachedPlanBeforeRapLookup", &testExportControllerUsesCachedPlanBeforeRapLookup},
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
        {"EditorFrameCommandsUseCutPlanner", &testEditorFrameCommandsUseCutPlanner},
        {"EditorHistoryUndoRedoPlannerFrameCommands", &testEditorHistoryUndoRedoPlannerFrameCommands},
        {"EditorCommandsNoOpCases", &testEditorCommandsNoOpCases},
        {"EditorReevaluationAddsBracketMarkersAndAdjustsScenes", &testEditorReevaluationAddsBracketMarkersAndAdjustsScenes},
        {"PlannerNativeReevaluationPreservesCutIntent", &testPlannerNativeReevaluationPreservesCutIntent},
        {"PlannerNativeReevaluationPreservesTailCutBoundary", &testPlannerNativeReevaluationPreservesTailCutBoundary},
        {"EditorReevaluationPreservesTailCutBoundary", &testEditorReevaluationPreservesTailCutBoundary},
        {"EditorReevaluationPreservesPlannerKeptGapBetweenEdgeCuts", &testEditorReevaluationPreservesPlannerKeptGapBetweenEdgeCuts},
        {"EvaluatePlacedMarkerAffectsOnlyNewMarker", &testEvaluatePlacedMarkerAffectsOnlyNewMarker},
        {"EvaluatePlacedMarkerUsesCanonicalFrames", &testEvaluatePlacedMarkerUsesCanonicalFrames},
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
