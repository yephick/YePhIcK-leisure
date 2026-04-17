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

export namespace llvc{

using namespace ::std;
using namespace ::winrt;

struct MFLifetime{
    MFLifetime();
    ~MFLifetime();
};

bool hasDecoderForSubtype(const GUID& subtype);

struct KeyFrameCadenceInfo{
    wstring summary{};
    wstring interval{};
};

KeyFrameCadenceInfo analyzeKeyFrameCadence(IMFSourceReader* reader, DWORD videoStreamIndex, uint32_t fpsNum, uint32_t fpsDen);

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
bool shouldTreatMpegTsSampleAsKeyframe(bool isContainerSync, bool isBitstreamRap);
vector<uint8_t> normalizeH264SampleForMpegTs(const uint8_t* data, size_t size, uint32_t nalLengthFieldSize);
vector<uint8_t> extractH264ParameterSetsAnnexBForMpegTs(const vector<uint8_t>& sequenceHeader);
int64_t resolveReaderSampleTime100ns(int64_t sampleTime100ns, int64_t readSampleTimestamp100ns);
bool bitstreamLooksLikeRandomAccessPoint(const uint8_t* data, size_t size, const GUID& subtype, uint32_t nalLengthFieldSize, bool allowInconclusive);
optional<int64_t> resolveMpegTsDecodeTime100ns(optional<int64_t>& nextInferredDecodeTime100ns, int64_t sampleDuration100ns, int64_t nominalVideoFrameDuration100ns);
optional<int64_t> sanitizeMpegTsVideoDecodeTime100ns(optional<int64_t> decodeTime100ns, int64_t presentationTime100ns);
com_ptr<IMFMediaType> chooseBestNativeVideoMediaType(const com_ptr<IMFSourceReader>& reader, DWORD streamIndex);
com_ptr<IMFMediaType> chooseBestNativeVideoMediaTypeForSubtypes(const com_ptr<IMFSourceReader>& reader, DWORD streamIndex, const vector<GUID>& allowedSubtypes);
com_ptr<IMFMediaType> chooseFirstNativeVideoMediaTypeForSubtypes(const com_ptr<IMFSourceReader>& reader, DWORD streamIndex, const vector<GUID>& allowedSubtypes);
com_ptr<IMFMediaType> createPcmFloatAudioType(uint32_t sampleRate, uint32_t channels);
com_ptr<IMFMediaType> createAacOutputType(uint32_t sampleRate, uint32_t channels);
vector<float> decodeAudioRangeToFloat(const com_ptr<IMFSourceReader>& reader, DWORD audioStreamIndex, int64_t rangeStart100ns, int64_t rangeEnd100ns, uint32_t channels, uint32_t sampleRate);

wstring pickExportOutputPath(const std::filesystem::path& sourceFsPath, const vector<wstring>& allowedExtensions, const wchar_t* defaultExt, int64_t outputDuration100ns, HWND ownerWindow);
void appendCrossfadedAudioSegment(vector<float>& mixedAudio, const vector<float>& segmentAudio, uint32_t audioChannels, size_t fadeFrames);
void writePcmAudioFramesToWriter(const com_ptr<IMFSinkWriter>& writer, DWORD writerAudioStreamIndex, const vector<float>& audioFrames, uint32_t audioChannels, uint32_t audioSampleRate, uint64_t& writtenFrames);
void writeMixedAudioForKeepRanges(const com_ptr<IMFSourceReader>& audioReader, DWORD audioStreamIndex, const vector<pair<int64_t, int64_t>>& keepRanges100ns, const com_ptr<IMFSinkWriter>& writer, DWORD writerAudioStreamIndex, uint32_t audioChannels, uint32_t audioSampleRate, int crossfadeMs, float gain, const function<void(double)>& progressCallback = {}, const function<bool()>& shouldCancel = {});
VideoWriteStats writeVideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const com_ptr<IMFSinkWriter>& writer, DWORD writerVideoStreamIndex, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, int64_t sourceDuration100ns, const function<void(double)>& progressCallback, const function<bool()>& shouldCancel = {});
VideoWriteStats writeWebmVp9VideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const std::wstring& outputPath, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, int64_t sourceDuration100ns, int64_t outputDuration100ns, uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen, const function<void(double)>& progressCallback, const function<bool()>& shouldCancel = {});
VideoWriteStats writeMpegTsH264VideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const std::wstring& outputPath, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, const com_ptr<IMFMediaType>& videoMediaType, int64_t sourceDuration100ns, const com_ptr<IMFSourceReader>& audioReader, DWORD audioStreamIndex, const com_ptr<IMFMediaType>& audioMediaType, bool keepAudio, const function<void(double)>& progressCallback, const function<void(double)>& audioProgressCallback, const function<bool()>& shouldCancel = {});
void applyExportFileMetadata(const std::wstring& sourcePath, const std::wstring& outputPath, const std::wstring& exportComment);

}

