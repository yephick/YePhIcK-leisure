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

struct NamedTest{
    const char* name;
    void (*fn)();
};

}

int wmain(){
    const vector<NamedTest> tests{
        {"UtilsParsing", &testUtilsParsing},
        {"TimelineIntervalNormalization", &testTimelineIntervalNormalization},
        {"TimelineMath", &testTimelineMath},
        {"TimelineDerivedHelpers", &testTimelineDerivedHelpers},
        {"TimelineTickDensityAndLabels", &testTimelineTickDensityAndLabels},
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
        {"EditorHistoryUndoRedo", &testEditorHistoryUndoRedo},
        {"EditorHistorySnapshotGuards", &testEditorHistorySnapshotGuards},
        {"EditorCommands", &testEditorCommands},
        {"EditorCommandsNoOpCases", &testEditorCommandsNoOpCases},
        {"EditorReevaluationAddsBracketMarkersAndAdjustsScenes", &testEditorReevaluationAddsBracketMarkersAndAdjustsScenes},
        {"EditorReevaluationPreservesTailCutBoundary", &testEditorReevaluationPreservesTailCutBoundary},
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
