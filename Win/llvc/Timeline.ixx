export module llvc.Timeline;

import std;
import llvc.CutPlanner;
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

struct FrameRate final{
    uint32_t num{};
    uint32_t den{1};
    constexpr explicit operator double() const noexcept{ return den != 0 ? static_cast<double>(num) / den : 0.0; }
};

struct Rational final{
    uint32_t width{1};
    uint32_t height{1};
};

using ZoomLevel = uint32_t;
using ThumbnailId = uint32_t;

struct TimelineViewportRequest final{
    ZoomLevel zoom{};
    FrameIndex firstVisibleFrame{};
    FrameIndex endVisibleFrameExclusive{};
    double viewportWidthPixels{};
    double viewportHeightPixels{};
};

struct TimelineMarkerRenderItem final{
    FrameIndex frameIndex{};
    double x{};
    bool isEvaluatedAgainstRap{};
    bool wasMovedForRapSafety{};
};

struct TimelineCutSceneRenderItem final{
    FrameIndex firstFrame{};
    FrameIndex endFrameExclusive{};
    double left{};
    double width{};
    uint32_t sceneIndex{};
};

struct TimelineThumbnailRenderItem final{
    FrameIndex frameIndex{};
    double x{};
    uint32_t width{};
    uint32_t height{};
    vector<uint8_t> bgraPixels{};
};

struct TimelineWaveformRenderItem final{
    FrameIndex firstFrame{};
    FrameIndex endFrameExclusive{};
    float peak{};
};

struct TimelineRenderModel final{
    double totalCanvasWidth{};
    vector<TimelineMarkerRenderItem> markers{};
    vector<TimelineCutSceneRenderItem> cutScenes{};
    vector<TimelineThumbnailRenderItem> thumbnails{};
    vector<TimelineWaveformRenderItem> waveform{};
};

enum class ThumbnailBuildState : uint8_t{
    NotRequested,
    Queued,
    Ready,
    Failed,
};

struct ThumbnailFrame final{
    ThumbnailId id{};
    FrameIndex frameIndex{};
    uint32_t width{};
    uint32_t height{};
    vector<uint8_t> bgraPixels{};
};

struct ThumbnailFrameView final{
    ThumbnailId id{};
    FrameIndex frameIndex{};
    uint32_t width{};
    uint32_t height{};
    span<const uint8_t> bgraPixels{};
};

struct ThumbnailSlot final{
    FrameIndex frameIndex{};
    optional<ThumbnailId> thumbnailId{};
    ThumbnailBuildState state{ThumbnailBuildState::NotRequested};
};

struct ThumbnailZoomTrack final{
    ZoomLevel zoom{};
    FrameIndex framesBetweenThumbnails{1};
    vector<ThumbnailSlot> slots{};
};

struct IThumbnailSourceReader{
    virtual ~IThumbnailSourceReader() = default;
};

struct ThumbnailPreviewSetup final{
    IThumbnailSourceReader* sourceReader{};
    FrameIndex sourceFrameCount{};
    FrameRate sourceFrameRate{};
    vector<ZoomLevel> zoomLevels{};
    uint32_t thumbnailHeight{};
    Rational sourceAspectRatio{};
};

class ThumbnailPreviewStore final{
public:
    void setup(const ThumbnailPreviewSetup& setup);
    void reset();

    uint32_t thumbnailWidth() const{ return m_thumbnailWidth; }
    uint32_t thumbnailHeight() const{ return m_thumbnailHeight; }
    FrameIndex sourceFrameCount() const{ return m_sourceFrameCount; }
    FrameRate sourceFrameRate() const{ return m_sourceFrameRate; }
    size_t zoomTrackCount() const{ return m_zoomTracks.size(); }
    size_t slotCount(ZoomLevel zoom) const;
    FrameIndex framesBetweenThumbnails(ZoomLevel zoom) const;

