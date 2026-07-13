export module llvc.EditorCommands;

import std;
import llvc.CutPlanner;
import llvc.Project;
import llvc.Timeline;
import llvc.EditorController;

export namespace llvc{

using namespace ::std;

struct EditorCommandResult final{
    bool changed{false};
    bool markerCountIncreased{false};
    bool refreshTimelineTicks{false};
    bool refreshKeyframeTicks{false};
    bool refreshCutOverlays{false};
    bool refreshVideoDetails{false};
    bool refreshWindowTitle{false};
};

EditorCommandResult executeToggleMarkerCommand(Project& project, double fps, EditorHistoryState& history, int64_t time100ns);
EditorCommandResult executeToggleCutBlockCommand(Project& project, EditorHistoryState& history, int64_t time100ns);
EditorCommandResult executeSetCutBlockCommand(Project& project, EditorHistoryState& history, int64_t time100ns, bool cutScene);
EditorCommandResult executeToggleMarkerFrameCommand(Project& project, EditorHistoryState& history, FrameIndex frameIndex);
EditorCommandResult executeToggleCutSceneFrameCommand(Project& project, EditorHistoryState& history, FrameIndex frameIndex, FrameIndex sourceFrameCount);
EditorCommandResult executeSetCutSceneFrameCommand(Project& project, EditorHistoryState& history, FrameIndex frameIndex, bool cutScene, FrameIndex sourceFrameCount);
EditorCommandResult executeRapNudgeCommand(Project& project, const Timeline& timeline, const vector<int64_t>& rapTimes100ns, EditorHistoryState& history, int64_t cursorTime100ns, bool expandScene);

}

