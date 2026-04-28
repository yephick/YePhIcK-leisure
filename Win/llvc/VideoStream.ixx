module;

#include <winrt/base.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <combaseapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>

export module llvc.VideoStream;

import std;
import llvc.Utils;

export namespace llvc{

using namespace ::std;
using namespace ::winrt;

struct VideoWriteStats{
    uint64_t readSampleCount{};
    uint64_t droppedByCutCount{};
    uint64_t droppedWaitingRapCount{};
    uint64_t writtenSampleCount{};
};

bool isH264VideoSubtype(const GUID& subtype){
    return subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_H264_ES;
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

int64_t resolveReaderSampleTime100ns(int64_t sampleTime100ns, int64_t readSampleTimestamp100ns){
    if(sampleTime100ns > 0){
        return sampleTime100ns;
    }
    if(readSampleTimestamp100ns > 0){
        return readSampleTimestamp100ns;
    }
    return max<int64_t>(0, sampleTime100ns);
}

optional<int64_t> resolveMpegTsDecodeTime100ns(optional<int64_t>& nextInferredDecodeTime100ns, int64_t sampleDuration100ns, int64_t nominalVideoFrameDuration100ns){
    const auto decodeStep100ns{sampleDuration100ns > 0 ? sampleDuration100ns : nominalVideoFrameDuration100ns};
    if(decodeStep100ns <= 0){
        return nullopt;
    }

    const auto inferredDecodeTime100ns{max<int64_t>(0, nextInferredDecodeTime100ns.value_or(0))};
    nextInferredDecodeTime100ns = inferredDecodeTime100ns + decodeStep100ns;
    return inferredDecodeTime100ns;
}

optional<int64_t> sanitizeMpegTsVideoDecodeTime100ns(optional<int64_t> decodeTime100ns, int64_t presentationTime100ns){
    constexpr int64_t maxReasonableReorderDelay100ns{20'000'000};
    if(!decodeTime100ns.has_value() || *decodeTime100ns <= 0){
        return nullopt;
    }
    if(*decodeTime100ns > presentationTime100ns){
        return nullopt;
    }
    if((presentationTime100ns - *decodeTime100ns) > maxReasonableReorderDelay100ns){
        return nullopt;
    }
    return decodeTime100ns;
}

optional<size_t> findAnnexBStartCodeOffset(const uint8_t* data, size_t size){
    if(!data || size < 3){
        return nullopt;
    }

    for(size_t offset{}; offset + 3 <= size; ++offset){
        if(data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1){
            return offset;
        }
        if(offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 && data[offset + 3] == 1){
            return offset;
        }
    }
    return nullopt;
}

vector<uint8_t> convertLengthPrefixedNalSampleToAnnexB(const uint8_t* data, size_t size, uint32_t nalLengthFieldSize){
    vector<uint8_t> converted;
    if(!data || size == 0){
        return converted;
    }

    const auto nalSizeField{clamp<uint32_t>(nalLengthFieldSize, 1, 4)};
    size_t offset{};
    while(offset + nalSizeField <= size){
        if(bytesAreZeroPadding(data + offset, size - offset)){
            offset = size;
            break;
        }

        uint32_t nalLength{};
        for(uint32_t i{}; i < nalSizeField; ++i){
            nalLength = (nalLength << 8) | data[offset + i];
        }
        offset += nalSizeField;
        if(nalLength == 0 || offset + nalLength > size){
            converted.clear();
            return converted;
        }

        converted.insert(converted.end(), {0x00, 0x00, 0x00, 0x01});
        converted.insert(converted.end(), data + offset, data + offset + nalLength);
        offset += nalLength;
    }

    if(offset != size && !bytesAreZeroPadding(data + offset, size - offset)){
        converted.clear();
    }
    return converted;
}

vector<uint8_t> normalizeH264SampleForMpegTs(const uint8_t* data, size_t size, uint32_t nalLengthFieldSize){
    if(const auto annexBOffset{findAnnexBStartCodeOffset(data, size)}){
        return vector<uint8_t>{data + *annexBOffset, data + size};
    }
    return convertLengthPrefixedNalSampleToAnnexB(data, size, nalLengthFieldSize);
}

vector<uint8_t> extractH264ParameterSetsAnnexBForMpegTs(const vector<uint8_t>& sequenceHeader){
    vector<uint8_t> parameterSets;
    const auto* data{sequenceHeader.data()};
    const auto size{sequenceHeader.size()};
    if(!data || size == 0){
        return parameterSets;
    }

    if(const auto annexBOffset{findAnnexBStartCodeOffset(data, size)}){
        return vector<uint8_t>{data + *annexBOffset, data + size};
    }

    if(size < 7 || data[0] != 0x01){
        return parameterSets;
    }

    size_t offset{5};
    const auto appendParameterSet = [&](uint16_t unitLength) -> bool{
        if(unitLength == 0 || offset + unitLength > size){
            return false;
        }
        parameterSets.insert(parameterSets.end(), {0x00, 0x00, 0x00, 0x01});
        parameterSets.insert(parameterSets.end(), data + offset, data + offset + unitLength);
        offset += unitLength;
        return true;
    };

    const auto spsCount{static_cast<uint8_t>(data[offset++] & 0x1F)};
    if(spsCount == 0){
        return {};
    }
    for(uint8_t i{}; i < spsCount; ++i){
        if(offset + 2 > size){
            return {};
        }
        const auto unitLength{static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1])};
        offset += 2;
        if(!appendParameterSet(unitLength)){
            return {};
        }
    }