    void requestBuild(ZoomLevel zoom, FrameIndex firstFrame, FrameIndex endFrameExclusive);
    uint64_t buildGeneration() const{ return m_buildGeneration; }
    bool hasPendingBuild() const{ return m_buildPending; }
    bool initialFullPassPending() const{ return m_initialFullPassPending; }
    void completeBuildPass(bool fullPass){
        if(fullPass){ m_initialFullPassPending = false; }
        m_buildPending = false;
    }
    void cancelBuilds(){ ++m_buildGeneration; m_buildPending = false; m_initialFullPassPending = false; }
    optional<ThumbnailFrameView> tryGet(ZoomLevel zoom, FrameIndex frameIndex) const;
    void putReadyFrame(FrameIndex frameIndex, uint32_t width, uint32_t height, vector<uint8_t> bgraPixels);
    span<const ThumbnailFrame> readyFrames() const{ return m_masterFrames; }

private:
    ThumbnailZoomTrack* findTrack(ZoomLevel zoom);
    const ThumbnailZoomTrack* findTrack(ZoomLevel zoom) const;
    optional<size_t> slotOrdinalForFrame(const ThumbnailZoomTrack& track, FrameIndex frameIndex) const;

private:
    IThumbnailSourceReader* m_sourceReader{};
    FrameIndex m_sourceFrameCount{};
    FrameRate m_sourceFrameRate{};
    uint32_t m_thumbnailWidth{};
    uint32_t m_thumbnailHeight{};
    vector<ThumbnailFrame> m_masterFrames{};
    unordered_map<FrameIndex, ThumbnailId> m_thumbnailByFrame{};
    vector<ThumbnailZoomTrack> m_zoomTracks{};
    uint64_t m_buildGeneration{1};
    bool m_buildPending{};
    bool m_initialFullPassPending{};
};

struct AudioWaveformSample final{
    FrameIndex firstFrame{};
    FrameIndex endFrameExclusive{};
    float peak{};
    bool ready{};
};

struct AudioWaveformSetup final{
    FrameIndex sourceFrameCount{};
    FrameRate sourceFrameRate{};
    vector<ZoomLevel> zoomLevels{};
};

struct AudioWaveformAnalysisView final{
    wstring sourcePath{};
    vector<float> peaks{};
    vector<bool> chunksBuilt{};
    bool ready{};
    bool failed{};
    bool inProgress{};
};

class AudioWaveformStore final{
public:
    void setup(const AudioWaveformSetup& setup){
        reset();
        m_sourceFrameCount = setup.sourceFrameCount;
        m_sourceFrameRate = setup.sourceFrameRate;
        m_zoomLevels = setup.zoomLevels;
        sort(m_zoomLevels.begin(), m_zoomLevels.end());
        m_zoomLevels.erase(unique(m_zoomLevels.begin(), m_zoomLevels.end()), m_zoomLevels.end());
    }

    void reset(){
        lock_guard lock{m_analysisMutex};
        ++m_analysisGeneration;
        m_sourceFrameCount = 0;
        m_sourceFrameRate = {};
        m_zoomLevels.clear();
        m_samples.clear();
        m_analysisQueued = false;
        m_sourcePath.clear();
        m_peaks.clear();
        m_chunksBuilt.clear();
        m_ready = false;
        m_failed = false;
        m_inProgress = false;
    }

    FrameIndex sourceFrameCount() const{ return m_sourceFrameCount; }
    FrameRate sourceFrameRate() const{ return m_sourceFrameRate; }
    size_t zoomLevelCount() const{ return m_zoomLevels.size(); }
    void replaceSamples(vector<AudioWaveformSample> samples){ m_samples = move(samples); }
    span<const AudioWaveformSample> samples() const{ return m_samples; }
    bool analysisQueued() const{ return m_analysisQueued; }
    void analysisQueued(bool value){ m_analysisQueued = value; }
    uint64_t analysisGeneration() const{ return m_analysisGeneration; }
    void cancelAnalysis(){
        lock_guard lock{m_analysisMutex};
        ++m_analysisGeneration;
        m_analysisQueued = false;
        m_inProgress = false;
    }
    bool hasPendingAnalysis() const{
        lock_guard lock{m_analysisMutex};
        return m_analysisQueued || m_inProgress;
    }

