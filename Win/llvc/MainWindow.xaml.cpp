#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"
#include "MainWindow.Helpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <functional>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

#include <microsoft.ui.xaml.window.h>
#include <shobjidl_core.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Editing.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>

#pragma comment(lib, "Shell32.lib")

using namespace std;
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Input;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Input;
using namespace Windows::Foundation;
using namespace Windows::Media::Playback;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Windows::System;
using namespace Windows::UI::Core;

namespace winrt::llvc::implementation{

using Control = MainWindow::Control;
using REArgs = MainWindow::REArgs;
using PREArgs = MainWindow::PREArgs;
using AAction = MainWindow::AAction;
using RBVArgs = MainWindow::RBVArgs;
using SVVCArgs = MainWindow::SVVCArgs;
using SCArgs = MainWindow::SCArgs;
using KRArgs = MainWindow::KRArgs;
using DEArgs = MainWindow::DEArgs;
using WEArgs = MainWindow::WEArgs;
using MPSession = MainWindow::MPSession;
using SFile = MainWindow::SFile;
using FState = MainWindow::FState;
using IOpBool = MainWindow::IOpBool;
using TS = MainWindow::TS;

constexpr auto W_POS_L{L"WindowLeft"};
constexpr auto W_POS_T{L"WindowTop"};
constexpr auto W_POS_W{L"WindowWidth"};
constexpr auto W_POS_H{L"WindowHeight"};
constexpr auto W_POS_DPI{L"WindowDpi"};
constexpr auto S_RECENT_VIDEOS{L"RecentVideos"};
constexpr auto S_RECENT_PROJECTS{L"RecentProjects"};
constexpr auto S_MAX_RECENT_VIDEOS{L"MaxRecentVideos"};
constexpr auto S_MAX_RECENT_PROJECTS{L"MaxRecentProjects"};
constexpr auto S_DEFAULT_MAX_RECENT{5};

constexpr auto P_FILE_PATH{L"file_path"};
constexpr auto P_STORYLINE_ZOOM{L"storyline_zoom"};
constexpr auto P_KEEP_AUDIO{L"keep_audio"};
constexpr auto P_AUDIO_CROSSFADE_MS{L"audio_crossfade_ms"};
constexpr auto P_RAP_MARKERS{L"rap_markers"};
constexpr auto P_CUT_SCENES{L"cut_scenes"};

constexpr auto PROJECT_KEY{L"llvc project"};
constexpr auto UNNAMED_PROJECT{L"Untitled video cut"};
constexpr auto PROJECT_EXT{L".llvc"};

constexpr array<int32_t, 8> AUDIO_CROSSFADE_PRESETS_MS{{0, 50, 100, 250, 500, 750, 1000}};

bool isControlModifierActive(VirtualKeyModifiers modifiers){
    if((modifiers & VirtualKeyModifiers::Control) == VirtualKeyModifiers::Control){
        return true;
    }

    const auto ctrlState{InputKeyboardSource::GetKeyStateForCurrentThread(VirtualKey::Control)};
    return (ctrlState & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
}

uint32_t estimateFrameNumberFromTime100ns(const Ratio& fps, int64_t time100ns){
    if(fps.den == 0){
        return 0;
    }
    return max<uint32_t>(0, static_cast<uint32_t>(time100ns / 10'000'000.0 * fps));
}

wstring formatDurationFileTag(int64_t duration100ns){
    const auto totalMs{max<int64_t>(0, (duration100ns + 5'000) / 10'000)};
    const auto minutes{totalMs / 60'000};
    const auto seconds{(totalMs / 1'000) % 60};
    const auto millis{totalMs % 1'000};
    return std::format(L"{:02}{:02}{:03}", minutes, seconds, millis);
}

vector<int64_t> buildSceneBoundaries100ns(const vector<IndexedFrameSample>& markers, int64_t totalDuration100ns){
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

vector<IndexedFrameSample> buildRapMarkersFromSelection(const vector<IndexedFrameSample>& markers, const vector<uint32_t>& selectedMarkerIndices){
    vector<IndexedFrameSample> rapMarkers;
    rapMarkers.reserve(selectedMarkerIndices.size());
    for(const auto index: selectedMarkerIndices){
        if(index < markers.size()){
            rapMarkers.push_back(markers[index]);
        }
    }
    sort(rapMarkers.begin(), rapMarkers.end(), [](const IndexedFrameSample& a, const IndexedFrameSample& b){
        return a.time100ns < b.time100ns;
    });
    rapMarkers.erase(unique(rapMarkers.begin(), rapMarkers.end(), [](const IndexedFrameSample& a, const IndexedFrameSample& b){
        return a.time100ns == b.time100ns;
    }), rapMarkers.end());
    return rapMarkers;
}

vector<pair<int64_t, int64_t>> buildCutRanges100ns(const vector<uint32_t>& cutScenes, const vector<IndexedFrameSample>& rapMarkers, int64_t totalDuration100ns){
    const auto boundaries{buildSceneBoundaries100ns(rapMarkers, totalDuration100ns)};
    if(boundaries.size() < 2){
        return {};
    }

    const auto sceneCount{boundaries.size() - 1};
    vector<pair<int64_t, int64_t>> ranges;
    for(const auto sceneIndex: cutScenes){
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
        }else{
            merged.back().second = max(merged.back().second, range.second);
        }
    }
    return merged;
}

vector<pair<int64_t, int64_t>> invertCutRanges100ns(const vector<pair<int64_t, int64_t>>& cutRanges, int64_t totalDuration100ns){
    vector<pair<int64_t, int64_t>> keepRanges;
    int64_t cursor{};
    for(const auto& [start, end]: cutRanges){
        if(start > cursor){
            keepRanges.emplace_back(cursor, start);
        }
        cursor = max(cursor, end);
    }
    if(cursor < totalDuration100ns){
        keepRanges.emplace_back(cursor, totalDuration100ns);
    }
    return keepRanges;
}

vector<pair<int64_t, int64_t>> buildEffectiveCutRangesWithRapPreroll(const vector<uint32_t>& cutScenes, const vector<IndexedFrameSample>& rapMarkers, int64_t totalDuration100ns, const vector<int64_t>& rapTimes100ns){
    const auto boundaries{buildSceneBoundaries100ns(rapMarkers, totalDuration100ns)};
    if(boundaries.size() < 2){
        return {};
    }

    const auto sceneCount{boundaries.size() - 1};
    vector<bool> isCut(sceneCount, false);
    for(const auto scene: cutScenes){
        if(scene < sceneCount){
            isCut[scene] = true;
        }
    }

    vector<pair<int64_t, int64_t>> keepRanges;
    for(size_t i{0}; i < sceneCount; ++i){
        if(isCut[i]){
            continue;
        }
        auto start{boundaries[i]};
        const auto end{boundaries[i + 1]};
        const auto rapIt{upper_bound(rapTimes100ns.begin(), rapTimes100ns.end(), start)};
        if(rapIt != rapTimes100ns.begin()){
            start = *(rapIt - 1);
        }
        if(end > start){
            keepRanges.emplace_back(start, end);
        }
    }

    if(keepRanges.empty()){
        return {{0, totalDuration100ns}};
    }

    sort(keepRanges.begin(), keepRanges.end());
    vector<pair<int64_t, int64_t>> mergedKeep;
    for(const auto& range: keepRanges){
        if(mergedKeep.empty() || range.first > mergedKeep.back().second){
            mergedKeep.push_back(range);
        }else{
            mergedKeep.back().second = max(mergedKeep.back().second, range.second);
        }
    }

    vector<pair<int64_t, int64_t>> cutRanges;
    int64_t cursor{};
    for(const auto& [start, end]: mergedKeep){
        if(start > cursor){
            cutRanges.emplace_back(cursor, start);
        }
        cursor = max(cursor, end);
    }
    if(cursor < totalDuration100ns){
        cutRanges.emplace_back(cursor, totalDuration100ns);
    }

    return cutRanges;
}

int64_t removedDurationBefore(const vector<pair<int64_t, int64_t>>& ranges, int64_t time100ns){
    int64_t removed{};
    for(const auto& [start, end]: ranges){
        if(time100ns <= start){
            break;
        }
        removed += min(time100ns, end) - start;
        if(time100ns < end){
            break;
        }
    }
    return removed;
}

uint32_t getNalLengthFieldSize(const com_ptr<IMFMediaType>& mediaType, const GUID& subtype){
    UINT8* configData{};
    UINT32 configSize{};
    if(FAILED(mediaType->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &configData, &configSize)) || !configData || configSize == 0){
        return 4;
    }

    if(subtype == MFVideoFormat_H264){
        if(configSize >= 5){
            const auto result{(configData[4] & 0x03) + 1};
            CoTaskMemFree(configData);
            return result;
        }
    }else if(subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265){
        if(configSize >= 22){
            const auto result{(configData[21] & 0x03) + 1};
            CoTaskMemFree(configData);
            return result;
        }
    }

    CoTaskMemFree(configData);
    return 4;
}

bool isTrueRandomAccessPointSample(const com_ptr<IMFSample>& sample, const GUID& subtype, uint32_t nalLengthFieldSize, bool allowInconclusive){
    if(subtype != MFVideoFormat_H264 && subtype != MFVideoFormat_HEVC && subtype != MFVideoFormat_H265){
        return true;
    }

    com_ptr<IMFMediaBuffer> contiguousBuffer;
    if(FAILED(sample->ConvertToContiguousBuffer(contiguousBuffer.put())) || !contiguousBuffer){
        return allowInconclusive; // parsing unavailable
    }

    BYTE* data{};
    DWORD maxLength{};
    DWORD currentLength{};
    if(FAILED(contiguousBuffer->Lock(&data, &maxLength, &currentLength)) || !data || currentLength == 0){
        return allowInconclusive;
    }

    auto classifyNalType = [&](const uint8_t nalHeader) -> std::optional<bool>{
        if(subtype == MFVideoFormat_H264){
            const auto nalType = static_cast<uint8_t>(nalHeader & 0x1F);
            if(nalType >= 1 && nalType <= 5){
                return nalType == 5; // IDR only
            }
        }else{
            const auto nalType = static_cast<uint8_t>((nalHeader >> 1) & 0x3F);
            if(nalType <= 31){
                return nalType == 19 || nalType == 20 || nalType == 21; // HEVC IDR/CRA
            }
        }
        return std::nullopt;
    };

    // Try MP4/MOV length-prefixed NAL units first
    {
        const auto nalSizeField{clamp<uint32_t>(nalLengthFieldSize, 1, 4)};
        size_t offset{};
        while(offset + nalSizeField <= currentLength){
            uint32_t nalLength{};
            for(uint32_t i{0}; i < nalSizeField; ++i){
                nalLength = (nalLength << 8) | data[offset + i];
            }
            offset += nalSizeField;

            if(nalLength == 0 || offset + nalLength > currentLength){
                break;
            }

            if(const auto maybeRap{classifyNalType(data[offset])}; maybeRap.has_value()){
                const auto result{maybeRap.value()};
                contiguousBuffer->Unlock();
                return result;
            }

            offset += nalLength;
        }
    }

    // Fallback: try Annex B start-code format
    for(size_t i{0}; i + 4 < currentLength; ++i){
        size_t nalStart{};
        if(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1){
            nalStart = i + 3;
        }else if(i + 4 < currentLength && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1){
            nalStart = i + 4;
        }else{
            continue;
        }

        if(nalStart >= currentLength){
            continue;
        }

        if(const auto maybeRap{classifyNalType(data[nalStart])}; maybeRap.has_value()){
            const auto result{maybeRap.value()};
            contiguousBuffer->Unlock();
            return result;
        }
    }

    contiguousBuffer->Unlock();
    return allowInconclusive; // inconclusive parsing
}

bool isContainerSyncSample(const com_ptr<IMFSample>& sample){
    UINT32 cleanPoint{};
    return SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0;
}

#ifndef _DEBUG
struct NullExportLog{
    template<typename... Args>
    void open(Args&&...){ }

    template<typename T>
    NullExportLog& operator<<(const T&){ return *this; }

    explicit operator bool() const{ return false; }
    void flush(){ }
};
#endif

com_ptr<IMFMediaType> chooseBestNativeVideoMediaType(IMFSourceReader* reader, DWORD streamIndex){
    com_ptr<IMFMediaType> bestType;
    double bestFps{};
    uint32_t bestWidth{};
    uint32_t bestHeight{};

    for(DWORD mediaTypeIndex{0};; ++mediaTypeIndex){
        com_ptr<IMFMediaType> type;
        const auto hr{reader->GetNativeMediaType(streamIndex, mediaTypeIndex, type.put())};
        if(hr == MF_E_NO_MORE_TYPES){
            break;
        }
        check_hresult(hr);

        GUID major{GUID_NULL};
        check_hresult(type->GetGUID(MF_MT_MAJOR_TYPE, &major));
        if(major != MFMediaType_Video){
            continue;
        }

        uint32_t fpsNum{};
        uint32_t fpsDen{};
        (void)MFGetAttributeRatio(type.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        const auto fps{(fpsNum > 0 && fpsDen > 0) ? (static_cast<double>(fpsNum) / fpsDen) : 0.0};

        uint32_t width{};
        uint32_t height{};
        (void)MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &width, &height);

        const auto betterFps{fps > (bestFps + 0.0001)};
        const auto sameFps{fabs(fps - bestFps) <= 0.0001};
        const auto betterResolution{(width * height) > (bestWidth * bestHeight)};
        if(!bestType || betterFps || (sameFps && betterResolution)){
            bestType = type;
            bestFps = fps;
            bestWidth = width;
            bestHeight = height;
        }
    }

    return bestType;
}

com_ptr<IMFMediaType> createPcmFloatAudioType(uint32_t sampleRate, uint32_t channels){
    com_ptr<IMFMediaType> type;
    check_hresult(MFCreateMediaType(type.put()));
    check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    check_hresult(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 4));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * channels * 4));
    check_hresult(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    return type;
}

com_ptr<IMFMediaType> createAacOutputType(uint32_t sampleRate, uint32_t channels){
    com_ptr<IMFMediaType> type;
    check_hresult(MFCreateMediaType(type.put()));
    check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    check_hresult(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000 / 8));
    check_hresult(type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0));
    check_hresult(type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29));
    return type;
}

vector<float> decodeAudioRangeToFloat(
    IMFSourceReader* reader,
    const DWORD audioStreamIndex,
    const int64_t rangeStart100ns,
    const int64_t rangeEnd100ns,
    const uint32_t channels,
    const uint32_t sampleRate){

    vector<float> out;
    if(rangeEnd100ns <= rangeStart100ns || channels == 0 || sampleRate == 0){
        return out;
    }

    PROPVARIANT pos{.vt = VT_I8, .hVal = {.QuadPart = rangeStart100ns}};
    check_hresult(reader->SetCurrentPosition(GUID_NULL, pos));
    PropVariantClear(&pos);

    int64_t lastSampleStart100ns{-1};
    int64_t lastSampleEnd100ns{-1};
    for(;;){
        DWORD actualStream{};
        DWORD flags{};
        LONGLONG timestamp{};
        com_ptr<IMFSample> sample;
        check_hresult(reader->ReadSample(audioStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put()));
        if((flags & MF_SOURCE_READERF_STREAMTICK) && timestamp >= rangeEnd100ns){
            break;
        }
        if(flags & MF_SOURCE_READERF_ENDOFSTREAM){
            break;
        }
        if(!sample){
            continue;
        }

        LONGLONG sampleStart100ns{};
        if(FAILED(sample->GetSampleTime(&sampleStart100ns))){
            sampleStart100ns = timestamp;
        }
        LONGLONG sampleDuration100ns{};
        if(FAILED(sample->GetSampleDuration(&sampleDuration100ns)) || sampleDuration100ns <= 0){
            continue;
        }

        const auto sampleEnd100ns{sampleStart100ns + sampleDuration100ns};
        if(sampleStart100ns > lastSampleStart100ns || sampleEnd100ns > lastSampleEnd100ns){
            lastSampleStart100ns = sampleStart100ns;
            lastSampleEnd100ns = sampleEnd100ns;
        }

        if(sampleEnd100ns <= rangeStart100ns){
            continue;
        }
        if(sampleStart100ns >= rangeEnd100ns){
            break;
        }

        com_ptr<IMFMediaBuffer> contiguous;
        check_hresult(sample->ConvertToContiguousBuffer(contiguous.put()));
        BYTE* bytes{};
        DWORD maxLen{};
        DWORD curLen{};
        check_hresult(contiguous->Lock(&bytes, &maxLen, &curLen));

        const auto totalFrames    {static_cast<int64_t>(curLen / (channels * sizeof(float)))};
        const auto trimStart100ns {max<int64_t>(0, rangeStart100ns - sampleStart100ns)};
        const auto trimEnd100ns   {max<int64_t>(0, sampleEnd100ns - rangeEnd100ns)};
        const auto trimStartFrames{min<int64_t>(totalFrames, (trimStart100ns * sampleRate) / 10'000'000)};
        const auto trimEndFrames  {min<int64_t>(totalFrames - trimStartFrames, (trimEnd100ns * sampleRate) / 10'000'000)};
        const auto keepFrames     {max<int64_t>(0, totalFrames - trimStartFrames - trimEndFrames)};

        if(keepFrames > 0){
            const auto* src{reinterpret_cast<const float*>(bytes)};
            const auto begin{src + (trimStartFrames * channels)};
            out.insert(out.end(), begin, begin + (keepFrames * channels));
        }

        contiguous->Unlock();
    }

    return out;
}

vector<uint32_t> remapCutScenesAfterMarkerRemoval(const vector<uint32_t>& cutScenes, uint32_t removePos, uint32_t newSceneCount){
    vector<uint32_t> updatedCuts;
    updatedCuts.reserve(cutScenes.size());
    for(const auto sceneIndex: cutScenes){
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
        if(sceneIndex < newSceneCount){
            result.push_back(sceneIndex);
        }
    }
    sort(result.begin(), result.end());
    result.erase(unique(result.begin(), result.end()), result.end());
    return result;
}

vector<uint32_t> remapCutScenesAfterMarkerInsertion(const vector<uint32_t>& cutScenes, uint32_t insertPos){
    vector<uint32_t> updatedCuts;
    updatedCuts.reserve(cutScenes.size() + 1);
    for(const auto sceneIndex: cutScenes){
        if(sceneIndex < insertPos){
            updatedCuts.push_back(sceneIndex);
        }else if(sceneIndex == insertPos){
            updatedCuts.push_back(sceneIndex);
            updatedCuts.push_back(sceneIndex + 1);
        }else{
            updatedCuts.push_back(sceneIndex + 1);
        }
    }
    sort(updatedCuts.begin(), updatedCuts.end());
    updatedCuts.erase(unique(updatedCuts.begin(), updatedCuts.end()), updatedCuts.end());
    return updatedCuts;
}

void refreshSelectedMarkers(vector<uint32_t>& selectedKeyFrames, size_t markerCount){
    selectedKeyFrames.clear();
    selectedKeyFrames.reserve(markerCount);
    for(uint32_t i{0}; i < markerCount; ++i){
        selectedKeyFrames.push_back(i);
    }
}

struct LoadedProjectData{
    wstring loadedFilePath;
    double zoomLevel{};
    bool keepAudio{};
    int32_t audioCrossfadeMs{};
    vector<IndexedFrameSample> markers;
    vector<uint32_t> cutScenes;
};

LoadedProjectData parseProjectLines(const Windows::Foundation::Collections::IVector<winrt::hstring>& lines, double defaultZoomLevel, bool defaultKeepAudio, int32_t defaultAudioCrossfadeMs){
    LoadedProjectData data{};
    data.zoomLevel = defaultZoomLevel;
    data.keepAudio = defaultKeepAudio;
    data.audioCrossfadeMs = defaultAudioCrossfadeMs;

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

        if(key == P_FILE_PATH){
            data.loadedFilePath = value;
        }else if(key == P_STORYLINE_ZOOM){
            try{ data.zoomLevel = stod(value); }catch(...){ }
        }else if(key == P_RAP_MARKERS){
            data.markers = parseKeyframeVector(value);
        }else if(key == P_CUT_SCENES){
            data.cutScenes = parseIndexList(value);
        }else if(key == P_KEEP_AUDIO){
            data.keepAudio = !(value == L"0" || value == L"false" || value == L"False");
        }else if(key == P_AUDIO_CROSSFADE_MS){
            try{ data.audioCrossfadeMs = stoi(value); }catch(...){ }
        }
    }