    if(offset >= size){
        return {};
    }
    const auto ppsCount{data[offset++]};
    for(uint8_t i{}; i < ppsCount; ++i){
        if(offset + 2 > size){
            return {};
        }
        const auto unitLength{static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1])};
        offset += 2;
        if(!appendParameterSet(unitLength)){
            return {};
        }
    }

    if(parameterSets.empty() || (offset != size && !bytesAreZeroPadding(data + offset, size - offset))){
        return {};
    }
    return parameterSets;
}

bool h264AnnexBSampleContainsParameterSets(const uint8_t* data, size_t size){
    if(!data || size < 5){
        return false;
    }

    bool sawSps{};
    bool sawPps{};
    for(size_t i{}; i + 4 <= size; ++i){
        size_t nalStart{};
        if(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1){
            nalStart = i + 3;
        }else if(i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1){
            nalStart = i + 4;
        }else{
            continue;
        }

        if(nalStart >= size){
            continue;
        }

        switch(data[nalStart] & 0x1F){
        case 7:
            sawSps = true;
            break;
        case 8:
            sawPps = true;
            break;
        default:
            break;
        }

        if(sawSps && sawPps){
            return true;
        }
    }

    return false;
}

uint32_t getNalLengthFieldSize(const com_ptr<IMFMediaType>& mediaType, const GUID& subtype){
    if(!mediaType){
        return 4;
    }

    UINT8* configData{};
    UINT32 configSize{};
    if(FAILED(mediaType->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &configData, &configSize)) || !configData || configSize == 0){
        return 4;
    }

    if(isH264VideoSubtype(subtype)){
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

bool bitstreamLooksLikeRandomAccessPoint(const uint8_t* data, size_t size, const GUID& subtype, uint32_t nalLengthFieldSize, bool allowInconclusive){
    if(!data || size == 0){
        return allowInconclusive;
    }

    auto classifyNalType = [&](const uint8_t nalHeader) -> optional<bool>{
        if(isH264VideoSubtype(subtype)){
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
        return nullopt;
    };

    {
        const auto nalSizeField{clamp<uint32_t>(nalLengthFieldSize, 1, 4)};
        size_t offset{};
        while(offset + nalSizeField <= size){
            uint32_t nalLength{};
            for(uint32_t i{}; i < nalSizeField; ++i){
                nalLength = (nalLength << 8) | data[offset + i];
            }
            offset += nalSizeField;

            if(nalLength == 0 || offset + nalLength > size){
                break;
            }

            if(const auto maybeRap{classifyNalType(data[offset])}; maybeRap.has_value()){
                return maybeRap.value();
            }

            offset += nalLength;
        }
    }

    for(size_t i{}; i + 4 < size; ++i){
        size_t nalStart{};
        if(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1){
            nalStart = i + 3;
        }else if(i + 4 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1){
            nalStart = i + 4;
        }else{
            continue;
        }

        if(nalStart >= size){
            continue;
        }

        if(const auto maybeRap{classifyNalType(data[nalStart])}; maybeRap.has_value()){
            return maybeRap.value();
        }
    }

    return allowInconclusive;
}

bool isTrueRandomAccessPointSample(const com_ptr<IMFSample>& sample, const GUID& subtype, uint32_t nalLengthFieldSize, bool allowInconclusive){
    if(!isH264VideoSubtype(subtype) && subtype != MFVideoFormat_HEVC && subtype != MFVideoFormat_H265){
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

    const auto result{bitstreamLooksLikeRandomAccessPoint(data, currentLength, subtype, nalLengthFieldSize, allowInconclusive)};
    contiguousBuffer->Unlock();
    return result;
}

bool isContainerSyncSample(const com_ptr<IMFSample>& sample){
    UINT32 cleanPoint{};
    return SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0;
}

bool shouldTreatMpegTsSampleAsKeyframe(bool, bool isBitstreamRap){
    return isBitstreamRap;
}

}
