module;

#include "pch.h"

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

export namespace llvc{

using namespace ::std;
using namespace ::winrt;

struct VideoWriteStats{
    uint64_t readSampleCount{};
    uint64_t droppedByCutCount{};
    uint64_t droppedWaitingRapCount{};
    uint64_t writtenSampleCount{};
};

wstring formatDurationFileTag(int64_t duration100ns);
vector<pair<int64_t, int64_t>> invertCutRanges100ns(const vector<pair<int64_t, int64_t>>& cutRanges, int64_t totalDuration100ns);
int64_t removedDurationBefore(const vector<pair<int64_t, int64_t>>& ranges, int64_t time100ns);
uint32_t getNalLengthFieldSize(const com_ptr<IMFMediaType>& mediaType, const GUID& subtype);
bool isTrueRandomAccessPointSample(const com_ptr<IMFSample>& sample, const GUID& subtype, uint32_t nalLengthFieldSize, bool allowInconclusive);
bool isContainerSyncSample(const com_ptr<IMFSample>& sample);
com_ptr<IMFMediaType> chooseBestNativeVideoMediaType(const com_ptr<IMFSourceReader>& reader, DWORD streamIndex);
com_ptr<IMFMediaType> chooseBestNativeVideoMediaTypeForSubtypes(const com_ptr<IMFSourceReader>& reader, DWORD streamIndex, const vector<GUID>& allowedSubtypes);
com_ptr<IMFMediaType> createPcmFloatAudioType(uint32_t sampleRate, uint32_t channels);
com_ptr<IMFMediaType> createAacOutputType(uint32_t sampleRate, uint32_t channels);
vector<float> decodeAudioRangeToFloat(const com_ptr<IMFSourceReader>& reader, DWORD audioStreamIndex, int64_t rangeStart100ns, int64_t rangeEnd100ns, uint32_t channels, uint32_t sampleRate);

wstring pickExportOutputPath(const std::filesystem::path& sourceFsPath, const wchar_t* defaultExt, int64_t outputDuration100ns, HWND ownerWindow, bool mp4Only);
void appendCrossfadedAudioSegment(vector<float>& mixedAudio, const vector<float>& segmentAudio, uint32_t audioChannels, size_t fadeFrames);
void writePcmAudioFramesToWriter(const com_ptr<IMFSinkWriter>& writer, DWORD writerAudioStreamIndex, const vector<float>& audioFrames, uint32_t audioChannels, uint32_t audioSampleRate, uint64_t& writtenFrames);
void writeMixedAudioForKeepRanges(const com_ptr<IMFSourceReader>& audioReader, DWORD audioStreamIndex, const vector<pair<int64_t, int64_t>>& keepRanges100ns, const com_ptr<IMFSinkWriter>& writer, DWORD writerAudioStreamIndex, uint32_t audioChannels, uint32_t audioSampleRate, int crossfadeMs, float gain, const function<bool()>& shouldCancel = {});
VideoWriteStats writeVideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const com_ptr<IMFSinkWriter>& writer, DWORD writerVideoStreamIndex, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, int64_t sourceDuration100ns, const function<void(double)>& progressCallback, const function<bool()>& shouldCancel = {});
void applyExportFileMetadata(const std::wstring& sourcePath, const std::wstring& outputPath, const std::wstring& exportComment);

}