    return data;
}

MainWindow::MainWindow(){
    InitializeComponent();

    m_player = MediaPlayer();
    PreviewPlayer().SetMediaPlayer(m_player);

    m_naturalDurationChangedRevoker = m_player.PlaybackSession().NaturalDurationChanged(auto_revoke, {this, &MainWindow::onNaturalDurationChanged});

    m_positionTimer = DispatcherTimer();
    m_positionTimer.Interval(chrono::milliseconds(80));
    m_positionTimer.Tick({this, &MainWindow::onPositionTimerTick});
    m_positionTimer.Start();

    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();

    restoreWindowPlacement();
    loadAppSettings();
    Closed({this, &MainWindow::onClosed});
    refreshRecentVideosMenu();
    refreshRecentProjectsMenu();
    m_lastSavedProjectSnapshot = buildProjectSnapshot();
    updateWindowTitle();
}

HWND MainWindow::getWindowHandle() const{
    HWND hwnd{};
    const auto projected{const_cast<MainWindow*>(this)->get_strong()};
    check_hresult(projected.as<::IWindowNative>()->get_WindowHandle(&hwnd));
    return hwnd;
}


auto pixelsToDips(int32_t pixelValue, uint32_t dpi){
    return static_cast<int32_t>(lround((pixelValue * 96.0) / (dpi == 0 ? 96 : dpi)));
}

