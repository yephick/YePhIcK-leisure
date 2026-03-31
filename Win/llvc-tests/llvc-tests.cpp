#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

import std;
import llvc.Utils;
import llvc.Project;
import llvc.Timeline;
import llvc.EditorController;
import llvc.EditorCommands;

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

template<typename T>
void expectEqual(const T& actual, const T& expected, const string& message){
    if(!(actual == expected)){
        throw TestFailure(message);
    }
}

void expectNear(double actual, double expected, double tolerance, const string& message){
    if(abs(actual - expected) > tolerance){
        throw TestFailure(message);
    }
}

[[noreturn]] void fail(const string& message){
    throw TestFailure(message);
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

void testProjectCutRangesAndBoundaries(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100), marker(300), marker(600)});
    project.refreshSelectedMarkers();
    project.cutScenes({1, 2});

    expectEqual(project.buildSceneBoundaries100ns(), vector<int64_t>{0, 100, 300, 600, 1'000}, "buildSceneBoundaries100ns should include edges and sorted markers");
    expectEqual(project.buildCutRanges100ns(), vector<pair<int64_t, int64_t>>{{100, 600}}, "buildCutRanges100ns should merge adjacent cut scenes");
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
    project.videoFile(winrt::Windows::Storage::StorageFile{nullptr});
    const auto missingVideo{llvc::buildExportPreflightState(project, true, true, true, false, true)};
    expect(!missingVideo.canExport, "buildExportPreflightState should block when no video is loaded");

    const auto tempPath{filesystem::temp_directory_path() / L"llvc-tests-export-preflight.tmp"};
    {
        ofstream tempFile(tempPath, ios::binary | ios::trunc);
        tempFile << "x";
    }
    project.videoFile(winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(tempPath.wstring()).get());
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

struct NamedTest{
    const char* name;
    void (*fn)();
};

}

int wmain(){
    const vector<NamedTest> tests{
        {"UtilsParsing", &testUtilsParsing},
        {"UtilsWorkflowPolicies", &testUtilsWorkflowPolicies},
        {"TimelineIntervalNormalization", &testTimelineIntervalNormalization},
        {"TimelineMath", &testTimelineMath},
        {"TimelineDerivedHelpers", &testTimelineDerivedHelpers},
        {"TimelineTickDensityAndLabels", &testTimelineTickDensityAndLabels},
        {"TimelineZoomAnchorsAndThumbnailPlanning", &testTimelineZoomAnchorsAndThumbnailPlanning},
        {"ProjectCutRangesAndBoundaries", &testProjectCutRangesAndBoundaries},
        {"ProjectNoCutPlanAndEmptyAlignment", &testProjectNoCutPlanAndEmptyAlignment},
        {"ProjectEffectiveExportAlignment", &testProjectEffectiveExportAlignment},
        {"ProjectTailCutPreservesLastMarker", &testProjectTailCutPreservesLastMarker},
        {"ProjectMarkerToggleAndSceneRemap", &testProjectMarkerToggleAndSceneRemap},
        {"ProjectAudioNormalization", &testProjectAudioNormalization},
        {"ProjectRapMarkersSelectionNormalization", &testProjectRapMarkersSelectionNormalization},
        {"ProjectEffectiveExportProgressAndStablePlan", &testProjectEffectiveExportProgressAndStablePlan},
        {"ProjectEffectiveExportMergesAlignedBlocks", &testProjectEffectiveExportMergesAlignedBlocks},
        {"ProjectOutputDurationTracksMergedRanges", &testProjectOutputDurationTracksMergedRanges},
        {"ProjectEffectiveExportPreservesAlreadySafeTailCut", &testProjectEffectiveExportPreservesAlreadySafeTailCut},
        {"ProjectEstimateCoordinatorHelpers", &testProjectEstimateCoordinatorHelpers},
        {"ExportPreflightAndPlanSummary", &testExportPreflightAndPlanSummary},
        {"EditorHistoryUndoRedo", &testEditorHistoryUndoRedo},
        {"EditorHistorySnapshotGuards", &testEditorHistorySnapshotGuards},
        {"EditorCommands", &testEditorCommands},
        {"EditorCommandsNoOpCases", &testEditorCommandsNoOpCases},
        {"EditorReevaluationAddsBracketMarkersAndAdjustsScenes", &testEditorReevaluationAddsBracketMarkersAndAdjustsScenes},
        {"EditorReevaluationPreservesTailCutBoundary", &testEditorReevaluationPreservesTailCutBoundary},
        {"EvaluatePlacedMarkerAffectsOnlyNewMarker", &testEvaluatePlacedMarkerAffectsOnlyNewMarker},
        {"EvaluatePlacedMarkerPreservesExistingCutRangesWhenInsertedIntoKeptScene", &testEvaluatePlacedMarkerPreservesExistingCutRangesWhenInsertedIntoKeptScene},
        {"EditorReevaluationOnlyShrinksExistingCutScenes", &testEditorReevaluationOnlyShrinksExistingCutScenes},
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
