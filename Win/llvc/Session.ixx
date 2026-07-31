module;

#include <winrt/Windows.Foundation.h>

export module llvc.Session;

import std;
import llvc.CutPlanner;
import llvc.Project;
import llvc.Media;
import llvc.Timeline;
import llvc.EditorController;
import llvc.ExportCoordinator;

export namespace llvc{

using namespace ::std;

class SessionActivity{
public:
    virtual ~SessionActivity() = default;
    virtual void cancelOutstandingWork(){}
    virtual void waitForIdle(){}
    virtual void reset(){}
};

struct RapLookupState final{
    vector<int64_t> times100ns{};
    vector<int64_t> targetTimes100ns{};
    bool attempted{};
    bool succeeded{};
    bool partial{};
    bool inProgress{};
    std::optional<EditorSnapshot> lastReevaluatedSnapshot{};
    wstring lastReevaluatedSourcePath{};
    bool pendingReevaluate{};
    bool pendingReevaluateWithoutUndo{};
    bool forceFullScan{};
    vector<int64_t> pendingAutoEvaluateMarkerTimes100ns{};
    int pendingNudgeDirection{};
    vector<int64_t> queuedTargetTimes100ns{};
    uint32_t retryCount{};
    bool canceled{};
    wstring lastError{};
    uint64_t completionId{};

    void reset(){ *this = {}; }
};

struct RapLookupResult final{
    vector<int64_t> times100ns{};
    bool succeeded{};
    bool canceled{};
};

class MediaSession final : public SessionActivity{
public:
    void load(wstring sourcePath, unique_ptr<VideoSource> source, VideoSource::InspectionResult inspection){
        reset();
        m_sourcePath = move(sourcePath);
        m_source = shared_ptr<VideoSource>{move(source)};
        m_inspection = move(inspection);
    }

    void cancelOutstandingWork() override{ m_rapCancelRequested.store(true, memory_order_release); }
    void waitForIdle() override{
        unique_lock lock{m_activityMutex};
        m_activityIdle.wait(lock, [this]{ return m_activeRapLookups.load(memory_order_acquire) == 0; });
    }
    void reset() override{
        cancelOutstandingWork();
        waitForIdle();
        m_source.reset();
        m_inspection = {};
        m_sourcePath.clear();
        m_sourceFrameCount = 0;
        m_rapLookup.reset();
    }

    bool hasSource() const{ return m_source != nullptr; }
    const wstring& sourcePath() const{ return m_sourcePath; }
    VideoSource* source(){ return m_source.get(); }
    const VideoSource* source() const{ return m_source.get(); }
    VideoSource::InspectionResult& inspection(){ return m_inspection; }
    const VideoSource::InspectionResult& inspection() const{ return m_inspection; }

    void sourceFrameCount(FrameIndex value){ m_sourceFrameCount = value; }
    FrameIndex sourceFrameCount() const{ return m_sourceFrameCount; }

    FrameIndex frameIndexForTime100ns(int64_t time100ns) const{
        if(time100ns <= 0 || m_inspection.frameRate.num == 0 || m_inspection.frameRate.den == 0){
            return 0;
        }

        const auto frameIndex{static_cast<FrameIndex>((static_cast<long double>(time100ns) * m_inspection.frameRate.num) / (static_cast<long double>(m_inspection.frameRate.den) * 10'000'000.0L))};
        return m_sourceFrameCount == 0 ? frameIndex : min(frameIndex, m_sourceFrameCount - 1);
    }

    int64_t time100nsForFrame(FrameIndex frameIndex) const{
        if(m_inspection.frameRate.num == 0 || m_inspection.frameRate.den == 0){
            return 0;
        }
        // Ceil instead of truncating.  At rates such as 30000/1001,
        // truncation can produce a timestamp just before the frame start,
        // which frameIndexForTime100ns then maps back to the previous frame.
        const auto exactTime100ns{
            (static_cast<long double>(frameIndex) * m_inspection.frameRate.den * 10'000'000.0L)
            / m_inspection.frameRate.num};
        return static_cast<int64_t>(ceil(exactTime100ns));
    }

