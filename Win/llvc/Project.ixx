module;

#include "pch.h"
#include <winrt/Windows.Storage.h>

export module llvc.Project;

import std;
import llvc.Utils;

export namespace llvc{

using namespace ::std;
using namespace ::winrt;

struct IndexedFrameSample{
    int64_t time100ns{};
    int64_t duration100ns{};
    bool cleanPoint{};
    uint32_t sampleIndex{};
};

struct Project final{
    using AAction = ::winrt::Windows::Foundation::IAsyncAction;
    using SFile = ::winrt::Windows::Storage::StorageFile;

    Project(): m_lastSavedProjectSnapshot{_buildProjectSnapshot()} {}

    void reset(){ new (this) Project; }
    void clearTimeline();
    AAction open(const SFile& file);
    AAction save(const SFile& file);
    bool isDirty() const;
    void videoFile(const SFile& f);
    void setZoom(double v);
    void keepAudio(bool v);
    void audioXfadeMs(int32_t valueMs);
    void audioVolumePct(int32_t valuePct);
    void frameIndex(const vector<IndexedFrameSample>& v);
    void cutScenes(const vector<uint32_t>& v);

    inline auto& videoFile()    const{ return m_loadedFile; }
    inline auto  zoom()         const{ return m_zoom; }
    inline bool  keepAudio()    const{ return m_keepAudio; }
    inline auto  audioXfadeMs() const{ return m_audioCrossfadeMs; }
    inline auto  audioVolumePct() const{ return m_audioVolumePct; }
    inline auto& frameIndex()   const{ return m_frameIndex; }
    inline auto& selKeyFrames() const{ return m_selectedKeyFrames; }
    inline auto& cutScenes()    const{ return m_cutScenes; }

    vector<IndexedFrameSample> buildRapMarkersFromSelection() const;
    vector<pair<int64_t, int64_t>> buildCutRanges100ns() const;
    vector<int64_t> buildSceneBoundaries100ns() const;
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

private:
    // these are persisted on disk
    SFile m_loadedFile{nullptr};
    double m_zoom{4};
    bool m_keepAudio{true};
    int32_t m_audioCrossfadeMs{0};
    int32_t m_audioVolumePct{100};
    vector<IndexedFrameSample> m_frameIndex{};
    vector<uint32_t> m_selectedKeyFrames{};
    vector<uint32_t> m_cutScenes{};

    int64_t m_timelineDuration100ns{0};
    bool m_isDirty{false};
    wstring m_lastSavedProjectSnapshot{}; // must be *after* all the members to reflect proper initial state
};


}