auto dipsToPixels(int32_t dipValue, uint32_t dpi){
    return static_cast<int32_t>(lround((dipValue * (dpi == 0 ? 96U : dpi)) / 96));
}

bool MainWindow::isRectVisibleOnAnyMonitor(const RECT& rect){
    const auto monitor{::MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)};
    if(!monitor){
        return false;
    }

    MONITORINFO monitorInfo{.cbSize = sizeof(monitorInfo)};
    if(!::GetMonitorInfoW(monitor, &monitorInfo)){
        return false;
    }

    RECT intersection{};
    return ::IntersectRect(&intersection, &rect, &monitorInfo.rcWork) != FALSE;
}

void MainWindow::restoreWindowPlacement(){
    const auto localSettings{ApplicationData::Current().LocalSettings()};
    const auto values{localSettings.Values()};

    if(!values.HasKey(W_POS_L) || !values.HasKey(W_POS_T) || !values.HasKey(W_POS_W) || !values.HasKey(W_POS_H)){
        return;
    }

    const auto left{unbox_value<int32_t>(values.Lookup(W_POS_L))};
    const auto top{unbox_value<int32_t>(values.Lookup(W_POS_T))};

    const auto hwnd{getWindowHandle()};

    SetWindowPos(hwnd, nullptr, left, top, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);

    const auto currentDpi{::GetDpiForWindow(hwnd)};
    const auto hasDpiData{values.HasKey(W_POS_DPI)};
    const auto storedWidth{unbox_value<int32_t>(values.Lookup(W_POS_W))};
    const auto storedHeight{unbox_value<int32_t>(values.Lookup(W_POS_H))};
    const auto width{hasDpiData ? dipsToPixels(storedWidth, currentDpi) : storedWidth};
    const auto height{hasDpiData ? dipsToPixels(storedHeight, currentDpi) : storedHeight};

    if(!isRectVisibleOnAnyMonitor(RECT{left, top, left + width, top + height})){
        values.Remove(W_POS_L);
        values.Remove(W_POS_T);
        values.Remove(W_POS_W);
        values.Remove(W_POS_H);
        values.Remove(W_POS_DPI);
        return;
    }

    SetWindowPos(hwnd, nullptr, left, top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
}

void MainWindow::loadAppSettings(){
    const auto values{ApplicationData::Current().LocalSettings().Values()};

    m_maxRecentVideos = S_DEFAULT_MAX_RECENT;
    m_maxRecentProjects = S_DEFAULT_MAX_RECENT;

    if(values.HasKey(S_MAX_RECENT_VIDEOS)){
        const auto parsed{unbox_value<int32_t>(values.Lookup(S_MAX_RECENT_VIDEOS))};
        m_maxRecentVideos = static_cast<uint32_t>(clamp(parsed, 1, 20));
    }
    if(values.HasKey(S_MAX_RECENT_PROJECTS)){
        const auto parsed{unbox_value<int32_t>(values.Lookup(S_MAX_RECENT_PROJECTS))};
        m_maxRecentProjects = static_cast<uint32_t>(clamp(parsed, 1, 20));
    }

    if(values.HasKey(S_RECENT_VIDEOS)){
        m_recentVideos = splitRecentItems(unbox_value<hstring>(values.Lookup(S_RECENT_VIDEOS)).c_str());
    }
    if(values.HasKey(S_RECENT_PROJECTS)){
        m_recentProjects = splitRecentItems(unbox_value<hstring>(values.Lookup(S_RECENT_PROJECTS)).c_str());
    }
    if(m_recentVideos.size() > m_maxRecentVideos){
        m_recentVideos.resize(m_maxRecentVideos);
    }
    if(m_recentProjects.size() > m_maxRecentProjects){
        m_recentProjects.resize(m_maxRecentProjects);
    }
}

void MainWindow::saveAppSettings() const{
    const auto values{ApplicationData::Current().LocalSettings().Values()};
    values.Insert(S_MAX_RECENT_VIDEOS, box_value(static_cast<int32_t>(m_maxRecentVideos)));
    values.Insert(S_MAX_RECENT_PROJECTS, box_value(static_cast<int32_t>(m_maxRecentProjects)));
    values.Insert(S_RECENT_VIDEOS, box_value(hstring(joinRecentItems(m_recentVideos))));
    values.Insert(S_RECENT_PROJECTS, box_value(hstring(joinRecentItems(m_recentProjects))));
}

void MainWindow::saveWindowPlacement() const{
    const auto hwnd{getWindowHandle()};

    RECT bounds{};
    if(!GetWindowRect(hwnd, &bounds)){
        return;
    }

    WINDOWPLACEMENT placement{.length = sizeof(placement)};
    if(GetWindowPlacement(hwnd, &placement) && placement.showCmd == SW_SHOWMAXIMIZED){
        bounds = placement.rcNormalPosition;
    }

    const auto localSettings{ApplicationData::Current().LocalSettings()};
    const auto values{localSettings.Values()};
    const auto dpi{::GetDpiForWindow(hwnd)};
    values.Insert(W_POS_L, box_value(static_cast<int32_t>(bounds.left)));
    values.Insert(W_POS_T, box_value(static_cast<int32_t>(bounds.top)));
    values.Insert(W_POS_W, box_value(pixelsToDips(static_cast<int32_t>(bounds.right - bounds.left), dpi)));
    values.Insert(W_POS_H, box_value(pixelsToDips(static_cast<int32_t>(bounds.bottom - bounds.top), dpi)));
    values.Insert(W_POS_DPI, box_value(static_cast<int32_t>(dpi)));
}

void MainWindow::onClosed(const Control&, const WEArgs&){
    m_isClosing = true;

    if(m_positionTimer){
        m_positionTimer.Stop();
    }

    m_naturalDurationChangedRevoker.revoke();
    saveWindowPlacement();
    saveAppSettings();
}

void MainWindow::startButton_Click(const Control&, const REArgs&){
    if(m_player){
        applyAudioSettingsToPlayer();
        m_player.Play();
    }
}

void MainWindow::pauseButton_Click(const Control&, const REArgs&){
    if(m_player){
        m_player.Pause();
    }
}

void MainWindow::stopButton_Click(const Control&, const REArgs&){
    if(m_player){
        m_player.Pause();
        m_player.PlaybackSession().Position(chrono::seconds(0));
        updateTimelineCursorFromPlayback();
    }
}

void MainWindow::timelineZoomSlider_ValueChanged(const Control&, const RBVArgs&){
    if(m_loadedFile && m_timelineDurationSeconds > 0){
        renderTimelineAsync();
    }
    updateWindowTitle();
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

void MainWindow::keepAudioCheckBox_Changed(const Control&, const REArgs&){
    m_keepAudio = KeepAudioCheckBox().IsChecked().GetBoolean();
    updateAudioUiAndPlaybackState();
    updateWindowTitle();
}

void MainWindow::audioCrossfadeComboBox_SelectionChanged(const Control&, const Control&){
    const auto selected{AudioCrossfadeComboBox().SelectedItem().try_as<Controls::ComboBoxItem>()};
    if(!selected){
        return;
    }

    if(!selected.Tag()){
        return;
    }

    try{
        const auto tag{unbox_value<hstring>(selected.Tag())};
        m_audioCrossfadeMs = normalizeAudioCrossfadeMs(stoi(wstring(tag.c_str())));
    }catch(...){
        m_audioCrossfadeMs = 0;
    }

    syncAudioCrossfadeComboSelection();
    updateWindowTitle();
}

void MainWindow::timelineHorizontalScrollBar_ValueChanged(const Control&, const RBVArgs& args){
    if(m_isClosing){
        return;
    }

    const auto scrollViewer{TimelineScrollViewer()};
    const auto offset{scrollViewer.HorizontalOffset()};
    if(fabs(offset - args.NewValue()) > 0.5){
        const auto targetOffset{box_value(args.NewValue()).as<IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }
}

void MainWindow::timelineScrollViewer_ViewChanged(const Control&, const SVVCArgs&){
    syncTimelineHorizontalScrollBar();
}

void MainWindow::timelineScrollViewer_SizeChanged(const Control&, const SCArgs&){
    syncTimelineHorizontalScrollBar();
}

void MainWindow::timelineCanvas_PointerPressed(const Control&, const PREArgs& e){
    if(m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    if(!point.Properties().IsLeftButtonPressed()){
        return;
    }

    m_isTimelineDragging = true;
    m_timelineDragMoved = false;
    m_timelineDragPointerId = e.Pointer().PointerId();
    m_timelineDragStartX = point.Position().X;
    m_timelineDragStartOffset = TimelineScrollViewer().HorizontalOffset();

    tryFocusTimelineCanvas(FocusState::Programmatic);
    TimelineCanvas().CapturePointer(e.Pointer());
    e.Handled(true);
}

void MainWindow::timelineCanvas_PointerMoved(const Control&, const PREArgs& e){
    if(!m_isTimelineDragging || e.Pointer().PointerId() != m_timelineDragPointerId){
        return;
    }

    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    const auto deltaX{point.Position().X - m_timelineDragStartX};

    if(fabs(deltaX) > 4.0){
        m_timelineDragMoved = true;
    }

    const auto scrollViewer{TimelineScrollViewer()};
    const auto viewportWidth{scrollViewer.ViewportWidth()};
    const auto maxOffset{max(0.0, TimelineCanvas().Width() - viewportWidth)};
    const auto target{clamp(m_timelineDragStartOffset - deltaX, 0.0, maxOffset)};
    const auto targetOffset{box_value(target).as<IReference<double>>()};
    scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    e.Handled(true);
}

bool MainWindow::toggleSelectedKeyframeAtCanvasX(double pointerX){
    if(m_timelineDurationSeconds <= 0 || TimelineTickCanvas().Width() <= 0){
        return false;
    }

    constexpr auto hitTolerancePx{4.0};
    const auto width{TimelineTickCanvas().Width()};
    const auto clicked100ns{static_cast<int64_t>(clamp(pointerX, 0.0, width) / width * (m_timelineDurationSeconds * 10'000'000.0))};

    auto nearestIndex{m_frameIndex.size()};
    auto nearestDistance{hitTolerancePx + 1.0};
    for(size_t i{0}; i < m_frameIndex.size(); ++i){
        const auto x{clamp((static_cast<double>(m_frameIndex[i].time100ns) / (m_timelineDurationSeconds * 10'000'000.0)) * width, 0.0, width)};
        const auto distance{fabs(pointerX - x)};
        if(distance <= hitTolerancePx && distance < nearestDistance){
            nearestDistance = distance;
            nearestIndex = i;
        }
    }

    if(nearestIndex != m_frameIndex.size()){
        const auto removePos{static_cast<uint32_t>(nearestIndex)};
        m_cutScenes = remapCutScenesAfterMarkerRemoval(m_cutScenes, removePos, static_cast<uint32_t>(m_frameIndex.size()));
        m_frameIndex.erase(m_frameIndex.begin() + removePos);
    }else{
        const auto insertIt{lower_bound(m_frameIndex.begin(), m_frameIndex.end(), clicked100ns, [](const IndexedFrameSample& a, int64_t t){
            return a.time100ns < t;
        })};
        const auto insertPos{static_cast<uint32_t>(distance(m_frameIndex.begin(), insertIt))};

        m_cutScenes = remapCutScenesAfterMarkerInsertion(m_cutScenes, insertPos);

        const auto frameNumber{estimateFrameNumberFromTime100ns(m_mediaInfo.frameRate, clicked100ns)};
        m_frameIndex.insert(insertIt, IndexedFrameSample{.time100ns=clicked100ns, .duration100ns=0, .cleanPoint=true, .sampleIndex=frameNumber});
    }

    refreshSelectedMarkers(m_selectedKeyFrames, m_frameIndex.size());

    renderTimelineTicks();
    renderKeyframeTicks();
    renderCutOverlays();
    updateWindowTitle();
    return true;
}


void MainWindow::timelineCanvas_PointerReleased(const Control&, const PREArgs& e){
    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    if(point.Properties().PointerUpdateKind() == PointerUpdateKind::RightButtonReleased){
        if(toggleSelectedKeyframeAtCanvasX(point.Position().X)){
            e.Handled(true);
            return;
        }
    }

    if(!m_isTimelineDragging || e.Pointer().PointerId() != m_timelineDragPointerId){
        return;
    }
    const auto dragged{m_timelineDragMoved};

    m_isTimelineDragging = false;
    m_timelineDragMoved = false;
    TimelineCanvas().ReleasePointerCapture(e.Pointer());

    if(!dragged){
        if(isControlModifierActive(e.KeyModifiers())){
            toggleCutBlockAtCanvasX(point.Position().X);
        }else{
            seekTimelineToCanvasX(point.Position().X);
        }
    }

    e.Handled(true);
}

void MainWindow::timelineCanvas_PointerCanceled(const Control&, const PREArgs& e){
    if(m_isTimelineDragging && e.Pointer().PointerId() == m_timelineDragPointerId){
        m_isTimelineDragging = false;
        m_timelineDragMoved = false;
        TimelineCanvas().ReleasePointerCapture(e.Pointer());
        e.Handled(true);
    }
}

void MainWindow::timelineCanvas_PointerCaptureLost(const Control&, const PREArgs&){
    m_isTimelineDragging = false;
    m_timelineDragMoved = false;
}

void MainWindow::timelineCanvas_Loaded(const Control&, const REArgs&){
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

void MainWindow::timelineTickCanvas_PointerReleased(const Control&, const PREArgs& e){
    const auto point{e.GetCurrentPoint(TimelineTickCanvas())};
    if(point.Properties().PointerUpdateKind() == PointerUpdateKind::RightButtonReleased && toggleSelectedKeyframeAtCanvasX(point.Position().X)){
        tryFocusTimelineCanvas(FocusState::Programmatic);
        e.Handled(true);
        return;
    }

    if(point.Properties().PointerUpdateKind() == PointerUpdateKind::LeftButtonReleased){
        if(isControlModifierActive(e.KeyModifiers())){
            if(toggleCutBlockAtCanvasX(point.Position().X)){
                tryFocusTimelineCanvas(FocusState::Programmatic);
                e.Handled(true);
                return;
            }
        }
    }
}


void MainWindow::onNaturalDurationChanged(const MPSession& sender, const Control&){
    const auto duration{sender.NaturalDuration()};
    m_timelineDurationSeconds = max(0.0, duration.count() / 10'000'000.0);

    if(m_loadedFile && m_timelineDurationSeconds > 0){
        const auto weak{get_weak()};
        if(DispatcherQueue().HasThreadAccess()){
            renderTimelineAsync();
            return;
        }

        DispatcherQueue().TryEnqueue([weak](){
            if(const auto self{weak.get()}){
                self->renderTimelineAsync();
            }
        });
    }
}

void MainWindow::onPositionTimerTick(const Control&, const Control&){
    if(m_isClosing){
        return;
    }

    (void)trySkipCurrentCutDuringPlayback();
    updateTimelineCursorFromPlayback();
}

void MainWindow::updateTimelineCursorFromPlayback(){
    if(m_isClosing || !m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto current{m_player.PlaybackSession().Position()};
    const auto seconds{max(0.0, current.count() / 10'000'000.0)};
    const auto ratio{clamp(seconds / m_timelineDurationSeconds, 0.0, 1.0)};
    const auto left{ratio * TimelineCanvas().Width()};
    Controls::Canvas::SetLeft(TimelineCursor(), left);

    if(m_player.PlaybackSession().PlaybackState() == MediaPlaybackState::Playing){
        ensureTimelineCursorVisible(left);
    }

    syncTimelineHorizontalScrollBar();
}

void MainWindow::syncTimelineHorizontalScrollBar(){
    const auto scrollViewer{TimelineScrollViewer()};
    scrollViewer.UpdateLayout();

    const auto viewportWidth{max(1.0, scrollViewer.ViewportWidth())};
    const auto extentWidth{max(viewportWidth, scrollViewer.ExtentWidth())};
    const auto scrollableWidth{max(0.0, extentWidth - viewportWidth)};

    auto bar{TimelineHorizontalScrollBar()};
    bar.Minimum(0.0);
    bar.Maximum(max(1.0, scrollableWidth));
    bar.LargeChange(max(32.0, viewportWidth * 0.8));
    bar.SmallChange(24.0);
    bar.IsEnabled(scrollableWidth > 0.0);
    bar.Visibility(Visibility::Visible);

    const auto currentValue{bar.Value()};
    const auto offset{clamp(scrollViewer.HorizontalOffset(), 0.0, bar.Maximum())};
    if(fabs(currentValue - offset) > 0.5){
        bar.Value(offset);
    }
}

void MainWindow::seekTimelineToCanvasX(double pointerX){
    if(!m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto x{clamp(pointerX, 0.0, TimelineCanvas().Width())};
    const auto ratio{x / TimelineCanvas().Width()};
    const auto target100ns{static_cast<int64_t>(ratio * (m_timelineDurationSeconds * 10'000'000.0))};

    m_player.PlaybackSession().Position(TimeSpan{target100ns});
    updateTimelineCursorFromPlayback();
}

void MainWindow::ensureTimelineCursorVisible(double cursorLeft){
    const auto scrollViewer{TimelineScrollViewer()};
    const auto currentOffset{scrollViewer.HorizontalOffset()};
    const auto viewportWidth{scrollViewer.ViewportWidth()};

    if(viewportWidth <= 0){
        return;
    }

    constexpr auto cursorPadding{48.0};
    const auto minVisible{currentOffset + cursorPadding};
    const auto maxVisible{currentOffset + viewportWidth - cursorPadding};
    const auto maxOffset{max(0.0, TimelineCanvas().Width() - viewportWidth)};

    if(cursorLeft < minVisible){
        const auto targetOffset{box_value(clamp(cursorLeft - cursorPadding, 0.0, maxOffset)).as<IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }else if(cursorLeft > maxVisible){
        const auto targetOffset{box_value(clamp(cursorLeft + cursorPadding - viewportWidth, 0.0, maxOffset)).as<IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }
}

void MainWindow::renderTimelineTicks(){
    TimelineTickCanvas().Children().Clear();

    const auto width{TimelineCanvas().Width()};
    TimelineTickCanvas().Width(width);
    if(m_timelineDurationSeconds <= 0 || width <= 0){
        return;
    }

    const auto majorTickCount{clamp(static_cast<int>(ceil(width / 120.0)), 6, 36)};

    for(int i{0}; i <= majorTickCount; ++i){
        const auto ratio{static_cast<double>(i) / majorTickCount};
        const auto x{ratio * width};

        Shapes::Line majorTick{};
        majorTick.X1(x);
        majorTick.X2(x);
        majorTick.Y1(7);
        majorTick.Y2(23);
        majorTick.Stroke(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 180, 180, 180)));
        majorTick.StrokeThickness(1.0);
        TimelineTickCanvas().Children().Append(majorTick);

        Controls::TextBlock label{};
        const auto totalSeconds{static_cast<int>(ratio * m_timelineDurationSeconds + 0.5)};
        const auto minutes{totalSeconds / 60};
        const auto seconds{totalSeconds % 60};
        auto text{to_wstring(minutes)};
        text += L":";
        if(seconds < 10){
            text += L"0";
        }
        text += to_wstring(seconds);
        label.Text(text);
        label.FontSize(11);
        label.Foreground(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 200, 200, 200)));
        Controls::Canvas::SetLeft(label, max(0.0, x + 3.0));
        Controls::Canvas::SetTop(label, 0);
        TimelineTickCanvas().Children().Append(label);
    }
}

void MainWindow::renderKeyframeTicks(){
    if(m_frameIndex.empty() || m_timelineDurationSeconds <= 0 || TimelineTickCanvas().Width() <= 0){
        return;
    }

    const auto width {TimelineTickCanvas().Width()};
    const auto total100ns {static_cast<double>(m_timelineDurationSeconds * 10'000'000.0)};
    uint32_t cleanOrdinal{};
    for(const auto& frame: m_frameIndex){
        if(!frame.cleanPoint){
            continue;
        }

        const auto x {clamp((frame.time100ns / total100ns) * width, 0.0, width)};
        // XXX: looks like isSelected is always true
        const auto isSelected{find(m_selectedKeyFrames.begin(), m_selectedKeyFrames.end(), cleanOrdinal) != m_selectedKeyFrames.end()};

        Shapes::Line tick{};
        tick.X1(x);
        tick.X2(x);
        tick.Y1(0);
        tick.Y2(isSelected ? 8.0 : 5.0);
        tick.Stroke(Media::SolidColorBrush(isSelected
            ? Windows::UI::ColorHelper::FromArgb(255, 255, 80, 80)
            : Windows::UI::ColorHelper::FromArgb(255, 80, 200, 255)));
        tick.StrokeThickness(isSelected ? 2.0 : 1.0);
        TimelineTickCanvas().Children().Append(tick);
        ++cleanOrdinal;
    }
}

void MainWindow::renderCutOverlays(){
    CutOverlayLayer().Children().Clear();

    const auto width{TimelineCanvas().Width()};
    CutOverlayLayer().Width(width);
    if(m_timelineDurationSeconds <= 0 || width <= 0){
        return;
    }

    const auto duration100ns{static_cast<int64_t>(m_timelineDurationSeconds * 10'000'000.0)};
    const auto rapMarkers{buildRapMarkersFromSelection(m_frameIndex, m_selectedKeyFrames)};
    const auto cutRanges100ns{buildCutRanges100ns(m_cutScenes, rapMarkers, duration100ns)};
    const auto overlayColor {Windows::UI::ColorHelper::FromArgb(180, 0, 0, 0)};
    for(const auto& [startTime100ns, endTime100ns]: cutRanges100ns){
        const auto start{clamp((static_cast<double>(startTime100ns) / 10'000'000.0) / m_timelineDurationSeconds, 0.0, 1.0)};
        const auto end{clamp((static_cast<double>(endTime100ns) / 10'000'000.0) / m_timelineDurationSeconds, 0.0, 1.0)};
        if(end <= start){
            continue;
        }

        Shapes::Rectangle block{};
        const auto left{start * width};
        block.Width(max(1.0, (end - start) * width));
        block.Height(86.0);
        block.Fill(Media::SolidColorBrush(overlayColor));
        block.IsHitTestVisible(false);
        Controls::Canvas::SetLeft(block, left);
        Controls::Canvas::SetTop(block, 0.0);
        CutOverlayLayer().Children().Append(block);
    }
}


bool MainWindow::toggleCutBlockAtCanvasX(const double pointerX){
    if(m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return false;
    }

    const auto width{TimelineCanvas().Width()};
    const auto clicked100ns{static_cast<int64_t>(clamp(pointerX, 0.0, width) / width * (m_timelineDurationSeconds * 10'000'000.0))};
    const auto rapMarkers{buildRapMarkersFromSelection(m_frameIndex, m_selectedKeyFrames)};
    const auto boundaries{buildSceneBoundaries100ns(rapMarkers, static_cast<int64_t>(m_timelineDurationSeconds * 10'000'000.0))};
    if(boundaries.size() < 2){
        return false;
    }

    auto sceneIndex{boundaries.size() - 2};
    for(size_t i{0}; i + 1 < boundaries.size(); ++i){
        if(clicked100ns < boundaries[i + 1]){
            sceneIndex = i;
            break;
        }
    }

    const auto it{find(m_cutScenes.begin(), m_cutScenes.end(), sceneIndex)};
    if(it == m_cutScenes.end()){
        m_cutScenes.push_back(static_cast<uint32_t>(sceneIndex));
        sort(m_cutScenes.begin(), m_cutScenes.end());
    }else{
        m_cutScenes.erase(it);
    }

    renderCutOverlays();
    updateWindowTitle();
    return true;
}


bool MainWindow::trySkipCurrentCutDuringPlayback(){
    if(!m_player || m_cutScenes.empty() || m_timelineDurationSeconds <= 0){
        return false;
    }

    const auto state{m_player.PlaybackSession().PlaybackState()};
    if(state != MediaPlaybackState::Playing){
        return false;
    }

    const auto duration100ns{static_cast<int64_t>(m_timelineDurationSeconds * 10'000'000.0)};
    const auto rapMarkers{buildRapMarkersFromSelection(m_frameIndex, m_selectedKeyFrames)};
    const auto cutRanges100ns{buildCutRanges100ns(m_cutScenes, rapMarkers, duration100ns)};
    const auto now100ns{max<int64_t>(0, m_player.PlaybackSession().Position().count())};
    for(const auto& [start100ns, end100ns]: cutRanges100ns){
        if(now100ns >= start100ns && now100ns < end100ns){
            m_player.PlaybackSession().Position(TimeSpan{end100ns});
            return true;
        }
    }

    return false;
}


void MainWindow::stepByFrame(int delta){
    if(!m_player || m_frameIndex.empty() || delta == 0){
        return;
    }

    const auto current {m_player.PlaybackSession().Position().count()};
    constexpr auto fallbackFrameDuration100ns{333'667LL}; // ~29.97 fps

    vector<int64_t> frameDurations;
    frameDurations.reserve(m_frameIndex.size());
    for(const auto& sample: m_frameIndex){
        if(sample.duration100ns > 0){
            frameDurations.push_back(sample.duration100ns);
        }
    }

    if(frameDurations.empty()){
        for(size_t i{1}; i < m_frameIndex.size(); ++i){
            const auto sampleCount {static_cast<int64_t>(m_frameIndex[i].sampleIndex) - static_cast<int64_t>(m_frameIndex[i - 1].sampleIndex)};
            const auto timeDelta {m_frameIndex[i].time100ns - m_frameIndex[i - 1].time100ns};
            if(sampleCount > 0 && timeDelta > 0){
                frameDurations.push_back(timeDelta / sampleCount);
            }
        }
    }

    auto frameStep100ns{fallbackFrameDuration100ns};
    if(!frameDurations.empty()){
        nth_element(frameDurations.begin(), frameDurations.begin() + frameDurations.size() / 2, frameDurations.end());
        frameStep100ns = max(1LL, frameDurations[frameDurations.size() / 2]);
    }

    const auto direction {delta < 0 ? -1 : 1};
    const auto duration100ns {static_cast<int64_t>(max(0.0, m_timelineDurationSeconds) * 10'000'000.0)};
    const auto target {clamp(current + (direction * frameStep100ns), 0LL, duration100ns)};
    m_player.PlaybackSession().Position(TimeSpan{target});
    updateTimelineCursorFromPlayback();
}

void MainWindow::tryFocusTimelineCanvas(FState focusState){
    const auto canvas{TimelineCanvas()};
    if(canvas && canvas.XamlRoot()){
        canvas.Focus(focusState);
    }
}

bool MainWindow::handleStorylineKeyDown(const KRArgs& args){
    const auto focused{Input::FocusManager::GetFocusedElement(Content().XamlRoot()).try_as<DependencyObject>()};
    const auto focusOnMenu{focused && isInMenuSubtree(focused)};
    const auto focusInDialog{focused && isInDialogSubtree(focused)};

    if(!focusInDialog && (args.Key() == VirtualKey::Tab || args.Key() == VirtualKey::Escape)){
        tryFocusTimelineCanvas(FocusState::Programmatic);
        args.Handled(true);
        return true;
    }

    if(focusOnMenu || focusInDialog){
        return false;
    }

    tryFocusTimelineCanvas(FocusState::Programmatic);

    const auto ctrlState{InputKeyboardSource::GetKeyStateForCurrentThread(VirtualKey::Control)};
    const auto ctrlDown{(ctrlState & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down};

    if(ctrlDown){
        if(args.Key() == VirtualKey::O){
            (void)openProjectMenuItem_Click(nullptr, {});
            args.Handled(true);
            return true;
        }
        if(args.Key() == VirtualKey::S){
            (void)saveProjectMenuItem_Click(nullptr, {});
            args.Handled(true);
            return true;
        }
        if(args.Key() == VirtualKey::N){
            (void)newProjectMenuItem_Click(nullptr, {});
            args.Handled(true);
            return true;
        }
    }

    switch(args.Key()){
    case VirtualKey::Space:
        if(m_player){
            const auto state {m_player.PlaybackSession().PlaybackState()};
            if(state == MediaPlaybackState::Playing){
                m_player.Pause();
            }else{
                m_player.Play();
            }
        }
        args.Handled(true);
        return true;
    case VirtualKey::Left:
        stepByFrame(-1);
        args.Handled(true);
        return true;
    case VirtualKey::Right:
        stepByFrame(1);
        args.Handled(true);
        return true;
    default:
        return false;
    }
}

void MainWindow::window_PreviewKeyDown(const Control&, const KRArgs& args){
    (void)handleStorylineKeyDown(args);
}

void MainWindow::window_KeyDown(const Control&, const KRArgs& args){
    (void)handleStorylineKeyDown(args);
}

void MainWindow::rootGrid_PointerReleased(const Control&, const PREArgs& args){
    const auto source {args.OriginalSource().try_as<DependencyObject>()};
    if(!source){
        return;
    }
    if(isInMenuSubtree(source) || isInDialogSubtree(source)){
        return;
    }
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

TS MainWindow::secondsToTimeSpan(double seconds){
    return chrono::duration_cast<TimeSpan>(chrono::duration<double>(seconds));
}

AAction MainWindow::newProjectMenuItem_Click(const Control&, const REArgs&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    resetProjectState(true);
    StatusText().Text(L"New project created");
}

AAction MainWindow::openProjectMenuItem_Click(const Control&, const REArgs&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    FileOpenPicker picker{};
    picker.FileTypeFilter().Append(PROJECT_EXT);
    picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);

    auto initWithWindow{picker.as<IInitializeWithWindow>()};
    check_hresult(initWithWindow->Initialize(getWindowHandle()));

    if(const auto file{co_await picker.PickSingleFileAsync()}){
        co_await openProjectFileAsync(file);
    }
}

AAction MainWindow::saveProjectMenuItem_Click(const Control&, const REArgs&){
    StorageFile target{nullptr};

    if(!m_projectPath.empty()){
        try{
            target = co_await StorageFile::GetFileFromPathAsync(m_projectPath);
        }catch(...){
            target = nullptr;
        }
    }

    if(!target){
        FileSavePicker picker{};
        picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
        picker.FileTypeChoices().Insert(PROJECT_KEY, single_threaded_vector<hstring>({PROJECT_EXT}));
        picker.SuggestedFileName(UNNAMED_PROJECT);

        auto initWithWindow{picker.as<IInitializeWithWindow>()};
        check_hresult(initWithWindow->Initialize(getWindowHandle()));

        target = co_await picker.PickSaveFileAsync();
        if(!target){
            co_return;
        }
    }

    co_await saveProjectFileAsync(target);
}

AAction MainWindow::saveProjectAsMenuItem_Click(const Control&, const REArgs&){
    FileSavePicker picker{};
    picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
    picker.FileTypeChoices().Insert(PROJECT_KEY, single_threaded_vector<hstring>({PROJECT_EXT}));
    picker.SuggestedFileName(UNNAMED_PROJECT);

    auto initWithWindow{picker.as<IInitializeWithWindow>()};
    check_hresult(initWithWindow->Initialize(getWindowHandle()));

    const auto target{co_await picker.PickSaveFileAsync()};
    if(!target){
        co_return;
    }

    co_await saveProjectFileAsync(target);
}

AAction MainWindow::closeProjectMenuItem_Click(const Control&, const REArgs&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    resetProjectState(true);
    StatusText().Text(L"Project closed");
}

AAction MainWindow::loadVideoMenuItem_Click(const Control&, const REArgs&){
    co_await pickAndLoadVideoAsync();
}

AAction MainWindow::recentVideoMenuItem_Click(const Control& sender, const REArgs&){
    const auto item{sender.try_as<Controls::MenuFlyoutItem>()};
    if(!item || !item.Tag()){
        co_return;
    }

    auto openFailed{false};
    const auto path{unbox_value<hstring>(item.Tag())};
    try{
        const auto file{co_await StorageFile::GetFileFromPathAsync(path)};
        co_await loadVideoFileAsync(file);
    }catch(const winrt::hresult_error&){
        openFailed = true;
    }

    if(openFailed){
        co_await showInfoDialogAsync(L"Open failed", L"Could not open selected recent video.");
    }
}

AAction MainWindow::recentProjectMenuItem_Click(const Control& sender, const REArgs&){
    const auto item{sender.try_as<Controls::MenuFlyoutItem>()};
    if(!item || !item.Tag()){
        co_return;
    }

    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    auto openFailed{false};
    const auto path{unbox_value<hstring>(item.Tag())};
    try{
        const auto file{co_await StorageFile::GetFileFromPathAsync(path)};
        co_await openProjectFileAsync(file);
    }catch(...){
        openFailed = true;
    }

    if(openFailed){
        co_await showInfoDialogAsync(L"Open failed", L"Could not open selected recent project.");
    }
}

AAction MainWindow::propertiesMenuItem_Click(const Control&, const REArgs&){
    co_await showPropertiesDialogAsync();
}

AAction MainWindow::exitMenuItem_Click(const Control&, const REArgs&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    Close();
}

AAction MainWindow::aboutMenuItem_Click(const Control&, const REArgs&){
    co_await showInfoDialogAsync(L"About llvc", L"llvc - Lossless Video Cut\nv0.1 - still in alpha\n\xA9 02'2026 YePhIcK");
}

AAction MainWindow::optionsMenuItem_Click(const Control&, const REArgs&){
    co_await showOptionsDialogAsync();
}

AAction MainWindow::pickAndLoadVideoAsync(){
    FileOpenPicker picker{};
    picker.SuggestedStartLocation(PickerLocationId::VideosLibrary);
    picker.FileTypeFilter().Append(L".mp4");
    picker.FileTypeFilter().Append(L".mov");

    const auto hwnd{getWindowHandle()};
    auto initWithWindow{picker.as<IInitializeWithWindow>()};
    check_hresult(initWithWindow->Initialize(hwnd));

    if(const auto file{co_await picker.PickSingleFileAsync()}){
        co_await loadVideoFileAsync(file);
    }
}

void _refreshRecentFilesMenu(MenuFlyoutSubItem menu, const vector<hstring>& recent, const RoutedEventHandler& h){
    menu.Items().Clear();

    if(recent.empty()){
        Controls::MenuFlyoutItem empty{};
        empty.Text(L"(none)");
        empty.IsEnabled(false);
        menu.Items().Append(empty);
        return;
    }

    for(const auto& path: recent){
        Controls::MenuFlyoutItem item{};
        item.Text(path);
        item.Tag(box_value(path));
        item.Click(h);
        menu.Items().Append(item);
    }
}

void MainWindow::refreshRecentVideosMenu(){
    _refreshRecentFilesMenu(RecentVideosMenu(), m_recentVideos, {this, &MainWindow::recentVideoMenuItem_Click});
}

void MainWindow::refreshRecentProjectsMenu(){
    _refreshRecentFilesMenu(RecentProjectsMenu(), m_recentProjects, {this, &MainWindow::recentProjectMenuItem_Click});
}

void _addRecentVideo(vector<hstring>& recent, size_t maxCount, const hstring& path){
    recent.erase(remove(recent.begin(), recent.end(), path), recent.end());
    recent.insert(recent.begin(), path);
    if(recent.size() > maxCount){
        recent.resize(maxCount);
    }
}

void MainWindow::addRecentVideo(const hstring& path){
    if(path.empty()){
        return;
    }

    _addRecentVideo(m_recentVideos, m_maxRecentVideos, path);
    refreshRecentVideosMenu();
    saveAppSettings();
}

void MainWindow::addRecentProject(const hstring& path){
    if(path.empty()){
        return;
    }

    _addRecentVideo(m_recentProjects, m_maxRecentProjects, path);
    refreshRecentProjectsMenu();
    saveAppSettings();
}

void MainWindow::resetProjectState(bool clearLoadedVideo){
    ++m_timelineRenderVersion;
    m_projectPath.clear();
    m_selectedKeyFrames.clear();
    m_cutScenes.clear();
    m_frameIndex.clear();
    m_mediaInfo = MediaInspectionResult{};
    m_timelineDurationSeconds = 0;
    m_keepAudio = true;
    m_audioCrossfadeMs = 0;
    TimelineZoomSlider().Value(3);
    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();

    ThumbnailLayer().Children().Clear();
    CutOverlayLayer().Children().Clear();
    TimelineTickCanvas().Children().Clear();
    TimelineCanvas().Width(640.0);
    TimelineTickCanvas().Width(640.0);
    Controls::Canvas::SetLeft(TimelineCursor(), 0);
    syncTimelineHorizontalScrollBar();

    if(clearLoadedVideo){
        if(m_player){
            m_player.Pause();
        }
        m_loadedFile = nullptr;
        m_player.Source(nullptr);
    }

    StatusText().Text(L"Load or drag-and-drop an .mp4/.mov file to preview.");
    m_lastSavedProjectSnapshot = buildProjectSnapshot();
    updateWindowTitle();
}

wstring MainWindow::buildProjectSnapshot(){
    const auto rapMarkers{buildRapMarkersFromSelection(m_frameIndex, m_selectedKeyFrames)};
    const auto snapshot{std::format(
        L"{}={}\n{}={:.15g}\n{}={}\n{}={}\n{}={}\n{}={}\n",
        P_FILE_PATH,
        (m_loadedFile ? m_loadedFile.Path().c_str() : L""),
        P_STORYLINE_ZOOM,
        TimelineZoomSlider().Value(),
        P_KEEP_AUDIO,
        m_keepAudio ? 1 : 0,
        P_AUDIO_CROSSFADE_MS,
        m_audioCrossfadeMs,
        P_RAP_MARKERS,
        serializeKeyframeVector(rapMarkers),
        P_CUT_SCENES,
        serializeIndexList(m_cutScenes))};
    return snapshot;
}

bool MainWindow::isProjectDirty(){
    return buildProjectSnapshot() != m_lastSavedProjectSnapshot;
}

void MainWindow::updateWindowTitle(){
    wstring projectName{L"Untitled"};
    if(!m_projectPath.empty()){
        const auto projectPath{filesystem::path(m_projectPath.c_str())};
        const auto stem{projectPath.stem().wstring()};
        projectName = stem.empty() ? projectPath.filename().wstring() : stem;
        if(projectName.empty()){
            projectName = L"Untitled";
        }
    }

    if(isProjectDirty()){
        projectName += L"*";
    }

    const wstring loadedFile{m_loadedFile ? m_loadedFile.Path().c_str() : L"No file"};
    Title(hstring(std::format(L"llvc - Lossless Video Cut - {} - {}", projectName, loadedFile)));
}

IOpBool MainWindow::ensureProjectSavedBeforeContinuingAsync(){
    if(!isProjectDirty()){
        co_return true;
    }

    Controls::ContentDialog dialog{};
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(L"Unsaved changes"));
    dialog.Content(box_value(L"Current project has unsaved changes. Save before continuing?"));
    dialog.PrimaryButtonText(L"Save");
    dialog.SecondaryButtonText(L"Don't save");
    dialog.CloseButtonText(L"Cancel");

    const auto choice{co_await dialog.ShowAsync()};
    if(choice == Controls::ContentDialogResult::Primary){
        co_await saveProjectMenuItem_Click(nullptr, RoutedEventArgs{});
        co_return !isProjectDirty();
    }
    if(choice == Controls::ContentDialogResult::Secondary){
        co_return true;
    }

    co_return false;
}

AAction MainWindow::openProjectFileAsync(const SFile& file){
    resetProjectState(true);

    const auto lines{co_await FileIO::ReadLinesAsync(file)};
    auto projectData{parseProjectLines(lines, TimelineZoomSlider().Value(), m_keepAudio, m_audioCrossfadeMs)};

    if(!projectData.loadedFilePath.empty()){
        try{
            const auto videoFile{co_await StorageFile::GetFileFromPathAsync(projectData.loadedFilePath)};
            co_await loadVideoFileAsync(videoFile);
        }catch(...){
            StatusText().Text(L"Project opened, but referenced video could not be loaded");
        }
    }

    projectData.zoomLevel = clamp(projectData.zoomLevel, TimelineZoomSlider().Minimum(), TimelineZoomSlider().Maximum());
    TimelineZoomSlider().Value(projectData.zoomLevel);
    m_keepAudio = projectData.keepAudio;
    m_audioCrossfadeMs = normalizeAudioCrossfadeMs(projectData.audioCrossfadeMs);
    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();

    m_frameIndex = std::move(projectData.markers);
    sort(m_frameIndex.begin(), m_frameIndex.end(), [](const IndexedFrameSample& a, const IndexedFrameSample& b){
        return a.time100ns < b.time100ns;
    });
    const auto markerCount{m_frameIndex.size()};
    refreshSelectedMarkers(m_selectedKeyFrames, markerCount);

    m_cutScenes.clear();
    const auto sceneCount{static_cast<uint32_t>(markerCount + 1)};
    for(const auto sceneIndex: projectData.cutScenes){
        if(sceneIndex < sceneCount){
            m_cutScenes.push_back(sceneIndex);
        }
    }
    sort(m_cutScenes.begin(), m_cutScenes.end());
    m_cutScenes.erase(unique(m_cutScenes.begin(), m_cutScenes.end()), m_cutScenes.end());
    m_projectPath = file.Path();

    addRecentProject(file.Path());
    m_lastSavedProjectSnapshot = buildProjectSnapshot();
    StatusText().Text(L"Project loaded");
    updateWindowTitle();
}

AAction MainWindow::saveProjectFileAsync(const SFile& file){
    vector<hstring> lines;
    lines.emplace_back(L"# llvc project file");
    lines.emplace_back(wstring(P_FILE_PATH) + L"=" + wstring(m_loadedFile ? m_loadedFile.Path().c_str() : L""));
    lines.emplace_back(wstring(P_STORYLINE_ZOOM) + L"=" + to_wstring(TimelineZoomSlider().Value()));
    lines.emplace_back(wstring(P_KEEP_AUDIO) + L"=" + wstring(m_keepAudio ? L"1" : L"0"));
    lines.emplace_back(wstring(P_AUDIO_CROSSFADE_MS) + L"=" + to_wstring(m_audioCrossfadeMs));
    lines.emplace_back(wstring(P_RAP_MARKERS) + L"=" + serializeKeyframeVector(buildRapMarkersFromSelection(m_frameIndex, m_selectedKeyFrames)));
    lines.emplace_back(wstring(P_CUT_SCENES) + L"=" + serializeIndexList(m_cutScenes));

    co_await FileIO::WriteLinesAsync(file, single_threaded_vector<hstring>(move(lines)));
    m_projectPath = file.Path();
    addRecentProject(file.Path());
    m_lastSavedProjectSnapshot = buildProjectSnapshot();
    StatusText().Text(L"Project saved");
    updateWindowTitle();
}

AAction MainWindow::showInfoDialogAsync(const hstring& title, const hstring& message){
    Controls::ContentDialog dialog{};
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(title));
    dialog.Content(box_value(message));
    dialog.CloseButtonText(L"OK");
    co_await dialog.ShowAsync();
}

AAction MainWindow::showPropertiesDialogAsync(){
    if(!m_loadedFile || !m_mediaInfo.isValid){
        co_await showInfoDialogAsync(L"Properties", L"No video is currently loaded.");
        co_return;
    }

    wstring content;
    content += L"File: "; content += m_loadedFile.Path().c_str(); content += L"\n";
    content += L"Container: "; content += m_mediaInfo.container; content += L"\n";
    content += L"Duration: "; content += m_mediaInfo.duration; content += L"\n";
    content += L"Size: "; content += m_mediaInfo.fileSize; content += L"\n";
    content += L"Video codec: "; content += m_mediaInfo.videoCodec; content += L"\n";
    content += L"Resolution: "; content += m_mediaInfo.resolution; content += L"\n";
    content += L"FPS: "; content += formatRatio(m_mediaInfo.frameRate.num, m_mediaInfo.frameRate.den, L" fps"); content += L"\n";
    content += L"Video bitrate: "; content += m_mediaInfo.videoBitrate; content += L"\n";
    content += L"Random access points: "; content += m_mediaInfo.keyFrameSummary; content += L"\n";
    content += L"Random access interval: "; content += m_mediaInfo.keyFrameInterval; content += L"\n";
    content += L"All samples independent: "; content += m_mediaInfo.allSamplesIndependent; content += L"\n";
    content += L"Max random access spacing: "; content += m_mediaInfo.maxKeyFrameSpacing; content += L"\n";
    content += L"Audio codec: "; content += m_mediaInfo.audioCodec; content += L"\n";
    content += L"Audio bitrate: "; content += m_mediaInfo.audioBitrate;

    co_await showInfoDialogAsync(L"Properties", hstring(content));
}

AAction MainWindow::showOptionsDialogAsync(){
    Controls::StackPanel panel{};
    panel.Spacing(10);

    Controls::TextBlock videosLabel{};
    videosLabel.Text(L"Recent videos to keep (1-20)");
    Controls::NumberBox videosCount{};
    videosCount.Minimum(1);
    videosCount.Maximum(20);
    videosCount.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
    videosCount.Value(m_maxRecentVideos);

    Controls::TextBlock projectsLabel{};
    projectsLabel.Text(L"Recent projects to keep (1-20)");
    Controls::NumberBox projectsCount{};
    projectsCount.Minimum(1);
    projectsCount.Maximum(20);
    projectsCount.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
    projectsCount.Value(m_maxRecentProjects);

    panel.Children().Append(videosLabel);
    panel.Children().Append(videosCount);
    panel.Children().Append(projectsLabel);
    panel.Children().Append(projectsCount);

    Controls::ContentDialog dialog{};
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(L"Options"));
    dialog.Content(panel);
    dialog.PrimaryButtonText(L"Save");
    dialog.CloseButtonText(L"Cancel");

    const auto dialogResult{co_await dialog.ShowAsync()};
    if(dialogResult != Controls::ContentDialogResult::Primary){
        co_return;
    }

    m_maxRecentVideos = static_cast<uint32_t>(clamp(static_cast<int>(lround(videosCount.Value())), 1, 20));
    m_maxRecentProjects = static_cast<uint32_t>(clamp(static_cast<int>(lround(projectsCount.Value())), 1, 20));

    if(m_recentVideos.size() > m_maxRecentVideos){
        m_recentVideos.resize(m_maxRecentVideos);
    }
    if(m_recentProjects.size() > m_maxRecentProjects){
        m_recentProjects.resize(m_maxRecentProjects);
    }

    refreshRecentVideosMenu();
    refreshRecentProjectsMenu();
    saveAppSettings();
}

bool MainWindow::isSupportedVideoSubtype(const GUID& subtype){
    return subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265;
}

wstring MainWindow::guidToCodecName(const GUID& subtype, bool isVideo){
    if(isVideo){
        if(subtype == MFVideoFormat_H264){
            return L"H.264";
        }
        if(subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265){
            return L"HEVC";
        }
    }else{
        if(subtype == MFAudioFormat_AAC){
            return L"AAC";
        }
        if(subtype == MFAudioFormat_MP3){
            return L"MP3";
        }
        if(subtype == MFAudioFormat_PCM){
            return L"PCM";
        }
    }

    return formatGuid(subtype);
}

MediaInspectionResult MainWindow::inspectMediaFile(const wstring& filePath){
    MediaInspectionResult result{ .keyFrameSummary       = L"unknown",
                                  .keyFrameInterval      = L"unknown",
                                  .allSamplesIndependent = L"unknown",
                                  .maxKeyFrameSpacing    = L"unknown" };
    MFLifetime mf{};

    com_ptr<IMFSourceReader> reader;
    check_hresult(MFCreateSourceReaderFromURL(filePath.c_str(), nullptr, reader.put()));

    uint32_t videoCount{};
    uint32_t audioCount{};
    bool hasText{};
    GUID videoSubtype{GUID_NULL};
    GUID audioSubtype{GUID_NULL};
    uint32_t width{};
    uint32_t height{};
    uint32_t fpsNum{};
    uint32_t fpsDen{};
    uint32_t videoBitrate{};
    uint32_t audioBitrate{};
    constexpr auto invalidStreamIndex{numeric_limits<DWORD>::max()};
    auto videoStreamIndex{invalidStreamIndex};
    uint32_t allSamplesIndependent{};
    uint32_t maxKeyFrameSpacing{};

    for(DWORD streamIndex{0};; ++streamIndex){
        com_ptr<IMFMediaType> type;
        const HRESULT hr{reader->GetNativeMediaType(streamIndex, 0, type.put())};
        if(hr == MF_E_INVALIDSTREAMNUMBER){
            break;
        }
        check_hresult(hr);

        GUID major{GUID_NULL};
        GUID subtype{GUID_NULL};
        check_hresult(type->GetGUID(MF_MT_MAJOR_TYPE, &major));
        check_hresult(type->GetGUID(MF_MT_SUBTYPE, &subtype));

        if(major == MFMediaType_Video){
            ++videoCount;
            videoSubtype = subtype;
            videoStreamIndex = streamIndex;
            MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &width, &height);
            MFGetAttributeRatio(type.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
            (void)type->GetUINT32(MF_MT_AVG_BITRATE, &videoBitrate);
            if(SUCCEEDED(type->GetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, &allSamplesIndependent))){
                result.allSamplesIndependent = allSamplesIndependent != 0 ? L"yes" : L"no";
            }
            if(SUCCEEDED(type->GetUINT32(MF_MT_MAX_KEYFRAME_SPACING, &maxKeyFrameSpacing))){
                result.maxKeyFrameSpacing = to_wstring(maxKeyFrameSpacing) + L" frames";
            }
        }else if(major == MFMediaType_Audio){
            ++audioCount;
            audioSubtype = subtype;
            (void)type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &audioBitrate);
        }else{
            hasText = true;
        }
    }

    if(videoStreamIndex != invalidStreamIndex){
        if(auto bestVideoType{chooseBestNativeVideoMediaType(reader.get(), videoStreamIndex)}){
            (void)reader->SetCurrentMediaType(videoStreamIndex, nullptr, bestVideoType.get());

            MFGetAttributeSize(bestVideoType.get(), MF_MT_FRAME_SIZE, &width, &height);
            MFGetAttributeRatio(bestVideoType.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
            (void)bestVideoType->GetUINT32(MF_MT_AVG_BITRATE, &videoBitrate);
            if(SUCCEEDED(bestVideoType->GetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, &allSamplesIndependent))){
                result.allSamplesIndependent = allSamplesIndependent != 0 ? L"yes" : L"no";
            }
            if(SUCCEEDED(bestVideoType->GetUINT32(MF_MT_MAX_KEYFRAME_SPACING, &maxKeyFrameSpacing))){
                result.maxKeyFrameSpacing = to_wstring(maxKeyFrameSpacing) + L" frames";
            }
        }
    }

    if(videoCount != 1){
        result.errorMessage = L"Expected exactly one video stream.";
        return result;
    }
    if(audioCount > 1){
        result.errorMessage = L"Multiple audio streams are not supported.";
        return result;
    }
    if(hasText){
        result.errorMessage = L"Subtitle/text streams are not supported.";
        return result;
    }
    if(!isSupportedVideoSubtype(videoSubtype)){
        result.errorMessage = L"Video codec not supported. Only H.264 and HEVC are allowed.";
        return result;
    }

    wstring lowerPath{filePath};
    transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::towlower);
    if(lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == L".mp4"){
        result.container = L"MP4";
    }else if(lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == L".mov"){
        result.container = L"MOV";
    }else{
        result.errorMessage = L"Container not supported. Only MP4 and MOV are allowed.";
        return result;
    }

    if(videoSubtype == MFVideoFormat_HEVC || videoSubtype == MFVideoFormat_H265){
        if(!hasDecoderForSubtype(videoSubtype)){
            result.errorMessage = L"HEVC support missing (install HEVC Video Extensions)";
            return result;
        }
    }else if(!hasDecoderForSubtype(videoSubtype)){
        result.errorMessage = L"No decoder available";
        return result;
    }

    PROPVARIANT duration{};
    PropVariantInit(&duration);
    if(SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) && duration.vt == VT_UI8){
        const auto seconds {duration.uhVal.QuadPart / 10'000'000.0};
        result.duration = std::format(L"{:.3f} s", seconds);
    }
    PropVariantClear(&duration);

    result.videoCodec = guidToCodecName(videoSubtype, true);
    result.audioCodec = audioCount == 0 ? L"none" : guidToCodecName(audioSubtype, false);
    result.resolution = width > 0 ? (to_wstring(width) + L"x" + to_wstring(height)) : L"-";
    result.frameRate = {fpsNum, fpsDen};
    result.videoBitrate = videoBitrate > 0 ? (to_wstring(videoBitrate / 1000) + L" kbps") : L"-";
    result.audioBitrate = audioBitrate > 0 ? (to_wstring((audioBitrate * 8) / 1000) + L" kbps") : L"none";
    if(videoStreamIndex != invalidStreamIndex){
        analyzeKeyFrameCadence(reader.get(), videoStreamIndex, fpsNum, fpsDen, result);
    }
    result.isValid = true;
    return result;
}