    RapLookupState& rapLookup(){ return m_rapLookup; }
    const RapLookupState& rapLookup() const{ return m_rapLookup; }

    vector<int64_t> rapLookupTargets(const Project& project, int64_t sourceDuration100ns) const{
        return buildRapLookupTimesForExportAlignment(project, sourceDuration100ns);
    }

    vector<int64_t> markerTimesForRapScan(const Project& project) const{
        vector<int64_t> targets;
        const auto sourceDuration100ns{project.timelineDuration100ns()};
        if(sourceDuration100ns <= 0){
            return targets;
        }
        if(!project.hasCanonicalFrameModel()){
            return targets;
        }
        targets.reserve(project.cutPlanner().markers().size());
        for(const auto& marker: project.cutPlanner().markers().markers()){
            const auto time100ns{time100nsForFrame(marker.frameIndex)};
            if(time100ns > 0 && time100ns < sourceDuration100ns){
                targets.push_back(time100ns);
            }
        }
        sort(targets.begin(), targets.end());
        targets.erase(unique(targets.begin(), targets.end()), targets.end());
        return targets;
    }

    void commitRapLookup(Project& project, bool succeeded, bool partial, vector<int64_t> times100ns, vector<int64_t> targetTimes100ns){
        completeRapLookup(succeeded, partial, move(times100ns), move(targetTimes100ns));
        if(!m_rapLookup.succeeded){
            return;
        }
        vector<FrameIndex> rapFrames;
        rapFrames.reserve(m_rapLookup.times100ns.size());
        for(const auto time100ns: m_rapLookup.times100ns){
            rapFrames.push_back(frameIndexForTime100ns(time100ns));
        }
        project.cutPlanner().rapFrames().replaceAll(move(rapFrames));
    }

    std::optional<vector<int64_t>> availableRapTimes(bool allowPartial, span<const int64_t> requiredPartialTargets100ns = {}) const{
        if(!m_rapLookup.attempted || !m_rapLookup.succeeded || m_rapLookup.times100ns.empty()){
            return nullopt;
        }
        if(m_rapLookup.partial){
            if(!allowPartial){
                return nullopt;
            }
            vector<int64_t> required{requiredPartialTargets100ns.begin(), requiredPartialTargets100ns.end()};
            sort(required.begin(), required.end());
            required.erase(unique(required.begin(), required.end()), required.end());
            if(!includes(m_rapLookup.targetTimes100ns.begin(), m_rapLookup.targetTimes100ns.end(), required.begin(), required.end())){
                return nullopt;
            }
        }
        return m_rapLookup.times100ns;
    }

    bool beginRapLookup(){
        if(m_rapLookup.inProgress || !m_source){
            return false;
        }
        m_rapCancelRequested.store(false, memory_order_release);
        m_rapLookup.inProgress = true;
        m_rapLookup.canceled = false;
        m_rapLookup.lastError.clear();
        return true;
    }

    // UI requests are coalesced while a scan is active.  The worker that owns
    // the current scan remains the only caller touching VideoSource.
    bool queueRapLookup(vector<int64_t> targetTimes100ns, bool forceFullScan = false){
        sort(targetTimes100ns.begin(), targetTimes100ns.end());
        targetTimes100ns.erase(unique(targetTimes100ns.begin(), targetTimes100ns.end()), targetTimes100ns.end());
        m_rapLookup.forceFullScan = m_rapLookup.forceFullScan || forceFullScan;
        m_rapLookup.queuedTargetTimes100ns.insert(m_rapLookup.queuedTargetTimes100ns.end(), targetTimes100ns.begin(), targetTimes100ns.end());
        sort(m_rapLookup.queuedTargetTimes100ns.begin(), m_rapLookup.queuedTargetTimes100ns.end());
        m_rapLookup.queuedTargetTimes100ns.erase(unique(m_rapLookup.queuedTargetTimes100ns.begin(), m_rapLookup.queuedTargetTimes100ns.end()), m_rapLookup.queuedTargetTimes100ns.end());
        return !m_rapLookup.queuedTargetTimes100ns.empty() || m_rapLookup.forceFullScan;
    }

