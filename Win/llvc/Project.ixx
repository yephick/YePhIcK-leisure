module;

#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Foundation.Collections.h>

export module llvc.Project;

import std;
import llvc.Utils;
import llvc.CutPlanner;

export namespace llvc{

using namespace ::std;
using namespace ::winrt;

struct IndexedFrameSample{
    int64_t time100ns{};
    int64_t duration100ns{};
    bool cleanPoint{};
    bool evaluated{};
    uint32_t sampleIndex{};
};

struct Project;

struct EffectiveExportPlan{
    FrameIndex sourceFrameCount{};
    vector<ExportFrameRange> requestedCutRanges{};
    vector<ExportFrameRange> effectiveCutRanges{};
    FrameIndex requestedOutputFrameCount{};
    FrameIndex effectiveOutputFrameCount{};
    vector<pair<int64_t, int64_t>> requestedCutRanges100ns{};
    vector<pair<int64_t, int64_t>> effectiveCutRanges100ns{};
    int64_t sourceDuration100ns{};
    int64_t requestedOutputDuration100ns{};
    int64_t effectiveOutputDuration100ns{};
    bool hasRequestedCuts{false};
    bool emptyAfterAlignment{false};
    bool materiallyDifferent{false};
};

struct ExportPreflightState{
    bool canExport{};
    wstring blockMessage{};
    bool needsRapReevaluation{};
    int64_t requestedOutputDuration100ns{};
    size_t requestedCutBlockCount{};
};

struct EffectiveExportPlanSummary{
    size_t repositionedMarkers{};
    size_t shrunkCutScenes{};
    int64_t shrunkTotal100ns{};
};

struct ExportOverlayEstimates{
    uint64_t estimatedTargetBytes{};
    uint64_t estimatedSavingsBytes{};
    uint64_t estimatedDroppedAudioBytes{};
};

vector<int64_t> buildCleanKeyframeTimes100ns(const vector<IndexedFrameSample>& index);
wstring formatDuration100ns(int64_t duration100ns);
vector<int64_t> buildRapLookupTimesForExportAlignment(const Project& project, int64_t sourceDuration100ns);
int64_t calculateOutputDuration100ns(int64_t totalDuration100ns, const vector<pair<int64_t, int64_t>>& cutRanges100ns);
bool projectHasRequestedCuts(const Project& project);
bool cutPlanUsesUnevaluatedSceneEdgeMarkers(const Project& project);
bool effectiveExportPlanNeedsRapAlignment(const Project& project);
EffectiveExportPlan buildDirectEffectiveExportPlan(const Project& project, int64_t sourceDuration100ns);
ExportPreflightState buildExportPreflightState(const Project& project, bool hasMedia, bool supportsLosslessExport, bool hasSupportedExportExtensions, bool sourceHasAudio, bool supportsAudioExport);
EffectiveExportPlanSummary summarizeEffectiveExportPlan(const EffectiveExportPlan& plan);
ExportOverlayEstimates buildExportOverlayEstimates(uint64_t sourceSizeBytes, int64_t sourceDuration100ns, int64_t outputDuration100ns, bool keepAudio, bool sourceHasAudio, uint64_t audioBitrateBytesPerSecond);

struct Project final{
    using AAction = ::winrt::Windows::Foundation::IAsyncAction;

    Project(): m_lastSavedProjectSnapshot{_buildProjectSnapshot()} {}

    void reset(){ *this = Project{}; }
    void setZoomWithoutDirty(double v);
    void clearTimeline();
    AAction open(const ::winrt::Windows::Storage::StorageFile& file);
    AAction save(const ::winrt::Windows::Storage::StorageFile& file);
    bool isDirty() const;
    void markClean();
    void markDirty();
    void videoFilePath(const ::winrt::hstring& path);
    void setZoom(double v);
    void keepAudio(bool v);
    void audioXfadeMs(int32_t valueMs);
    void audioVolumePct(int32_t valuePct);
    void synchronizeCutPlannerFrames(FrameIndex sourceFrameCount);
    void synchronizeLegacyEditModelFromCutPlanner(FrameIndex sourceFrameCount);
    void frameIndex(const vector<IndexedFrameSample>& v);
    void cutScenes(const vector<uint32_t>& v);

    inline bool hasVideoFile() const{ return !m_loadedFilePath.empty(); }
    inline auto const& videoFilePath() const{ return m_loadedFilePath; }
    ::winrt::hstring videoFileName() const;
    inline auto  zoom()         const{ return m_zoom; }
    inline bool  keepAudio()    const{ return m_keepAudio; }
    inline auto  audioXfadeMs() const{ return m_audioCrossfadeMs; }
    inline auto  audioVolumePct() const{ return m_audioVolumePct; }
    inline auto& frameIndex()   const{ return m_frameIndex; }
    inline auto& selKeyFrames() const{ return m_selectedKeyFrames; }
    inline auto& cutScenes()    const{ return m_cutScenes; }
    bool hasCanonicalFrameModel() const{ return m_sourceFrameCount > 0; }
    FrameIndex sourceFrameCount() const{ return m_sourceFrameCount; }
    FrameIndex frameIndexForTime100ns(int64_t time100ns) const{
        if(m_sourceFrameCount == 0 || m_timelineDuration100ns <= 0){ return 0; }
        return min<FrameIndex>(static_cast<FrameIndex>((static_cast<long double>(max<int64_t>(0, time100ns)) * m_sourceFrameCount) / m_timelineDuration100ns), m_sourceFrameCount - 1);
    }
    int64_t time100nsForFrame(FrameIndex frameIndex) const{
        return m_sourceFrameCount == 0 ? 0 : static_cast<int64_t>((static_cast<long double>(min(frameIndex, m_sourceFrameCount)) * m_timelineDuration100ns) / m_sourceFrameCount);
    }
    CutPlanner& cutPlanner(){ return m_cutPlanner; }
    const CutPlanner& cutPlanner() const{ return m_cutPlanner; }