void MainWindow::window_DragOver(const Control&, const DEArgs& e){
    e.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
}

AAction MainWindow::window_Drop(const Control&, const DEArgs& e){
    const auto view{e.DataView()};
    if(!view.Contains(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems())){
        StatusText().Text(L"Dropped content is not a file");
        co_return;
    }

    const auto items{co_await view.GetStorageItemsAsync()};
    if(items.Size() != 1){
        StatusText().Text(L"Only support a single .mp4 or .mov file");
        co_return;
    }

    const auto file{items.GetAt(0).try_as<StorageFile>()};
    if(!file){
        StatusText().Text(L"Dropped content is not a file");
        __debugbreak(); // this should have been caught earlier in this function!
        co_return;
    }

    {
        const auto ext{file.FileType()};
        wstring lower{ext.c_str()};
        transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        if(lower != L".mp4" && lower != L".mov"){
            StatusText().Text(L"Only .mp4 and .mov files are supported");
            co_return;
        }
    }

    co_await loadVideoFileAsync(file);
}

AAction MainWindow::loadVideoFileAsync(const SFile& file){
    MediaInspectionResult inspected{};
    try{
        inspected = inspectMediaFile(file.Path().c_str());
    }catch(const winrt::hresult_error& ex){
        inspected.errorMessage = L"No decoder available";
        if(ex.code() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)){
            inspected.errorMessage = L"File not found";
        }
    }

    if(!inspected.isValid){
        wstring status{L"Open rejected: "};
        status += inspected.errorMessage;
        StatusText().Text(status);
        co_await showInfoDialogAsync(L"Unsupported media", hstring(status));
        co_return;
    }

    const auto basicProperties{co_await file.GetBasicPropertiesAsync()};
    inspected.fileSize = formatFileSize(basicProperties.Size());
    m_mediaInfo = inspected;

    wstring status{L"Loaded: "};
    status += file.Name().c_str();
    status += L" (loading story line...)";
    StatusText().Text(status);

    m_frameIndex.clear();

    const auto source{Windows::Media::Core::MediaSource::CreateFromStorageFile(file)};
    m_player.Source(source);
    m_player.IsMuted(false);
    m_loadedFile = file;
    if(m_projectPath.empty()){
        m_selectedKeyFrames.clear();
        m_cutScenes.clear();
    }
    addRecentVideo(file.Path());

    ThumbnailLayer().Children().Clear();
    CutOverlayLayer().Children().Clear();
    TimelineCanvas().Width(640.0);
    TimelineTickCanvas().Width(640.0);
    TimelineTickCanvas().Children().Clear();
    m_timelineDurationSeconds = 0;
    Controls::Canvas::SetLeft(TimelineCursor(), 0);
    syncTimelineHorizontalScrollBar();

    updateAudioUiAndPlaybackState();
    updateWindowTitle();
}