    bool hasQueuedRapLookup() const{ return m_rapLookup.forceFullScan || !m_rapLookup.queuedTargetTimes100ns.empty(); }

    vector<int64_t> takeQueuedRapLookupTargets(){
        auto targets{move(m_rapLookup.queuedTargetTimes100ns)};
        m_rapLookup.queuedTargetTimes100ns.clear();
        return targets;
    }

    void requestRapRetry(){
        ++m_rapLookup.retryCount;
        m_rapLookup.attempted = false;
        m_rapLookup.succeeded = false;
        m_rapLookup.canceled = false;
        m_rapLookup.lastError.clear();
    }

    RapLookupResult scanRapMarkers(
        const vector<int64_t>& targetTimes100ns,
        const function<void(double)>& progressCallback,
        const function<bool()>& externalCancelRequested){
        struct ActivityGuard final{
            MediaSession& owner;
            ~ActivityGuard(){
                if(owner.m_activeRapLookups.fetch_sub(1, memory_order_acq_rel) == 1){
                    lock_guard lock{owner.m_activityMutex};
                    owner.m_activityIdle.notify_all();
                }
            }
        };

        auto source{m_source};
        if(!source){
            return {};
        }
        m_activeRapLookups.fetch_add(1, memory_order_acq_rel);
        ActivityGuard activity{*this};
        const auto canceled{[this, &externalCancelRequested]{
            return m_rapCancelRequested.load(memory_order_acquire)
                || (externalCancelRequested && externalCancelRequested());
        }};
        if(canceled()){
            return {.canceled = true};
        }

        RapLookupResult result{};
        result.times100ns = source->collectRapTimes100ns(targetTimes100ns, progressCallback, canceled);
        result.canceled = canceled();
        if(result.canceled){
            result.times100ns.clear();
            return result;
        }
        sort(result.times100ns.begin(), result.times100ns.end());
        result.times100ns.erase(unique(result.times100ns.begin(), result.times100ns.end()), result.times100ns.end());
        result.succeeded = !result.times100ns.empty();
        return result;
    }

    ::winrt::Windows::Foundation::IAsyncOperation<bool> scanAndCommitRapMarkersAsync(
        Project& project,
        vector<int64_t> targetTimes100ns,
        function<void(double)> progressCallback = {},
        function<bool()> externalCancelRequested = {}){
        if(!beginRapLookup()){
            queueRapLookup(move(targetTimes100ns));
            co_return false;
        }

        ::winrt::apartment_context callerThread;
        RapLookupResult result{};
        try{
            co_await ::winrt::resume_background();
            result = scanRapMarkers(targetTimes100ns, progressCallback, externalCancelRequested);
        }catch(...){
            result = {};
        }

        co_await callerThread;
        if(result.canceled){
            abandonRapLookup();
        }else{
            const auto partial{!targetTimes100ns.empty()};
            commitRapLookup(project, result.succeeded, partial, move(result.times100ns), move(targetTimes100ns));
        }
        co_return result.succeeded;
    }

    void completeRapLookup(bool succeeded, bool partial, vector<int64_t> times100ns, vector<int64_t> targetTimes100ns){
        sort(times100ns.begin(), times100ns.end());
        times100ns.erase(unique(times100ns.begin(), times100ns.end()), times100ns.end());
        m_rapLookup.times100ns = succeeded ? move(times100ns) : vector<int64_t>{};
        m_rapLookup.targetTimes100ns = succeeded ? move(targetTimes100ns) : vector<int64_t>{};
        m_rapLookup.attempted = true;
        m_rapLookup.succeeded = succeeded;
        m_rapLookup.partial = succeeded && partial;
        m_rapLookup.forceFullScan = false;
        m_rapLookup.inProgress = false;
        m_rapLookup.canceled = false;
        m_rapLookup.lastError = succeeded ? wstring{} : L"The RAP scan did not return any random-access points.";
        ++m_rapLookup.completionId;
    }