    void setupAnalysis(wstring sourcePath, size_t bucketCount, size_t chunkCount){
        lock_guard lock{m_analysisMutex};
        if(m_sourcePath == sourcePath && m_peaks.size() == bucketCount && m_chunksBuilt.size() == chunkCount){
            return;
        }
        m_sourcePath = move(sourcePath);
        m_peaks.assign(bucketCount, 0.0f);
        m_chunksBuilt.assign(chunkCount, false);
        m_ready = false;
        m_failed = false;
        m_inProgress = false;
    }
    AudioWaveformAnalysisView analysisView() const{
        lock_guard lock{m_analysisMutex};
        return {.sourcePath = m_sourcePath, .peaks = m_peaks, .chunksBuilt = m_chunksBuilt, .ready = m_ready, .failed = m_failed, .inProgress = m_inProgress};
    }
    bool beginAnalysis(){
        lock_guard lock{m_analysisMutex};
        if(m_sourcePath.empty() || m_ready || m_failed || m_inProgress){ return false; }
        m_inProgress = true;
        return true;
    }
    void completeAnalysisChunk(size_t chunkIndex, span<const float> peaks){
        lock_guard lock{m_analysisMutex};
        if(chunkIndex >= m_chunksBuilt.size() || m_peaks.empty()){ return; }
        const auto first{(chunkIndex * m_peaks.size()) / m_chunksBuilt.size()};
        const auto count{min(peaks.size(), m_peaks.size() - first)};
        copy_n(peaks.begin(), count, m_peaks.begin() + first);
        m_chunksBuilt[chunkIndex] = true;
        m_ready = all_of(m_chunksBuilt.begin(), m_chunksBuilt.end(), [](bool ready){ return ready; });
    }
    void failAnalysis(){ lock_guard lock{m_analysisMutex}; m_failed = true; m_inProgress = false; }
    void endAnalysis(){ lock_guard lock{m_analysisMutex}; m_inProgress = false; m_ready = m_ready || (!m_chunksBuilt.empty() && all_of(m_chunksBuilt.begin(), m_chunksBuilt.end(), [](bool ready){ return ready; })); }

private:
    FrameIndex m_sourceFrameCount{};
    FrameRate m_sourceFrameRate{};
    vector<ZoomLevel> m_zoomLevels{};
    vector<AudioWaveformSample> m_samples{};
    bool m_analysisQueued{};
    mutable mutex m_analysisMutex{};
    wstring m_sourcePath{};
    vector<float> m_peaks{};
    vector<bool> m_chunksBuilt{};
    bool m_ready{};
    bool m_failed{};
    bool m_inProgress{};
    uint64_t m_analysisGeneration{1};
};

struct TimelineThumbnailRenderState final{
    wstring sourcePath{};
    vector<bool> built{};
    double totalWidth{};
    double thumbnailWidth{};
    int thumbnailCount{};

    void reset(){ *this = {}; }
};

struct TimelineRenderSchedulingState final{
    uint64_t renderVersion{};
    uint64_t viewportRequestVersion{};
    bool renderCompleted{};

    void reset(){ *this = {}; }
};

struct Timeline final{
    static constexpr int64_t HnsPerSecond{10'000'000LL};

