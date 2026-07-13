module;

export module llvc.EditorController;

import std;
import llvc.CutPlanner;
import llvc.Project;
import llvc.Timeline;

export namespace llvc{

using namespace ::std;

struct EditorSnapshot final{
    vector<EditMarker> plannerMarkers{};
    vector<FrameIndex> plannerRapFrames{};
    vector<uint32_t> plannerCutScenes{};
    bool keepAudio{true};
    int32_t audioCrossfadeMs{0};
    int32_t audioVolumePct{100};
};

struct EditorHistoryState final{
    vector<EditorSnapshot> undoStack{};
    vector<EditorSnapshot> redoStack{};
    bool isApplying{false};
};

struct UndoRedoResult final{
    bool changed{false};
    bool wasUndo{false};
};

struct MarkerToggleResult final{
    bool changed{false};
    bool markerCountIncreased{false};
};

struct ReevaluateMarkersResult final{
    bool changed{false};
    bool hadMarkers{false};
    bool undoSnapshotCreated{false};
    uint32_t replacedCount{0};
};

struct EvaluatePlacedMarkerResult final{
    bool changed{false};
    bool markerEvaluated{false};
    uint32_t addedRapMarkerCount{0};
};

EditorSnapshot captureEditorSnapshot(const Project& project);
bool isSameEditorSnapshot(const EditorSnapshot& left, const EditorSnapshot& right);
void clearEditorHistory(EditorHistoryState& history);
bool pushUndoSnapshotIfChanged(const Project& project, EditorHistoryState& history);
bool applyEditorSnapshot(Project& project, EditorHistoryState& history, const EditorSnapshot& snapshot);
UndoRedoResult undo(Project& project, EditorHistoryState& history);
UndoRedoResult redo(Project& project, EditorHistoryState& history);
MarkerToggleResult toggleSelectedKeyframe(Project& project, double fps, EditorHistoryState& history, int64_t time100ns);
bool toggleCutBlock(Project& project, EditorHistoryState& history, int64_t time100ns);
bool setCutBlock(Project& project, EditorHistoryState& history, int64_t time100ns, bool cutScene);
ReevaluateMarkersResult reevaluateClearCutMarkers(Project& project, const Timeline& timeline, const vector<int64_t>& rapTimes100ns, EditorHistoryState& history, bool pushUndoState);
ReevaluateMarkersResult reevaluateClearCutMarkers(Project& project, FrameIndex sourceFrameCount, EditorHistoryState& history, bool pushUndoState);
EvaluatePlacedMarkerResult evaluatePlacedMarkerFrameAgainstRap(Project& project, const vector<FrameIndex>& rapFrames, FrameIndex markerFrame);
EvaluatePlacedMarkerResult evaluatePlacedMarkerAgainstRap(Project& project, const vector<int64_t>& rapTimes100ns, int64_t markerTime100ns);

}

