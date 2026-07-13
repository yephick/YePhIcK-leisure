module;

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>

#include <shobjidl_core.h>

#include <wil/resource.h>


export module llvc.Export;

import std;
import llvc.VideoStream;

export namespace llvc{

using namespace ::std;
using namespace ::winrt;

struct MFLifetime{
    MFLifetime();
    MFLifetime(const MFLifetime&) = delete;
    MFLifetime& operator=(const MFLifetime&) = delete;
    ~MFLifetime();
};

bool hasDecoderForSubtype(const GUID& subtype);

struct KeyFrameCadenceInfo{
    wstring summary{};
    wstring interval{};
};

KeyFrameCadenceInfo analyzeKeyFrameCadence(IMFSourceReader* reader, DWORD videoStreamIndex, uint32_t fpsNum, uint32_t fpsDen);

wstring formatDurationFileTag(int64_t duration100ns);
com_ptr<IMFMediaType> chooseBestNativeVideoMediaType(const com_ptr<IMFSourceReader>& reader, DWORD streamIndex);
com_ptr<IMFMediaType> chooseBestNativeVideoMediaTypeForSubtypes(const com_ptr<IMFSourceReader>& reader, DWORD streamIndex, const vector<GUID>& allowedSubtypes);
com_ptr<IMFMediaType> chooseFirstNativeVideoMediaTypeForSubtypes(const com_ptr<IMFSourceReader>& reader, DWORD streamIndex, const vector<GUID>& allowedSubtypes);
com_ptr<IMFMediaType> createPcmFloatAudioType(uint32_t sampleRate, uint32_t channels);
com_ptr<IMFMediaType> createAacOutputType(uint32_t sampleRate, uint32_t channels);
vector<float> decodeAudioRangeToFloat(const com_ptr<IMFSourceReader>& reader, DWORD audioStreamIndex, int64_t rangeStart100ns, int64_t rangeEnd100ns, uint32_t channels, uint32_t sampleRate);

wstring pickExportOutputPath(const std::filesystem::path& sourceFsPath, const vector<wstring>& allowedExtensions, const wchar_t* defaultExt, int64_t outputDuration100ns, HWND ownerWindow);
void appendCrossfadedAudioSegment(vector<float>& mixedAudio, const vector<float>& segmentAudio, uint32_t audioChannels, size_t fadeFrames);
void writePcmAudioFramesToWriter(IMFSinkWriter* writer, DWORD writerAudioStreamIndex, const vector<float>& audioFrames, uint32_t audioChannels, uint32_t audioSampleRate, uint64_t& writtenFrames);
void writeMixedAudioForKeepRanges(const com_ptr<IMFSourceReader>& audioReader, DWORD audioStreamIndex, const vector<pair<int64_t, int64_t>>& keepRanges100ns, IMFSinkWriter* writer, DWORD writerAudioStreamIndex, uint32_t audioChannels, uint32_t audioSampleRate, int crossfadeMs, float gain, const function<void(double)>& progressCallback = {}, const function<bool()>& shouldCancel = {});
void applyExportFileMetadata(const std::wstring& sourcePath, const std::wstring& outputPath, const std::wstring& exportComment);

}