    class ActivityLease final{
    public:
        ActivityLease() = default;
        ActivityLease(Timeline* owner, uint64_t generation): m_owner{owner}, m_generation{generation}{}
        ActivityLease(const ActivityLease&) = delete;
        ActivityLease& operator=(const ActivityLease&) = delete;
        ActivityLease(ActivityLease&& other) noexcept: m_owner{exchange(other.m_owner, nullptr)}, m_generation{other.m_generation}{}
        ActivityLease& operator=(ActivityLease&& other) noexcept{
            if(this != &other){ release(); m_owner = exchange(other.m_owner, nullptr); m_generation = other.m_generation; }
            return *this;
        }
        ~ActivityLease(){ release(); }
        bool canceled() const;
        void release();
    private:
        Timeline* m_owner{};
        uint64_t m_generation{};
    };

    void reset();
    void cancelOutstandingWork();
    void waitForIdle();
    ActivityLease beginActivity();
    bool activityCanceled() const{ return m_activityCanceled.load(memory_order_acquire); }
    ThumbnailPreviewStore& thumbnails(){ return m_thumbnails; }
    const ThumbnailPreviewStore& thumbnails() const{ return m_thumbnails; }
    AudioWaveformStore& waveforms(){ return m_waveforms; }
    const AudioWaveformStore& waveforms() const{ return m_waveforms; }
    TimelineThumbnailRenderState& thumbnailRenderState(){ return m_thumbnailRenderState; }
    const TimelineThumbnailRenderState& thumbnailRenderState() const{ return m_thumbnailRenderState; }
    TimelineRenderSchedulingState& renderScheduling(){ return m_renderScheduling; }
    const TimelineRenderSchedulingState& renderScheduling() const{ return m_renderScheduling; }
    uint64_t invalidateRender(){ m_renderScheduling.renderCompleted = false; return ++m_renderScheduling.renderVersion; }
    uint64_t beginRender(bool completesTimelineRender){
        if(completesTimelineRender){ m_renderScheduling.renderCompleted = false; }
        return ++m_renderScheduling.renderVersion;
    }
    bool isCurrentRender(uint64_t version) const{ return version == m_renderScheduling.renderVersion; }
    void completeRender(uint64_t version){ if(isCurrentRender(version)){ m_renderScheduling.renderCompleted = true; } }
    uint64_t requestViewportRender(){ return ++m_renderScheduling.viewportRequestVersion; }
    bool isCurrentViewportRequest(uint64_t version) const{ return version == m_renderScheduling.viewportRequestVersion; }
    bool renderCompleted() const{ return m_renderScheduling.renderCompleted; }

    FrameIndex pointToFrame(double x, double width, FrameIndex frameCount) const;
    double frameToPoint(FrameIndex frameIndex, double width, FrameIndex frameCount) const;
    TimelineRenderModel buildRenderModel(const CutPlanner& planner, FrameIndex sourceFrameCount, const TimelineViewportRequest& request) const;

private:
    void endActivity();

private:
    atomic_bool m_activityCanceled{};
    atomic_uint64_t m_activityGeneration{1};
    atomic_uint32_t m_activeActivities{};
    mutable mutex m_activityMutex{};
    mutable condition_variable m_activityIdle{};

public:
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

private:
    ThumbnailPreviewStore m_thumbnails{};
    AudioWaveformStore m_waveforms{};
    TimelineThumbnailRenderState m_thumbnailRenderState{};
    TimelineRenderSchedulingState m_renderScheduling{};
};

}