namespace llvc{

using namespace std;
using namespace winrt;
using namespace winrt::Windows::Storage;

constexpr auto P_FILE_PATH{L"file_path"};
constexpr auto P_STORYLINE_ZOOM{L"storyline_zoom"};
constexpr auto P_KEEP_AUDIO{L"keep_audio"};
constexpr auto P_AUDIO_CROSSFADE_MS{L"audio_crossfade_ms"};
constexpr auto P_AUDIO_VOLUME_PCT{L"audio_volume_pct"};
constexpr auto P_CUT_MARKERS{L"cut_markers"};
constexpr auto P_CUT_SCENES{L"cut_scenes"};

struct LoadedProjectData{
    wstring loadedFilePath;
    wstring zoomLevel;
    wstring keepAudio;
    wstring audioXfadeMs;
    wstring audioVolumePct;
    wstring markers;
    wstring cutScenes;
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
        if(key == P_CUT_SCENES)         { data.cutScenes      = value; } else
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
    m_timelineDuration100ns = 0;
}

Project::AAction Project::open(const SFile& file){
    const auto lines{co_await FileIO::ReadLinesAsync(file)};
    auto projectData{_parseProjectLines(lines)};

    m_loadedFile = co_await StorageFile::GetFileFromPathAsync(projectData.loadedFilePath);
    m_keepAudio = projectData.keepAudio != L"0";
    try{ audioXfadeMs(stoi(projectData.audioXfadeMs)); } catch(...){}
    try{ audioVolumePct(stoi(projectData.audioVolumePct)); } catch(...){}
    try{ m_zoom = stod(projectData.zoomLevel); } catch(...){}

    m_frameIndex = std::move(_parseKeyframeVector(projectData.markers));
    sort(m_frameIndex.begin(), m_frameIndex.end(), [](const auto& a, const auto& b){ return a.time100ns < b.time100ns; });
    refreshSelectedMarkers();

    const auto markerCount{m_frameIndex.size()};
    const auto sceneCount{static_cast<uint32_t>(markerCount + 1)};
    const auto cutScenes{parseIndexList(projectData.cutScenes)};
    for(const auto sceneIndex: cutScenes){
        if(sceneIndex < sceneCount){
            m_cutScenes.push_back(sceneIndex);
        }
    }
    sort(m_cutScenes.begin(), m_cutScenes.end());
    m_cutScenes.erase(unique(m_cutScenes.begin(), m_cutScenes.end()), m_cutScenes.end());

    m_lastSavedProjectSnapshot = _buildProjectSnapshot();
    m_isDirty = false;
}

Project::AAction Project::save(const SFile& file){
    vector<hstring> lines;
    lines.emplace_back(L"# llvc project file");
    lines.emplace_back(wstring(P_FILE_PATH) + L"=" + wstring(m_loadedFile ? m_loadedFile.Path().c_str() : L""));
    lines.emplace_back(wstring(P_STORYLINE_ZOOM) + L"=" + to_wstring(m_zoom));
    lines.emplace_back(wstring(P_KEEP_AUDIO) + L"=" + wstring(m_keepAudio ? L"1" : L"0"));
    lines.emplace_back(wstring(P_AUDIO_CROSSFADE_MS) + L"=" + to_wstring(m_audioCrossfadeMs));
    lines.emplace_back(wstring(P_AUDIO_VOLUME_PCT) + L"=" + to_wstring(m_audioVolumePct));
    lines.emplace_back(wstring(P_CUT_MARKERS) + L"=" + _serializeCutMarkers());
    lines.emplace_back(wstring(P_CUT_SCENES) + L"=" + serializeIndexList(m_cutScenes));

    co_await FileIO::WriteLinesAsync(file, single_threaded_vector<hstring>(move(lines)));
    m_lastSavedProjectSnapshot = _buildProjectSnapshot();
    m_isDirty = false;
}

bool Project::isDirty() const{
    const auto curr{_buildProjectSnapshot()};
    const auto v{m_isDirty || curr != m_lastSavedProjectSnapshot};
    return v;
}

void Project::videoFile(const SFile& f){
    m_isDirty = false;
    m_loadedFile = f;
}

void Project::setZoom(double v){
    m_isDirty = (m_isDirty || m_zoom != v);
    m_zoom = v;
}

void Project::keepAudio(bool v){
    m_isDirty = (m_isDirty || m_keepAudio != v);
    m_keepAudio = v;
}

constexpr array<int32_t, 10> AUDIO_CROSSFADE_PRESETS_MS{{0, 50, 100, 250, 500, 750, 1000, 2000, 5000}};

void Project::audioXfadeMs(int32_t valueMs){
    const auto nearest{min_element(AUDIO_CROSSFADE_PRESETS_MS.begin(), AUDIO_CROSSFADE_PRESETS_MS.end(), [valueMs](auto a, auto b){
        return abs(a - valueMs) < abs(b - valueMs);
    })};
    const auto v{nearest == AUDIO_CROSSFADE_PRESETS_MS.end() ? 0 : *nearest};
    m_isDirty = (m_isDirty || m_audioCrossfadeMs != v);
    m_audioCrossfadeMs = v;
}

void Project::audioVolumePct(int32_t valuePct){
    const auto clamped{clamp(valuePct, 0, 150)};
    const auto rounded{(clamped / 5) * 5};
    m_isDirty = (m_isDirty || m_audioVolumePct != rounded);
    m_audioVolumePct = rounded;
}

void Project::frameIndex(const vector<IndexedFrameSample>& v){
    m_frameIndex = std::move(v);
    m_isDirty = true;
}

void Project::cutScenes(const vector<uint32_t>& v){
    m_cutScenes = std::move(v);
    m_isDirty = true;
}

vector<IndexedFrameSample> Project::buildRapMarkersFromSelection() const{
    vector<IndexedFrameSample> rapMarkers;
    rapMarkers.reserve(m_selectedKeyFrames.size());
    for(const auto index: m_selectedKeyFrames){
        if(index < m_frameIndex.size()){
            rapMarkers.push_back(m_frameIndex[index]);
        }
    }
    sort(rapMarkers.begin(), rapMarkers.end(), [](const IndexedFrameSample& a, const IndexedFrameSample& b){ return a.time100ns < b.time100ns; });
    rapMarkers.erase(unique(rapMarkers.begin(), rapMarkers.end(), [](const IndexedFrameSample& a, const IndexedFrameSample& b){ return a.time100ns == b.time100ns; }), rapMarkers.end());
    return rapMarkers;
}

vector<pair<int64_t, int64_t>> Project::buildCutRanges100ns() const{
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

vector<int64_t> Project::buildSceneBoundaries100ns() const{
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

vector<pair<int64_t, int64_t>> Project::buildEffectiveCutRangesWithRapPreroll(int64_t totalDuration100ns, const vector<int64_t>& rapTimes100ns) const{
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
    if(boundaries.size() < 2){
        return {};
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
    for(const auto& [start, end]: rawCutBlocks){
        const auto startRapIt{lower_bound(rapTimes100ns.begin(), rapTimes100ns.end(), start)};
        if(startRapIt == rapTimes100ns.end()){
            continue;
        }

        const auto endRapIt{upper_bound(rapTimes100ns.begin(), rapTimes100ns.end(), end)};
        if(endRapIt == rapTimes100ns.begin()){
            continue;
        }

        const auto alignedStart{*startRapIt};
        const auto alignedEnd{*(endRapIt - 1)};
        if(alignedEnd > alignedStart){
            cutRanges.emplace_back(alignedStart, alignedEnd);
        }
    }

    if(cutRanges.empty()){
        return {};
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
    return mergedCuts;
}

int64_t Project::outputDuration100ns() const{
    const auto sourceDuration100ns{m_timelineDuration100ns};
    const auto cutRanges100ns{buildCutRanges100ns()};

    int64_t removedTotal100ns{};
    for(const auto& [start, end]: cutRanges100ns){
        removedTotal100ns += (end - start);
    }
    return max<int64_t>(0, sourceDuration100ns - removedTotal100ns);
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
        m_frameIndex.insert(insertIt, IndexedFrameSample{.time100ns = clicked100ns, .duration100ns = 0, .cleanPoint = true, .sampleIndex = frameNumber});
    }

    refreshSelectedMarkers();

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
    const auto snapshot{std::format(
        L"{}={:.15g}\n{}={}\n{}={}\n{}={}\n{}={}\n{}={}\n",
        P_STORYLINE_ZOOM,
        m_zoom,
        P_KEEP_AUDIO,
        m_keepAudio ? 1 : 0,
        P_AUDIO_CROSSFADE_MS,
        m_audioCrossfadeMs,
        P_AUDIO_VOLUME_PCT,
        m_audioVolumePct,
        P_CUT_MARKERS,
        _serializeCutMarkers(),
        P_CUT_SCENES,
        serializeIndexList(m_cutScenes)
    )};
    return snapshot;
}

wstring Project::_serializeCutMarkers() const{
    const auto markers{buildRapMarkersFromSelection()};
    wstring out;
    bool first{true};
    for(const auto& k: markers){
        if(!k.cleanPoint){ continue; }
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
                out.push_back(IndexedFrameSample{.time100ns = t, .duration100ns = 0, .cleanPoint = true, .sampleIndex = sampleIndex});
            }catch(...){}
        }
        if(sep == wstring::npos){ break; }
        start = sep + 1;
    }
    return out;
}

}