namespace llvc{

using namespace ::std;
using namespace ::winrt;

namespace{

mutex g_mediaFoundationLifetimeMutex;
uint32_t g_mediaFoundationLifetimeCount{};

}

MFLifetime::MFLifetime(){
    lock_guard lock{g_mediaFoundationLifetimeMutex};
    if(g_mediaFoundationLifetimeCount == 0){
        check_hresult(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    }
    ++g_mediaFoundationLifetimeCount;
}

MFLifetime::~MFLifetime(){
    lock_guard lock{g_mediaFoundationLifetimeMutex};
    if(g_mediaFoundationLifetimeCount == 0){
        return;
    }
    if(--g_mediaFoundationLifetimeCount == 0){
        MFShutdown();
    }
}

bool hasDecoderForSubtype(const GUID& subtype){
    MFT_REGISTER_TYPE_INFO inType{};
    inType.guidMajorType = MFMediaType_Video;
    inType.guidSubtype = subtype;

    IMFActivate** activates{};
    UINT32 count{};
    const HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE,
        &inType,
        nullptr,
        &activates,
        &count);

    if(SUCCEEDED(hr) && activates){
        for(UINT32 i = 0; i < count; ++i){
            activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }

    return SUCCEEDED(hr) && count > 0;
}

KeyFrameCadenceInfo analyzeKeyFrameCadence(IMFSourceReader* reader, DWORD videoStreamIndex, uint32_t fpsNum, uint32_t fpsDen){
    KeyFrameCadenceInfo info{.summary = L"unknown", .interval = L"unknown"};
    if(!reader){
        return info;
    }

    constexpr uint32_t maxSamplesToInspect{900};
    constexpr int64_t maxSpan100ns{60LL * 10'000'000LL};

    PROPVARIANT startPos{};
    startPos.vt = VT_I8;
    startPos.hVal.QuadPart = 0;
    check_hresult(reader->SetCurrentPosition(GUID_NULL, startPos));
    PropVariantClear(&startPos);

    uint32_t sampledFrames{};
    uint32_t keyFrames{};
    bool cleanPointSeen{};
    LONGLONG firstTimestamp{-1};
    LONGLONG previousKeyTimestamp{-1};
    vector<double> keyIntervalsSec{};

    for(uint32_t i = 0; i < maxSamplesToInspect; ++i){
        DWORD actualStreamIndex{};
        DWORD flags{};
        LONGLONG timestamp{};
        com_ptr<IMFSample> sample{};

        const HRESULT hr = reader->ReadSample(videoStreamIndex, 0, &actualStreamIndex, &flags, &timestamp, sample.put());
        if(FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)){
            break;
        }
        if(!sample){
            continue;
        }

        ++sampledFrames;
        if(firstTimestamp < 0){
            firstTimestamp = timestamp;
        }

        UINT32 cleanPoint{};
        if(SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0){
            cleanPointSeen = true;
            ++keyFrames;
            if(previousKeyTimestamp >= 0 && timestamp > previousKeyTimestamp){
                keyIntervalsSec.push_back((timestamp - previousKeyTimestamp) / 10'000'000.0);
            }
            previousKeyTimestamp = timestamp;
        }

        if(firstTimestamp >= 0 && timestamp - firstTimestamp >= maxSpan100ns){
            break;
        }
    }

    if(sampledFrames == 0){
        info.summary = L"unknown (no samples read)";
        info.interval = L"unknown";
        return info;
    }

    if(!cleanPointSeen){
        info.summary = L"unknown (clean-point flags unavailable)";
        info.interval = L"unknown";
        return info;
    }

    const auto ratio {(1.0 * keyFrames) / sampledFrames};
    info.summary = std::format(L"{} key frames / {} sampled frames ({:.2f}%)", keyFrames, sampledFrames, ratio * 100.0);

    if(keyIntervalsSec.empty()){
        info.interval = L"unknown (insufficient key frames sampled)";
        return info;
    }

    const auto sum{accumulate(keyIntervalsSec.begin(), keyIntervalsSec.end(), 0.0)};
    const auto avg{sum / keyIntervalsSec.size()};
    const auto minIt{min_element(keyIntervalsSec.begin(), keyIntervalsSec.end())};
    const auto maxIt{max_element(keyIntervalsSec.begin(), keyIntervalsSec.end())};

    auto text{std::format(L"avg {:.3f} s, min {:.3f} s, max {:.3f} s", avg, *minIt, *maxIt)};
    if(fpsNum > 0 && fpsDen > 0){
        const auto fps{(1.0 * fpsNum) / fpsDen};
        text += std::format(L" (~{:.1f} frames avg)", avg * fps);
    }

    info.interval = text;
    return info;
}

wstring formatDurationFileTag(int64_t duration100ns){
    const auto totalMs{max<int64_t>(0, (duration100ns + 5'000) / 10'000)};
    const auto minutes{totalMs / 60'000};
    const auto seconds{(totalMs / 1'000) % 60};
    const auto millis{totalMs % 1'000};
    return std::format(L"{:02}{:02}{:03}", minutes, seconds, millis);
}

com_ptr<IMFMediaType> chooseBestNativeVideoMediaType(const com_ptr<IMFSourceReader>& reader, DWORD streamIndex){
    return chooseBestNativeVideoMediaTypeForSubtypes(reader, streamIndex, {});
}

com_ptr<IMFMediaType> chooseBestNativeVideoMediaTypeForSubtypes(const com_ptr<IMFSourceReader>& reader, DWORD streamIndex, const vector<GUID>& allowedSubtypes){
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
        GUID subtype{GUID_NULL};
        check_hresult(type->GetGUID(MF_MT_MAJOR_TYPE, &major));
        check_hresult(type->GetGUID(MF_MT_SUBTYPE, &subtype));
        if(major != MFMediaType_Video){
            continue;
        }
        if(!allowedSubtypes.empty() && ranges::find(allowedSubtypes, subtype) == allowedSubtypes.end()){
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

com_ptr<IMFMediaType> chooseFirstNativeVideoMediaTypeForSubtypes(const com_ptr<IMFSourceReader>& reader, DWORD streamIndex, const vector<GUID>& allowedSubtypes){
    for(DWORD mediaTypeIndex{0};; ++mediaTypeIndex){
        com_ptr<IMFMediaType> type;
        const auto hr{reader->GetNativeMediaType(streamIndex, mediaTypeIndex, type.put())};
        if(hr == MF_E_NO_MORE_TYPES){
            break;
        }
        check_hresult(hr);

        GUID major{GUID_NULL};
        GUID subtype{GUID_NULL};
        check_hresult(type->GetGUID(MF_MT_MAJOR_TYPE, &major));
        check_hresult(type->GetGUID(MF_MT_SUBTYPE, &subtype));
        if(major != MFMediaType_Video){
            continue;
        }
        if(!allowedSubtypes.empty() && ranges::find(allowedSubtypes, subtype) == allowedSubtypes.end()){
            continue;
        }

        return type;
    }

    return nullptr;
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

vector<float> decodeAudioRangeToFloat(const com_ptr<IMFSourceReader>& reader, DWORD audioStreamIndex, int64_t rangeStart100ns, int64_t rangeEnd100ns, uint32_t channels, uint32_t sampleRate){
    vector<float> out;
    if(rangeEnd100ns <= rangeStart100ns || channels == 0 || sampleRate == 0){
        return out;
    }

    PROPVARIANT pos{.vt = VT_I8, .hVal = {.QuadPart = rangeStart100ns}};
    check_hresult(reader->SetCurrentPosition(GUID_NULL, pos));
    PropVariantClear(&pos);

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

        const auto totalFrames{static_cast<int64_t>(curLen / (channels * sizeof(float)))};
        const auto trimStart100ns{max<int64_t>(0, rangeStart100ns - sampleStart100ns)};
        const auto trimEnd100ns{max<int64_t>(0, sampleEnd100ns - rangeEnd100ns)};
        const auto trimStartFrames{min<int64_t>(totalFrames, (trimStart100ns * sampleRate) / 10'000'000)};
        const auto trimEndFrames{min<int64_t>(totalFrames - trimStartFrames, (trimEnd100ns * sampleRate) / 10'000'000)};
        const auto keepFrames{max<int64_t>(0, totalFrames - trimStartFrames - trimEndFrames)};

        if(keepFrames > 0){
            const auto* src{reinterpret_cast<const float*>(bytes)};
            const auto begin{src + (trimStartFrames * channels)};
            out.insert(out.end(), begin, begin + (keepFrames * channels));
        }

        contiguous->Unlock();
    }

    return out;
}

wstring pickExportOutputPath(const std::filesystem::path& sourceFsPath, const vector<wstring>& allowedExtensions, const wchar_t* defaultExt, int64_t outputDuration100ns, HWND ownerWindow){
    if(allowedExtensions.empty()){
        return {};
    }

    com_ptr<IFileSaveDialog> saveDialog;
    check_hresult(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(saveDialog.put())));

    DWORD options{};
    check_hresult(saveDialog->GetOptions(&options));
    check_hresult(saveDialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT));

    const auto requestedDefaultExt{defaultExt && *defaultExt ? wstring{defaultExt} : allowedExtensions.front()};
    auto defaultExtensionIndex{size_t{}};
    auto foundDefaultExtension{false};
    for(size_t i{}; i < allowedExtensions.size(); ++i){
        if(_wcsicmp(allowedExtensions[i].c_str(), requestedDefaultExt.c_str()) == 0){
            defaultExtensionIndex = i;
            foundDefaultExtension = true;
            break;
        }
    }
    if(!foundDefaultExtension){
        defaultExtensionIndex = 0;
    }

    vector<wstring> labels;
    labels.reserve(allowedExtensions.size());
    vector<wstring> patterns;
    patterns.reserve(allowedExtensions.size());
    vector<COMDLG_FILTERSPEC> fileTypes;
    fileTypes.reserve(allowedExtensions.size());

    for(const auto& extension: allowedExtensions){
        auto normalizedExtension{extension};
        if(normalizedExtension.empty()){
            continue;
        }
        if(normalizedExtension.front() != L'.'){
            normalizedExtension.insert(normalizedExtension.begin(), L'.');
        }

        auto label{normalizedExtension.substr(1)};
        ranges::transform(label, label.begin(), [](wchar_t c){
            return static_cast<wchar_t>(towupper(c));
        });
        label += L" video";

        labels.push_back(std::move(label));
        patterns.push_back(L"*" + normalizedExtension);
    }

    for(size_t i{}; i < labels.size(); ++i){
        fileTypes.push_back(COMDLG_FILTERSPEC{
            .pszName = labels[i].c_str(),
            .pszSpec = patterns[i].c_str()});
    }

    check_hresult(saveDialog->SetFileTypes(static_cast<UINT>(fileTypes.size()), fileTypes.data()));
    check_hresult(saveDialog->SetFileTypeIndex(static_cast<UINT>(defaultExtensionIndex + 1)));

    const auto& selectedDefaultExtension{allowedExtensions[defaultExtensionIndex]};
    const auto defaultExtensionWithoutDot{
        !selectedDefaultExtension.empty() && selectedDefaultExtension.front() == L'.'
            ? selectedDefaultExtension.substr(1)
            : selectedDefaultExtension};
    check_hresult(saveDialog->SetDefaultExtension(defaultExtensionWithoutDot.c_str()));

    (void)outputDuration100ns;
    const auto now{chrono::system_clock::now()};
    const auto nowTimeT{chrono::system_clock::to_time_t(now)};
    tm localTime{};
    wchar_t timestamp[32]{};
    if(localtime_s(&localTime, &nowTimeT) != 0 || wcsftime(timestamp, size(timestamp), L"%Y%m%d-%H%M%S", &localTime) == 0){
        wcscpy_s(timestamp, L"export");
    }
    const auto suggestedName{sourceFsPath.stem().wstring() + L" - " + timestamp};
    check_hresult(saveDialog->SetFileName(suggestedName.c_str()));

    const auto sourceFolder{sourceFsPath.parent_path().wstring()};
    if(!sourceFolder.empty()){
        com_ptr<IShellItem> folderItem;
        if(SUCCEEDED(SHCreateItemFromParsingName(sourceFolder.c_str(), nullptr, IID_PPV_ARGS(folderItem.put())))){
            (void)saveDialog->SetDefaultFolder(folderItem.get());
            (void)saveDialog->SetFolder(folderItem.get());
        }
    }

    const auto showHr{saveDialog->Show(ownerWindow)};
    if(showHr == HRESULT_FROM_WIN32(ERROR_CANCELLED)){
        return {};
    }
    check_hresult(showHr);

    com_ptr<IShellItem> resultItem;
    check_hresult(saveDialog->GetResult(resultItem.put()));
    PWSTR selectedPath{};
    check_hresult(resultItem->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath));
    wstring outputPath{selectedPath ? selectedPath : L""};
    if(selectedPath){
        ::CoTaskMemFree(selectedPath);
    }
    return outputPath;
}

void appendCrossfadedAudioSegment(vector<float>& mixedAudio, const vector<float>& segmentAudio, uint32_t audioChannels, size_t fadeFrames){
    if(segmentAudio.empty()){
        return;
    }

    if(mixedAudio.empty() || fadeFrames == 0){
        mixedAudio.insert(mixedAudio.end(), segmentAudio.begin(), segmentAudio.end());
        return;
    }

    const auto mixedFrames{mixedAudio.size() / audioChannels};
    const auto segmentFrames{segmentAudio.size() / audioChannels};
    const auto overlapFrames{min<size_t>(fadeFrames, min(mixedFrames, segmentFrames))};
    if(overlapFrames == 0){
        mixedAudio.insert(mixedAudio.end(), segmentAudio.begin(), segmentAudio.end());
        return;
    }

    for(size_t frame{0}; frame < overlapFrames; ++frame){
        float fadeOut{};
        float fadeIn{};
        if(overlapFrames == 1){
            fadeOut = 0.0f;
            fadeIn = 1.0f;
        }else{
            const auto t{static_cast<float>(frame) / static_cast<float>(overlapFrames - 1)};
            fadeOut = cosf(t * static_cast<float>(numbers::pi_v<double> * 0.5));
            fadeIn = sinf(t * static_cast<float>(numbers::pi_v<double> * 0.5));
        }
        for(uint32_t ch{0}; ch < audioChannels; ++ch){
            const auto dstIndex{(mixedFrames - overlapFrames + frame) * audioChannels + ch};
            const auto srcIndex{frame * audioChannels + ch};
            mixedAudio[dstIndex] = (mixedAudio[dstIndex] * fadeOut) + (segmentAudio[srcIndex] * fadeIn);
        }
    }

    mixedAudio.insert(mixedAudio.end(), segmentAudio.begin() + static_cast<std::ptrdiff_t>(overlapFrames * audioChannels), segmentAudio.end());
}

void writePcmAudioFramesToWriter(IMFSinkWriter* writer, DWORD writerAudioStreamIndex, const vector<float>& audioFrames, uint32_t audioChannels, uint32_t audioSampleRate, uint64_t& writtenFrames){
    if(audioFrames.empty()){
        return;
    }

    const auto framesToWrite{audioFrames.size() / audioChannels};
    const auto bytesToWrite{static_cast<DWORD>(audioFrames.size() * sizeof(float))};

    com_ptr<IMFSample> audioSample;
    check_hresult(MFCreateSample(audioSample.put()));
    com_ptr<IMFMediaBuffer> audioBuffer;
    check_hresult(MFCreateMemoryBuffer(bytesToWrite, audioBuffer.put()));

    BYTE* audioBytes{};
    DWORD audioMaxLen{};
    DWORD audioCurLen{};
    check_hresult(audioBuffer->Lock(&audioBytes, &audioMaxLen, &audioCurLen));
    memcpy(audioBytes, audioFrames.data(), bytesToWrite);
    check_hresult(audioBuffer->Unlock());
    check_hresult(audioBuffer->SetCurrentLength(bytesToWrite));
    check_hresult(audioSample->AddBuffer(audioBuffer.get()));

    const auto sampleTime100ns{static_cast<LONGLONG>((writtenFrames * 10'000'000ULL) / audioSampleRate)};
    const auto sampleDuration100ns{static_cast<LONGLONG>((framesToWrite * 10'000'000ULL) / audioSampleRate)};
    check_hresult(audioSample->SetSampleTime(sampleTime100ns));
    check_hresult(audioSample->SetSampleDuration(sampleDuration100ns));
    check_hresult(writer->WriteSample(writerAudioStreamIndex, audioSample.get()));

    writtenFrames += framesToWrite;
}

void writeMixedAudioForKeepRanges(const com_ptr<IMFSourceReader>& audioReader, DWORD audioStreamIndex, const vector<pair<int64_t, int64_t>>& keepRanges100ns, IMFSinkWriter* writer, DWORD writerAudioStreamIndex, uint32_t audioChannels, uint32_t audioSampleRate, int crossfadeMs, float gain, const function<void(double)>& progressCallback, const function<bool()>& shouldCancel){
    const auto fadeFrames{crossfadeMs <= 0 ? size_t{0} : static_cast<size_t>((static_cast<int64_t>(audioSampleRate) * crossfadeMs) / 1000)};
    const auto applyGain{max(0.0f, gain)};
    vector<float> tailBuffer;
    tailBuffer.reserve(fadeFrames * audioChannels);
    uint64_t writtenFrames{};
    const auto totalKeepDuration100ns{
        accumulate(
            keepRanges100ns.begin(),
            keepRanges100ns.end(),
            int64_t{0},
            [](int64_t total, const pair<int64_t, int64_t>& range){
                return total + max<int64_t>(0, range.second - range.first);
            })};
    auto processedDuration100ns{int64_t{0}};

    auto flushTailIfNeeded = [&](){
        if(fadeFrames == 0){
            writePcmAudioFramesToWriter(writer, writerAudioStreamIndex, tailBuffer, audioChannels, audioSampleRate, writtenFrames);
            tailBuffer.clear();
            return;
        }

        const auto tailFrames{tailBuffer.size() / audioChannels};
        if(tailFrames > fadeFrames){
            const auto flushFrames{tailFrames - fadeFrames};
            vector<float> flushAudio;
            flushAudio.reserve(flushFrames * audioChannels);
            flushAudio.insert(flushAudio.end(), tailBuffer.begin(), tailBuffer.begin() + static_cast<std::ptrdiff_t>(flushFrames * audioChannels));
            writePcmAudioFramesToWriter(writer, writerAudioStreamIndex, flushAudio, audioChannels, audioSampleRate, writtenFrames);
            tailBuffer.erase(tailBuffer.begin(), tailBuffer.begin() + static_cast<std::ptrdiff_t>(flushFrames * audioChannels));
        }
    };

    auto appendChunk = [&](vector<float>& chunkAudio){
        if(chunkAudio.empty()){
            return;
        }

        if(!tailBuffer.empty()){
            tailBuffer.insert(tailBuffer.end(), chunkAudio.begin(), chunkAudio.end());
        }else{
            tailBuffer = move(chunkAudio);
        }

        flushTailIfNeeded();
    };

    if(progressCallback){
        progressCallback(totalKeepDuration100ns > 0 ? 0.0 : 100.0);
    }

    for(const auto& [keepStart, keepEnd] : keepRanges100ns){
        if(shouldCancel && shouldCancel()){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
        }
        if(keepEnd <= keepStart){
            continue;
        }

        const auto boundaryNeedsCrossfade{!tailBuffer.empty() && fadeFrames > 0};
        vector<float> boundaryBuffer;
        if(boundaryNeedsCrossfade){
            boundaryBuffer.reserve(fadeFrames * audioChannels);
        }
        auto boundaryCrossfadePending{boundaryNeedsCrossfade};

        PROPVARIANT pos{.vt = VT_I8, .hVal = {.QuadPart = keepStart}};
        check_hresult(audioReader->SetCurrentPosition(GUID_NULL, pos));
        PropVariantClear(&pos);

        for(;;){
            if(shouldCancel && shouldCancel()){
                throw hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
            }
            DWORD actualStream{};
            DWORD flags{};
            LONGLONG timestamp{};
            com_ptr<IMFSample> sample;
            check_hresult(audioReader->ReadSample(audioStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put()));
            if((flags & MF_SOURCE_READERF_STREAMTICK) && timestamp >= keepEnd){
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
            if(sampleEnd100ns <= keepStart){
                continue;
            }
            if(sampleStart100ns >= keepEnd){
                break;
            }

            com_ptr<IMFMediaBuffer> contiguous;
            check_hresult(sample->ConvertToContiguousBuffer(contiguous.put()));
            BYTE* bytes{};
            DWORD maxLen{};
            DWORD curLen{};
            check_hresult(contiguous->Lock(&bytes, &maxLen, &curLen));

            const auto totalFrames{static_cast<int64_t>(curLen / (audioChannels * sizeof(float)))};
            const auto trimStart100ns{max<int64_t>(0, keepStart - sampleStart100ns)};
            const auto trimEnd100ns{max<int64_t>(0, sampleEnd100ns - keepEnd)};
            const auto trimStartFrames{min<int64_t>(totalFrames, (trimStart100ns * audioSampleRate) / 10'000'000)};
            const auto trimEndFrames{min<int64_t>(totalFrames - trimStartFrames, (trimEnd100ns * audioSampleRate) / 10'000'000)};
            const auto keepFrames{max<int64_t>(0, totalFrames - trimStartFrames - trimEndFrames)};

            if(keepFrames > 0){
                const auto* src{reinterpret_cast<const float*>(bytes)};
                const auto* begin{src + (trimStartFrames * audioChannels)};
                vector<float> chunkAudio(begin, begin + (keepFrames * audioChannels));
                if(applyGain != 1.0f){
                    for(auto& sampleValue : chunkAudio){
                        sampleValue *= applyGain;
                    }
                }

                if(boundaryCrossfadePending){
                    boundaryBuffer.insert(boundaryBuffer.end(), chunkAudio.begin(), chunkAudio.end());
                    const auto boundaryFrames{boundaryBuffer.size() / audioChannels};
                    if(boundaryFrames >= fadeFrames){
                        appendCrossfadedAudioSegment(tailBuffer, boundaryBuffer, audioChannels, fadeFrames);
                        boundaryBuffer.clear();
                        boundaryCrossfadePending = false;
                        flushTailIfNeeded();
                    }
                }else{
                    appendChunk(chunkAudio);
                }
            }

            contiguous->Unlock();
        }

        if(boundaryCrossfadePending && !boundaryBuffer.empty()){
            appendCrossfadedAudioSegment(tailBuffer, boundaryBuffer, audioChannels, fadeFrames);
            flushTailIfNeeded();
        }

        processedDuration100ns += max<int64_t>(0, keepEnd - keepStart);
        if(progressCallback && totalKeepDuration100ns > 0){
            progressCallback((100.0 * processedDuration100ns) / totalKeepDuration100ns);
        }
    }

    writePcmAudioFramesToWriter(writer, writerAudioStreamIndex, tailBuffer, audioChannels, audioSampleRate, writtenFrames);
    if(progressCallback){
        progressCallback(100.0);
    }
}

void applyExportFileMetadata(const std::wstring& sourcePath, const std::wstring& outputPath, const std::wstring& exportComment){
    wil::unique_hfile sourceHandle{::CreateFileW(sourcePath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if(sourceHandle){
        FILETIME creationTime{};
        FILETIME lastAccessTime{};
        FILETIME lastWriteTime{};
        if(::GetFileTime(sourceHandle.get(), &creationTime, &lastAccessTime, &lastWriteTime)){
            wil::unique_hfile outputHandle{::CreateFileW(outputPath.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            if(outputHandle){
                (void)::SetFileTime(outputHandle.get(), &creationTime, &lastAccessTime, &lastWriteTime);
            }
        }
    }

    com_ptr<IPropertyStore> propertyStore;
    const auto propertyStoreHr{SHGetPropertyStoreFromParsingName(outputPath.c_str(), nullptr, GPS_READWRITE, IID_PPV_ARGS(propertyStore.put()))};
    if(SUCCEEDED(propertyStoreHr) && propertyStore){
        PROPVARIANT encodedByValue{};
        if(SUCCEEDED(InitPropVariantFromString(L"llvc", &encodedByValue))){
            (void)propertyStore->SetValue(PKEY_Media_EncodedBy, encodedByValue);
            PropVariantClear(&encodedByValue);
        }

        if(!exportComment.empty()){
            PROPVARIANT commentValue{};
            if(SUCCEEDED(InitPropVariantFromString(exportComment.c_str(), &commentValue))){
                (void)propertyStore->SetValue(PKEY_Comment, commentValue);
                PropVariantClear(&commentValue);
            }
        }

        (void)propertyStore->Commit();
    }
}

}