namespace llvc{

void ThumbnailPreviewStore::setup(const ThumbnailPreviewSetup& setup){
    reset();
    m_sourceReader = setup.sourceReader;
    m_sourceFrameCount = setup.sourceFrameCount;
    m_sourceFrameRate = setup.sourceFrameRate;
    m_thumbnailHeight = setup.thumbnailHeight;

    if(setup.thumbnailHeight > 0 && setup.sourceAspectRatio.height > 0){
        const auto derivedWidth{
            (static_cast<uint64_t>(setup.thumbnailHeight) * max(1u, setup.sourceAspectRatio.width))
            / setup.sourceAspectRatio.height};
        m_thumbnailWidth = static_cast<uint32_t>(max<uint64_t>(1, derivedWidth));
    }

    auto zoomLevels{setup.zoomLevels};
    sort(zoomLevels.begin(), zoomLevels.end());
    zoomLevels.erase(unique(zoomLevels.begin(), zoomLevels.end()), zoomLevels.end());

    m_zoomTracks.reserve(zoomLevels.size());
    for(const auto zoom: zoomLevels){
        const auto safeZoom{max<ZoomLevel>(1, zoom)};
        const auto spacing{max<FrameIndex>(1, safeZoom)};
        ThumbnailZoomTrack track{.zoom = zoom, .framesBetweenThumbnails = spacing};
        if(m_sourceFrameCount > 0){
            const auto slotCount{static_cast<size_t>((m_sourceFrameCount + spacing - 1) / spacing)};
            track.slots.reserve(slotCount);
            for(size_t i{}; i < slotCount; ++i){
                track.slots.push_back(ThumbnailSlot{.frameIndex = static_cast<FrameIndex>(i) * spacing});
            }
        }
        m_zoomTracks.push_back(move(track));
    }
}

void ThumbnailPreviewStore::reset(){
    ++m_buildGeneration;
    m_buildPending = false;
    m_initialFullPassPending = false;
    m_sourceReader = nullptr;
    m_sourceFrameCount = 0;
    m_sourceFrameRate = {};
    m_thumbnailWidth = 0;
    m_thumbnailHeight = 0;
    m_masterFrames.clear();
    m_thumbnailByFrame.clear();
    m_zoomTracks.clear();
}

size_t ThumbnailPreviewStore::slotCount(ZoomLevel zoom) const{
    const auto track{findTrack(zoom)};
    return track ? track->slots.size() : 0;
}

FrameIndex ThumbnailPreviewStore::framesBetweenThumbnails(ZoomLevel zoom) const{
    const auto track{findTrack(zoom)};
    return track ? track->framesBetweenThumbnails : 0;
}

void ThumbnailPreviewStore::requestBuild(ZoomLevel zoom, FrameIndex firstFrame, FrameIndex endFrameExclusive){
    auto track{findTrack(zoom)};
    if(!track || endFrameExclusive <= firstFrame){
        return;
    }

    m_buildPending = true;
    // The first request is always the whole-strip pass.  Viewport requests
    // may add queued slots, but cannot replace that pass before it completes.
    if(firstFrame == 0 && endFrameExclusive >= m_sourceFrameCount){
        m_initialFullPassPending = true;
    }

    for(auto& slot: track->slots){
        if(slot.frameIndex < firstFrame || slot.frameIndex >= endFrameExclusive || slot.thumbnailId){
            continue;
        }
        slot.state = ThumbnailBuildState::Queued;
        if(const auto existing{m_thumbnailByFrame.find(slot.frameIndex)}; existing != m_thumbnailByFrame.end()){
            slot.thumbnailId = existing->second;
            slot.state = ThumbnailBuildState::Ready;
        }
    }
}

optional<ThumbnailFrameView> ThumbnailPreviewStore::tryGet(ZoomLevel zoom, FrameIndex frameIndex) const{
    const auto track{findTrack(zoom)};
    if(!track){
        return nullopt;
    }

    const auto ordinal{slotOrdinalForFrame(*track, frameIndex)};
    if(!ordinal || *ordinal >= track->slots.size()){
        return nullopt;
    }

    const auto& slot{track->slots[*ordinal]};
    if(!slot.thumbnailId){
        return nullopt;
    }

    const auto frameIt{find_if(m_masterFrames.begin(), m_masterFrames.end(), [id = *slot.thumbnailId](const auto& frame){
        return frame.id == id;
    })};
    if(frameIt == m_masterFrames.end()){
        return nullopt;
    }

    return ThumbnailFrameView{
        .id = frameIt->id,
        .frameIndex = frameIt->frameIndex,
        .width = frameIt->width,
        .height = frameIt->height,
        .bgraPixels = frameIt->bgraPixels,
    };
}

void ThumbnailPreviewStore::putReadyFrame(FrameIndex frameIndex, uint32_t width, uint32_t height, vector<uint8_t> bgraPixels){
    ThumbnailId id{};
    if(const auto existing{m_thumbnailByFrame.find(frameIndex)}; existing != m_thumbnailByFrame.end()){
        id = existing->second;
        if(const auto frameIt{find_if(m_masterFrames.begin(), m_masterFrames.end(), [id](const auto& frame){ return frame.id == id; })}; frameIt != m_masterFrames.end()){
            frameIt->width = width;
            frameIt->height = height;
            frameIt->bgraPixels = move(bgraPixels);
        }
    }else{
        id = static_cast<ThumbnailId>(m_masterFrames.size() + 1);
        m_thumbnailByFrame.emplace(frameIndex, id);
        m_masterFrames.push_back(ThumbnailFrame{.id = id, .frameIndex = frameIndex, .width = width, .height = height, .bgraPixels = move(bgraPixels)});
    }

    for(auto& track: m_zoomTracks){
        const auto ordinal{slotOrdinalForFrame(track, frameIndex)};
        if(ordinal && *ordinal < track.slots.size() && track.slots[*ordinal].frameIndex == frameIndex){
            track.slots[*ordinal].thumbnailId = id;
            track.slots[*ordinal].state = ThumbnailBuildState::Ready;
        }
    }
}

ThumbnailZoomTrack* ThumbnailPreviewStore::findTrack(ZoomLevel zoom){
    const auto it{find_if(m_zoomTracks.begin(), m_zoomTracks.end(), [zoom](const auto& track){ return track.zoom == zoom; })};
    return it == m_zoomTracks.end() ? nullptr : &*it;
}

const ThumbnailZoomTrack* ThumbnailPreviewStore::findTrack(ZoomLevel zoom) const{
    const auto it{find_if(m_zoomTracks.begin(), m_zoomTracks.end(), [zoom](const auto& track){ return track.zoom == zoom; })};
    return it == m_zoomTracks.end() ? nullptr : &*it;
}

optional<size_t> ThumbnailPreviewStore::slotOrdinalForFrame(const ThumbnailZoomTrack& track, FrameIndex frameIndex) const{
    if(track.framesBetweenThumbnails == 0){
        return nullopt;
    }
    return static_cast<size_t>(frameIndex / track.framesBetweenThumbnails);
}

void Timeline::reset(){
    cancelOutstandingWork();
    waitForIdle();
    m_thumbnails.reset();
    m_waveforms.reset();
    m_thumbnailRenderState.reset();
    m_renderScheduling.reset();
}

void Timeline::cancelOutstandingWork(){
    m_activityGeneration.fetch_add(1, memory_order_acq_rel);
    m_activityCanceled.store(true, memory_order_release);
    m_thumbnails.cancelBuilds();
    m_waveforms.cancelAnalysis();
}

void Timeline::waitForIdle(){
    unique_lock lock{m_activityMutex};
    m_activityIdle.wait(lock, [this]{ return m_activeActivities.load(memory_order_acquire) == 0; });
}

Timeline::ActivityLease Timeline::beginActivity(){
    m_activityCanceled.store(false, memory_order_release);
    m_activeActivities.fetch_add(1, memory_order_acq_rel);
    return ActivityLease{this, m_activityGeneration.load(memory_order_acquire)};
}

bool Timeline::ActivityLease::canceled() const{
    return !m_owner
        || m_owner->activityCanceled()
        || m_owner->m_activityGeneration.load(memory_order_acquire) != m_generation;
}

void Timeline::ActivityLease::release(){
    if(m_owner){
        m_owner->endActivity();
        m_owner = nullptr;
    }
}

void Timeline::endActivity(){
    if(m_activeActivities.fetch_sub(1, memory_order_acq_rel) == 1){
        lock_guard lock{m_activityMutex};
        m_activityIdle.notify_all();
    }
}

FrameIndex Timeline::pointToFrame(double x, double width, FrameIndex frameCount) const{
    if(width <= 0.0 || frameCount == 0 || !isfinite(x) || !isfinite(width)){
        return 0;
    }

    const auto clampedX{clamp(x, 0.0, width)};
    const auto frame{static_cast<FrameIndex>((clampedX / width) * static_cast<double>(frameCount))};
    return min(frame, frameCount - 1);
}

double Timeline::frameToPoint(FrameIndex frameIndex, double width, FrameIndex frameCount) const{
    if(width <= 0.0 || frameCount == 0 || !isfinite(width)){
        return 0.0;
    }

    const auto ratio{static_cast<double>(min(frameIndex, frameCount)) / static_cast<double>(frameCount)};
    return clamp(ratio * width, 0.0, width);
}

TimelineRenderModel Timeline::buildRenderModel(const CutPlanner& planner, FrameIndex sourceFrameCount, const TimelineViewportRequest& request) const{
    TimelineRenderModel model{};
    if(sourceFrameCount == 0 || request.viewportWidthPixels <= 0.0){
        return model;
    }
    const auto visibleFrameCount{request.endVisibleFrameExclusive > request.firstVisibleFrame ? request.endVisibleFrameExclusive - request.firstVisibleFrame : sourceFrameCount};
    model.totalCanvasWidth = request.viewportWidthPixels * (static_cast<double>(sourceFrameCount) / static_cast<double>(max<FrameIndex>(1, visibleFrameCount)));
    for(const auto& marker: planner.markers().markers()){
        model.markers.push_back({.frameIndex = marker.frameIndex, .x = frameToPoint(marker.frameIndex, model.totalCanvasWidth, sourceFrameCount), .isEvaluatedAgainstRap = marker.isEvaluatedAgainstRap, .wasMovedForRapSafety = marker.wasMovedForRapSafety});
    }
    auto cutRanges{planner.buildCutSceneRanges(sourceFrameCount)};
    vector<SceneFrameRange> mergedCutRanges;
    for(const auto& range: cutRanges){
        if(!mergedCutRanges.empty() && mergedCutRanges.back().endFrameExclusive == range.firstFrame){
            mergedCutRanges.back().endFrameExclusive = range.endFrameExclusive;
        }else{
            mergedCutRanges.push_back(range);
        }
    }
    for(const auto& range: mergedCutRanges){
        const auto left{frameToPoint(range.firstFrame, model.totalCanvasWidth, sourceFrameCount)};
        const auto right{frameToPoint(range.endFrameExclusive, model.totalCanvasWidth, sourceFrameCount)};
        model.cutScenes.push_back({.firstFrame = range.firstFrame, .endFrameExclusive = range.endFrameExclusive, .left = left, .width = max(0.0, right - left), .sceneIndex = range.sceneIndex});
    }
    for(const auto& thumbnail: m_thumbnails.readyFrames()){
        if(thumbnail.frameIndex < request.firstVisibleFrame || thumbnail.frameIndex >= request.endVisibleFrameExclusive){
            continue;
        }
        model.thumbnails.push_back({
            .frameIndex = thumbnail.frameIndex,
            .x = frameToPoint(thumbnail.frameIndex, model.totalCanvasWidth, sourceFrameCount),
            .width = thumbnail.width,
            .height = thumbnail.height,
            .bgraPixels = thumbnail.bgraPixels,
        });
    }
    for(const auto& sample: m_waveforms.samples()){
        model.waveform.push_back({.firstFrame = sample.firstFrame, .endFrameExclusive = sample.endFrameExclusive, .peak = sample.peak});
    }
    return model;
}

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
