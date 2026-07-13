export module llvc.CutPlanner;

import std;

export namespace llvc{

using namespace ::std;

using FrameIndex = uint64_t;

struct EditMarker final{
    FrameIndex frameIndex{};
    bool isEvaluatedAgainstRap{};
    bool wasMovedForRapSafety{};

    bool operator==(const EditMarker&) const = default;
};

struct ExportFrameRange final{
    FrameIndex firstFrame{};
    FrameIndex endFrameExclusive{};

    bool operator==(const ExportFrameRange&) const = default;
};

struct SceneFrameRange final{
    FrameIndex firstFrame{};
    FrameIndex endFrameExclusive{};
    uint32_t sceneIndex{};
    bool cut{};

    bool operator==(const SceneFrameRange&) const = default;
};

struct EffectiveFrameExportPlan final{
    FrameIndex sourceFrameCount{};
    vector<ExportFrameRange> requestedCutRanges{};
    vector<ExportFrameRange> effectiveCutRanges{};
    FrameIndex requestedOutputFrameCount{};
    FrameIndex effectiveOutputFrameCount{};
    bool hasRequestedCuts{};
    bool materiallyDifferent{};
    bool emptyAfterAlignment{};
};

struct PlannerReevaluateMarkersResult final{
    bool changed{};
    bool hadMarkers{};
    bool undoSnapshotCreated{};
    uint32_t replacedCount{};
};

class MarkerSet final{
public:
    void reset(){ m_markers.clear(); }
    bool empty() const{ return m_markers.empty(); }
    size_t size() const{ return m_markers.size(); }
    span<const EditMarker> markers() const{ return m_markers; }

    bool contains(FrameIndex frameIndex) const{
        return findMarker(frameIndex) != m_markers.end();
    }

    bool add(FrameIndex frameIndex){
        const auto it{lower_bound(m_markers.begin(), m_markers.end(), frameIndex, markerLessThanFrame)};
        if(it != m_markers.end() && it->frameIndex == frameIndex){
            return false;
        }
        m_markers.insert(it, EditMarker{.frameIndex = frameIndex});
        return true;
    }

    bool remove(FrameIndex frameIndex){
        const auto it{findMarker(frameIndex)};
        if(it == m_markers.end()){
            return false;
        }
        m_markers.erase(it);
        return true;
    }

    bool toggle(FrameIndex frameIndex){
        return remove(frameIndex) ? false : add(frameIndex);
    }

    optional<size_t> markerOrdinalForFrame(FrameIndex frameIndex) const{
        const auto it{findMarker(frameIndex)};
        if(it == m_markers.end()){
            return nullopt;
        }
        return static_cast<size_t>(distance(m_markers.begin(), it));
    }

    optional<FrameIndex> previousMarkerFrame(FrameIndex frameIndex) const{
        const auto it{lower_bound(m_markers.begin(), m_markers.end(), frameIndex, markerLessThanFrame)};
        if(it == m_markers.begin()){
            return nullopt;
        }
        return prev(it)->frameIndex;
    }

    optional<FrameIndex> nextMarkerFrame(FrameIndex frameIndex) const{
        const auto it{upper_bound(m_markers.begin(), m_markers.end(), frameIndex, [](FrameIndex frame, const EditMarker& marker){
            return frame < marker.frameIndex;
        })};
        if(it == m_markers.end()){
            return nullopt;
        }
        return it->frameIndex;
    }

    void replaceAll(vector<EditMarker> markers){
        sort(markers.begin(), markers.end(), [](const auto& a, const auto& b){ return a.frameIndex < b.frameIndex; });
        markers.erase(unique(markers.begin(), markers.end(), [](const auto& a, const auto& b){ return a.frameIndex == b.frameIndex; }), markers.end());
        m_markers = move(markers);
    }

private:
    static bool markerLessThanFrame(const EditMarker& marker, FrameIndex frameIndex){
        return marker.frameIndex < frameIndex;
    }

    vector<EditMarker>::const_iterator findMarker(FrameIndex frameIndex) const{
        const auto it{lower_bound(m_markers.begin(), m_markers.end(), frameIndex, markerLessThanFrame)};
        return it != m_markers.end() && it->frameIndex == frameIndex ? it : m_markers.end();
    }