    vector<IndexedFrameSample> buildRapMarkersFromSelection() const;
    vector<pair<int64_t, int64_t>> buildCutRanges100ns() const;
    EffectiveFrameExportPlan buildFrameExportPlan(bool rapAligned) const;
    vector<int64_t> buildSceneBoundaries100ns() const;
    EffectiveExportPlan buildEffectiveExportPlanWithRapPreroll(int64_t totalDuration100ns, const vector<int64_t>& rapTimes100ns, const function<void(double)>& progressCallback = {}) const;
    vector<pair<int64_t, int64_t>> buildEffectiveCutRangesWithRapPreroll(int64_t totalDuration100ns, const vector<int64_t>& rapTimes100ns) const;
    int64_t outputDuration100ns() const;
    void refreshSelectedMarkers();
    void remapCutScenesAfterMarkerRemoval(uint32_t removePos);
    void remapCutScenesAfterMarkerInsertion(uint32_t insertPos);
    void timelineDuration100ns(int64_t duration);
    inline int64_t timelineDuration100ns() const{ return m_timelineDuration100ns; }
    bool toggleSelectedKeyframeAtTime100ns(int64_t time100ns, double fps);
    bool toggleCutBlockAtTime100ns(int64_t time100ns);
    bool setCutBlockAtTime100ns(int64_t time100ns, bool cutScene);

private:
    wstring _buildProjectSnapshot() const;
    wstring _serializeCutMarkers() const;
    static vector<IndexedFrameSample> _parseKeyframeVector(const wstring& text);
    std::optional<size_t> _sceneIndexAtTime100ns(int64_t time100ns) const;
    void _syncCutPlannerFromLegacyState();
    static wstring _serializePlannerMarkers(span<const EditMarker> markers);
    static vector<EditMarker> _parsePlannerMarkers(const wstring& text);

private:
    // these are persisted on disk
    ::winrt::hstring m_loadedFilePath{};
    double m_zoom{4};
    bool m_keepAudio{true};
    int32_t m_audioCrossfadeMs{0};
    int32_t m_audioVolumePct{100};
    vector<IndexedFrameSample> m_frameIndex{};
    vector<uint32_t> m_selectedKeyFrames{};
    vector<uint32_t> m_cutScenes{};
    CutPlanner m_cutPlanner{};

    int64_t m_timelineDuration100ns{0};
    FrameIndex m_sourceFrameCount{};
    bool m_isDirty{false};
    wstring m_lastSavedProjectSnapshot{}; // must be *after* all the members to reflect proper initial state
};


}