namespace llvc{

using namespace ::std;
using namespace ::winrt;

wstring formatDurationFileTag(int64_t duration100ns){
    const auto totalMs{max<int64_t>(0, (duration100ns + 5'000) / 10'000)};
    const auto minutes{totalMs / 60'000};
    const auto seconds{(totalMs / 1'000) % 60};
    const auto millis{totalMs % 1'000};
    return std::format(L"{:02}{:02}{:03}", minutes, seconds, millis);
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
        return allowInconclusive;
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
                return nalType == 5;
            }
        }else{
            const auto nalType = static_cast<uint8_t>((nalHeader >> 1) & 0x3F);
            if(nalType <= 31){
                return nalType == 19 || nalType == 20 || nalType == 21;
            }
        }
        return std::nullopt;
    };

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
    return allowInconclusive;
}

bool isContainerSyncSample(const com_ptr<IMFSample>& sample){
    UINT32 cleanPoint{};
    return SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0;
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

wstring pickExportOutputPath(const std::filesystem::path& sourceFsPath, const wchar_t* defaultExt, int64_t outputDuration100ns, HWND ownerWindow, bool mp4Only){
    com_ptr<IFileSaveDialog> saveDialog;
    check_hresult(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(saveDialog.put())));

    DWORD options{};
    check_hresult(saveDialog->GetOptions(&options));
    check_hresult(saveDialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT));

    if(mp4Only){
        const COMDLG_FILTERSPEC fileTypes[]{
            {L"MP4 video", L"*.mp4"},
        };
        check_hresult(saveDialog->SetFileTypes(static_cast<UINT>(size(fileTypes)), fileTypes));
        check_hresult(saveDialog->SetFileTypeIndex(1));
        check_hresult(saveDialog->SetDefaultExtension(L"mp4"));
    }else{
        const COMDLG_FILTERSPEC fileTypes[]{
            {L"MP4 video", L"*.mp4"},
            {L"MOV video", L"*.mov"},
        };
        check_hresult(saveDialog->SetFileTypes(static_cast<UINT>(size(fileTypes)), fileTypes));

        const auto isMovDefault{_wcsicmp(defaultExt, L".mov") == 0};
        check_hresult(saveDialog->SetFileTypeIndex(isMovDefault ? 2U : 1U));
        check_hresult(saveDialog->SetDefaultExtension(isMovDefault ? L"mov" : L"mp4"));
    }

    const auto suggestedName{sourceFsPath.stem().wstring() + L" - " + formatDurationFileTag(outputDuration100ns)};
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

void writePcmAudioFramesToWriter(const com_ptr<IMFSinkWriter>& writer, DWORD writerAudioStreamIndex, const vector<float>& audioFrames, uint32_t audioChannels, uint32_t audioSampleRate, uint64_t& writtenFrames){
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

void writeMixedAudioForKeepRanges(const com_ptr<IMFSourceReader>& audioReader, DWORD audioStreamIndex, const vector<pair<int64_t, int64_t>>& keepRanges100ns, const com_ptr<IMFSinkWriter>& writer, DWORD writerAudioStreamIndex, uint32_t audioChannels, uint32_t audioSampleRate, int crossfadeMs, float gain, const function<bool()>& shouldCancel){
    const auto fadeFrames{crossfadeMs <= 0 ? size_t{0} : static_cast<size_t>((static_cast<int64_t>(audioSampleRate) * crossfadeMs) / 1000)};
    const auto applyGain{max(0.0f, gain)};
    vector<float> tailBuffer;
    tailBuffer.reserve(fadeFrames * audioChannels);
    uint64_t writtenFrames{};

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
    }

    writePcmAudioFramesToWriter(writer, writerAudioStreamIndex, tailBuffer, audioChannels, audioSampleRate, writtenFrames);
}

VideoWriteStats writeVideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const com_ptr<IMFSinkWriter>& writer, DWORD writerVideoStreamIndex, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, int64_t sourceDuration100ns, const function<void(double)>& progressCallback, const function<bool()>& shouldCancel){
    auto waitingForCleanPoint{false};
    auto markDiscontinuityOnNextWrittenSample{false};
    auto dtsPtsShift100ns{static_cast<int64_t>(0)};
    auto dtsPtsShiftInitialized{false};
    int64_t lastInTime100ns{-1};
    VideoWriteStats stats{};

    if(progressCallback){
        progressCallback(0.0);
    }

    for(;;){
        if(shouldCancel && shouldCancel()){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
        }
        DWORD actualStream{};
        DWORD flags{};
        LONGLONG timestamp{};
        com_ptr<IMFSample> sample;
        const auto hr{reader->ReadSample(videoStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put())};
        if(FAILED(hr)){
            check_hresult(hr);
        }
        if(flags & MF_SOURCE_READERF_ENDOFSTREAM){
            break;
        }
        if(!sample){
            continue;
        }

        ++stats.readSampleCount;

        LONGLONG sampleTime100ns{};
        if(FAILED(sample->GetSampleTime(&sampleTime100ns))){
            sampleTime100ns = timestamp;
        }
        const auto inTime100ns{max<int64_t>(0, sampleTime100ns)};
        if(inTime100ns > lastInTime100ns){
            lastInTime100ns = inTime100ns;
        }

        auto dropped{false};
        for(const auto& [start, end] : effectiveCutRanges100ns){
            if(inTime100ns < start){
                break;
            }
            if(inTime100ns < end){
                dropped = true;
                break;
            }
        }
        if(dropped){
            ++stats.droppedByCutCount;
            waitingForCleanPoint = true;
            markDiscontinuityOnNextWrittenSample = true;
            continue;
        }

        if(waitingForCleanPoint){
            const auto isContainerSync{isContainerSyncSample(sample)};
            const auto isBitstreamRap{isTrueRandomAccessPointSample(sample, videoSubtype, nalLengthFieldSize, false)};
            if(!(isContainerSync && isBitstreamRap)){
                ++stats.droppedWaitingRapCount;
                continue;
            }
            waitingForCleanPoint = false;
        }

        const auto outTimeRaw100ns{inTime100ns - removedDurationBefore(effectiveCutRanges100ns, inTime100ns)};
        auto outTime100ns{outTimeRaw100ns};

        UINT64 decodeTimestamp100ns{};
        auto hasDecodeTimestamp{false};
        if(SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &decodeTimestamp100ns))){
            hasDecodeTimestamp = true;
            const auto decodeTimeSigned{static_cast<int64_t>(decodeTimestamp100ns)};
            const auto removedAtPresentationTime{removedDurationBefore(effectiveCutRanges100ns, inTime100ns)};
            auto outDecodeTime100ns{decodeTimeSigned - removedAtPresentationTime};
            if(!dtsPtsShiftInitialized){
                dtsPtsShift100ns = max<int64_t>(0, -outDecodeTime100ns);
                dtsPtsShiftInitialized = true;
            }
            outTime100ns += dtsPtsShift100ns;
            outDecodeTime100ns += dtsPtsShift100ns;
            check_hresult(sample->SetUINT64(MFSampleExtension_DecodeTimestamp, static_cast<UINT64>(max<int64_t>(0, outDecodeTime100ns))));
        }

        if(!dtsPtsShiftInitialized && !hasDecodeTimestamp){
            dtsPtsShiftInitialized = true;
        }

        if(!hasDecodeTimestamp && dtsPtsShiftInitialized && dtsPtsShift100ns > 0){
            outTime100ns += dtsPtsShift100ns;
        }

        check_hresult(sample->SetSampleTime(outTime100ns));

        LONGLONG duration100ns{};
        if(SUCCEEDED(sample->GetSampleDuration(&duration100ns)) && duration100ns > 0){
            check_hresult(sample->SetSampleDuration(duration100ns));
        }

        if(markDiscontinuityOnNextWrittenSample){
            check_hresult(sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE));
            markDiscontinuityOnNextWrittenSample = false;
        }

        check_hresult(writer->WriteSample(writerVideoStreamIndex, sample.get()));
        ++stats.writtenSampleCount;

        if(progressCallback && sourceDuration100ns > 0 && (stats.readSampleCount % 24 == 0)) {
            const auto pct{(100.0 * inTime100ns) / sourceDuration100ns};
            progressCallback(pct);
        }
    }

    if(progressCallback){
        progressCallback(100.0);
    }
    return stats;
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
