module;

export module llvc.EditorController;

import std;
import llvc.Project;
import llvc.Timeline;

export namespace llvc{

using namespace ::std;

struct EditorSnapshot final{
    vector<IndexedFrameSample> frameIndex{};
    vector<uint32_t> cutScenes{};
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

}

namespace llvc{

EditorSnapshot captureEditorSnapshot(const Project& project){
    return EditorSnapshot{
        .frameIndex = project.frameIndex(),
        .cutScenes = project.cutScenes(),
        .keepAudio = project.keepAudio(),
        .audioCrossfadeMs = project.audioXfadeMs(),
        .audioVolumePct = project.audioVolumePct(),
    };
}

bool isSameEditorSnapshot(const EditorSnapshot& left, const EditorSnapshot& right){
    if(left.keepAudio != right.keepAudio
        || left.audioCrossfadeMs != right.audioCrossfadeMs
        || left.audioVolumePct != right.audioVolumePct
        || left.cutScenes != right.cutScenes
        || left.frameIndex.size() != right.frameIndex.size()){
        return false;
    }

    for(size_t i{}; i < left.frameIndex.size(); ++i){
        const auto& a{left.frameIndex[i]};
        const auto& b{right.frameIndex[i]};
        if(a.time100ns != b.time100ns || a.duration100ns != b.duration100ns || a.cleanPoint != b.cleanPoint || a.sampleIndex != b.sampleIndex){
            return false;
        }
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
    project.frameIndex(snapshot.frameIndex);
    project.refreshSelectedMarkers();
    project.cutScenes(snapshot.cutScenes);
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
    optional<int64_t> preservedTailCutStartTime100ns;
    if(!previousCutRanges.empty()){
        const auto totalDuration100ns{project.timelineDuration100ns()};
        const auto& tailCutRange{previousCutRanges.back()};
        if(tailCutRange.second >= totalDuration100ns && !originalMarkers.empty()){
            const auto lastMarkerTime100ns{originalMarkers.back().time100ns};
            if(lastMarkerTime100ns == tailCutRange.first){
                preservedTailCutStartTime100ns = lastMarkerTime100ns;
            }
        }
    }

    for(const auto& marker: originalMarkers){
        if(preservedTailCutStartTime100ns && marker.time100ns == *preservedTailCutStartTime100ns){
            updatedMarkers.push_back(marker);
            continue;
        }

        if(binary_search(rapTimes100ns.begin(), rapTimes100ns.end(), marker.time100ns)){
            updatedMarkers.push_back(marker);
            continue;
        }

        ++result.replacedCount;
        const auto nextIt{lower_bound(rapTimes100ns.begin(), rapTimes100ns.end(), marker.time100ns)};
        if(nextIt != rapTimes100ns.begin()){
            updatedMarkers.push_back(IndexedFrameSample{.time100ns = *(nextIt - 1), .duration100ns = 0, .cleanPoint = true, .sampleIndex = 0});
        }
        if(nextIt != rapTimes100ns.end()){
            updatedMarkers.push_back(IndexedFrameSample{.time100ns = *nextIt, .duration100ns = 0, .cleanPoint = true, .sampleIndex = 0});
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
        if(timeline.isTimeInsideRanges(midpoint, previousCutRanges)){
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

}