bool MainWindow::sourceHasAudio() const{
    return m_mediaInfo.isValid && m_mediaInfo.audioCodec != L"none";
}

int32_t MainWindow::normalizeAudioCrossfadeMs(int32_t valueMs){
    const auto nearest{min_element(AUDIO_CROSSFADE_PRESETS_MS.begin(), AUDIO_CROSSFADE_PRESETS_MS.end(), [valueMs](auto a, auto b){
        return abs(a - valueMs) < abs(b - valueMs);
    })};
    return nearest == AUDIO_CROSSFADE_PRESETS_MS.end() ? 0 : *nearest;
}

void MainWindow::syncAudioCrossfadeComboSelection(){
    const auto target{normalizeAudioCrossfadeMs(m_audioCrossfadeMs)};
    m_audioCrossfadeMs = target;

    const auto combo{AudioCrossfadeComboBox()};
    const auto items{combo.Items()};
    for(uint32_t i{0}; i < items.Size(); ++i){
        const auto item{items.GetAt(i).try_as<Controls::ComboBoxItem>()};
        if(!item){
            continue;
        }

        if(!item.Tag()){
            continue;
        }

        try{
            const auto tag{unbox_value<hstring>(item.Tag())};
            if(stoi(wstring(tag.c_str())) == target){
                combo.SelectedIndex(static_cast<int32_t>(i));
                return;
            }
        }catch(...){ }
    }

    combo.SelectedIndex(0);
}