namespace llvc{

EditorSnapshot captureEditorSnapshot(const Project& project){
    const auto& planner{project.cutPlanner()};
    return EditorSnapshot{
        .plannerMarkers = {planner.markers().markers().begin(), planner.markers().markers().end()},
        .plannerRapFrames = {planner.rapFrames().frames().begin(), planner.rapFrames().frames().end()},
        .plannerCutScenes = {planner.cutScenes().cutSceneIndexes().begin(), planner.cutScenes().cutSceneIndexes().end()},
        .keepAudio = project.keepAudio(),
        .audioCrossfadeMs = project.audioXfadeMs(),
        .audioVolumePct = project.audioVolumePct(),
    };
}

bool isSameEditorSnapshot(const EditorSnapshot& left, const EditorSnapshot& right){
    if(left.keepAudio != right.keepAudio
        || left.audioCrossfadeMs != right.audioCrossfadeMs
        || left.audioVolumePct != right.audioVolumePct
        || left.plannerMarkers != right.plannerMarkers
        || left.plannerRapFrames != right.plannerRapFrames
        || left.plannerCutScenes != right.plannerCutScenes){
        return false;
    }
    return true;
}

void clearEditorHistory(EditorHistoryState& history){
    history.undoStack.clear();
    history.redoStack.clear();
}

bool pushUndoSnapshotIfChanged(const Project& project, EditorHistoryState& history){
    if(history.isApplying){
        return false;
    }

    const auto current{captureEditorSnapshot(project)};
    if(!history.undoStack.empty() && isSameEditorSnapshot(history.undoStack.back(), current)){
        return false;
    }

    history.undoStack.push_back(current);
    history.redoStack.clear();
    return true;
}

bool applyEditorSnapshot(Project& project, EditorHistoryState& history, const EditorSnapshot& snapshot){
    const auto current{captureEditorSnapshot(project)};
    if(isSameEditorSnapshot(current, snapshot)){
        return false;
    }

    history.isApplying = true;
    project.cutPlanner().markers().replaceAll(snapshot.plannerMarkers);
    project.cutPlanner().rapFrames().replaceAll(snapshot.plannerRapFrames);
    project.cutPlanner().cutScenes().replaceCutSceneIndexes(snapshot.plannerCutScenes);
    if(!project.hasCanonicalFrameModel()){
        project.synchronizeLegacyEditModelFromCutPlanner(static_cast<FrameIndex>(project.timelineDuration100ns()));
    }
    project.keepAudio(snapshot.keepAudio);
    project.audioXfadeMs(snapshot.audioCrossfadeMs);
    project.audioVolumePct(snapshot.audioVolumePct);
    history.isApplying = false;
    return true;
}

UndoRedoResult undo(Project& project, EditorHistoryState& history){
    if(history.undoStack.empty()){
        return {};
    }

    const auto previous{history.undoStack.back()};
    history.undoStack.pop_back();
    history.redoStack.push_back(captureEditorSnapshot(project));
    return UndoRedoResult{
        .changed = applyEditorSnapshot(project, history, previous),
        .wasUndo = true,
    };
}

UndoRedoResult redo(Project& project, EditorHistoryState& history){
    if(history.redoStack.empty()){
        return {};
    }

    const auto next{history.redoStack.back()};
    history.redoStack.pop_back();
    history.undoStack.push_back(captureEditorSnapshot(project));
    return UndoRedoResult{
        .changed = applyEditorSnapshot(project, history, next),
        .wasUndo = false,
    };
}

MarkerToggleResult toggleSelectedKeyframe(Project& project, double fps, EditorHistoryState& history, int64_t time100ns){
    const auto markerCountBefore{project.frameIndex().size()};
    (void)pushUndoSnapshotIfChanged(project, history);
    if(!project.toggleSelectedKeyframeAtTime100ns(time100ns, fps)){
        return {};
    }

    const auto markerCountAfter{project.frameIndex().size()};
    return MarkerToggleResult{
        .changed = true,
        .markerCountIncreased = markerCountAfter > markerCountBefore,
    };
}

bool toggleCutBlock(Project& project, EditorHistoryState& history, int64_t time100ns){
    (void)pushUndoSnapshotIfChanged(project, history);
    return project.toggleCutBlockAtTime100ns(time100ns);
}

bool setCutBlock(Project& project, EditorHistoryState& history, int64_t time100ns, bool cutScene){
    (void)pushUndoSnapshotIfChanged(project, history);
    return project.setCutBlockAtTime100ns(time100ns, cutScene);
}

ReevaluateMarkersResult reevaluateClearCutMarkers(Project& project, FrameIndex sourceFrameCount, EditorHistoryState& history, bool pushUndoState){
    ReevaluateMarkersResult result{};
    const auto beforeState{captureEditorSnapshot(project)};
    const auto plannerResult{project.cutPlanner().reevaluateCutScenesAgainstRap(sourceFrameCount)};
    result.hadMarkers = plannerResult.hadMarkers;
    result.replacedCount = plannerResult.replacedCount;
    if(!result.hadMarkers){
        return result;
    }

    if(plannerResult.changed && pushUndoState){
        history.undoStack.push_back(beforeState);
        history.redoStack.clear();
        result.undoSnapshotCreated = true;
    }
    result.changed = plannerResult.changed;
    if(result.changed){
        project.markDirty();
    }
    return result;
}

ReevaluateMarkersResult reevaluateClearCutMarkers(Project& project, const Timeline& timeline, const vector<int64_t>& rapTimes100ns, EditorHistoryState& history, bool pushUndoState){
    ReevaluateMarkersResult result{};
    const auto originalMarkers{project.frameIndex()};
    result.hadMarkers = !originalMarkers.empty();
    if(!result.hadMarkers){
        return result;
    }

    const auto previousCutRanges{project.buildCutRanges100ns()};
    const auto beforeState{captureEditorSnapshot(project)};
    result.undoSnapshotCreated = pushUndoState ? pushUndoSnapshotIfChanged(project, history) : false;

    vector<IndexedFrameSample> updatedMarkers;
    updatedMarkers.reserve(originalMarkers.size() * 2);
    std::optional<int64_t> preservedTailCutStartTime100ns;
    if(!previousCutRanges.empty()){
        const auto totalDuration100ns{project.timelineDuration100ns()};
        const auto& tailCutRange{previousCutRanges.back()};
        if(tailCutRange.second >= totalDuration100ns && any_of(originalMarkers.begin(), originalMarkers.end(), [&](const auto& marker){ return marker.time100ns == tailCutRange.first; })){
            preservedTailCutStartTime100ns = tailCutRange.first;
        }
    }
    vector<pair<int64_t, int64_t>> alignedCutRanges;
    alignedCutRanges.reserve(previousCutRanges.size());
    const auto totalDuration100ns{project.timelineDuration100ns()};
    for(const auto& [start, end]: previousCutRanges){
        int64_t alignedStart{start};
        int64_t alignedEnd{end};

        const auto preserveTailStart{preservedTailCutStartTime100ns && start == *preservedTailCutStartTime100ns && end >= totalDuration100ns};
        if(start > 0 && !preserveTailStart){
            const auto startRapIt{lower_bound(rapTimes100ns.begin(), rapTimes100ns.end(), start)};
            if(startRapIt == rapTimes100ns.end()){
                continue;
            }
            alignedStart = *startRapIt;
        }

        if(end < totalDuration100ns){
            const auto endRapIt{upper_bound(rapTimes100ns.begin(), rapTimes100ns.end(), end)};
            if(endRapIt == rapTimes100ns.begin()){
                continue;
            }
            alignedEnd = *(endRapIt - 1);
        }

        if(alignedEnd > alignedStart){
            alignedCutRanges.emplace_back(alignedStart, alignedEnd);
        }
    }

    const auto addGreenMarker = [&updatedMarkers](int64_t time100ns){
        updatedMarkers.push_back(IndexedFrameSample{
            .time100ns = time100ns,
            .duration100ns = 0,
            .cleanPoint = true,
            .evaluated = true,
            .sampleIndex = 0,
        });
    };

    const auto addBracketRapMarkersAround = [&](int64_t time100ns){
        const auto nextIt{lower_bound(rapTimes100ns.begin(), rapTimes100ns.end(), time100ns)};
        if(nextIt != rapTimes100ns.begin()){
            const auto previousRapTime100ns{*(nextIt - 1)};
            if(previousRapTime100ns > 0){
                addGreenMarker(previousRapTime100ns);
            }
        }
        if(nextIt != rapTimes100ns.end() && *nextIt != time100ns && *nextIt < totalDuration100ns){
            addGreenMarker(*nextIt);
        }
    };

    for(auto marker: originalMarkers){
        const auto isTrueRap{binary_search(rapTimes100ns.begin(), rapTimes100ns.end(), marker.time100ns)};
        if(isTrueRap){
            marker.cleanPoint = true;
            marker.evaluated = true;
        }else{
            marker.cleanPoint = false;
            marker.evaluated = false;
            ++result.replacedCount;
        }
        updatedMarkers.push_back(marker);

        if(!isTrueRap){
            addBracketRapMarkersAround(marker.time100ns);
        }
    }

    for(const auto& [start, end]: alignedCutRanges){
        if(start > 0){
            addGreenMarker(start);
        }
        if(end > 0 && end < totalDuration100ns){
            addGreenMarker(end);
        }
    }

    sort(updatedMarkers.begin(), updatedMarkers.end(), [](const auto& a, const auto& b){ return a.time100ns < b.time100ns; });
    updatedMarkers.erase(unique(updatedMarkers.begin(), updatedMarkers.end(), [](const auto& a, const auto& b){ return a.time100ns == b.time100ns; }), updatedMarkers.end());

    project.frameIndex(std::move(updatedMarkers));
    project.refreshSelectedMarkers();

    const auto sceneBoundaries{project.buildSceneBoundaries100ns()};
    vector<uint32_t> scenes;
    for(size_t i{}; i + 1 < sceneBoundaries.size(); ++i){
        const auto midpoint{sceneBoundaries[i] + (sceneBoundaries[i + 1] - sceneBoundaries[i]) / 2};
        if(timeline.isTimeInsideRanges(midpoint, alignedCutRanges)){
            scenes.push_back(static_cast<uint32_t>(i));
        }
    }
    project.cutScenes(std::move(scenes));

    result.changed = !isSameEditorSnapshot(beforeState, captureEditorSnapshot(project));
    if(!result.changed && result.undoSnapshotCreated && !history.undoStack.empty()){
        history.undoStack.pop_back();
        result.undoSnapshotCreated = false;
    }

    return result;
}

EvaluatePlacedMarkerResult evaluatePlacedMarkerFrameAgainstRap(Project& project, const vector<FrameIndex>& rapFrames, FrameIndex markerFrame){
    EvaluatePlacedMarkerResult result{};
    auto& planner{project.cutPlanner()};
    if(!planner.markers().contains(markerFrame)){
        return result;
    }
    planner.rapFrames().replaceAll(rapFrames);

    auto markers{vector<EditMarker>{planner.markers().markers().begin(), planner.markers().markers().end()}};
    const auto markerIt{find_if(markers.begin(), markers.end(), [markerFrame](const auto& marker){ return marker.frameIndex == markerFrame; })};
    if(markerIt == markers.end()){
        return result;
    }

    const auto wasEvaluated{markerIt->isEvaluatedAgainstRap};
    markerIt->isEvaluatedAgainstRap = planner.rapFrames().contains(markerFrame);
    result.markerEvaluated = markerIt->isEvaluatedAgainstRap;
    planner.markers().replaceAll(move(markers));

    const auto markerCountBefore{planner.markers().size()};
    if(!result.markerEvaluated){
        if(const auto previous{planner.rapFrames().previousRapAtOrBefore(markerFrame)}; previous && *previous < markerFrame){
            (void)planner.addMarker(*previous);
        }
        if(const auto next{planner.rapFrames().nextRapAtOrAfter(markerFrame)}; next && *next > markerFrame){
            (void)planner.addMarker(*next);
        }
    }
    result.addedRapMarkerCount = static_cast<uint32_t>(planner.markers().size() - markerCountBefore);
    if(result.addedRapMarkerCount != 0){
        auto normalizedMarkers{vector<EditMarker>{planner.markers().markers().begin(), planner.markers().markers().end()}};
        for(auto& marker: normalizedMarkers){
            if(marker.frameIndex != markerFrame && planner.rapFrames().contains(marker.frameIndex)){
                marker.isEvaluatedAgainstRap = true;
                marker.wasMovedForRapSafety = true;
            }
        }
        planner.markers().replaceAll(move(normalizedMarkers));
    }
    result.changed = wasEvaluated != result.markerEvaluated || result.addedRapMarkerCount != 0;
    if(result.changed){ project.markDirty(); }
    return result;
}

EvaluatePlacedMarkerResult evaluatePlacedMarkerAgainstRap(Project& project, const vector<int64_t>& rapTimes100ns, int64_t markerTime100ns){
    EvaluatePlacedMarkerResult result{};
    if(project.hasCanonicalFrameModel()){
        vector<FrameIndex> rapFrames;
        rapFrames.reserve(rapTimes100ns.size());
        for(const auto rapTime100ns: rapTimes100ns){
            rapFrames.push_back(project.frameIndexForTime100ns(rapTime100ns));
        }
        return evaluatePlacedMarkerFrameAgainstRap(project, rapFrames, project.frameIndexForTime100ns(markerTime100ns));
    }
    auto markers{project.frameIndex()};
    if(markers.empty()){
        return result;
    }

    const auto markerIt{find_if(markers.begin(), markers.end(), [markerTime100ns](const auto& marker){
        return marker.time100ns == markerTime100ns;
    })};
    if(markerIt == markers.end()){
        return result;
    }

    const auto beforeMarkers{markers};
    const auto previousCutRanges{project.buildCutRanges100ns()};
    const auto isTrueRap{binary_search(rapTimes100ns.begin(), rapTimes100ns.end(), markerTime100ns)};
    if(isTrueRap){
        markerIt->cleanPoint = true;
        markerIt->evaluated = true;
        result.markerEvaluated = true;
    }else{
        markerIt->cleanPoint = false;
        markerIt->evaluated = false;

        const auto totalDuration100ns{project.timelineDuration100ns()};
        const auto nextIt{lower_bound(rapTimes100ns.begin(), rapTimes100ns.end(), markerTime100ns)};
        if(nextIt != rapTimes100ns.begin()){
            const auto previousRapTime100ns{*(nextIt - 1)};
            if(previousRapTime100ns > 0){
                markers.push_back(IndexedFrameSample{
                    .time100ns = previousRapTime100ns,
                    .duration100ns = 0,
                    .cleanPoint = true,
                    .evaluated = true,
                    .sampleIndex = 0,
                });
            }
        }
        if(nextIt != rapTimes100ns.end() && *nextIt < totalDuration100ns){
            markers.push_back(IndexedFrameSample{
                .time100ns = *nextIt,
                .duration100ns = 0,
                .cleanPoint = true,
                .evaluated = true,
                .sampleIndex = 0,
            });
        }
    }

    sort(markers.begin(), markers.end(), [](const auto& a, const auto& b){ return a.time100ns < b.time100ns; });

    vector<IndexedFrameSample> normalizedMarkers;
    normalizedMarkers.reserve(markers.size());
    for(const auto& marker: markers){
        if(!normalizedMarkers.empty() && normalizedMarkers.back().time100ns == marker.time100ns){
            normalizedMarkers.back().cleanPoint = normalizedMarkers.back().cleanPoint || marker.cleanPoint;
            normalizedMarkers.back().evaluated = normalizedMarkers.back().evaluated || marker.evaluated;
            normalizedMarkers.back().duration100ns = max(normalizedMarkers.back().duration100ns, marker.duration100ns);
            normalizedMarkers.back().sampleIndex = min(normalizedMarkers.back().sampleIndex, marker.sampleIndex);
            continue;
        }
        normalizedMarkers.push_back(marker);
    }

    result.addedRapMarkerCount = normalizedMarkers.size() > beforeMarkers.size()
        ? static_cast<uint32_t>(normalizedMarkers.size() - beforeMarkers.size())
        : 0;
    result.changed = !equal(
        beforeMarkers.begin(),
        beforeMarkers.end(),
        normalizedMarkers.begin(),
        normalizedMarkers.begin() + min(beforeMarkers.size(), normalizedMarkers.size()),
        [](const auto& left, const auto& right){
            return left.time100ns == right.time100ns
                && left.duration100ns == right.duration100ns
                && left.cleanPoint == right.cleanPoint
                && left.evaluated == right.evaluated
                && left.sampleIndex == right.sampleIndex;
        }) || beforeMarkers.size() != normalizedMarkers.size();

    if(!result.changed){
        return result;
    }

    project.frameIndex(std::move(normalizedMarkers));
    project.refreshSelectedMarkers();

    const auto isTimeInsideRanges = [&previousCutRanges](int64_t time100ns){
        for(const auto& [start100ns, end100ns]: previousCutRanges){
            if(time100ns < start100ns){
                return false;
            }
            if(time100ns < end100ns){
                return true;
            }
        }
        return false;
    };

    const auto sceneBoundaries{project.buildSceneBoundaries100ns()};
    vector<uint32_t> scenes;
    for(size_t i{}; i + 1 < sceneBoundaries.size(); ++i){
        const auto midpoint{sceneBoundaries[i] + (sceneBoundaries[i + 1] - sceneBoundaries[i]) / 2};
        if(isTimeInsideRanges(midpoint)){
            scenes.push_back(static_cast<uint32_t>(i));
        }
    }
    project.cutScenes(std::move(scenes));
    return result;
}

}