namespace llvc{

using namespace ::std;
using namespace ::winrt;

namespace{

constexpr int64_t WEBM_TIMECODE_SCALE_NS{1'000'000LL};
constexpr int64_t HNS_PER_MILLISECOND{10'000LL};

uint64_t clampWebmElementSize(uint64_t value){
    constexpr auto maxKnownEbmlSize{0x00FF'FFFF'FFFF'FFFFULL - 1};
    return min(value, maxKnownEbmlSize);
}

int64_t round100nsToWebmMs(int64_t value100ns){
    return max<int64_t>(0, (value100ns + (HNS_PER_MILLISECOND / 2)) / HNS_PER_MILLISECOND);
}

void appendEbmlId(vector<uint8_t>& out, uint32_t id){
    if(id > 0xFFFFFF){
        out.push_back(static_cast<uint8_t>((id >> 24) & 0xFF));
    }
    if(id > 0xFFFF){
        out.push_back(static_cast<uint8_t>((id >> 16) & 0xFF));
    }
    if(id > 0xFF){
        out.push_back(static_cast<uint8_t>((id >> 8) & 0xFF));
    }
    out.push_back(static_cast<uint8_t>(id & 0xFF));
}

void appendEbmlSize(vector<uint8_t>& out, uint64_t value){
    const auto safeValue{clampWebmElementSize(value)};
    for(int length{1}; length <= 8; ++length){
        const auto usableBits{7 * length};
        const auto maxValue{usableBits >= 63 ? (numeric_limits<uint64_t>::max)() : ((uint64_t{1} << usableBits) - 2)};
        if(safeValue > maxValue){
            continue;
        }

        vector<uint8_t> bytes(static_cast<size_t>(length), 0);
        auto remaining{safeValue};
        for(int i{length - 1}; i >= 0; --i){
            bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(remaining & 0xFF);
            remaining >>= 8;
        }
        bytes[0] |= static_cast<uint8_t>(1u << (8 - length));
        out.insert(out.end(), bytes.begin(), bytes.end());
        return;
    }

    throw hresult_error(E_INVALIDARG, L"EBML size is too large");
}

void appendEbmlUnknownSize8(vector<uint8_t>& out){
    out.push_back(0x01);
    out.insert(out.end(), 7, 0xFF);
}

vector<uint8_t> encodeUnsignedEbmlValue(uint64_t value, size_t minBytes = 1){
    size_t byteCount{1};
    auto remaining{value};
    while((remaining >>= 8) != 0){
        ++byteCount;
    }
    byteCount = max(byteCount, minBytes);

    vector<uint8_t> bytes(byteCount, 0);
    for(size_t i{}; i < byteCount; ++i){
        const auto shift{8 * (byteCount - 1 - i)};
        bytes[i] = static_cast<uint8_t>((value >> shift) & 0xFF);
    }
    return bytes;
}

vector<uint8_t> encodeFloat64EbmlValue(double value){
    uint64_t bits{};
    static_assert(sizeof(bits) == sizeof(value));
    memcpy(&bits, &value, sizeof(bits));

    vector<uint8_t> bytes(8, 0);
    for(size_t i{}; i < bytes.size(); ++i){
        const auto shift{8 * (bytes.size() - 1 - i)};
        bytes[i] = static_cast<uint8_t>((bits >> shift) & 0xFF);
    }
    return bytes;
}

void appendEbmlElement(vector<uint8_t>& out, uint32_t id, const vector<uint8_t>& payload){
    appendEbmlId(out, id);
    appendEbmlSize(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

void appendEbmlStringElement(vector<uint8_t>& out, uint32_t id, string_view value){
    vector<uint8_t> payload(value.begin(), value.end());
    appendEbmlElement(out, id, payload);
}

void appendEbmlUnsignedElement(vector<uint8_t>& out, uint32_t id, uint64_t value, size_t minBytes = 1){
    appendEbmlElement(out, id, encodeUnsignedEbmlValue(value, minBytes));
}

void appendEbmlFloat64Element(vector<uint8_t>& out, uint32_t id, double value){
    appendEbmlElement(out, id, encodeFloat64EbmlValue(value));
}

void appendVoidElement(vector<uint8_t>& out, size_t payloadSize){
    appendEbmlId(out, 0xEC);
    appendEbmlSize(out, payloadSize);
    out.insert(out.end(), payloadSize, 0x00);
}

void appendExactSizedVoidPadding(vector<uint8_t>& out, size_t totalBytes){
    if(totalBytes == 0){
        return;
    }
    if(totalBytes == 1){
        throw hresult_error(E_INVALIDARG, L"Cannot encode a one-byte WebM Void padding region");
    }

    switch(totalBytes % 3){
    case 1:
        if(totalBytes < 4){
            throw hresult_error(E_INVALIDARG, L"Cannot encode requested WebM Void padding");
        }
        out.push_back(0xEC);
        out.push_back(0x80);
        out.push_back(0xEC);
        out.push_back(0x80);
        totalBytes -= 4;
        break;
    case 2:
        out.push_back(0xEC);
        out.push_back(0x80);
        totalBytes -= 2;
        break;
    default:
        break;
    }

    while(totalBytes > 0){
        out.push_back(0xEC);
        out.push_back(0x81);
        out.push_back(0x00);
        totalBytes -= 3;
    }
}

class WebmVp9Writer final{
public:
    WebmVp9Writer(const wstring& outputPath, int64_t outputDuration100ns, uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen):
        m_stream{filesystem::path{outputPath}, ios::binary | ios::trunc}{
        if(!m_stream){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), L"Failed to create WebM output file");
        }

        writeEbmlHeader();
        writeSegmentHeader();
        reserveSeekHeadSpace();
        writeInfo(outputDuration100ns);
        writeTracks(width, height, fpsNum, fpsDen);
    }

    void finalize(){
        if(m_finalized){
            return;
        }

        writeCues();
        rewriteSeekHead();
        m_stream.flush();
        if(!m_stream){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while finalizing WebM output");
        }
        m_finalized = true;
    }

    void writeSample(const uint8_t* data, size_t size, int64_t time100ns, bool keyframe){
        if(!data || size == 0){
            return;
        }

        auto blockTimeMs{round100nsToWebmMs(time100ns)};
        const auto clusterAgeMs{m_clusterOpen ? (blockTimeMs - m_clusterTimeMs) : 0};
        const auto needNewCluster{
            !m_clusterOpen
            || blockTimeMs < m_clusterTimeMs
            || (keyframe && m_clusterHasBlocks)
            || clusterAgeMs > 30'000};
        if(needNewCluster){
            startCluster(blockTimeMs, keyframe);
        }

        auto relativeTimeMs{blockTimeMs - m_clusterTimeMs};
        if(relativeTimeMs < (numeric_limits<int16_t>::min)() || relativeTimeMs > (numeric_limits<int16_t>::max)()){
            startCluster(blockTimeMs, keyframe);
            relativeTimeMs = 0;
        }

        writeSimpleBlock(data, size, static_cast<int16_t>(relativeTimeMs), keyframe);
        m_clusterHasBlocks = true;
    }

private:
    struct CuePoint final{
        uint64_t timeMs{};
        uint64_t clusterPosition{};
    };

    uint64_t currentOffset(){
        const auto pos{m_stream.tellp()};
        if(pos == streampos(-1)){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed to query WebM output position");
        }
        return static_cast<uint64_t>(pos);
    }

    void writeBytes(const vector<uint8_t>& bytes){
        if(bytes.empty()){
            return;
        }
        m_stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<streamsize>(bytes.size()));
        if(!m_stream){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while writing WebM output");
        }
    }

    void writeBytes(const uint8_t* data, size_t size){
        if(!data || size == 0){
            return;
        }
        m_stream.write(reinterpret_cast<const char*>(data), static_cast<streamsize>(size));
        if(!m_stream){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while writing WebM output");
        }
    }

    void writeEbmlHeader(){
        vector<uint8_t> ebml;
        appendEbmlUnsignedElement(ebml, 0x4286, 1);
        appendEbmlUnsignedElement(ebml, 0x42F7, 1);
        appendEbmlUnsignedElement(ebml, 0x42F2, 4);
        appendEbmlUnsignedElement(ebml, 0x42F3, 8);
        appendEbmlStringElement(ebml, 0x4282, "webm");
        appendEbmlUnsignedElement(ebml, 0x4287, 4);
        appendEbmlUnsignedElement(ebml, 0x4285, 2);

        vector<uint8_t> header;
        appendEbmlId(header, 0x1A45DFA3);
        appendEbmlSize(header, ebml.size());
        header.insert(header.end(), ebml.begin(), ebml.end());
        writeBytes(header);
    }

    void writeSegmentHeader(){
        vector<uint8_t> segmentHeader;
        appendEbmlId(segmentHeader, 0x18538067);
        appendEbmlUnknownSize8(segmentHeader);
        writeBytes(segmentHeader);
        m_segmentDataStartOffset = currentOffset();
    }

    void reserveSeekHeadSpace(){
        vector<uint8_t> reservedSeekHead;
        appendVoidElement(reservedSeekHead, 512);
        m_seekHeadOffset = currentOffset();
        m_seekHeadReservedBytes = reservedSeekHead.size();
        writeBytes(reservedSeekHead);
    }

    void writeInfo(int64_t outputDuration100ns){
        const auto infoOffset{currentOffset()};
        vector<uint8_t> info;
        appendEbmlUnsignedElement(info, 0x2AD7B1, static_cast<uint64_t>(WEBM_TIMECODE_SCALE_NS), 3);
        appendEbmlStringElement(info, 0x4D80, "llvc");
        appendEbmlStringElement(info, 0x5741, "llvc");
        appendEbmlFloat64Element(info, 0x4489, static_cast<double>(max<int64_t>(0, outputDuration100ns)) / HNS_PER_MILLISECOND);

        vector<uint8_t> infoElement;
        appendEbmlId(infoElement, 0x1549A966);
        appendEbmlSize(infoElement, info.size());
        infoElement.insert(infoElement.end(), info.begin(), info.end());
        writeBytes(infoElement);
        if(infoOffset >= m_segmentDataStartOffset){
            m_infoPosition = infoOffset - m_segmentDataStartOffset;
        }
    }

    void writeTracks(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen){
        const auto tracksOffset{currentOffset()};
        vector<uint8_t> video;
        appendEbmlUnsignedElement(video, 0xB0, width ? width : 1);
        appendEbmlUnsignedElement(video, 0xBA, height ? height : 1);

        vector<uint8_t> trackEntry;
        appendEbmlUnsignedElement(trackEntry, 0xD7, 1);
        appendEbmlUnsignedElement(trackEntry, 0x73C5, 1);
        appendEbmlUnsignedElement(trackEntry, 0x83, 1);
        appendEbmlUnsignedElement(trackEntry, 0x9C, 0);
        appendEbmlStringElement(trackEntry, 0x86, "V_VP9");
        if(fpsNum > 0 && fpsDen > 0){
            const auto defaultDurationNs{
                static_cast<uint64_t>((1'000'000'000LL * static_cast<int64_t>(fpsDen) + (fpsNum / 2)) / static_cast<int64_t>(fpsNum))};
            if(defaultDurationNs > 0){
                appendEbmlUnsignedElement(trackEntry, 0x23E383, defaultDurationNs);
            }
        }

        vector<uint8_t> videoMaster;
        appendEbmlId(videoMaster, 0xE0);
        appendEbmlSize(videoMaster, video.size());
        videoMaster.insert(videoMaster.end(), video.begin(), video.end());
        trackEntry.insert(trackEntry.end(), videoMaster.begin(), videoMaster.end());

        vector<uint8_t> trackEntryMaster;
        appendEbmlId(trackEntryMaster, 0xAE);
        appendEbmlSize(trackEntryMaster, trackEntry.size());
        trackEntryMaster.insert(trackEntryMaster.end(), trackEntry.begin(), trackEntry.end());

        vector<uint8_t> tracks;
        tracks.insert(tracks.end(), trackEntryMaster.begin(), trackEntryMaster.end());

        vector<uint8_t> tracksElement;
        appendEbmlId(tracksElement, 0x1654AE6B);
        appendEbmlSize(tracksElement, tracks.size());
        tracksElement.insert(tracksElement.end(), tracks.begin(), tracks.end());
        writeBytes(tracksElement);
        if(tracksOffset >= m_segmentDataStartOffset){
            m_tracksPosition = tracksOffset - m_segmentDataStartOffset;
        }
    }

    void startCluster(int64_t clusterTimeMs, bool cueable){
        const auto clusterOffset{currentOffset()};
        vector<uint8_t> clusterHeader;
        appendEbmlId(clusterHeader, 0x1F43B675);
        appendEbmlUnknownSize8(clusterHeader);
        writeBytes(clusterHeader);

        vector<uint8_t> timecodeElement;
        appendEbmlUnsignedElement(timecodeElement, 0xE7, static_cast<uint64_t>(max<int64_t>(0, clusterTimeMs)));
        writeBytes(timecodeElement);

        m_clusterOpen = true;
        m_clusterTimeMs = clusterTimeMs;
        m_clusterHasBlocks = false;
        if(cueable && clusterOffset >= m_segmentDataStartOffset){
            const auto relativeClusterOffset{clusterOffset - m_segmentDataStartOffset};
            if(m_cues.empty() || m_cues.back().timeMs != static_cast<uint64_t>(clusterTimeMs)){
                m_cues.push_back(CuePoint{
                    .timeMs = static_cast<uint64_t>(clusterTimeMs),
                    .clusterPosition = relativeClusterOffset});
            }
        }
    }

    void writeSimpleBlock(const uint8_t* data, size_t size, int16_t relativeTimecodeMs, bool keyframe){
        vector<uint8_t> simpleBlock;
        simpleBlock.reserve(size + 4);
        simpleBlock.push_back(0x81);
        simpleBlock.push_back(static_cast<uint8_t>((relativeTimecodeMs >> 8) & 0xFF));
        simpleBlock.push_back(static_cast<uint8_t>(relativeTimecodeMs & 0xFF));
        simpleBlock.push_back(static_cast<uint8_t>(keyframe ? 0x80 : 0x00));

        vector<uint8_t> blockHeader;
        appendEbmlId(blockHeader, 0xA3);
        appendEbmlSize(blockHeader, simpleBlock.size() + size);
        writeBytes(blockHeader);
        writeBytes(simpleBlock);
        writeBytes(data, size);
    }

    void writeCues(){
        if(m_cues.empty()){
            return;
        }

        const auto cuesOffset{currentOffset()};
        vector<uint8_t> cuesPayload;
        for(const auto& cue: m_cues){
            vector<uint8_t> cueTrackPositions;
            appendEbmlUnsignedElement(cueTrackPositions, 0xF7, 1);
            appendEbmlUnsignedElement(cueTrackPositions, 0xF1, cue.clusterPosition);
            appendEbmlUnsignedElement(cueTrackPositions, 0xF0, 0);

            vector<uint8_t> cueTrackPositionsMaster;
            appendEbmlId(cueTrackPositionsMaster, 0xB7);
            appendEbmlSize(cueTrackPositionsMaster, cueTrackPositions.size());
            cueTrackPositionsMaster.insert(cueTrackPositionsMaster.end(), cueTrackPositions.begin(), cueTrackPositions.end());

            vector<uint8_t> cuePoint;
            appendEbmlUnsignedElement(cuePoint, 0xB3, cue.timeMs);
            cuePoint.insert(cuePoint.end(), cueTrackPositionsMaster.begin(), cueTrackPositionsMaster.end());

            vector<uint8_t> cuePointMaster;
            appendEbmlId(cuePointMaster, 0xBB);
            appendEbmlSize(cuePointMaster, cuePoint.size());
            cuePointMaster.insert(cuePointMaster.end(), cuePoint.begin(), cuePoint.end());

            cuesPayload.insert(cuesPayload.end(), cuePointMaster.begin(), cuePointMaster.end());
        }

        vector<uint8_t> cuesElement;
        appendEbmlId(cuesElement, 0x1C53BB6B);
        appendEbmlSize(cuesElement, cuesPayload.size());
        cuesElement.insert(cuesElement.end(), cuesPayload.begin(), cuesPayload.end());
        writeBytes(cuesElement);
        if(cuesOffset >= m_segmentDataStartOffset){
            m_cuesPosition = cuesOffset - m_segmentDataStartOffset;
        }
    }

    void rewriteSeekHead(){
        if(m_seekHeadReservedBytes == 0){
            return;
        }

        struct SeekHeadEntry final{
            uint32_t id{};
            uint64_t position{};
        };

        vector<SeekHeadEntry> entries;
        if(m_infoPosition){
            entries.push_back(SeekHeadEntry{.id = 0x1549A966, .position = *m_infoPosition});
        }
        if(m_tracksPosition){
            entries.push_back(SeekHeadEntry{.id = 0x1654AE6B, .position = *m_tracksPosition});
        }
        if(m_cuesPosition){
            entries.push_back(SeekHeadEntry{.id = 0x1C53BB6B, .position = *m_cuesPosition});
        }
        if(entries.empty()){
            return;
        }

        vector<uint8_t> seekHeadPayload;
        for(const auto& entry: entries){
            vector<uint8_t> seekIdPayload;
            appendEbmlId(seekIdPayload, entry.id);

            vector<uint8_t> seekEntry;
            appendEbmlElement(seekEntry, 0x53AB, seekIdPayload);
            appendEbmlUnsignedElement(seekEntry, 0x53AC, entry.position);

            vector<uint8_t> seekEntryMaster;
            appendEbmlId(seekEntryMaster, 0x4DBB);
            appendEbmlSize(seekEntryMaster, seekEntry.size());
            seekEntryMaster.insert(seekEntryMaster.end(), seekEntry.begin(), seekEntry.end());

            seekHeadPayload.insert(seekHeadPayload.end(), seekEntryMaster.begin(), seekEntryMaster.end());
        }

        vector<uint8_t> seekHead;
        appendEbmlId(seekHead, 0x114D9B74);
        appendEbmlSize(seekHead, seekHeadPayload.size());
        seekHead.insert(seekHead.end(), seekHeadPayload.begin(), seekHeadPayload.end());

        if(seekHead.size() > m_seekHeadReservedBytes){
            throw hresult_error(E_INVALIDARG, L"Reserved WebM SeekHead space was too small");
        }

        vector<uint8_t> replacement{seekHead};
        appendExactSizedVoidPadding(replacement, static_cast<size_t>(m_seekHeadReservedBytes - replacement.size()));

        const auto endOffset{currentOffset()};
        m_stream.seekp(static_cast<streamoff>(m_seekHeadOffset));
        if(!m_stream){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while seeking to rewrite WebM SeekHead");
        }

        writeBytes(replacement);

        m_stream.seekp(static_cast<streamoff>(endOffset));
        if(!m_stream){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while restoring WebM output position");
        }
    }

private:
    ofstream m_stream;
    uint64_t m_segmentDataStartOffset{};
    uint64_t m_seekHeadOffset{};
    uint64_t m_seekHeadReservedBytes{};
    optional<uint64_t> m_infoPosition{};
    optional<uint64_t> m_tracksPosition{};
    optional<uint64_t> m_cuesPosition{};
    bool m_clusterOpen{};
    bool m_clusterHasBlocks{};
    int64_t m_clusterTimeMs{};
    vector<CuePoint> m_cues{};
    bool m_finalized{};
};

uint32_t mpegTsCrc32(const uint8_t* data, size_t size){
    uint32_t crc{0xFFFF'FFFFu};
    for(size_t i{}; i < size; ++i){
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for(int bit{}; bit < 8; ++bit){
            crc = (crc & 0x8000'0000u) ? ((crc << 1) ^ 0x04C1'1DB7u) : (crc << 1);
        }
    }
    return crc;
}

void appendPts(vector<uint8_t>& out, uint8_t prefix, int64_t time100ns){
    const auto pts{static_cast<uint64_t>(max<int64_t>(0, time100ns) * 9 / 1000) & 0x1FFFFFFFFULL};
    out.push_back(static_cast<uint8_t>((prefix << 4) | (((pts >> 30) & 0x07) << 1) | 1));
    out.push_back(static_cast<uint8_t>((pts >> 22) & 0xFF));
    out.push_back(static_cast<uint8_t>((((pts >> 15) & 0x7F) << 1) | 1));
    out.push_back(static_cast<uint8_t>((pts >> 7) & 0xFF));
    out.push_back(static_cast<uint8_t>(((pts & 0x7F) << 1) | 1));
}

bool isH264VideoSubtype(const GUID& subtype){
    return subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_H264_ES;
}

bool aacSampleLooksAdts(const uint8_t* data, size_t size){
    return data && size >= 2 && data[0] == 0xFF && (data[1] & 0xF0) == 0xF0;
}

optional<uint8_t> aacSamplingFrequencyIndex(uint32_t sampleRate){
    switch(sampleRate){
    case 96000: return uint8_t{0};
    case 88200: return uint8_t{1};
    case 64000: return uint8_t{2};
    case 48000: return uint8_t{3};
    case 44100: return uint8_t{4};
    case 32000: return uint8_t{5};
    case 24000: return uint8_t{6};
    case 22050: return uint8_t{7};
    case 16000: return uint8_t{8};
    case 12000: return uint8_t{9};
    case 11025: return uint8_t{10};
    case 8000: return uint8_t{11};
    case 7350: return uint8_t{12};
    default: return nullopt;
    }
}

vector<uint8_t> addAdtsHeaderToAacFrame(const uint8_t* data, size_t size, uint32_t sampleRate, uint32_t channels){
    if(!data || size == 0){
        return {};
    }
    const auto samplingIndex{aacSamplingFrequencyIndex(sampleRate)};
    if(!samplingIndex || channels == 0 || channels > 7 || size + 7 > 0x1FFF){
        return {};
    }

    const auto profile{uint8_t{1}}; // AAC LC, encoded as object_type - 1.
    const auto channelConfig{static_cast<uint8_t>(channels)};
    const auto frameLength{static_cast<uint16_t>(size + 7)};

    vector<uint8_t> framed;
    framed.reserve(size + 7);
    framed.push_back(0xFF);
    framed.push_back(0xF1);
    framed.push_back(static_cast<uint8_t>((profile << 6) | ((*samplingIndex & 0x0F) << 2) | ((channelConfig >> 2) & 0x01)));
    framed.push_back(static_cast<uint8_t>(((channelConfig & 0x03) << 6) | ((frameLength >> 11) & 0x03)));
    framed.push_back(static_cast<uint8_t>((frameLength >> 3) & 0xFF));
    framed.push_back(static_cast<uint8_t>(((frameLength & 0x07) << 5) | 0x1F));
    framed.push_back(0xFC);
    framed.insert(framed.end(), data, data + size);
    return framed;
}

bool bytesAreZeroPadding(const uint8_t* data, size_t size){
    if(!data){
        return size == 0;
    }
    return all_of(data, data + size, [](const uint8_t value){ return value == 0; });
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

vector<uint8_t> normalizeH264SampleForMpegTsImpl(const uint8_t* data, size_t size, uint32_t nalLengthFieldSize){
    if(const auto annexBOffset{findAnnexBStartCodeOffset(data, size)}){
        return vector<uint8_t>{data + *annexBOffset, data + size};
    }
    return convertLengthPrefixedNalSampleToAnnexB(data, size, nalLengthFieldSize);
}

vector<uint8_t> extractH264ParameterSetsAnnexBFromSequenceHeader(const uint8_t* data, size_t size){
    vector<uint8_t> parameterSets;
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

class MpegTsWriter final{
public:
    explicit MpegTsWriter(const wstring& outputPath, bool includeAudio):
        m_includeAudio{includeAudio},
        m_stream{filesystem::path{outputPath}, ios::binary | ios::trunc}{
        if(!m_stream){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), L"Failed to create MPEG-TS output file");
        }
        writeTables();
    }

    void writeVideoSample(const uint8_t* data, size_t size, int64_t pts100ns, optional<int64_t> dts100ns, bool keyframe){
        if(!data || size == 0){
            return;
        }
        writeTablesIfDue(pts100ns);
        const auto trustedDts100ns{sanitizeMpegTsVideoDecodeTime100ns(dts100ns, pts100ns)};
        writePes(kVideoPid, 0xE0, data, size, pts100ns, trustedDts100ns, true, m_videoContinuityCounter);
    }

    void writeAudioSample(const uint8_t* data, size_t size, int64_t pts100ns){
        if(!m_includeAudio || !data || size == 0){
            return;
        }
        writeTablesIfDue(pts100ns);
        writePes(kAudioPid, 0xC0, data, size, pts100ns, nullopt, false, m_audioContinuityCounter);
    }

    void finalize(){
        m_stream.flush();
        if(!m_stream){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while finalizing MPEG-TS output");
        }
    }

private:
    static constexpr int64_t kTableRepeatInterval100ns{5'000'000};
    static constexpr uint16_t kPatPid{0x0000};
    static constexpr uint16_t kPmtPid{0x1000};
    static constexpr uint16_t kAudioPid{0x0100};
    static constexpr uint16_t kVideoPid{0x0101};

    void writeBytes(const uint8_t* data, size_t size){
        m_stream.write(reinterpret_cast<const char*>(data), static_cast<streamsize>(size));
        if(!m_stream){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while writing MPEG-TS output");
        }
    }

    void writeTables(){
        vector<uint8_t> pat{
            0x00,
            0x00, 0xB0, 0x0D,
            0x00, 0x01,
            0xC1, 0x00, 0x00,
            0x00, 0x01,
            static_cast<uint8_t>(0xE0 | ((kPmtPid >> 8) & 0x1F)),
            static_cast<uint8_t>(kPmtPid & 0xFF)};
        appendCrc(pat, 1);
        writePsiPacket(kPatPid, pat, m_patContinuityCounter);

        const auto sectionLength{static_cast<uint16_t>(m_includeAudio ? 0x17 : 0x12)};
        vector<uint8_t> pmt{
            0x00,
            0x02, static_cast<uint8_t>(0xB0 | ((sectionLength >> 8) & 0x0F)), static_cast<uint8_t>(sectionLength & 0xFF),
            0x00, 0x01,
            0xC1, 0x00, 0x00,
            static_cast<uint8_t>(0xE0 | ((kVideoPid >> 8) & 0x1F)),
            static_cast<uint8_t>(kVideoPid & 0xFF),
            0xF0, 0x00,
            0x1B,
            static_cast<uint8_t>(0xE0 | ((kVideoPid >> 8) & 0x1F)),
            static_cast<uint8_t>(kVideoPid & 0xFF),
            0xF0, 0x00};
        if(m_includeAudio){
            pmt.push_back(0x0F);
            pmt.push_back(static_cast<uint8_t>(0xE0 | ((kAudioPid >> 8) & 0x1F)));
            pmt.push_back(static_cast<uint8_t>(kAudioPid & 0xFF));
            pmt.push_back(0xF0);
            pmt.push_back(0x00);
        }
        appendCrc(pmt, 1);
        writePsiPacket(kPmtPid, pmt, m_pmtContinuityCounter);
    }

    void writeTablesIfDue(int64_t time100ns){
        if(m_nextTableWriteTime100ns < 0 || time100ns >= m_nextTableWriteTime100ns){
            writeTables();
            m_nextTableWriteTime100ns = max<int64_t>(0, time100ns) + kTableRepeatInterval100ns;
        }
    }

    void appendCrc(vector<uint8_t>& sectionWithPointer, size_t sectionOffset){
        const auto crc{mpegTsCrc32(sectionWithPointer.data() + sectionOffset, sectionWithPointer.size() - sectionOffset)};
        sectionWithPointer.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));
        sectionWithPointer.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
        sectionWithPointer.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
        sectionWithPointer.push_back(static_cast<uint8_t>(crc & 0xFF));
    }

    void writePsiPacket(uint16_t pid, const vector<uint8_t>& payload, uint8_t& continuityCounter){
        array<uint8_t, 188> packet{};
        packet.fill(0xFF);
        packet[0] = 0x47;
        packet[1] = static_cast<uint8_t>(0x40 | ((pid >> 8) & 0x1F));
        packet[2] = static_cast<uint8_t>(pid & 0xFF);
        packet[3] = static_cast<uint8_t>(0x10 | (continuityCounter++ & 0x0F));
        const auto copySize{min<size_t>(payload.size(), packet.size() - 4)};
        memcpy(packet.data() + 4, payload.data(), copySize);
        writeBytes(packet.data(), packet.size());
    }

    void writePes(uint16_t pid, uint8_t streamId, const uint8_t* data, size_t size, int64_t pts100ns, optional<int64_t> dts100ns, bool keyframe, uint8_t& continuityCounter){
        vector<uint8_t> pes;
        pes.reserve(size + 32);
        pes.insert(pes.end(), {0x00, 0x00, 0x01, streamId});
        const auto pesHeaderDataLength{static_cast<size_t>(dts100ns && *dts100ns != pts100ns ? 10 : 5)};
        const auto packetLength{size + 3 + pesHeaderDataLength};
        if((streamId & 0xE0) == 0xC0 && packetLength <= 0xFFFF){
            pes.push_back(static_cast<uint8_t>((packetLength >> 8) & 0xFF));
            pes.push_back(static_cast<uint8_t>(packetLength & 0xFF));
        }else{
            pes.push_back(0x00);
            pes.push_back(0x00);
        }
        pes.push_back(0x80);
        if(dts100ns && *dts100ns != pts100ns){
            pes.push_back(0xC0);
            pes.push_back(10);
            appendPts(pes, 0x03, pts100ns);
            appendPts(pes, 0x01, *dts100ns);
        }else{
            pes.push_back(0x80);
            pes.push_back(5);
            appendPts(pes, 0x02, pts100ns);
        }
        pes.insert(pes.end(), data, data + size);

        size_t offset{};
        bool payloadStart{true};
        while(offset < pes.size()){
            array<uint8_t, 188> packet{};
            packet.fill(0xFF);
            packet[0] = 0x47;
            packet[1] = static_cast<uint8_t>((payloadStart ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
            packet[2] = static_cast<uint8_t>(pid & 0xFF);

            const auto remaining{pes.size() - offset};
            const auto includePcr{payloadStart && pid == kVideoPid};
            size_t adaptationBytes{};
            size_t payloadBytes{};
            if(includePcr){
                adaptationBytes = 8;
                payloadBytes = min(remaining, packet.size() - 4 - adaptationBytes);
            }else{
                payloadBytes = min(remaining, packet.size() - 4);
            }
            if(payloadBytes == remaining){
                const auto availableAdaptationBytes{packet.size() - 4 - payloadBytes};
                adaptationBytes = includePcr ? max<size_t>(8, availableAdaptationBytes) : availableAdaptationBytes;
                payloadBytes = packet.size() - 4 - adaptationBytes;
            }

            if(adaptationBytes > 0){
                packet[3] = static_cast<uint8_t>(0x30 | (continuityCounter++ & 0x0F));
                packet[4] = static_cast<uint8_t>(adaptationBytes - 1);
                packet[5] = includePcr ? 0x10 : 0x00;
                size_t payloadOffset{6};
                if(includePcr){
                    writePcr(packet.data() + payloadOffset, pts100ns);
                    payloadOffset += 6;
                }
                while(payloadOffset < 4 + adaptationBytes){
                    packet[payloadOffset++] = 0xFF;
                }
                memcpy(packet.data() + payloadOffset, pes.data() + offset, payloadBytes);
            }else{
                packet[3] = static_cast<uint8_t>(0x10 | (continuityCounter++ & 0x0F));
                memcpy(packet.data() + 4, pes.data() + offset, payloadBytes);
            }

            writeBytes(packet.data(), packet.size());
            offset += payloadBytes;
            payloadStart = false;
        }
    }

    void writePcr(uint8_t* out, int64_t time100ns){
        const auto pcrBase{static_cast<uint64_t>(max<int64_t>(0, time100ns) * 9 / 1000) & 0x1FFFFFFFFULL};
        out[0] = static_cast<uint8_t>((pcrBase >> 25) & 0xFF);
        out[1] = static_cast<uint8_t>((pcrBase >> 17) & 0xFF);
        out[2] = static_cast<uint8_t>((pcrBase >> 9) & 0xFF);
        out[3] = static_cast<uint8_t>((pcrBase >> 1) & 0xFF);
        out[4] = static_cast<uint8_t>(((pcrBase & 0x01) << 7) | 0x7E);
        out[5] = 0x00;
    }

private:
    ofstream m_stream;
    bool m_includeAudio{};
    int64_t m_nextTableWriteTime100ns{-1};
    uint8_t m_patContinuityCounter{};
    uint8_t m_pmtContinuityCounter{};
    uint8_t m_audioContinuityCounter{};
    uint8_t m_videoContinuityCounter{};
};

}

MFLifetime::MFLifetime(){
    check_hresult(MFStartup(MF_VERSION, MFSTARTUP_FULL));
}

MFLifetime::~MFLifetime(){
    MFShutdown();
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

vector<uint8_t> normalizeH264SampleForMpegTs(const uint8_t* data, size_t size, uint32_t nalLengthFieldSize){
    return normalizeH264SampleForMpegTsImpl(data, size, nalLengthFieldSize);
}

vector<uint8_t> extractH264ParameterSetsAnnexBForMpegTs(const vector<uint8_t>& sequenceHeader){
    return extractH264ParameterSetsAnnexBFromSequenceHeader(sequenceHeader.data(), sequenceHeader.size());
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

bool bitstreamLooksLikeRandomAccessPoint(const uint8_t* data, size_t size, const GUID& subtype, uint32_t nalLengthFieldSize, bool allowInconclusive){
    if(!data || size == 0){
        return allowInconclusive;
    }

    auto classifyNalType = [&](const uint8_t nalHeader) -> std::optional<bool>{
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
        return std::nullopt;
    };

    {
        const auto nalSizeField{clamp<uint32_t>(nalLengthFieldSize, 1, 4)};
        size_t offset{};
        while(offset + nalSizeField <= size){
            uint32_t nalLength{};
            for(uint32_t i{0}; i < nalSizeField; ++i){
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

    for(size_t i{0}; i + 4 < size; ++i){
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

bool isContainerSyncSample(const com_ptr<IMFSample>& sample){
    UINT32 cleanPoint{};
    return SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0;
}

bool shouldTreatMpegTsSampleAsKeyframe(bool, bool isBitstreamRap){
    return isBitstreamRap;
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

void writeMixedAudioForKeepRanges(const com_ptr<IMFSourceReader>& audioReader, DWORD audioStreamIndex, const vector<pair<int64_t, int64_t>>& keepRanges100ns, const com_ptr<IMFSinkWriter>& writer, DWORD writerAudioStreamIndex, uint32_t audioChannels, uint32_t audioSampleRate, int crossfadeMs, float gain, const function<void(double)>& progressCallback, const function<bool()>& shouldCancel){
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

VideoWriteStats writeWebmVp9VideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const std::wstring& outputPath, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, int64_t sourceDuration100ns, int64_t outputDuration100ns, uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen, const function<void(double)>& progressCallback, const function<bool()>& shouldCancel){
    if(videoSubtype != MFVideoFormat_VP90){
        throw hresult_error(MF_E_INVALIDMEDIATYPE, L"WebM stream-copy export currently requires VP9 video");
    }

    auto waitingForCleanPoint{false};
    int64_t lastInTime100ns{-1};
    VideoWriteStats stats{};
    WebmVp9Writer writer(outputPath, outputDuration100ns, width, height, fpsNum, fpsDen);

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

        const auto outTime100ns{inTime100ns - removedDurationBefore(effectiveCutRanges100ns, inTime100ns)};
        const auto keyframe{isContainerSyncSample(sample) && isTrueRandomAccessPointSample(sample, videoSubtype, nalLengthFieldSize, false)};

        com_ptr<IMFMediaBuffer> contiguousBuffer;
        check_hresult(sample->ConvertToContiguousBuffer(contiguousBuffer.put()));

        BYTE* data{};
        DWORD maxLength{};
        DWORD currentLength{};
        check_hresult(contiguousBuffer->Lock(&data, &maxLength, &currentLength));
        writer.writeSample(data, currentLength, outTime100ns, keyframe);
        check_hresult(contiguousBuffer->Unlock());

        ++stats.writtenSampleCount;

        if(progressCallback && sourceDuration100ns > 0 && (stats.readSampleCount % 24 == 0)){
            const auto pct{(100.0 * inTime100ns) / sourceDuration100ns};
            progressCallback(pct);
        }
    }

    writer.finalize();

    if(progressCallback){
        progressCallback(100.0);
    }
    return stats;
}

VideoWriteStats writeMpegTsH264VideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const std::wstring& outputPath, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, const com_ptr<IMFMediaType>& videoMediaType, int64_t sourceDuration100ns, const com_ptr<IMFSourceReader>& audioReader, DWORD audioStreamIndex, const com_ptr<IMFMediaType>& audioMediaType, bool keepAudio, const function<void(double)>& progressCallback, const function<void(double)>& audioProgressCallback, const function<bool()>& shouldCancel){
    if(!isH264VideoSubtype(videoSubtype)){
        throw hresult_error(MF_E_INVALIDMEDIATYPE, L"MPEG-TS stream-copy export currently requires H.264 video");
    }

    vector<uint8_t> h264ParameterSetsAnnexB;
    if(videoMediaType){
        UINT8* sequenceHeaderBytes{};
        UINT32 sequenceHeaderSize{};
        if(SUCCEEDED(videoMediaType->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &sequenceHeaderBytes, &sequenceHeaderSize)) && sequenceHeaderBytes && sequenceHeaderSize > 0){
            h264ParameterSetsAnnexB = extractH264ParameterSetsAnnexBFromSequenceHeader(sequenceHeaderBytes, sequenceHeaderSize);
        }
        CoTaskMemFree(sequenceHeaderBytes);
    }

    constexpr auto invalidStream{(numeric_limits<DWORD>::max)()};
    auto includeAudio{keepAudio && audioReader && audioMediaType && audioStreamIndex != invalidStream};
    uint32_t audioSampleRate{};
    uint32_t audioChannels{};
    if(includeAudio){
        GUID audioSubtype{GUID_NULL};
        check_hresult(audioMediaType->GetGUID(MF_MT_SUBTYPE, &audioSubtype));
        if(audioSubtype != MFAudioFormat_AAC){
            throw hresult_error(MF_E_INVALIDMEDIATYPE, L"MPEG-TS audio passthrough currently requires AAC audio");
        }
        (void)audioMediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &audioSampleRate);
        (void)audioMediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &audioChannels);
        if(audioSampleRate == 0 || audioChannels == 0){
            throw hresult_error(MF_E_INVALIDMEDIATYPE, L"MPEG-TS audio passthrough requires AAC sample rate and channel metadata");
        }
    }

    MpegTsWriter writer{outputPath, includeAudio};
    auto waitingForCleanPoint{false};
    auto dtsPtsShift100ns{static_cast<int64_t>(0)};
    auto dtsPtsShiftInitialized{false};
    VideoWriteStats stats{};
    uint64_t audioReadSampleCount{};
    uint32_t videoFpsNum{};
    uint32_t videoFpsDen{};
    (void)MFGetAttributeRatio(videoMediaType.get(), MF_MT_FRAME_RATE, &videoFpsNum, &videoFpsDen);
    const auto nominalVideoFrameDuration100ns{
        (videoFpsNum > 0 && videoFpsDen > 0) ? max<int64_t>(1, static_cast<int64_t>((10'000'000LL * static_cast<int64_t>(videoFpsDen)) / videoFpsNum)) : int64_t{}};
    optional<int64_t> nextInferredDecodeTime100ns{};

    if(progressCallback){
        progressCallback(0.0);
    }
    if(includeAudio && audioProgressCallback){
        audioProgressCallback(0.0);
    }

    struct PendingVideoSample final{
        vector<uint8_t> data{};
        int64_t inTime100ns{};
        int64_t outTime100ns{};
        int64_t writeOrderTime100ns{};
        optional<int64_t> outDecodeTime100ns{};
        bool keyframe{};
    };

    struct PendingVideoAccessUnit final{
        vector<uint8_t> rawData{};
        int64_t inTime100ns{};
        optional<int64_t> decodeTime100ns{};
        int64_t duration100ns{};
    };

    struct PendingAudioSample final{
        vector<uint8_t> data{};
        int64_t inTime100ns{};
        int64_t outTime100ns{};
    };

    optional<PendingVideoAccessUnit> pendingVideoAccessUnit{};

    auto finalizePendingVideoAccessUnit = [&](PendingVideoAccessUnit&& accessUnit) -> optional<PendingVideoSample>{
        if(accessUnit.rawData.empty()){
            return nullopt;
        }

        auto dropped{false};
        for(const auto& [start, end] : effectiveCutRanges100ns){
            if(accessUnit.inTime100ns < start){
                break;
            }
            if(accessUnit.inTime100ns < end){
                dropped = true;
                break;
            }
        }
        if(dropped){
            ++stats.droppedByCutCount;
            waitingForCleanPoint = true;
            return nullopt;
        }

        auto outputData{normalizeH264SampleForMpegTs(accessUnit.rawData.data(), accessUnit.rawData.size(), nalLengthFieldSize)};
        if(outputData.empty()){
            throw hresult_error(MF_E_INVALIDMEDIATYPE, L"H.264 sample is neither Annex B nor a valid length-prefixed sample");
        }

        const auto isBitstreamRap{bitstreamLooksLikeRandomAccessPoint(outputData.data(), outputData.size(), videoSubtype, 0, true)};
        const auto isKeyframe{shouldTreatMpegTsSampleAsKeyframe(false, isBitstreamRap)};
        if(waitingForCleanPoint){
            if(!isKeyframe){
                ++stats.droppedWaitingRapCount;
                return nullopt;
            }
            waitingForCleanPoint = false;
        }

        const auto removedAtPresentationTime{removedDurationBefore(effectiveCutRanges100ns, accessUnit.inTime100ns)};
        auto outTime100ns{accessUnit.inTime100ns - removedAtPresentationTime};
        optional<int64_t> outDecodeTime100ns{};
        auto decodeTimeForOutput100ns{resolveMpegTsDecodeTime100ns(nextInferredDecodeTime100ns, accessUnit.duration100ns, nominalVideoFrameDuration100ns)};

        if(decodeTimeForOutput100ns.has_value()){
            if(!dtsPtsShiftInitialized){
                dtsPtsShift100ns = max<int64_t>(0, -*decodeTimeForOutput100ns);
                dtsPtsShiftInitialized = true;
            }
            outTime100ns += dtsPtsShift100ns;
            outDecodeTime100ns = max<int64_t>(0, *decodeTimeForOutput100ns + dtsPtsShift100ns);
        }else if(!dtsPtsShiftInitialized){
            dtsPtsShiftInitialized = true;
        }

        if(isKeyframe && !h264ParameterSetsAnnexB.empty() && !h264AnnexBSampleContainsParameterSets(outputData.data(), outputData.size())){
            vector<uint8_t> prefixedData;
            prefixedData.reserve(h264ParameterSetsAnnexB.size() + outputData.size());
            prefixedData.insert(prefixedData.end(), h264ParameterSetsAnnexB.begin(), h264ParameterSetsAnnexB.end());
            prefixedData.insert(prefixedData.end(), outputData.begin(), outputData.end());
            outputData = move(prefixedData);
        }

        return PendingVideoSample{
            .data = move(outputData),
            .inTime100ns = accessUnit.inTime100ns,
            .outTime100ns = outTime100ns,
            .writeOrderTime100ns = outTime100ns,
            .outDecodeTime100ns = outDecodeTime100ns,
            .keyframe = isKeyframe};
    };

    auto readNextVideoSample = [&]() -> optional<PendingVideoSample>{
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
                if(pendingVideoAccessUnit.has_value()){
                    auto completedAccessUnit{move(*pendingVideoAccessUnit)};
                    pendingVideoAccessUnit.reset();
                    if(auto completedSample{finalizePendingVideoAccessUnit(move(completedAccessUnit))}){
                        return completedSample;
                    }
                }
                return nullopt;
            }
            if(!sample){
                continue;
            }

            ++stats.readSampleCount;

            LONGLONG sampleTime100ns{};
            const auto hasSampleTime{SUCCEEDED(sample->GetSampleTime(&sampleTime100ns))};
            const auto startsNewAccessUnit{hasSampleTime || timestamp > 0};
            const auto inTime100ns{resolveReaderSampleTime100ns(hasSampleTime ? sampleTime100ns : 0, timestamp)};
            optional<int64_t> decodeTime100ns{};
            UINT64 decodeTimestamp100ns{};
            if(SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &decodeTimestamp100ns))){
                decodeTime100ns = static_cast<int64_t>(decodeTimestamp100ns);
            }
            LONGLONG sampleDuration100ns{};
            if(FAILED(sample->GetSampleDuration(&sampleDuration100ns)) || sampleDuration100ns < 0){
                sampleDuration100ns = 0;
            }

            com_ptr<IMFMediaBuffer> contiguousBuffer;
            check_hresult(sample->ConvertToContiguousBuffer(contiguousBuffer.put()));

            BYTE* data{};
            DWORD maxLength{};
            DWORD currentLength{};
            check_hresult(contiguousBuffer->Lock(&data, &maxLength, &currentLength));
            vector<uint8_t> rawChunkData{data, data + currentLength};
            contiguousBuffer->Unlock();

            if(startsNewAccessUnit){
                if(pendingVideoAccessUnit.has_value()){
                    auto completedAccessUnit{move(*pendingVideoAccessUnit)};
                    pendingVideoAccessUnit = PendingVideoAccessUnit{
                        .rawData = move(rawChunkData),
                        .inTime100ns = inTime100ns,
                        .decodeTime100ns = decodeTime100ns,
                        .duration100ns = sampleDuration100ns};
                    if(auto completedSample{finalizePendingVideoAccessUnit(move(completedAccessUnit))}){
                        return completedSample;
                    }
                    continue;
                }

                pendingVideoAccessUnit = PendingVideoAccessUnit{
                    .rawData = move(rawChunkData),
                    .inTime100ns = inTime100ns,
                    .decodeTime100ns = decodeTime100ns,
                    .duration100ns = sampleDuration100ns};
                continue;
            }

            if(!pendingVideoAccessUnit.has_value()){
                continue;
            }
            auto& accessUnit{*pendingVideoAccessUnit};
            accessUnit.rawData.insert(accessUnit.rawData.end(), rawChunkData.begin(), rawChunkData.end());
        }
    };

    auto readNextAudioSample = [&]() -> optional<PendingAudioSample>{
        if(!includeAudio){
            return nullopt;
        }

        for(;;){
            if(shouldCancel && shouldCancel()){
                throw hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
            }

            DWORD actualStream{};
            DWORD flags{};
            LONGLONG timestamp{};
            com_ptr<IMFSample> sample;
            const auto hr{audioReader->ReadSample(audioStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put())};
            if(FAILED(hr)){
                check_hresult(hr);
            }
            if(flags & MF_SOURCE_READERF_ENDOFSTREAM){
                return nullopt;
            }
            if(!sample){
                continue;
            }

            ++audioReadSampleCount;

            LONGLONG sampleTime100ns{};
            if(FAILED(sample->GetSampleTime(&sampleTime100ns))){
                sampleTime100ns = timestamp;
            }
            const auto inTime100ns{max<int64_t>(0, sampleTime100ns)};

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
                continue;
            }

            const auto removedAtPresentationTime{removedDurationBefore(effectiveCutRanges100ns, inTime100ns)};
            const auto outTime100ns{max<int64_t>(0, inTime100ns - removedAtPresentationTime + dtsPtsShift100ns)};

            com_ptr<IMFMediaBuffer> contiguousBuffer;
            check_hresult(sample->ConvertToContiguousBuffer(contiguousBuffer.put()));

            BYTE* data{};
            DWORD maxLength{};
            DWORD currentLength{};
            check_hresult(contiguousBuffer->Lock(&data, &maxLength, &currentLength));

            vector<uint8_t> outputData;
            if(aacSampleLooksAdts(data, currentLength)){
                outputData.assign(data, data + currentLength);
            }else{
                outputData = addAdtsHeaderToAacFrame(data, currentLength, audioSampleRate, audioChannels);
                if(outputData.empty()){
                    contiguousBuffer->Unlock();
                    throw hresult_error(MF_E_INVALIDMEDIATYPE, L"MPEG-TS AAC audio sample cannot be written as ADTS");
                }
            }
            contiguousBuffer->Unlock();

            if(audioProgressCallback && sourceDuration100ns > 0 && (audioReadSampleCount % 256 == 0)){
                audioProgressCallback((100.0 * inTime100ns) / sourceDuration100ns);
            }

            return PendingAudioSample{
                .data = move(outputData),
                .inTime100ns = inTime100ns,
                .outTime100ns = outTime100ns};
        }
    };

    auto nextVideo{readNextVideoSample()};
    auto nextAudio{readNextAudioSample()};
    optional<int64_t> outputTimelineBase100ns{};
    optional<int64_t> outputDecodeTimelineBase100ns{};

    while(nextVideo || nextAudio){
        if(shouldCancel && shouldCancel()){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
        }

        if(!outputTimelineBase100ns.has_value()){
            if(nextVideo){
                outputTimelineBase100ns = nextVideo->outTime100ns;
                outputDecodeTimelineBase100ns = nextVideo->outDecodeTime100ns.value_or(nextVideo->outTime100ns);
            }else if(nextAudio){
                outputTimelineBase100ns = nextAudio->outTime100ns;
            }
        }

        while(outputTimelineBase100ns.has_value() && nextAudio && nextAudio->outTime100ns < *outputTimelineBase100ns){
            nextAudio = readNextAudioSample();
        }

        const auto writeAudioNext{nextAudio && (!nextVideo || nextAudio->outTime100ns <= nextVideo->writeOrderTime100ns)};
        if(writeAudioNext){
            const auto rebasedOutTime100ns{max<int64_t>(0, nextAudio->outTime100ns - outputTimelineBase100ns.value_or(0))};
            writer.writeAudioSample(nextAudio->data.data(), nextAudio->data.size(), rebasedOutTime100ns);
            nextAudio = readNextAudioSample();
        }else if(nextVideo){
            const auto rebasedOutTime100ns{max<int64_t>(0, nextVideo->outTime100ns - outputTimelineBase100ns.value_or(0))};
            optional<int64_t> rebasedDecodeTime100ns{};
            if(nextVideo->outDecodeTime100ns.has_value()){
                rebasedDecodeTime100ns = max<int64_t>(0, *nextVideo->outDecodeTime100ns - outputDecodeTimelineBase100ns.value_or(0));
            }
            writer.writeVideoSample(nextVideo->data.data(), nextVideo->data.size(), rebasedOutTime100ns, rebasedDecodeTime100ns, nextVideo->keyframe);
            ++stats.writtenSampleCount;
            if(progressCallback && sourceDuration100ns > 0 && (stats.readSampleCount % 24 == 0)){
                progressCallback((100.0 * nextVideo->inTime100ns) / sourceDuration100ns);
            }
            nextVideo = readNextVideoSample();
        }
    }

    writer.finalize();
    if(progressCallback){
        progressCallback(100.0);
    }
    if(includeAudio && audioProgressCallback){
        audioProgressCallback(100.0);
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