namespace llvc{

namespace{

EditorCommandResult makeTimelineRefreshResult(bool changed){
    return EditorCommandResult{
        .changed = changed,
        .refreshTimelineTicks = changed,
        .refreshKeyframeTicks = changed,
        .refreshCutOverlays = changed,
        .refreshVideoDetails = changed,
        .refreshWindowTitle = changed,
    };
}

}

EditorCommandResult executeToggleMarkerCommand(Project& project, double fps, EditorHistoryState& history, int64_t time100ns){
    const auto result{toggleSelectedKeyframe(project, fps, history, time100ns)};
    auto commandResult{makeTimelineRefreshResult(result.changed)};
    commandResult.markerCountIncreased = result.markerCountIncreased;
    return commandResult;
}

EditorCommandResult executeToggleCutBlockCommand(Project& project, EditorHistoryState& history, int64_t time100ns){
    return makeTimelineRefreshResult(toggleCutBlock(project, history, time100ns));
}

EditorCommandResult executeSetCutBlockCommand(Project& project, EditorHistoryState& history, int64_t time100ns, bool cutScene){
    return makeTimelineRefreshResult(setCutBlock(project, history, time100ns, cutScene));
}

EditorCommandResult executeToggleMarkerFrameCommand(Project& project, EditorHistoryState& history, FrameIndex frameIndex){
    const auto markerCountBefore{project.cutPlanner().markers().size()};
    (void)pushUndoSnapshotIfChanged(project, history);
    const auto changed{project.cutPlanner().toggleMarker(frameIndex)};
    if(changed){
        project.markDirty();
    }

    auto result{makeTimelineRefreshResult(changed)};
    result.markerCountIncreased = project.cutPlanner().markers().size() > markerCountBefore;
    return result;
}

EditorCommandResult executeToggleCutSceneFrameCommand(Project& project, EditorHistoryState& history, FrameIndex frameIndex, FrameIndex sourceFrameCount){
    (void)pushUndoSnapshotIfChanged(project, history);
    const auto changed{project.cutPlanner().toggleCutSceneContainingFrame(frameIndex, sourceFrameCount)};
    if(changed){
        project.markDirty();
    }
    return makeTimelineRefreshResult(changed);
}

EditorCommandResult executeSetCutSceneFrameCommand(Project& project, EditorHistoryState& history, FrameIndex frameIndex, bool cutScene, FrameIndex sourceFrameCount){
    (void)pushUndoSnapshotIfChanged(project, history);
    const auto changed{project.cutPlanner().setCutSceneContainingFrame(frameIndex, cutScene, sourceFrameCount)};
    if(changed){
        project.markDirty();
    }
    return makeTimelineRefreshResult(changed);
}

EditorCommandResult executeRapNudgeCommand(Project& project, const Timeline& timeline, const vector<int64_t>& rapTimes100ns, EditorHistoryState& history, int64_t cursorTime100ns, bool expandScene){
    const auto boundaries{project.buildSceneBoundaries100ns()};
    if(boundaries.size() < 2){
        return {};
    }

    auto sceneIndex{boundaries.size() - 2};
    for(size_t i{}; i + 1 < boundaries.size(); ++i){
        if(cursorTime100ns < boundaries[i + 1]){
            sceneIndex = i;
            break;
        }
    }

    auto markers{project.frameIndex()};
    if(markers.empty()){
        return {};
    }

    const auto previousCutRanges{project.buildCutRanges100ns()};

    const auto moveBoundaryToDirectionalRap = [&](size_t boundaryIndex, bool moveTowardEarlier, int64_t minExclusive, int64_t maxExclusive) -> bool{
        if(boundaryIndex == 0 || boundaryIndex >= (boundaries.size() - 1)){
            return false;
        }

        const auto markerIndex{boundaryIndex - 1};
        if(markerIndex >= markers.size()){
            return false;
        }

        const auto currentBoundaryTime{boundaries[boundaryIndex]};
        if(binary_search(rapTimes100ns.begin(), rapTimes100ns.end(), currentBoundaryTime)){
            return false;
        }

        int64_t replacementBoundaryTime{};
        if(moveTowardEarlier){
            const auto it{lower_bound(rapTimes100ns.begin(), rapTimes100ns.end(), currentBoundaryTime)};
            if(it == rapTimes100ns.begin()){
                return false;
            }
            replacementBoundaryTime = *(it - 1);
        }else{
            const auto it{upper_bound(rapTimes100ns.begin(), rapTimes100ns.end(), currentBoundaryTime)};
            if(it == rapTimes100ns.end()){
                return false;
            }
            replacementBoundaryTime = *it;
        }

        if(replacementBoundaryTime <= minExclusive || replacementBoundaryTime >= maxExclusive){
            return false;
        }

        markers[markerIndex].time100ns = replacementBoundaryTime;
        markers[markerIndex].cleanPoint = true;
        markers[markerIndex].evaluated = true;
        return true;
    };

    const auto sceneCount{boundaries.size() - 1};
    vector<bool> isCut(sceneCount, false);
    for(const auto cutSceneIndex: project.cutScenes()){
        if(cutSceneIndex < sceneCount){
            isCut[cutSceneIndex] = true;
        }
    }

    const auto targetCutState{sceneIndex < sceneCount ? isCut[sceneIndex] : false};
    auto blockStartSceneIndex{sceneIndex};
    auto blockEndSceneIndex{sceneIndex};

    while(blockStartSceneIndex > 0 && isCut[blockStartSceneIndex - 1] == targetCutState){
        --blockStartSceneIndex;
    }
    while((blockEndSceneIndex + 1) < sceneCount && isCut[blockEndSceneIndex + 1] == targetCutState){
        ++blockEndSceneIndex;
    }

    const auto leftBoundaryIndex{blockStartSceneIndex};
    const auto rightBoundaryIndex{blockEndSceneIndex + 1};

    auto changed{false};
    constexpr auto noMinBound{numeric_limits<int64_t>::lowest()};
    constexpr auto noMaxBound{numeric_limits<int64_t>::max()};

    if(expandScene){
        if(leftBoundaryIndex > 0){
            changed = moveBoundaryToDirectionalRap(leftBoundaryIndex, true, noMinBound, boundaries[rightBoundaryIndex]) || changed;
        }
        if(rightBoundaryIndex + 1 < boundaries.size()){
            const auto leftTimeAfterMove{leftBoundaryIndex > 0 ? markers[leftBoundaryIndex - 1].time100ns : boundaries[leftBoundaryIndex]};
            changed = moveBoundaryToDirectionalRap(rightBoundaryIndex, false, leftTimeAfterMove, noMaxBound) || changed;
        }
    }else{
        if(rightBoundaryIndex + 1 < boundaries.size()){
            changed = moveBoundaryToDirectionalRap(rightBoundaryIndex, true, boundaries[leftBoundaryIndex], noMaxBound) || changed;
        }
        if(leftBoundaryIndex > 0){
            const auto rightTimeAfterMove{rightBoundaryIndex + 1 < boundaries.size() ? markers[rightBoundaryIndex - 1].time100ns : boundaries[rightBoundaryIndex]};
            changed = moveBoundaryToDirectionalRap(leftBoundaryIndex, false, noMinBound, rightTimeAfterMove) || changed;
        }
    }

    if(!changed){
        return {};
    }

    (void)pushUndoSnapshotIfChanged(project, history);
    sort(markers.begin(), markers.end(), [](const auto& a, const auto& b){ return a.time100ns < b.time100ns; });
    markers.erase(unique(markers.begin(), markers.end(), [](const auto& a, const auto& b){ return a.time100ns == b.time100ns; }), markers.end());
    project.frameIndex(std::move(markers));
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

    return makeTimelineRefreshResult(true);
}

}