    vector<EditMarker>::iterator findMarker(FrameIndex frameIndex){
        const auto it{lower_bound(m_markers.begin(), m_markers.end(), frameIndex, markerLessThanFrame)};
        return it != m_markers.end() && it->frameIndex == frameIndex ? it : m_markers.end();
    }

private:
    vector<EditMarker> m_markers;
};

class RapFrameSet final{
public:
    void reset(){ m_rapFrames.clear(); }
    bool empty() const{ return m_rapFrames.empty(); }
    size_t size() const{ return m_rapFrames.size(); }
    span<const FrameIndex> frames() const{ return m_rapFrames; }

    void replaceAll(vector<FrameIndex> rapFrames){
        sort(rapFrames.begin(), rapFrames.end());
        rapFrames.erase(unique(rapFrames.begin(), rapFrames.end()), rapFrames.end());
        m_rapFrames = move(rapFrames);
    }

    bool contains(FrameIndex frameIndex) const{
        return binary_search(m_rapFrames.begin(), m_rapFrames.end(), frameIndex);
    }

    optional<FrameIndex> previousRapAtOrBefore(FrameIndex frameIndex) const{
        const auto it{upper_bound(m_rapFrames.begin(), m_rapFrames.end(), frameIndex)};
        if(it == m_rapFrames.begin()){
            return nullopt;
        }
        return *prev(it);
    }

    optional<FrameIndex> nextRapAtOrAfter(FrameIndex frameIndex) const{
        const auto it{lower_bound(m_rapFrames.begin(), m_rapFrames.end(), frameIndex)};
        if(it == m_rapFrames.end()){
            return nullopt;
        }
        return *it;
    }

private:
    vector<FrameIndex> m_rapFrames;
};

class CutSceneSet final{
public:
    void reset(){ m_cutSceneIndexes.clear(); }
    span<const uint32_t> cutSceneIndexes() const{ return m_cutSceneIndexes; }

    bool containsSceneIndex(uint32_t sceneIndex) const{
        return binary_search(m_cutSceneIndexes.begin(), m_cutSceneIndexes.end(), sceneIndex);
    }

    bool toggleScene(uint32_t sceneIndex){
        return setScene(sceneIndex, !containsSceneIndex(sceneIndex));
    }

    bool setScene(uint32_t sceneIndex, bool cut){
        const auto it{lower_bound(m_cutSceneIndexes.begin(), m_cutSceneIndexes.end(), sceneIndex)};
        if(cut){
            if(it != m_cutSceneIndexes.end() && *it == sceneIndex){
                return false;
            }
            m_cutSceneIndexes.insert(it, sceneIndex);
            return true;
        }
        if(it == m_cutSceneIndexes.end() || *it != sceneIndex){
            return false;
        }
        m_cutSceneIndexes.erase(it);
        return true;
    }

    bool toggleSceneContainingFrame(FrameIndex frameIndex, const MarkerSet& markers, FrameIndex sourceFrameCount){
        const auto sceneIndex{sceneIndexContainingFrame(frameIndex, markers, sourceFrameCount)};
        return sceneIndex ? toggleScene(*sceneIndex) : false;
    }

    bool setSceneContainingFrame(FrameIndex frameIndex, bool cut, const MarkerSet& markers, FrameIndex sourceFrameCount){
        const auto sceneIndex{sceneIndexContainingFrame(frameIndex, markers, sourceFrameCount)};
        return sceneIndex ? setScene(*sceneIndex, cut) : false;
    }

    vector<SceneFrameRange> buildSceneRanges(const MarkerSet& markers, FrameIndex sourceFrameCount) const{
        vector<SceneFrameRange> ranges;
        if(sourceFrameCount == 0){
            return ranges;
        }

        FrameIndex firstFrame{};
        uint32_t sceneIndex{};
        for(const auto& marker: markers.markers()){
            const auto endFrame{min(marker.frameIndex, sourceFrameCount)};
            if(endFrame > firstFrame){
                ranges.push_back(SceneFrameRange{.firstFrame = firstFrame, .endFrameExclusive = endFrame, .sceneIndex = sceneIndex, .cut = containsSceneIndex(sceneIndex)});
                ++sceneIndex;
            }
            firstFrame = endFrame;
        }
        if(firstFrame < sourceFrameCount){
            ranges.push_back(SceneFrameRange{.firstFrame = firstFrame, .endFrameExclusive = sourceFrameCount, .sceneIndex = sceneIndex, .cut = containsSceneIndex(sceneIndex)});
        }
        return ranges;
    }

    vector<SceneFrameRange> buildCutSceneRanges(const MarkerSet& markers, FrameIndex sourceFrameCount) const{
        auto ranges{buildSceneRanges(markers, sourceFrameCount)};
        erase_if(ranges, [](const auto& range){ return !range.cut; });
        return ranges;
    }

