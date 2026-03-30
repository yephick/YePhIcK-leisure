#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

import std;
import llvc.Utils;
import llvc.Project;
import llvc.Timeline;
import llvc.EditorController;

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

void testProjectCutRangesAndBoundaries(){
    llvc::Project project{};
    project.timelineDuration100ns(1'000);
    project.frameIndex({marker(100), marker(300), marker(600)});
    project.refreshSelectedMarkers();
    project.cutScenes({1, 2});

    expectEqual(project.buildSceneBoundaries100ns(), vector<int64_t>{0, 100, 300, 600, 1'000}, "buildSceneBoundaries100ns should include edges and sorted markers");
    expectEqual(project.buildCutRanges100ns(), vector<pair<int64_t, int64_t>>{{100, 600}}, "buildCutRanges100ns should merge adjacent cut scenes");
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

void testProjectAudioNormalization(){
    llvc::Project project{};
    project.audioXfadeMs(620);
    expectEqual(project.audioXfadeMs(), int32_t{500}, "audioXfadeMs should snap to the nearest preset");

    project.audioVolumePct(203);
    expectEqual(project.audioVolumePct(), int32_t{200}, "audioVolumePct should clamp to 5 percent steps");
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
        {"ProjectCutRangesAndBoundaries", &testProjectCutRangesAndBoundaries},
        {"ProjectEffectiveExportAlignment", &testProjectEffectiveExportAlignment},
        {"ProjectTailCutPreservesLastMarker", &testProjectTailCutPreservesLastMarker},
        {"ProjectAudioNormalization", &testProjectAudioNormalization},
        {"EditorHistoryUndoRedo", &testEditorHistoryUndoRedo},
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