    void abandonRapLookup(){
        m_rapLookup.inProgress = false;
        m_rapLookup.canceled = true;
        m_rapLookup.lastError = L"The RAP scan was canceled.";
    }

private:
    wstring m_sourcePath;
    shared_ptr<VideoSource> m_source;
    VideoSource::InspectionResult m_inspection{};
    FrameIndex m_sourceFrameCount{};
    RapLookupState m_rapLookup{};
    atomic_bool m_rapCancelRequested{};
    atomic_uint32_t m_activeRapLookups{};
    mutable mutex m_activityMutex{};
    mutable condition_variable m_activityIdle{};
};

class PreviewController final : public SessionActivity{
public:
    void reset() override{
        m_playing = false;
        m_currentFrame = 0;
    }

    void play(){ m_playing = true; }
    void pause(){ m_playing = false; }
    void stop(){ reset(); }

    void seekToFrame(FrameIndex frameIndex, FrameIndex sourceFrameCount = 0){
        m_currentFrame = sourceFrameCount == 0 ? frameIndex : min(frameIndex, sourceFrameCount - 1);
    }
    void stepFrames(int64_t deltaFrames){
        if(deltaFrames < 0 && static_cast<FrameIndex>(-deltaFrames) > m_currentFrame){
            m_currentFrame = 0;
            return;
        }
        m_currentFrame = static_cast<FrameIndex>(static_cast<int64_t>(m_currentFrame) + deltaFrames);
    }
    void stepFrames(int64_t deltaFrames, FrameIndex sourceFrameCount){
        stepFrames(deltaFrames);
        if(sourceFrameCount > 0){
            m_currentFrame = min(m_currentFrame, sourceFrameCount - 1);
        }
    }
    void jumpToPercent(uint32_t percent, FrameIndex sourceFrameCount){
        if(sourceFrameCount == 0){
            m_currentFrame = 0;
            return;
        }
        m_currentFrame = (sourceFrameCount - 1) * min(percent, 100u) / 100u;
    }

    FrameIndex currentFrame() const{ return m_currentFrame; }
    bool isPlaying() const{ return m_playing; }

    bool skipCurrentCutSceneDuringPlayback(const CutPlanner& planner, FrameIndex sourceFrameCount){
        std::optional<FrameIndex> skipTarget;
        for(const auto& range: planner.buildCutSceneRanges(sourceFrameCount)){
            if(m_currentFrame >= range.firstFrame && m_currentFrame < range.endFrameExclusive){
                skipTarget = range.endFrameExclusive;
                continue;
            }
            if(skipTarget && range.firstFrame <= *skipTarget){
                skipTarget = max(*skipTarget, range.endFrameExclusive);
                continue;
            }
            if(skipTarget){ break; }
        }
        if(skipTarget){
            m_currentFrame = *skipTarget;
            return true;
        }
        return false;
    }

    FrameIndex seekTarget(FrameIndex frameIndex, FrameIndex sourceFrameCount){
        seekToFrame(frameIndex, sourceFrameCount);
        return m_currentFrame;
    }
    FrameIndex stepTarget(int64_t deltaFrames, FrameIndex sourceFrameCount){
        stepFrames(deltaFrames, sourceFrameCount);
        return m_currentFrame;
    }
    FrameIndex percentTarget(uint32_t percent, FrameIndex sourceFrameCount){
        jumpToPercent(percent, sourceFrameCount);
        return m_currentFrame;
    }

    std::optional<FrameIndex> playbackTargetForFrame(FrameIndex frameIndex, const CutPlanner& planner, FrameIndex sourceFrameCount){
        seekToFrame(frameIndex, sourceFrameCount);
        if(skipCurrentCutSceneDuringPlayback(planner, sourceFrameCount)){
            return m_currentFrame;
        }
        return nullopt;
    }