    void replaceCutSceneIndexes(vector<uint32_t> indexes){
        sort(indexes.begin(), indexes.end());
        indexes.erase(unique(indexes.begin(), indexes.end()), indexes.end());
        m_cutSceneIndexes = move(indexes);
    }

    void remapAfterMarkerInsertion(uint32_t insertedMarkerOrdinal){
        vector<uint32_t> updated;
        updated.reserve(m_cutSceneIndexes.size() + 1);
        for(const auto sceneIndex: m_cutSceneIndexes){
            if(sceneIndex < insertedMarkerOrdinal){
                updated.push_back(sceneIndex);
            }else if(sceneIndex == insertedMarkerOrdinal){
                updated.push_back(sceneIndex);
                updated.push_back(sceneIndex + 1);
            }else{
                updated.push_back(sceneIndex + 1);
            }
        }
        replaceCutSceneIndexes(move(updated));
    }

    void remapAfterMarkerRemoval(uint32_t removedMarkerOrdinal){
        vector<uint32_t> updated;
        updated.reserve(m_cutSceneIndexes.size());
        for(const auto sceneIndex: m_cutSceneIndexes){
            if(sceneIndex < removedMarkerOrdinal){
                updated.push_back(sceneIndex);
            }else if(sceneIndex == removedMarkerOrdinal || sceneIndex == removedMarkerOrdinal + 1){
                updated.push_back(removedMarkerOrdinal);
            }else{
                updated.push_back(sceneIndex - 1);
            }
        }
        replaceCutSceneIndexes(move(updated));
    }

private:
    static optional<uint32_t> sceneIndexContainingFrame(FrameIndex frameIndex, const MarkerSet& markers, FrameIndex sourceFrameCount){
        if(sourceFrameCount == 0){
            return nullopt;
        }

        const auto target{min(frameIndex, sourceFrameCount - 1)};
        uint32_t sceneIndex{};
        for(const auto& marker: markers.markers()){
            if(target < marker.frameIndex){
                return sceneIndex;
            }
            ++sceneIndex;
        }
        return sceneIndex;
    }

private:
    vector<uint32_t> m_cutSceneIndexes;
};

class CutPlanner final{
public:
    void reset(){
        m_markers.reset();
        m_rapFrames.reset();
        m_cutScenes.reset();
    }

    bool addMarker(FrameIndex frameIndex){
        const auto insertionOrdinal{markerInsertionOrdinal(frameIndex)};
        if(!m_markers.add(frameIndex)){
            return false;
        }
        m_cutScenes.remapAfterMarkerInsertion(insertionOrdinal);
        return true;
    }

    bool removeMarker(FrameIndex frameIndex){
        const auto ordinal{m_markers.markerOrdinalForFrame(frameIndex)};
        if(!ordinal || !m_markers.remove(frameIndex)){
            return false;
        }
        m_cutScenes.remapAfterMarkerRemoval(static_cast<uint32_t>(*ordinal));
        return true;
    }

    bool toggleMarker(FrameIndex frameIndex){
        return m_markers.contains(frameIndex) ? removeMarker(frameIndex) : addMarker(frameIndex);
    }

    bool toggleCutSceneContainingFrame(FrameIndex frameIndex, FrameIndex sourceFrameCount){
        return m_cutScenes.toggleSceneContainingFrame(frameIndex, m_markers, sourceFrameCount);
    }

    bool setCutSceneContainingFrame(FrameIndex frameIndex, bool cut, FrameIndex sourceFrameCount){
        return m_cutScenes.setSceneContainingFrame(frameIndex, cut, m_markers, sourceFrameCount);
    }

    vector<SceneFrameRange> buildSceneRanges(FrameIndex sourceFrameCount) const{
        return m_cutScenes.buildSceneRanges(m_markers, sourceFrameCount);
    }

    vector<SceneFrameRange> buildCutSceneRanges(FrameIndex sourceFrameCount) const{
        return m_cutScenes.buildCutSceneRanges(m_markers, sourceFrameCount);
    }

    bool hasRequestedCuts() const{ return !m_cutScenes.cutSceneIndexes().empty(); }
    bool needsRapAlignment() const{
        const auto markers{m_markers.markers()};
        return hasRequestedCuts() && any_of(markers.begin(), markers.end(), [](const auto& marker){ return !marker.isEvaluatedAgainstRap; });
    }