void MainWindow::applyAudioSettingsToPlayer(){
    if(!m_player){
        return;
    }

    const auto allowAudio{sourceHasAudio() && m_keepAudio};
    m_player.IsMuted(!allowAudio);
}

void MainWindow::updateAudioUiAndPlaybackState(){
    const auto hasAudio{sourceHasAudio()};
    if(!hasAudio){
        m_keepAudio = false;
    }

    KeepAudioCheckBox().IsEnabled(hasAudio);
    KeepAudioCheckBox().IsChecked(box_value(hasAudio && m_keepAudio).as<IReference<bool>>());
    AudioCrossfadeComboBox().IsEnabled(hasAudio && m_keepAudio);
    applyAudioSettingsToPlayer();
}

winrt::fire_and_forget MainWindow::renderTimelineAsync(){
    const auto lifetime{get_strong()};

    if(m_isClosing || !m_loadedFile || m_timelineDurationSeconds <= 0){
        co_return;
    }

    if(!DispatcherQueue().HasThreadAccess()){
        const auto weak{get_weak()};
        DispatcherQueue().TryEnqueue([weak](){
            if(const auto self{weak.get()}){
                self->renderTimelineAsync();
            }
        });
        co_return;
    }

    try{
        const auto renderVersion{++m_timelineRenderVersion};
        const auto zoom{TimelineZoomSlider().Value()};
        const auto totalWidth{max(800.0, m_timelineDurationSeconds * 14.0 * zoom)};
        const auto thumbnailCount{clamp(static_cast<int>(totalWidth / 150.0), 8, 96)};
        const auto thumbnailWidth{totalWidth / thumbnailCount};

        TimelineCanvas().Width(totalWidth);
        ThumbnailLayer().Children().Clear();
        CutOverlayLayer().Width(totalWidth);
        renderTimelineTicks();
        renderKeyframeTicks();
        renderCutOverlays();
        syncTimelineHorizontalScrollBar();

        const auto clip{co_await Windows::Media::Editing::MediaClip::CreateFromFileAsync(m_loadedFile)};
        Windows::Media::Editing::MediaComposition composition{};
        composition.Clips().Append(clip);

        if(renderVersion != m_timelineRenderVersion){
            co_return;
        }

        vector<bool> thumbnailBuilt(thumbnailCount, false);

        for(int builtCount{0}; builtCount < thumbnailCount; ++builtCount){
            if(renderVersion != m_timelineRenderVersion || m_isClosing){
                co_return;
            }

            const auto scrollViewer{TimelineScrollViewer()};
            const auto viewportWidth{max(0.0, scrollViewer.ViewportWidth())};
            const auto viewportLeft{scrollViewer.HorizontalOffset()};
            const auto viewportRight{viewportLeft + viewportWidth};
            const auto firstVisibleIndex{clamp(static_cast<int>(floor(viewportLeft / thumbnailWidth)), 0, thumbnailCount - 1)};
            const auto lastVisibleIndex{clamp(static_cast<int>(floor(max(viewportLeft, viewportRight - 1.0) / thumbnailWidth)), 0, thumbnailCount - 1)};

            auto nextIndex{-1};
            for(auto i{firstVisibleIndex}; i <= lastVisibleIndex; ++i){
                if(!thumbnailBuilt[i]){
                    nextIndex = i;
                    break;
                }
            }

            if(nextIndex < 0){
                auto left{firstVisibleIndex - 1};
                auto right{lastVisibleIndex + 1};
                while(nextIndex < 0 && (left >= 0 || right < thumbnailCount)){
                    if(right < thumbnailCount && !thumbnailBuilt[right]){
                        nextIndex = right;
                        break;
                    }
                    ++right;

                    if(left >= 0 && !thumbnailBuilt[left]){
                        nextIndex = left;
                        break;
                    }
                    --left;
                }
            }

            if(nextIndex < 0){
                break;
            }

            const auto t{(nextIndex + 0.5) / thumbnailCount};
            const auto stream{co_await composition.GetThumbnailAsync(secondsToTimeSpan(t * m_timelineDurationSeconds), 180, 96, Windows::Media::Editing::VideoFramePrecision::NearestFrame)};
            if(renderVersion != m_timelineRenderVersion || m_isClosing){
                co_return;
            }

            Controls::Image image{};
            image.Width(max(8.0, thumbnailWidth - 2.0));
            image.Height(86);
            image.Stretch(Media::Stretch::UniformToFill);

            Media::Imaging::BitmapImage bitmap{};
            co_await bitmap.SetSourceAsync(stream);
            image.Source(bitmap);

            Controls::Canvas::SetLeft(image, nextIndex * thumbnailWidth);
            ThumbnailLayer().Children().Append(image);
            thumbnailBuilt[nextIndex] = true;
            renderCutOverlays();
        }

        updateTimelineCursorFromPlayback();
        ensureTimelineCursorVisible(Controls::Canvas::GetLeft(TimelineCursor()));
        syncTimelineHorizontalScrollBar();

        wstring status{L"Loaded: "};
        status += m_loadedFile.Name().c_str();
        status += L" (story line ready)";
        StatusText().Text(status);
    }catch(const winrt::hresult_error& ex){
        wstring status{L"Failed to render story line: "};
        status += ex.message().c_str();
        StatusText().Text(status);
    }
}

}