    std::optional<FrameIndex> moveToMarker(const CutPlanner& planner, int direction){
        if(direction == 0){
            return nullopt;
        }
        const auto& markers{planner.markers()};
        const auto target{direction < 0 ? markers.previousMarkerFrame(m_currentFrame) : markers.nextMarkerFrame(m_currentFrame)};
        if(target){ m_currentFrame = *target; }
        return target;
    }

private:
    FrameIndex m_currentFrame{};
    bool m_playing{};
};

class ExportController final : public SessionActivity{
public:
    void cancelOutstandingWork() override{
        if(m_exportInProgress.load(memory_order_relaxed)){
            m_cancelRequested.store(true, memory_order_relaxed);
        }
    }
    void waitForIdle() override{
        unique_lock lock{m_activityMutex};
        m_activityIdle.wait(lock, [this]{ return m_activeExports.load(memory_order_acquire) == 0; });
    }
    void reset() override{
        m_exportInProgress.store(false, memory_order_relaxed);
        m_cancelRequested.store(false, memory_order_relaxed);
        clearCachedPlan();
    }

    bool isExportInProgress() const{ return m_exportInProgress.load(memory_order_relaxed); }
    void requestCancel(){ m_cancelRequested.store(true, memory_order_relaxed); }
    bool cancelRequested() const{ return m_cancelRequested.load(memory_order_relaxed); }

    bool beginExport(){
        if(m_exportInProgress.exchange(true, memory_order_acq_rel)){
            return false;
        }
        m_cancelRequested.store(false, memory_order_relaxed);
        m_activeExports.fetch_add(1, memory_order_acq_rel);
        return true;
    }

    void endExport(){
        if(!m_exportInProgress.exchange(false, memory_order_acq_rel)){
            return;
        }
        if(m_activeExports.fetch_sub(1, memory_order_acq_rel) == 1){
            lock_guard lock{m_activityMutex};
            m_activityIdle.notify_all();
        }
    }

    void cachePlan(EffectiveExportPlan plan, EditorSnapshot snapshot, wstring sourcePath) const{
        m_cachedPlan = move(plan);
        m_cachedSnapshot = move(snapshot);
        m_cachedSourcePath = move(sourcePath);
    }
    void clearCachedPlan(){ m_cachedPlan.reset(); m_cachedSnapshot.reset(); m_cachedSourcePath.clear(); }
    const std::optional<EffectiveExportPlan>& cachedPlan() const{ return m_cachedPlan; }
    bool cachedPlanMatches(const EditorSnapshot& snapshot, const wstring& sourcePath) const{
        return m_cachedPlan && m_cachedSnapshot && m_cachedSourcePath == sourcePath && isSameEditorSnapshot(snapshot, *m_cachedSnapshot);
    }
    std::optional<EffectiveExportPlan> tryGetCachedPlan(const EditorSnapshot& snapshot, const wstring& sourcePath) const{
        if(!cachedPlanMatches(snapshot, sourcePath)){
            return nullopt;
        }
        return m_cachedPlan;
    }
    std::optional<EffectiveExportPlan> prepareEffectivePlan(
        const Project& project,
        int64_t sourceDuration100ns,
        const vector<int64_t>& rapTimes100ns,
        const EditorSnapshot& snapshot,
        const wstring& sourcePath,
        const function<void(double)>& progressCallback = {}) const{
        if(cachedPlanMatches(snapshot, sourcePath)){
            if(progressCallback){ progressCallback(100.0); }
            return m_cachedPlan;
        }
        auto plan{
            effectiveExportPlanNeedsRapAlignment(project)
            ? project.buildEffectiveExportPlanWithRapPreroll(sourceDuration100ns, rapTimes100ns, progressCallback)
            : buildDirectEffectiveExportPlan(project, sourceDuration100ns)};
        if(!effectiveExportPlanNeedsRapAlignment(project) && progressCallback){
            progressCallback(100.0);
        }
        cachePlan(plan, snapshot, sourcePath);
        return plan;
    }

    ExportPreflightState preflight(const Project& project, const MediaSession& media, bool sourceHasAudio) const{
        return buildExportPreflightState(
            project,
            media.hasSource(),
            media.inspection().losslessExportSupport == CapabilityState::Supported,
            !media.inspection().supportedExportExtensions.empty(),
            sourceHasAudio,
            media.inspection().audioExportSupport == CapabilityState::Supported);
    }