    EffectiveFrameExportPlan buildDirectExportPlan(FrameIndex sourceFrameCount) const{
        EffectiveFrameExportPlan plan{.sourceFrameCount = sourceFrameCount};
        for(const auto& range: buildCutSceneRanges(sourceFrameCount)){
            plan.requestedCutRanges.push_back(ExportFrameRange{.firstFrame = range.firstFrame, .endFrameExclusive = range.endFrameExclusive});
        }
        plan.effectiveCutRanges = plan.requestedCutRanges;
        plan.hasRequestedCuts = !plan.requestedCutRanges.empty();
        plan.requestedOutputFrameCount = outputFrameCountAfterCuts(sourceFrameCount, plan.requestedCutRanges);
        plan.effectiveOutputFrameCount = plan.requestedOutputFrameCount;
        return plan;
    }

    EffectiveFrameExportPlan buildRapAlignedExportPlan(FrameIndex sourceFrameCount) const{
        auto plan{buildDirectExportPlan(sourceFrameCount)};
        vector<ExportFrameRange> aligned;
        vector<ExportFrameRange> mergedRequestedCuts;
        mergedRequestedCuts.reserve(plan.requestedCutRanges.size());
        for(const auto& range: plan.requestedCutRanges){
            if(!mergedRequestedCuts.empty() && range.firstFrame <= mergedRequestedCuts.back().endFrameExclusive){
                mergedRequestedCuts.back().endFrameExclusive = max(mergedRequestedCuts.back().endFrameExclusive, range.endFrameExclusive);
            }else{
                mergedRequestedCuts.push_back(range);
            }
        }
        aligned.reserve(mergedRequestedCuts.size());

        for(const auto& range: mergedRequestedCuts){
            auto start{range.firstFrame};
            auto end{range.endFrameExclusive};
            if(start > 0 && end < sourceFrameCount){
                const auto rapStart{m_rapFrames.nextRapAtOrAfter(start)};
                if(!rapStart){
                    continue;
                }
                start = *rapStart;
            }
            if(end < sourceFrameCount){
                const auto rapEnd{m_rapFrames.previousRapAtOrBefore(end)};
                if(!rapEnd){
                    continue;
                }
                end = *rapEnd;
            }
            if(end > start){
                aligned.push_back(ExportFrameRange{.firstFrame = start, .endFrameExclusive = end});
            }
        }

        plan.effectiveCutRanges = move(aligned);
        plan.emptyAfterAlignment = plan.hasRequestedCuts && plan.effectiveCutRanges.empty();
        plan.effectiveOutputFrameCount = outputFrameCountAfterCuts(sourceFrameCount, plan.effectiveCutRanges);
        plan.materiallyDifferent = plan.requestedCutRanges != plan.effectiveCutRanges;
        return plan;
    }

    PlannerReevaluateMarkersResult reevaluateMarkersAgainstRap(){
        PlannerReevaluateMarkersResult result{.hadMarkers = !m_markers.empty()};
        if(!result.hadMarkers || m_rapFrames.empty()){
            return result;
        }

        vector<EditMarker> updated;
        updated.reserve(m_markers.size());
        for(auto marker: m_markers.markers()){
            const auto previous{m_rapFrames.previousRapAtOrBefore(marker.frameIndex)};
            const auto next{m_rapFrames.nextRapAtOrAfter(marker.frameIndex)};
            if(!previous && !next){
                continue;
            }

            auto replacement{previous ? *previous : *next};
            if(previous && next){
                const auto previousDistance{marker.frameIndex - *previous};
                const auto nextDistance{*next - marker.frameIndex};
                replacement = previousDistance <= nextDistance ? *previous : *next;
            }

            marker.wasMovedForRapSafety = marker.frameIndex != replacement;
            marker.isEvaluatedAgainstRap = true;
            marker.frameIndex = replacement;
            result.changed = result.changed || marker.wasMovedForRapSafety;
            result.replacedCount += marker.wasMovedForRapSafety ? 1 : 0;
            updated.push_back(marker);
        }
        m_markers.replaceAll(move(updated));
        return result;
    }