namespace llvc{

using namespace std;

vector<int64_t> buildEvaluatedKeyframeTimes100ns(const vector<IndexedFrameSample>& index){
    vector<int64_t> times;
    times.reserve(index.size());
    for(const auto& sample: index){
        if(sample.evaluated){
            times.push_back(sample.time100ns);
        }
    }
    return times;
}

wstring formatDuration100ns(int64_t duration100ns){
    const auto totalMilliseconds{max<int64_t>(0, duration100ns) / 10'000};
    const auto milliseconds{totalMilliseconds % 1'000};
    const auto totalSeconds{totalMilliseconds / 1'000};
    const auto seconds{totalSeconds % 60};
    const auto totalMinutes{totalSeconds / 60};
    const auto minutes{totalMinutes % 60};
    const auto hours{totalMinutes / 60};
    if(hours > 0){
        return std::format(L"{}:{:02}:{:02}.{:03}", hours, minutes, seconds, milliseconds);
    }
    return std::format(L"{:02}:{:02}.{:03}", totalMinutes, seconds, milliseconds);
}

vector<int64_t> buildRapLookupTimesForExportAlignment(const Project& project, int64_t sourceDuration100ns){
    vector<int64_t> targets;
    if(sourceDuration100ns <= 0 || !projectHasRequestedCuts(project)){
        return targets;
    }

    if(project.hasCanonicalFrameModel()){
        targets.reserve(project.cutPlanner().markers().size());
        for(const auto& marker: project.cutPlanner().markers().markers()){
            if(marker.isEvaluatedAgainstRap){
                continue;
            }
            const auto time100ns{project.time100nsForFrame(marker.frameIndex)};
            if(time100ns > 0 && time100ns < sourceDuration100ns){
                targets.push_back(time100ns);
            }
        }
        sort(targets.begin(), targets.end());
        targets.erase(unique(targets.begin(), targets.end()), targets.end());
        return targets;
    }

    targets.reserve(project.frameIndex().size());
    for(const auto& marker: project.frameIndex()){
        if(marker.time100ns > 0 && marker.time100ns < sourceDuration100ns){
            targets.push_back(marker.time100ns);
        }
    }
    sort(targets.begin(), targets.end());
    targets.erase(unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

int64_t calculateOutputDuration100ns(int64_t totalDuration100ns, const vector<pair<int64_t, int64_t>>& cutRanges100ns){
    int64_t removedTotal100ns{};
    for(const auto& [start, end]: cutRanges100ns){
        if(end > start){
            removedTotal100ns += (end - start);
        }
    }
    return max<int64_t>(0, totalDuration100ns - removedTotal100ns);
}

bool projectHasRequestedCuts(const Project& project){
    return project.hasCanonicalFrameModel() && !project.cutPlanner().markers().empty()
        ? project.cutPlanner().hasRequestedCuts()
        : !project.buildCutRanges100ns().empty();
}

bool cutPlanUsesUnevaluatedSceneEdgeMarkers(const Project& project){
    if(project.hasCanonicalFrameModel() && !project.cutPlanner().markers().empty() && project.cutPlanner().hasRequestedCuts()){
        return project.cutPlanner().needsRapAlignment();
    }
    const auto& markers{project.frameIndex()};
    if(markers.empty()){
        return false;
    }

    const auto sceneCount{markers.size() + 1};
    vector<bool> isCut(sceneCount, false);
    for(const auto sceneIndex: project.cutScenes()){
        if(sceneIndex < sceneCount){
            isCut[sceneIndex] = true;
        }
    }

    for(size_t boundaryIndex{1}; boundaryIndex < sceneCount; ++boundaryIndex){
        if(isCut[boundaryIndex - 1] == isCut[boundaryIndex]){
            continue;
        }
        if(!markers[boundaryIndex - 1].cleanPoint){
            return true;
        }
    }

    return false;
}

bool effectiveExportPlanNeedsRapAlignment(const Project& project){
    return projectHasRequestedCuts(project) && cutPlanUsesUnevaluatedSceneEdgeMarkers(project);
}

EffectiveExportPlan buildDirectEffectiveExportPlan(const Project& project, int64_t sourceDuration100ns){
    const auto framePlan{project.buildFrameExportPlan(false)};
    const auto cutRanges{project.buildCutRanges100ns()};
    const auto outputDuration{project.outputDuration100ns()};
    return EffectiveExportPlan{
        .sourceFrameCount = framePlan.sourceFrameCount,
        .requestedCutRanges = framePlan.requestedCutRanges,
        .effectiveCutRanges = framePlan.effectiveCutRanges,
        .requestedOutputFrameCount = framePlan.requestedOutputFrameCount,
        .effectiveOutputFrameCount = framePlan.effectiveOutputFrameCount,
        .requestedCutRanges100ns = cutRanges,
        .effectiveCutRanges100ns = cutRanges,
        .sourceDuration100ns = sourceDuration100ns,
        .requestedOutputDuration100ns = outputDuration,
        .effectiveOutputDuration100ns = outputDuration,
        .hasRequestedCuts = !cutRanges.empty(),
        .emptyAfterAlignment = false,
        .materiallyDifferent = false,
    };
}

ExportPreflightState buildExportPreflightState(const Project& project, bool hasMedia, bool supportsLosslessExport, bool hasSupportedExportExtensions, bool sourceHasAudio, bool supportsAudioExport){
    ExportPreflightState state{
        .requestedOutputDuration100ns = project.outputDuration100ns(),
        .requestedCutBlockCount = project.buildCutRanges100ns().size(),
    };

    if(!project.hasVideoFile() || project.videoFilePath().empty()){
        state.blockMessage = L"Load a video before exporting.";
        return state;
    }
    if(!hasMedia){
        state.blockMessage = L"The current source is not ready for export yet.";
        return state;
    }
    if(!supportsLosslessExport || !hasSupportedExportExtensions){
        state.blockMessage = L"Lossless export is not supported for the current source.";
        return state;
    }
    if(project.keepAudio() && sourceHasAudio && !supportsAudioExport){
        state.blockMessage = L"Audio export is not supported for the current source.";
        return state;
    }

    state.canExport = true;
    state.needsRapReevaluation = effectiveExportPlanNeedsRapAlignment(project);
    return state;
}

EffectiveExportPlanSummary summarizeEffectiveExportPlan(const EffectiveExportPlan& plan){
    EffectiveExportPlanSummary summary{};
    const auto comparableCount{min(plan.requestedCutRanges100ns.size(), plan.effectiveCutRanges100ns.size())};
    for(size_t i{}; i < comparableCount; ++i){
        const auto& requested{plan.requestedCutRanges100ns[i]};
        const auto& effective{plan.effectiveCutRanges100ns[i]};
        if(requested.first != effective.first){
            ++summary.repositionedMarkers;
        }
        if(requested.second != effective.second){
            ++summary.repositionedMarkers;
        }

        const auto requestedDuration{max<int64_t>(0, requested.second - requested.first)};
        const auto effectiveDuration{max<int64_t>(0, effective.second - effective.first)};
        if(effectiveDuration < requestedDuration){
            ++summary.shrunkCutScenes;
            summary.shrunkTotal100ns += (requestedDuration - effectiveDuration);
        }
    }
    return summary;
}

ExportOverlayEstimates buildExportOverlayEstimates(uint64_t sourceSizeBytes, int64_t sourceDuration100ns, int64_t outputDuration100ns, bool keepAudio, bool sourceHasAudio, uint64_t audioBitrateBytesPerSecond){
    ExportOverlayEstimates estimates{};
    if(sourceSizeBytes == 0 || sourceDuration100ns <= 0){
        return estimates;
    }

    estimates.estimatedTargetBytes = static_cast<uint64_t>(llround(
        static_cast<long double>(sourceSizeBytes)
        * static_cast<long double>(max<int64_t>(0, outputDuration100ns))
        / static_cast<long double>(sourceDuration100ns)));
    estimates.estimatedSavingsBytes = sourceSizeBytes > estimates.estimatedTargetBytes ? (sourceSizeBytes - estimates.estimatedTargetBytes) : uint64_t{};

    if(!keepAudio && sourceHasAudio && audioBitrateBytesPerSecond > 0 && outputDuration100ns > 0){
        estimates.estimatedDroppedAudioBytes = static_cast<uint64_t>(llround(
            static_cast<long double>(audioBitrateBytesPerSecond)
            * static_cast<long double>(outputDuration100ns)
            / 10000000.0L));
    }

    return estimates;
}

using namespace winrt;
using namespace winrt::Windows::Storage;

constexpr auto P_FILE_PATH{L"file_path"};
constexpr auto P_STORYLINE_ZOOM{L"storyline_zoom"};
constexpr auto P_KEEP_AUDIO{L"keep_audio"};
constexpr auto P_AUDIO_CROSSFADE_MS{L"audio_crossfade_ms"};
constexpr auto P_AUDIO_VOLUME_PCT{L"audio_volume_pct"};
constexpr auto P_CUT_MARKERS{L"cut_markers"};
constexpr auto P_RAP_CUT_MARKERS{L"RAP_cut_markers"};
constexpr auto P_CUT_SCENES{L"cut_scenes"};
constexpr auto P_FRAME_MARKERS{L"frame_markers"};
constexpr auto P_RAP_FRAMES{L"rap_frames"};
constexpr auto P_SOURCE_FRAME_COUNT{L"source_frame_count"};

struct LoadedProjectData{
    wstring loadedFilePath;
    wstring zoomLevel;
    wstring keepAudio;
    wstring audioXfadeMs;
    wstring audioVolumePct;
    wstring markers;
    wstring rapMarkers;
    wstring cutScenes;
    wstring frameMarkers;
    wstring rapFrames;
    wstring sourceFrameCount;
};

LoadedProjectData _parseProjectLines(const Windows::Foundation::Collections::IVector<winrt::hstring>& lines){
    LoadedProjectData data{};

    for(const auto& lineH: lines){
        const wstring line{lineH.c_str()};
        const auto trimmed{trim(line)};
        if(trimmed.empty() || trimmed[0] == L'#'){
            continue;
        }

        const auto eqPos{line.find(L'=')};
        if(eqPos == wstring::npos){
            continue;
        }

        const auto key{trim(line.substr(0, eqPos))};
        const auto value{trim(line.substr(eqPos + 1))};

        if(key == P_FILE_PATH)          { data.loadedFilePath = value; } else
        if(key == P_STORYLINE_ZOOM)     { data.zoomLevel      = value; } else
        if(key == P_CUT_MARKERS)        { data.markers        = value; } else
        if(key == P_RAP_CUT_MARKERS)    { data.rapMarkers     = value; } else
        if(key == P_CUT_SCENES)         { data.cutScenes      = value; } else
        if(key == P_FRAME_MARKERS)      { data.frameMarkers   = value; } else
        if(key == P_RAP_FRAMES)         { data.rapFrames      = value; } else
        if(key == P_SOURCE_FRAME_COUNT) { data.sourceFrameCount = value; } else
        if(key == P_KEEP_AUDIO)         { data.keepAudio      = value; } else
        if(key == P_AUDIO_CROSSFADE_MS) { data.audioXfadeMs   = value; }
        if(key == P_AUDIO_VOLUME_PCT)   { data.audioVolumePct = value; }
    }

    return data;
}

void Project::clearTimeline(){
    m_frameIndex.clear();
    m_selectedKeyFrames.clear();
    m_cutScenes.clear();
    m_cutPlanner.reset();
    m_timelineDuration100ns = 0;
}

Project::AAction Project::open(const ::winrt::Windows::Storage::StorageFile& file){
    const auto lines{co_await FileIO::ReadLinesAsync(file)};
    auto projectData{_parseProjectLines(lines)};

    m_loadedFilePath = winrt::hstring(projectData.loadedFilePath);
    m_keepAudio = projectData.keepAudio != L"0";
    try{ audioXfadeMs(stoi(projectData.audioXfadeMs)); } catch(...){}
    try{ audioVolumePct(stoi(projectData.audioVolumePct)); } catch(...){}
    try{ m_zoom = stod(projectData.zoomLevel); } catch(...){}

    m_frameIndex = std::move(_parseKeyframeVector(projectData.markers));
    auto evaluatedMarkerTimes{parseInt64List(projectData.rapMarkers)};
    sort(evaluatedMarkerTimes.begin(), evaluatedMarkerTimes.end());
    evaluatedMarkerTimes.erase(unique(evaluatedMarkerTimes.begin(), evaluatedMarkerTimes.end()), evaluatedMarkerTimes.end());
    for(auto& marker: m_frameIndex){
        const auto evaluated{binary_search(evaluatedMarkerTimes.begin(), evaluatedMarkerTimes.end(), marker.time100ns)};
        marker.evaluated = evaluated;
        marker.cleanPoint = evaluated;
    }
    sort(m_frameIndex.begin(), m_frameIndex.end(), [](const auto& a, const auto& b){ return a.time100ns < b.time100ns; });
    refreshSelectedMarkers();

    const auto markerCount{m_frameIndex.size()};
    const auto sceneCount{static_cast<uint32_t>(markerCount + 1)};
    const auto cutScenes{parseIndexList(projectData.cutScenes)};
    m_cutScenes.clear();
    for(const auto sceneIndex: cutScenes){
        if(sceneIndex < sceneCount){
            m_cutScenes.push_back(sceneIndex);
        }
    }
    sort(m_cutScenes.begin(), m_cutScenes.end());
    m_cutScenes.erase(unique(m_cutScenes.begin(), m_cutScenes.end()), m_cutScenes.end());
    if(!projectData.frameMarkers.empty()){
        m_cutPlanner.markers().replaceAll(_parsePlannerMarkers(projectData.frameMarkers));
        vector<FrameIndex> rapFrames;
        for(const auto value: parseInt64List(projectData.rapFrames)){
            if(value >= 0){ rapFrames.push_back(static_cast<FrameIndex>(value)); }
        }
        m_cutPlanner.rapFrames().replaceAll(move(rapFrames));
        m_cutPlanner.cutScenes().replaceCutSceneIndexes(m_cutScenes);
        try{ m_sourceFrameCount = static_cast<FrameIndex>(stoull(projectData.sourceFrameCount)); } catch(...){}
        // Canonical projects never re-enter the time-based edit model.  The
        // legacy vectors above were only used to parse compatible cut indexes.
        m_frameIndex.clear();
        m_selectedKeyFrames.clear();
        m_cutScenes.clear();
    }else{
        _syncCutPlannerFromLegacyState();
    }

    m_lastSavedProjectSnapshot = _buildProjectSnapshot();
    m_isDirty = false;
}

Project::AAction Project::save(const ::winrt::Windows::Storage::StorageFile& file){
    vector<hstring> lines;
    lines.emplace_back(L"# llvc project file");
    lines.emplace_back(wstring(P_FILE_PATH) + L"=" + wstring(m_loadedFilePath.c_str()));
    lines.emplace_back(wstring(P_STORYLINE_ZOOM) + L"=" + to_wstring(m_zoom));
    lines.emplace_back(wstring(P_KEEP_AUDIO) + L"=" + wstring(m_keepAudio ? L"1" : L"0"));
    lines.emplace_back(wstring(P_AUDIO_CROSSFADE_MS) + L"=" + to_wstring(m_audioCrossfadeMs));
    lines.emplace_back(wstring(P_AUDIO_VOLUME_PCT) + L"=" + to_wstring(m_audioVolumePct));
    lines.emplace_back(wstring(P_CUT_SCENES) + L"=" + serializeIndexList(vector<uint32_t>{m_cutPlanner.cutScenes().cutSceneIndexes().begin(), m_cutPlanner.cutScenes().cutSceneIndexes().end()}));
    lines.emplace_back(wstring(P_FRAME_MARKERS) + L"=" + _serializePlannerMarkers(m_cutPlanner.markers().markers()));
    vector<int64_t> plannerRapFrames;
    plannerRapFrames.reserve(m_cutPlanner.rapFrames().size());
    for(const auto frame: m_cutPlanner.rapFrames().frames()){
        plannerRapFrames.push_back(static_cast<int64_t>(min<FrameIndex>(frame, static_cast<FrameIndex>(numeric_limits<int64_t>::max()))));
    }
    lines.emplace_back(wstring(P_RAP_FRAMES) + L"=" + serializeInt64List(plannerRapFrames));
    lines.emplace_back(wstring(P_SOURCE_FRAME_COUNT) + L"=" + to_wstring(m_sourceFrameCount));

    co_await FileIO::WriteLinesAsync(file, single_threaded_vector<hstring>(move(lines)));
    m_lastSavedProjectSnapshot = _buildProjectSnapshot();
    m_isDirty = false;
}

bool Project::isDirty() const{
    const auto curr{_buildProjectSnapshot()};
    const auto v{m_isDirty || curr != m_lastSavedProjectSnapshot};
    return v;
}

void Project::markClean(){
    m_lastSavedProjectSnapshot = _buildProjectSnapshot();
    m_isDirty = false;
}

void Project::markDirty(){
    m_isDirty = true;
}

void Project::videoFilePath(const ::winrt::hstring& path){
    m_isDirty = false;
    m_loadedFilePath = path;
}

::winrt::hstring Project::videoFileName() const{
    if(m_loadedFilePath.empty()){
        return {};
    }
    return ::winrt::hstring{::std::filesystem::path(m_loadedFilePath.c_str()).filename().wstring()};
}

void Project::setZoom(double v){
    m_isDirty = (m_isDirty || m_zoom != v);
    m_zoom = v;
}

void Project::setZoomWithoutDirty(double v){
    m_zoom = v;
    m_lastSavedProjectSnapshot = _buildProjectSnapshot();
    m_isDirty = false;
}

void Project::keepAudio(bool v){
    m_isDirty = (m_isDirty || m_keepAudio != v);
    m_keepAudio = v;
}

constexpr array<int32_t, 9> AUDIO_CROSSFADE_PRESETS_MS{{0, 50, 100, 250, 500, 750, 1000, 2000, 5000}};

void Project::audioXfadeMs(int32_t valueMs){
    const auto nearest{min_element(AUDIO_CROSSFADE_PRESETS_MS.begin(), AUDIO_CROSSFADE_PRESETS_MS.end(), [valueMs](auto a, auto b){
        return abs(a - valueMs) < abs(b - valueMs);
    })};
    const auto v{nearest == AUDIO_CROSSFADE_PRESETS_MS.end() ? 0 : *nearest};
    m_isDirty = (m_isDirty || m_audioCrossfadeMs != v);
    m_audioCrossfadeMs = v;
}

void Project::audioVolumePct(int32_t valuePct){
    const auto clamped{clamp(valuePct, 0, 250)};
    const auto rounded{(clamped / 5) * 5};
    m_isDirty = (m_isDirty || m_audioVolumePct != rounded);
    m_audioVolumePct = rounded;
}

void Project::frameIndex(const vector<IndexedFrameSample>& v){
    m_frameIndex = std::move(v);
    _syncCutPlannerFromLegacyState();
    m_isDirty = true;
}

void Project::cutScenes(const vector<uint32_t>& v){
    m_cutScenes = std::move(v);
    _syncCutPlannerFromLegacyState();
    m_isDirty = true;
}

void Project::synchronizeCutPlannerFrames(FrameIndex sourceFrameCount){
    if(sourceFrameCount == 0 || m_timelineDuration100ns <= 0){
        return;
    }

    if(m_sourceFrameCount == sourceFrameCount && !m_cutPlanner.markers().empty()){
        return;
    }
    // Canonical project files persist the planner directly.  Their legacy
    // time-marker list is intentionally empty, so metadata arrival must not
    // rebuild the planner from that compatibility-only representation.
    if(m_frameIndex.empty() && !m_cutPlanner.markers().empty()){
        m_sourceFrameCount = sourceFrameCount;
        return;
    }
    const auto legacyCutRanges{buildCutRanges100ns()};
    m_sourceFrameCount = sourceFrameCount;
    vector<EditMarker> markers;
    markers.reserve(m_frameIndex.size());
    for(const auto& marker: m_frameIndex){
        const auto frameIndex{static_cast<FrameIndex>((static_cast<long double>(max<int64_t>(0, marker.time100ns)) * sourceFrameCount) / m_timelineDuration100ns)};
        markers.push_back(EditMarker{.frameIndex = min(frameIndex, sourceFrameCount - 1), .isEvaluatedAgainstRap = marker.evaluated, .wasMovedForRapSafety = false});
    }
    m_cutPlanner.markers().replaceAll(move(markers));

    vector<uint32_t> plannerCutScenes;
    for(const auto& scene: m_cutPlanner.buildSceneRanges(sourceFrameCount)){
        const auto midpointFrame{scene.firstFrame + (scene.endFrameExclusive - scene.firstFrame) / 2};
        const auto midpointTime100ns{static_cast<int64_t>((static_cast<long double>(midpointFrame) * m_timelineDuration100ns) / sourceFrameCount)};
        if(any_of(legacyCutRanges.begin(), legacyCutRanges.end(), [midpointTime100ns](const auto& range){ return midpointTime100ns >= range.first && midpointTime100ns < range.second; })){
            plannerCutScenes.push_back(scene.sceneIndex);
        }
    }
    m_cutPlanner.cutScenes().replaceCutSceneIndexes(move(plannerCutScenes));
    // Legacy timestamp/index fields are load-migration input only. Once the
    // source frame model is known, CutPlanner is the sole edit authority.
    m_frameIndex.clear();
    m_selectedKeyFrames.clear();
    m_cutScenes.clear();
}

void Project::synchronizeLegacyEditModelFromCutPlanner(FrameIndex sourceFrameCount){
    if(sourceFrameCount == 0 || m_timelineDuration100ns <= 0){
        return;
    }

    vector<IndexedFrameSample> markers;
    markers.reserve(m_cutPlanner.markers().size());
    for(const auto& marker: m_cutPlanner.markers().markers()){
        auto time100ns{static_cast<int64_t>((static_cast<long double>(marker.frameIndex) * m_timelineDuration100ns) / sourceFrameCount)};
        if(marker.isEvaluatedAgainstRap){
            const auto existingRapMarker{find_if(m_frameIndex.begin(), m_frameIndex.end(), [&](const auto& existing){
                const auto existingFrame{static_cast<FrameIndex>((static_cast<long double>(max<int64_t>(0, existing.time100ns)) * sourceFrameCount) / m_timelineDuration100ns)};
                return existing.evaluated && existingFrame == marker.frameIndex;
            })};
            if(existingRapMarker != m_frameIndex.end()){
                time100ns = existingRapMarker->time100ns;
            }
        }
        markers.push_back({.time100ns = time100ns, .duration100ns = 0, .cleanPoint = marker.isEvaluatedAgainstRap, .evaluated = marker.isEvaluatedAgainstRap, .sampleIndex = static_cast<uint32_t>(min<FrameIndex>(marker.frameIndex, numeric_limits<uint32_t>::max()))});
    }
    m_frameIndex = move(markers);
    m_cutScenes.assign(m_cutPlanner.cutScenes().cutSceneIndexes().begin(), m_cutPlanner.cutScenes().cutSceneIndexes().end());
    refreshSelectedMarkers();
}

vector<IndexedFrameSample> Project::buildRapMarkersFromSelection() const{
    auto rapMarkers{m_frameIndex};
    sort(rapMarkers.begin(), rapMarkers.end(), [](const IndexedFrameSample& a, const IndexedFrameSample& b){ return a.time100ns < b.time100ns; });
    rapMarkers.erase(unique(rapMarkers.begin(), rapMarkers.end(), [](const IndexedFrameSample& a, const IndexedFrameSample& b){ return a.time100ns == b.time100ns; }), rapMarkers.end());
    return rapMarkers;
}

vector<pair<int64_t, int64_t>> Project::buildCutRanges100ns() const{
    if(m_sourceFrameCount > 0 && !m_cutPlanner.markers().empty()){
        vector<pair<int64_t, int64_t>> ranges;
        for(const auto& range: m_cutPlanner.buildCutSceneRanges(m_sourceFrameCount)){
            const auto start{static_cast<int64_t>((static_cast<long double>(range.firstFrame) * m_timelineDuration100ns) / m_sourceFrameCount)};
            const auto end{static_cast<int64_t>((static_cast<long double>(range.endFrameExclusive) * m_timelineDuration100ns) / m_sourceFrameCount)};
            if(end > start){ ranges.emplace_back(start, end); }
        }
        return ranges;
    }
    const auto boundaries{buildSceneBoundaries100ns()};
    if(boundaries.size() < 2){
        return {};
    }

    const auto sceneCount{boundaries.size() - 1};
    vector<pair<int64_t, int64_t>> ranges;
    for(const auto sceneIndex: m_cutScenes){
        if(sceneIndex >= sceneCount){
            continue;
        }
        const auto start{boundaries[sceneIndex]};
        const auto end{boundaries[sceneIndex + 1]};
        if(end > start){
            ranges.emplace_back(start, end);
        }
    }

    sort(ranges.begin(), ranges.end());
    vector<pair<int64_t, int64_t>> merged;
    for(const auto& range: ranges){
        if(merged.empty() || range.first > merged.back().second){
            merged.push_back(range);
        } else{
            merged.back().second = max(merged.back().second, range.second);
        }
    }
    return merged;
}

EffectiveFrameExportPlan Project::buildFrameExportPlan(bool rapAligned) const{
    if(m_sourceFrameCount == 0){
        return {};
    }
    return rapAligned
        ? m_cutPlanner.buildRapAlignedExportPlan(m_sourceFrameCount)
        : m_cutPlanner.buildDirectExportPlan(m_sourceFrameCount);
}

vector<int64_t> Project::buildSceneBoundaries100ns() const{
    if(m_sourceFrameCount > 0 && !m_cutPlanner.markers().empty()){
        vector<int64_t> boundaries{0};
        for(const auto& marker: m_cutPlanner.markers().markers()){
            boundaries.push_back(static_cast<int64_t>((static_cast<long double>(min(marker.frameIndex, m_sourceFrameCount)) * m_timelineDuration100ns) / m_sourceFrameCount));
        }
        boundaries.push_back(m_timelineDuration100ns);
        sort(boundaries.begin(), boundaries.end());
        boundaries.erase(unique(boundaries.begin(), boundaries.end()), boundaries.end());
        return boundaries;
    }
    const auto totalDuration100ns{m_timelineDuration100ns};
    const auto markers{buildRapMarkersFromSelection()};
    vector<int64_t> boundaries;
    boundaries.reserve(markers.size() + 2);
    boundaries.push_back(0);
    for(const auto& marker: markers){
        boundaries.push_back(clamp<int64_t>(marker.time100ns, 0, totalDuration100ns));
    }
    boundaries.push_back(totalDuration100ns);
    sort(boundaries.begin(), boundaries.end());
    boundaries.erase(unique(boundaries.begin(), boundaries.end()), boundaries.end());
    if(boundaries.empty() || boundaries.front() != 0){
        boundaries.insert(boundaries.begin(), 0);
    }
    if(boundaries.back() != totalDuration100ns){
        boundaries.push_back(totalDuration100ns);
    }
    return boundaries;
}

EffectiveExportPlan Project::buildEffectiveExportPlanWithRapPreroll(int64_t totalDuration100ns, const vector<int64_t>& rapTimes100ns, const function<void(double)>& progressCallback) const{
    EffectiveExportPlan plan{};
    plan.sourceDuration100ns = max<int64_t>(0, totalDuration100ns);
    if(m_sourceFrameCount > 0 && !m_cutPlanner.markers().empty()){
        // Reuse canonical RAP boundaries for unevaluated scenes, but do not
        // realign a plan whose markers have already been reevaluated.
        const auto framePlan{buildFrameExportPlan(effectiveExportPlanNeedsRapAlignment(*this))};
        plan.sourceFrameCount = framePlan.sourceFrameCount;
        plan.requestedCutRanges = framePlan.requestedCutRanges;
        plan.effectiveCutRanges = framePlan.effectiveCutRanges;
        plan.requestedOutputFrameCount = framePlan.requestedOutputFrameCount;
        plan.effectiveOutputFrameCount = framePlan.effectiveOutputFrameCount;
        plan.hasRequestedCuts = framePlan.hasRequestedCuts;
        plan.emptyAfterAlignment = framePlan.emptyAfterAlignment;
        plan.materiallyDifferent = framePlan.materiallyDifferent;
        const auto toTimeRanges{[&](const vector<ExportFrameRange>& ranges){
            vector<pair<int64_t, int64_t>> times;
            times.reserve(ranges.size());
            for(const auto& range: ranges){
                const auto start{static_cast<int64_t>((static_cast<long double>(range.firstFrame) * plan.sourceDuration100ns) / m_sourceFrameCount)};
                const auto end{static_cast<int64_t>((static_cast<long double>(range.endFrameExclusive) * plan.sourceDuration100ns) / m_sourceFrameCount)};
                if(end > start){ times.emplace_back(start, end); }
            }
            return times;
        }};
        plan.requestedCutRanges100ns = toTimeRanges(plan.requestedCutRanges);
        plan.effectiveCutRanges100ns = toTimeRanges(plan.effectiveCutRanges);
        plan.requestedOutputDuration100ns = calculateOutputDuration100ns(plan.sourceDuration100ns, plan.requestedCutRanges100ns);
        plan.effectiveOutputDuration100ns = calculateOutputDuration100ns(plan.sourceDuration100ns, plan.effectiveCutRanges100ns);
        if(progressCallback){ progressCallback(100.0); }
        return plan;
    }
    plan.requestedCutRanges100ns = buildCutRanges100ns();
    plan.hasRequestedCuts = !plan.requestedCutRanges100ns.empty();
    plan.requestedOutputDuration100ns = calculateOutputDuration100ns(plan.sourceDuration100ns, plan.requestedCutRanges100ns);

    if(!plan.hasRequestedCuts){
        plan.effectiveOutputDuration100ns = plan.requestedOutputDuration100ns;
        return plan;
    }

    const auto markers{buildRapMarkersFromSelection()};
    const auto totalMarkers{markers.size()};
    vector<int64_t> boundaries;
    boundaries.reserve(markers.size() + 2);
    boundaries.push_back(0);
    for(size_t markerIndex{}; markerIndex < markers.size(); ++markerIndex){
        const auto& marker{markers[markerIndex]};
        boundaries.push_back(clamp<int64_t>(marker.time100ns, 0, totalDuration100ns));
        if(progressCallback && totalMarkers > 0){
            progressCallback((100.0 * static_cast<double>(markerIndex + 1)) / static_cast<double>(totalMarkers));
        }
    }
    boundaries.push_back(totalDuration100ns);
    sort(boundaries.begin(), boundaries.end());
    boundaries.erase(unique(boundaries.begin(), boundaries.end()), boundaries.end());
    if(boundaries.empty() || boundaries.front() != 0){
        boundaries.insert(boundaries.begin(), 0);
    }
    if(boundaries.back() != totalDuration100ns){
        boundaries.push_back(totalDuration100ns);
    }
    if(boundaries.size() < 2){
        plan.emptyAfterAlignment = true;
        plan.materiallyDifferent = true;
        plan.effectiveOutputDuration100ns = plan.sourceDuration100ns;
        return plan;
    }

    const auto sceneCount{boundaries.size() - 1};
    vector<bool> isCut(sceneCount, false);
    for(const auto scene: m_cutScenes){
        if(scene < sceneCount){
            isCut[scene] = true;
        }
    }

    vector<pair<int64_t, int64_t>> rawCutBlocks;
    for(size_t i{}; i < sceneCount;){
        if(!isCut[i]){
            ++i;
            continue;
        }

        auto blockStart{i};
        auto blockEnd{i + 1};
        ++i;
        while(i < sceneCount && isCut[i]){
            blockEnd = i + 1;
            ++i;
        }

        const auto start{boundaries[blockStart]};
        const auto end{boundaries[blockEnd]};
        if(end > start){
            rawCutBlocks.emplace_back(start, end);
        }
    }

    vector<pair<int64_t, int64_t>> cutRanges;
    cutRanges.reserve(rawCutBlocks.size());
    for(size_t blockIndex{}; blockIndex < rawCutBlocks.size(); ++blockIndex){
        const auto& [start, end]{rawCutBlocks[blockIndex]};
        int64_t alignedStart{};
        if(start <= 0){
            alignedStart = 0;
        }else if(end >= totalDuration100ns){
            alignedStart = start;
        }else{
            const auto startRapIt{lower_bound(rapTimes100ns.begin(), rapTimes100ns.end(), start)};
            if(startRapIt == rapTimes100ns.end()){
                continue;
            }
            alignedStart = *startRapIt;
        }

        int64_t alignedEnd{};
        if(end >= totalDuration100ns){
            alignedEnd = totalDuration100ns;
        }else{
            const auto endRapIt{upper_bound(rapTimes100ns.begin(), rapTimes100ns.end(), end)};
            if(endRapIt == rapTimes100ns.begin()){
                continue;
            }
            alignedEnd = *(endRapIt - 1);
        }

        if(alignedEnd > alignedStart){
            cutRanges.emplace_back(alignedStart, alignedEnd);
        }
    }

    if(cutRanges.empty()){
        plan.emptyAfterAlignment = true;
        plan.materiallyDifferent = true;
        plan.effectiveOutputDuration100ns = plan.sourceDuration100ns;
        return plan;
    }

    sort(cutRanges.begin(), cutRanges.end());
    vector<pair<int64_t, int64_t>> mergedCuts;
    for(const auto& range: cutRanges){
        if(mergedCuts.empty() || range.first > mergedCuts.back().second){
            mergedCuts.push_back(range);
        } else{
            mergedCuts.back().second = max(mergedCuts.back().second, range.second);
        }
    }

    plan.effectiveCutRanges100ns = std::move(mergedCuts);
    plan.effectiveOutputDuration100ns = calculateOutputDuration100ns(plan.sourceDuration100ns, plan.effectiveCutRanges100ns);
    plan.materiallyDifferent = plan.requestedCutRanges100ns != plan.effectiveCutRanges100ns;
    if(progressCallback){
        progressCallback(100.0);
    }
    return plan;
}

vector<pair<int64_t, int64_t>> Project::buildEffectiveCutRangesWithRapPreroll(int64_t totalDuration100ns, const vector<int64_t>& rapTimes100ns) const{
    return buildEffectiveExportPlanWithRapPreroll(totalDuration100ns, rapTimes100ns).effectiveCutRanges100ns;
}

int64_t Project::outputDuration100ns() const{
    return calculateOutputDuration100ns(m_timelineDuration100ns, buildCutRanges100ns());
}

void Project::refreshSelectedMarkers(){
    const auto markerCount{m_frameIndex.size()};
    const auto sav{std::move(m_selectedKeyFrames)};
    m_selectedKeyFrames.reserve(markerCount);
    for(uint32_t i{0}; i < markerCount; ++i){
        m_selectedKeyFrames.push_back(i);
    }
    const auto neq{sav != m_selectedKeyFrames};
    m_isDirty = (m_isDirty || neq);
}

void Project::_syncCutPlannerFromLegacyState(){
    vector<EditMarker> markers;
    markers.reserve(m_frameIndex.size());
    vector<FrameIndex> rapFrames;
    const auto existingRapFrames{m_cutPlanner.rapFrames().frames()};
    rapFrames.reserve(existingRapFrames.size() + m_frameIndex.size());
    rapFrames.insert(rapFrames.end(), existingRapFrames.begin(), existingRapFrames.end());
    for(const auto& marker: m_frameIndex){
        const auto frameIndex{static_cast<FrameIndex>(marker.sampleIndex)};
        markers.push_back(EditMarker{
            .frameIndex = frameIndex,
            .isEvaluatedAgainstRap = marker.evaluated,
            .wasMovedForRapSafety = false,
        });
        if(marker.cleanPoint || marker.evaluated){
            rapFrames.push_back(frameIndex);
        }
    }

    m_cutPlanner.markers().replaceAll(move(markers));
    m_cutPlanner.rapFrames().replaceAll(move(rapFrames));
    m_cutPlanner.cutScenes().replaceCutSceneIndexes(m_cutScenes);
}

wstring Project::_serializePlannerMarkers(span<const EditMarker> markers){
    wstring text;
    for(const auto& marker: markers){
        if(!text.empty()){ text += L","; }
        text += to_wstring(marker.frameIndex);
        text += marker.isEvaluatedAgainstRap ? L":1" : L":0";
        text += marker.wasMovedForRapSafety ? L":1" : L":0";
    }
    return text;
}

vector<EditMarker> Project::_parsePlannerMarkers(const wstring& text){
    vector<EditMarker> markers;
    wistringstream items{text};
    for(wstring item; getline(items, item, L',');){
        wistringstream fields{trim(item)};
        vector<wstring> parts;
        for(wstring field; getline(fields, field, L':');){ parts.push_back(move(field)); }
        if(parts.empty() || parts.front().empty()){ continue; }
        try{
            markers.push_back(EditMarker{
                .frameIndex = static_cast<FrameIndex>(stoull(parts[0])),
                .isEvaluatedAgainstRap = parts.size() > 1 && parts[1] == L"1",
                .wasMovedForRapSafety = parts.size() > 2 && parts[2] == L"1",
            });
        }catch(...){ }
    }
    return markers;
}

void Project::remapCutScenesAfterMarkerRemoval(uint32_t removePos){
    vector<uint32_t> updatedCuts;
    updatedCuts.reserve(m_cutScenes.size());
    for(const auto sceneIndex: m_cutScenes){
        if(sceneIndex < removePos){
            updatedCuts.push_back(sceneIndex);
        }else if(sceneIndex == removePos || sceneIndex == (removePos + 1)){
            updatedCuts.push_back(removePos);
        }else{
            updatedCuts.push_back(sceneIndex - 1);
        }
    }

    vector<uint32_t> result;
    result.reserve(updatedCuts.size());
    for(const auto sceneIndex: updatedCuts){
        if(sceneIndex < m_frameIndex.size()){
            result.push_back(sceneIndex);
        }
    }
    sort(result.begin(), result.end());
    result.erase(unique(result.begin(), result.end()), result.end());

    m_cutScenes = std::move(result);
}

void Project::remapCutScenesAfterMarkerInsertion(uint32_t insertPos){
    vector<uint32_t> updatedCuts;
    updatedCuts.reserve(m_cutScenes.size() + 1);
    for(const auto sceneIndex: m_cutScenes){
        if(sceneIndex < insertPos){
            updatedCuts.push_back(sceneIndex);
        } else if(sceneIndex == insertPos){
            updatedCuts.push_back(sceneIndex);
            updatedCuts.push_back(sceneIndex + 1);
        } else{
            updatedCuts.push_back(sceneIndex + 1);
        }
    }
    sort(updatedCuts.begin(), updatedCuts.end());
    updatedCuts.erase(unique(updatedCuts.begin(), updatedCuts.end()), updatedCuts.end());
    m_cutScenes = std::move(updatedCuts);
}

void Project::timelineDuration100ns(int64_t duration){
    m_timelineDuration100ns = max<int64_t>(0, duration);
}

bool Project::toggleSelectedKeyframeAtTime100ns(int64_t time100ns, double fps){
    if(m_timelineDuration100ns <= 0){
        return false;
    }

    constexpr int64_t hitTolerance100ns{50'000};
    const auto clicked100ns{clamp<int64_t>(time100ns, 0, m_timelineDuration100ns)};

    auto nearestIndex{m_frameIndex.size()};
    int64_t nearestDistance{hitTolerance100ns + 1};
    for(size_t i{0}; i < m_frameIndex.size(); ++i){
        const auto distance{llabs(m_frameIndex[i].time100ns - clicked100ns)};
        if(distance <= hitTolerance100ns && distance < nearestDistance){
            nearestDistance = distance;
            nearestIndex = i;
        }
    }

    if(nearestIndex != m_frameIndex.size()){
        const auto removePos{static_cast<uint32_t>(nearestIndex)};
        remapCutScenesAfterMarkerRemoval(removePos);
        m_frameIndex.erase(m_frameIndex.begin() + removePos);
    } else{
        const auto insertIt{lower_bound(m_frameIndex.begin(), m_frameIndex.end(), clicked100ns, [](const IndexedFrameSample& a, int64_t t){
            return a.time100ns < t;
        })};
        const auto insertPos{static_cast<uint32_t>(distance(m_frameIndex.begin(), insertIt))};

        remapCutScenesAfterMarkerInsertion(insertPos);

        const auto frameNumber{static_cast<uint32_t>(clicked100ns / 10'000'000.0 * fps)};
        m_frameIndex.insert(insertIt, IndexedFrameSample{.time100ns = clicked100ns, .duration100ns = 0, .cleanPoint = false, .evaluated = false, .sampleIndex = frameNumber});
    }

    refreshSelectedMarkers();
    _syncCutPlannerFromLegacyState();

    return true;
}

bool Project::toggleCutBlockAtTime100ns(int64_t time100ns){
    const auto sceneIndex{_sceneIndexAtTime100ns(time100ns)};
    if(!sceneIndex){
        return false;
    }

    const auto it{find(m_cutScenes.begin(), m_cutScenes.end(), *sceneIndex)};
    if(it == m_cutScenes.end()){
        m_cutScenes.push_back(static_cast<uint32_t>(*sceneIndex));
        sort(m_cutScenes.begin(), m_cutScenes.end());
    }else{
        m_cutScenes.erase(it);
    }

    _syncCutPlannerFromLegacyState();
    return true;
}

bool Project::setCutBlockAtTime100ns(int64_t time100ns, bool cutScene){
    const auto sceneIndex{_sceneIndexAtTime100ns(time100ns)};
    if(!sceneIndex){
        return false;
    }

    const auto it{find(m_cutScenes.begin(), m_cutScenes.end(), *sceneIndex)};
    if(cutScene){
        if(it == m_cutScenes.end()){
            m_cutScenes.push_back(static_cast<uint32_t>(*sceneIndex));
            sort(m_cutScenes.begin(), m_cutScenes.end());
        }
    } else if(it != m_cutScenes.end()){
        m_cutScenes.erase(it);
    }

    _syncCutPlannerFromLegacyState();
    return true;
}


std::optional<size_t> Project::_sceneIndexAtTime100ns(int64_t time100ns) const{
    if(m_timelineDuration100ns <= 0){
        return std::nullopt;
    }

    const auto clicked100ns{clamp<int64_t>(time100ns, 0, m_timelineDuration100ns)};
    const auto boundaries{buildSceneBoundaries100ns()};
    if(boundaries.size() < 2){
        return std::nullopt;
    }

    auto sceneIndex{boundaries.size() - 2};
    for(size_t i{0}; i + 1 < boundaries.size(); ++i){
        if(clicked100ns < boundaries[i + 1]){
            sceneIndex = i;
            break;
        }
    }

    return sceneIndex;
}

wstring Project::_buildProjectSnapshot() const{
    const auto plannerCuts{vector<uint32_t>{m_cutPlanner.cutScenes().cutSceneIndexes().begin(), m_cutPlanner.cutScenes().cutSceneIndexes().end()}};
    const auto snapshot{std::format(
        L"{}={:.15g}\n{}={}\n{}={}\n{}={}\n{}={}\n{}={}\n{}={}\n{}={}\n",
        P_STORYLINE_ZOOM,
        m_zoom,
        P_KEEP_AUDIO,
        m_keepAudio ? 1 : 0,
        P_AUDIO_CROSSFADE_MS,
        m_audioCrossfadeMs,
        P_AUDIO_VOLUME_PCT,
        m_audioVolumePct,
        P_CUT_SCENES,
        serializeIndexList(plannerCuts),
        P_FRAME_MARKERS,
        _serializePlannerMarkers(m_cutPlanner.markers().markers()),
        P_RAP_FRAMES,
        serializeInt64List(vector<int64_t>{m_cutPlanner.rapFrames().frames().begin(), m_cutPlanner.rapFrames().frames().end()}),
        P_SOURCE_FRAME_COUNT,
        m_sourceFrameCount
    )};
    return snapshot;
}

wstring Project::_serializeCutMarkers() const{
    wstring out;
    bool first{true};
    for(const auto& k: m_frameIndex){
        if(!first){ out += L";"; }
        first = false;
        out += std::format(L"{}", k.time100ns);
    }
    return out;
}

vector<IndexedFrameSample> Project::_parseKeyframeVector(const wstring& text){
    vector<IndexedFrameSample> out;
    size_t start{};
    while(start <= text.size()){
        const auto sep{text.find(L';', start)};
        const auto item{trim(text.substr(start, sep == wstring::npos ? wstring::npos : sep - start))};
        if(!item.empty()){
            const auto at{item.find(L'@')};
            const auto timeToken{trim(at == wstring::npos ? item : item.substr(0, at))};
            const auto sampleToken{at == wstring::npos ? wstring{} : trim(item.substr(at + 1))};
            try{
                const auto t{stoll(timeToken)};
                uint32_t sampleIndex{};
                if(!sampleToken.empty()){
                    sampleIndex = stoul(sampleToken);
                }
                out.push_back(IndexedFrameSample{.time100ns = t, .duration100ns = 0, .cleanPoint = false, .evaluated = false, .sampleIndex = sampleIndex});
            }catch(...){}
        }
        if(sep == wstring::npos){ break; }
        start = sep + 1;
    }
    return out;
}

}