    std::optional<EffectiveExportPlan> availablePlan(const Project& project, const MediaSession& media, const function<void(double)>& progressCallback = {}) const{
        const auto sourceDuration100ns{project.timelineDuration100ns()};
        if(sourceDuration100ns <= 0 || !project.hasVideoFile()){
            return nullopt;
        }
        const auto snapshot{captureEditorSnapshot(project)};
        const wstring sourcePath{project.videoFilePath().c_str()};
        if(const auto cachedPlan{tryGetCachedPlan(snapshot, sourcePath)}){
            if(progressCallback){ progressCallback(100.0); }
            return cachedPlan;
        }
        if(!effectiveExportPlanNeedsRapAlignment(project)){
            return prepareEffectivePlan(project, sourceDuration100ns, {}, snapshot, sourcePath, progressCallback);
        }
        const auto targets{media.rapLookupTargets(project, sourceDuration100ns)};
        const auto rapTimes{media.availableRapTimes(true, targets)};
        if(!rapTimes){
            return nullopt;
        }
        return prepareEffectivePlan(project, sourceDuration100ns, *rapTimes, snapshot, sourcePath, progressCallback);
    }

    winrt::Windows::Foundation::IAsyncAction run(ExportCoordinatorRequest request, ExportCoordinatorResult& result){
        request.beginExportActivity = [this]{ return beginExport(); };
        request.endExportActivity = [this]{ endExport(); };
        return runExportAsync(move(request), result);
    }

private:
    atomic_bool m_exportInProgress{};
    atomic_bool m_cancelRequested{};
    atomic_uint32_t m_activeExports{};
    mutable mutex m_activityMutex{};
    mutable condition_variable m_activityIdle{};
    mutable std::optional<EffectiveExportPlan> m_cachedPlan{};
    mutable std::optional<EditorSnapshot> m_cachedSnapshot{};
    mutable wstring m_cachedSourcePath{};
};

class Session final{
public:
    void reset(){
        invalidateActivities();
        waitForIdle();

        m_project.reset();
        m_media.reset();
        m_timeline.reset();
        m_preview.reset();
        m_exporter.reset();
        clearEditorHistory(m_editorHistory);
    }

    uint64_t activityGeneration() const{ return m_activityGeneration.load(memory_order_acquire); }
    void invalidateActivities(){
        m_activityGeneration.fetch_add(1, memory_order_acq_rel);
        cancelOutstandingWork();
        m_timeline.invalidateRender();
        m_timeline.requestViewportRender();
    }

    void cancelOutstandingWork(){
        m_media.cancelOutstandingWork();
        m_timeline.cancelOutstandingWork();
        m_preview.cancelOutstandingWork();
        m_exporter.cancelOutstandingWork();
    }

    void waitForIdle(){
        m_media.waitForIdle();
        m_timeline.waitForIdle();
        m_preview.waitForIdle();
        m_exporter.waitForIdle();
    }

    Project& project(){ return m_project; }
    const Project& project() const{ return m_project; }

    MediaSession& media(){ return m_media; }
    const MediaSession& media() const{ return m_media; }

    Timeline& timeline(){ return m_timeline; }
    const Timeline& timeline() const{ return m_timeline; }

    PreviewController& preview(){ return m_preview; }
    const PreviewController& preview() const{ return m_preview; }

    ExportController& exporter(){ return m_exporter; }
    const ExportController& exporter() const{ return m_exporter; }

    EditorHistoryState& editorHistory(){ return m_editorHistory; }
    const EditorHistoryState& editorHistory() const{ return m_editorHistory; }

    TimelineRenderModel buildTimelineRenderModel(const TimelineViewportRequest& request) const{
        return m_timeline.buildRenderModel(m_project.cutPlanner(), m_media.sourceFrameCount(), request);
    }

private:
    atomic<uint64_t> m_activityGeneration{1};
    Project m_project;
    MediaSession m_media;
    Timeline m_timeline;
    PreviewController m_preview;
    ExportController m_exporter;
    EditorHistoryState m_editorHistory;
};

}