    // Keep the requested cut spans, then rebuild their boundaries from RAP frames.
    // This is deliberately range based: independently snapping markers changes scenes.
    PlannerReevaluateMarkersResult reevaluateCutScenesAgainstRap(FrameIndex sourceFrameCount){
        PlannerReevaluateMarkersResult result{.hadMarkers = !m_markers.empty()};
        if(!result.hadMarkers || m_rapFrames.empty() || sourceFrameCount == 0){
            return result;
        }

        auto requestedCuts{buildCutSceneRanges(sourceFrameCount)};
        vector<SceneFrameRange> mergedRequestedCuts;
        for(const auto& range: requestedCuts){
            if(!mergedRequestedCuts.empty() && mergedRequestedCuts.back().endFrameExclusive == range.firstFrame){
                mergedRequestedCuts.back().endFrameExclusive = range.endFrameExclusive;
            }else{
                mergedRequestedCuts.push_back(range);
            }
        }
        const auto originalMarkers{m_markers.markers()};
        vector<EditMarker> updated;
        updated.reserve(originalMarkers.size() * 3);
        const auto addRapMarker = [&](FrameIndex frame){
            if(frame > 0 && frame < sourceFrameCount){
                updated.push_back(EditMarker{.frameIndex = frame, .isEvaluatedAgainstRap = true, .wasMovedForRapSafety = true});
            }
        };

        for(auto marker: originalMarkers){
            const auto isRap{m_rapFrames.contains(marker.frameIndex)};
            marker.isEvaluatedAgainstRap = isRap;
            if(!isRap){
                ++result.replacedCount;
                if(const auto previous{m_rapFrames.previousRapAtOrBefore(marker.frameIndex)}; previous && *previous < marker.frameIndex){
                    addRapMarker(*previous);
                }
                if(const auto next{m_rapFrames.nextRapAtOrAfter(marker.frameIndex)}; next && *next > marker.frameIndex){
                    addRapMarker(*next);
                }
            }
            updated.push_back(marker);
        }

        vector<ExportFrameRange> alignedCuts;
        for(const auto& range: mergedRequestedCuts){
            auto start{range.firstFrame};
            auto end{range.endFrameExclusive};
            const auto isTailCut{end >= sourceFrameCount && m_markers.contains(start)};
            if(start > 0 && !isTailCut){
                const auto rap{m_rapFrames.nextRapAtOrAfter(start)};
                if(!rap){
                    continue;
                }
                start = *rap;
            }
            if(end < sourceFrameCount){
                const auto rap{m_rapFrames.previousRapAtOrBefore(end)};
                if(!rap){
                    continue;
                }
                end = *rap;
            }
            if(end > start){
                alignedCuts.push_back({.firstFrame = start, .endFrameExclusive = end});
                addRapMarker(start);
                addRapMarker(end);
            }
        }

        m_markers.replaceAll(move(updated));
        vector<uint32_t> cutScenes;
        for(const auto& scene: buildSceneRanges(sourceFrameCount)){
            const auto midpoint{scene.firstFrame + (scene.endFrameExclusive - scene.firstFrame) / 2};
            if(any_of(alignedCuts.begin(), alignedCuts.end(), [midpoint](const auto& range){
                return midpoint >= range.firstFrame && midpoint < range.endFrameExclusive;
            })){
                cutScenes.push_back(scene.sceneIndex);
            }
        }
        m_cutScenes.replaceCutSceneIndexes(move(cutScenes));
        result.changed = originalMarkers.size() != m_markers.size()
            || !equal(originalMarkers.begin(), originalMarkers.end(), m_markers.markers().begin(), m_markers.markers().end());
        return result;
    }

    MarkerSet& markers(){ return m_markers; }
    const MarkerSet& markers() const{ return m_markers; }
    RapFrameSet& rapFrames(){ return m_rapFrames; }
    const RapFrameSet& rapFrames() const{ return m_rapFrames; }
    CutSceneSet& cutScenes(){ return m_cutScenes; }
    const CutSceneSet& cutScenes() const{ return m_cutScenes; }

private:
    uint32_t markerInsertionOrdinal(FrameIndex frameIndex) const{
        const auto markers{m_markers.markers()};
        const auto it{lower_bound(markers.begin(), markers.end(), frameIndex, [](const auto& marker, FrameIndex frame){ return marker.frameIndex < frame; })};
        return static_cast<uint32_t>(distance(markers.begin(), it));
    }

    static FrameIndex outputFrameCountAfterCuts(FrameIndex sourceFrameCount, span<const ExportFrameRange> cutRanges){
        auto removedFrameCount{FrameIndex{}};
        for(const auto& range: cutRanges){
            if(range.endFrameExclusive > range.firstFrame){
                removedFrameCount += range.endFrameExclusive - range.firstFrame;
            }
        }
        return removedFrameCount > sourceFrameCount ? 0 : sourceFrameCount - removedFrameCount;
    }

private:
    MarkerSet m_markers;
    RapFrameSet m_rapFrames;
    CutSceneSet m_cutScenes;
};

}
