module;

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

export module llvc.VideoContainer;

import std;
import llvc.Export;
import llvc.VideoStream;
import llvc.Utils;

export namespace llvc{

using namespace ::std;
using namespace ::winrt;

class MediaFoundationSinkWriter final{
public:
    explicit MediaFoundationSinkWriter(const wstring& outputPath);
    MediaFoundationSinkWriter(const MediaFoundationSinkWriter&) = delete;
    MediaFoundationSinkWriter& operator=(const MediaFoundationSinkWriter&) = delete;
    ~MediaFoundationSinkWriter();

    HRESULT addStream(const com_ptr<IMFMediaType>& outputType, const com_ptr<IMFMediaType>& inputType, DWORD& streamIndex) noexcept;
    void beginWriting() const;
    void finalize() const;
    IMFSinkWriter* writer() const noexcept;

private:
    IMFSinkWriter* m_writer{};
};

enum class VideoContainerExportKind : uint8_t{
    MediaFoundation,
    WebmVp9,
    MpegTs,
    Wmv
};

struct VideoContainerExportRequest final{
    wstring sourcePath{};
    wstring temporaryOutputPath{};
    vector<pair<int64_t, int64_t>> effectiveCutRanges100ns{};
    int64_t sourceDuration100ns{};
    int64_t outputDuration100ns{};
    bool keepAudio{};
    bool allowAudio{};
    int audioCrossfadeMs{};
    int audioVolumePct{100};
    function<void(double)> onVideoProgress{};
    function<void(double)> onAudioProgress{};
    function<bool()> shouldCancel{};
};

VideoWriteStats writeWebmVp9VideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const std::wstring& outputPath, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, int64_t sourceDuration100ns, int64_t outputDuration100ns, uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen, const function<void(double)>& progressCallback, const function<bool()>& shouldCancel);
VideoWriteStats writeMpegTsH264VideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const std::wstring& outputPath, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, const com_ptr<IMFMediaType>& videoMediaType, int64_t sourceDuration100ns, const com_ptr<IMFSourceReader>& audioReader, DWORD audioStreamIndex, const com_ptr<IMFMediaType>& audioMediaType, bool keepAudio, const function<void(double)>& progressCallback, const function<void(double)>& audioProgressCallback, const function<bool()>& shouldCancel);
VideoWriteStats writeMediaFoundationVideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, IMFSinkWriter* writer, DWORD writerVideoStreamIndex, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, int64_t sourceDuration100ns, const function<void(double)>& progressCallback, const function<bool()>& shouldCancel);
void writeVideoContainerForExport(VideoContainerExportKind exportKind, const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const com_ptr<IMFMediaType>& sourceVideoType, GUID videoSubtype, uint32_t nalLengthFieldSize, const VideoContainerExportRequest& request);

}


namespace llvc{

using namespace ::std;
using namespace ::winrt;

MediaFoundationSinkWriter::MediaFoundationSinkWriter(const wstring& outputPath){
    com_ptr<IMFAttributes> writerAttributes;
    checkHresult(MFCreateAttributes(writerAttributes.put(), 1));
    checkHresult(writerAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
    com_ptr<IMFSinkWriter> writer;
    checkHresult(MFCreateSinkWriterFromURL(outputPath.c_str(), nullptr, writerAttributes.get(), writer.put()));
    m_writer = writer.detach();
}

MediaFoundationSinkWriter::~MediaFoundationSinkWriter(){
    if(m_writer){
        m_writer->Release();
    }
}

HRESULT MediaFoundationSinkWriter::addStream(const com_ptr<IMFMediaType>& outputType, const com_ptr<IMFMediaType>& inputType, DWORD& streamIndex) noexcept{
    if(!m_writer || !outputType || !inputType){
        return E_INVALIDARG;
    }

    auto hr{m_writer->AddStream(outputType.get(), &streamIndex)};
    if(SUCCEEDED(hr)){
        hr = m_writer->SetInputMediaType(streamIndex, inputType.get(), nullptr);
    }
    return hr;
}

void MediaFoundationSinkWriter::beginWriting() const{
    checkHresult(m_writer->BeginWriting());
}

void MediaFoundationSinkWriter::finalize() const{
    checkHresult(m_writer->Finalize());
}

IMFSinkWriter* MediaFoundationSinkWriter::writer() const noexcept{
    return m_writer;
}
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

    throwHresult(E_INVALIDARG, L"EBML size is too large");
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
        throwHresult(E_INVALIDARG, L"Cannot encode a one-byte WebM Void padding region");
    }

    switch(totalBytes % 3){
    case 1:
        if(totalBytes < 4){
            throwHresult(E_INVALIDARG, L"Cannot encode requested WebM Void padding");
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
            throwHresult(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), L"Failed to create WebM output file");
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
            throwHresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while finalizing WebM output");
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
            throwHresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed to query WebM output position");
        }
        return static_cast<uint64_t>(pos);
    }

    void writeBytes(const vector<uint8_t>& bytes){
        if(bytes.empty()){
            return;
        }
        m_stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<streamsize>(bytes.size()));
        if(!m_stream){
            throwHresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while writing WebM output");
        }
    }

    void writeBytes(const uint8_t* data, size_t size){
        if(!data || size == 0){
            return;
        }
        m_stream.write(reinterpret_cast<const char*>(data), static_cast<streamsize>(size));
        if(!m_stream){
            throwHresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while writing WebM output");
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
            throwHresult(E_INVALIDARG, L"Reserved WebM SeekHead space was too small");
        }

        vector<uint8_t> replacement{seekHead};
        appendExactSizedVoidPadding(replacement, static_cast<size_t>(m_seekHeadReservedBytes - replacement.size()));

        const auto endOffset{currentOffset()};
        m_stream.seekp(static_cast<streamoff>(m_seekHeadOffset));
        if(!m_stream){
            throwHresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while seeking to rewrite WebM SeekHead");
        }

        writeBytes(replacement);

        m_stream.seekp(static_cast<streamoff>(endOffset));
        if(!m_stream){
            throwHresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while restoring WebM output position");
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

class MpegTsWriter final{
public:
    explicit MpegTsWriter(const wstring& outputPath, bool includeAudio):
        m_includeAudio{includeAudio},
        m_stream{filesystem::path{outputPath}, ios::binary | ios::trunc}{
        if(!m_stream){
            throwHresult(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), L"Failed to create MPEG-TS output file");
        }
        writeTables();
    }

    void writeVideoSample(const uint8_t* data, size_t size, int64_t pts100ns, optional<int64_t> dts100ns, bool keyframe){
        (void)keyframe;
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
            throwHresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while finalizing MPEG-TS output");
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
            throwHresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), L"Failed while writing MPEG-TS output");
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
        (void)keyframe;
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

VideoWriteStats processVideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, int64_t sourceDuration100ns, const function<void(const com_ptr<IMFSample>&, int64_t, int64_t, bool)>& writeSample, const function<void()>& onDroppedByCut, const function<void(double)>& progressCallback, const function<bool()>& shouldCancel){
    auto waitingForCleanPoint{false};
    int64_t lastInTime100ns{-1};
    VideoWriteStats stats{};

    if(progressCallback){
        progressCallback(0.0);
    }

    for(;;){
        if(shouldCancel && shouldCancel()){
            throwHresult(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
        }

        DWORD actualStream{};
        DWORD flags{};
        LONGLONG timestamp{};
        com_ptr<IMFSample> sample;
        const auto hr{reader->ReadSample(videoStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put())};
        if(FAILED(hr)){
            checkHresult(hr);
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
            if(onDroppedByCut){
                onDroppedByCut();
            }
            continue;
        }

        const auto isContainerSync{isContainerSyncSample(sample)};
        const auto isBitstreamRap{isTrueRandomAccessPointSample(sample, videoSubtype, nalLengthFieldSize, false)};
        const auto keyframe{isContainerSync && isBitstreamRap};
        if(waitingForCleanPoint){
            if(!keyframe){
                ++stats.droppedWaitingRapCount;
                continue;
            }
            waitingForCleanPoint = false;
        }

        const auto outTime100ns{inTime100ns - removedDurationBefore(effectiveCutRanges100ns, inTime100ns)};
        writeSample(sample, inTime100ns, outTime100ns, keyframe);
        ++stats.writtenSampleCount;

        if(progressCallback && sourceDuration100ns > 0 && (stats.readSampleCount % 24 == 0)){
            const auto pct{(100.0 * inTime100ns) / sourceDuration100ns};
            progressCallback(pct);
        }
    }

    if(progressCallback){
        progressCallback(100.0);
    }
    return stats;
}

VideoWriteStats writeMediaFoundationVideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, IMFSinkWriter* writer, DWORD writerVideoStreamIndex, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, int64_t sourceDuration100ns, const function<void(double)>& progressCallback, const function<bool()>& shouldCancel){
    auto markDiscontinuityOnNextWrittenSample{false};
    auto dtsPtsShift100ns{static_cast<int64_t>(0)};
    auto dtsPtsShiftInitialized{false};

    return processVideoSamplesForExport(
        reader,
        videoStreamIndex,
        effectiveCutRanges100ns,
        videoSubtype,
        nalLengthFieldSize,
        sourceDuration100ns,
        [&](const com_ptr<IMFSample>& sample, int64_t inTime100ns, int64_t outTimeRaw100ns, bool){
            auto outTime100ns{outTimeRaw100ns};

            UINT64 decodeTimestamp100ns{};
            auto hasDecodeTimestamp{false};
            if(SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &decodeTimestamp100ns))){
                hasDecodeTimestamp = true;
                const auto decodeTimeSigned{static_cast<int64_t>(decodeTimestamp100ns)};
                const auto removedAtPresentationTime{inTime100ns - outTimeRaw100ns};
                auto outDecodeTime100ns{decodeTimeSigned - removedAtPresentationTime};
                if(!dtsPtsShiftInitialized){
                    dtsPtsShift100ns = max<int64_t>(0, -outDecodeTime100ns);
                    dtsPtsShiftInitialized = true;
                }
                outTime100ns += dtsPtsShift100ns;
                outDecodeTime100ns += dtsPtsShift100ns;
                checkHresult(sample->SetUINT64(MFSampleExtension_DecodeTimestamp, static_cast<UINT64>(max<int64_t>(0, outDecodeTime100ns))));
            }

            if(!dtsPtsShiftInitialized && !hasDecodeTimestamp){
                dtsPtsShiftInitialized = true;
            }

            if(!hasDecodeTimestamp && dtsPtsShiftInitialized && dtsPtsShift100ns > 0){
                outTime100ns += dtsPtsShift100ns;
            }

            checkHresult(sample->SetSampleTime(outTime100ns));

            LONGLONG duration100ns{};
            if(SUCCEEDED(sample->GetSampleDuration(&duration100ns)) && duration100ns > 0){
                checkHresult(sample->SetSampleDuration(duration100ns));
            }

            if(markDiscontinuityOnNextWrittenSample){
                checkHresult(sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE));
                markDiscontinuityOnNextWrittenSample = false;
            }

            checkHresult(writer->WriteSample(writerVideoStreamIndex, sample.get()));
        },
        [&](){
            markDiscontinuityOnNextWrittenSample = true;
        },
        progressCallback,
        shouldCancel);
}

VideoWriteStats writeWebmVp9VideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const std::wstring& outputPath, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, int64_t sourceDuration100ns, int64_t outputDuration100ns, uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen, const function<void(double)>& progressCallback, const function<bool()>& shouldCancel){
    if(videoSubtype != MFVideoFormat_VP90){
        throwHresult(MF_E_INVALIDMEDIATYPE, L"WebM stream-copy export currently requires VP9 video");
    }

    WebmVp9Writer writer(outputPath, outputDuration100ns, width, height, fpsNum, fpsDen);

    auto stats{processVideoSamplesForExport(
        reader,
        videoStreamIndex,
        effectiveCutRanges100ns,
        videoSubtype,
        nalLengthFieldSize,
        sourceDuration100ns,
        [&](const com_ptr<IMFSample>& sample, int64_t, int64_t outTime100ns, bool keyframe){
            com_ptr<IMFMediaBuffer> contiguousBuffer;
            checkHresult(sample->ConvertToContiguousBuffer(contiguousBuffer.put()));

            BYTE* data{};
            DWORD maxLength{};
            DWORD currentLength{};
            checkHresult(contiguousBuffer->Lock(&data, &maxLength, &currentLength));
            writer.writeSample(data, currentLength, outTime100ns, keyframe);
            checkHresult(contiguousBuffer->Unlock());
        },
        {},
        progressCallback,
        shouldCancel)};

    writer.finalize();
    return stats;
}

VideoWriteStats writeMpegTsH264VideoSamplesForExport(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const std::wstring& outputPath, const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns, GUID videoSubtype, uint32_t nalLengthFieldSize, const com_ptr<IMFMediaType>& videoMediaType, int64_t sourceDuration100ns, const com_ptr<IMFSourceReader>& audioReader, DWORD audioStreamIndex, const com_ptr<IMFMediaType>& audioMediaType, bool keepAudio, const function<void(double)>& progressCallback, const function<void(double)>& audioProgressCallback, const function<bool()>& shouldCancel){
    if(!isH264VideoSubtype(videoSubtype)){
        throwHresult(MF_E_INVALIDMEDIATYPE, L"MPEG-TS stream-copy export currently requires H.264 video");
    }

    vector<uint8_t> h264ParameterSetsAnnexB;
    if(videoMediaType){
        UINT8* sequenceHeaderBytes{};
        UINT32 sequenceHeaderSize{};
        if(SUCCEEDED(videoMediaType->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &sequenceHeaderBytes, &sequenceHeaderSize)) && sequenceHeaderBytes && sequenceHeaderSize > 0){
            h264ParameterSetsAnnexB = extractH264ParameterSetsAnnexBForMpegTs(vector<uint8_t>{sequenceHeaderBytes, sequenceHeaderBytes + sequenceHeaderSize});
        }
        CoTaskMemFree(sequenceHeaderBytes);
    }

    constexpr auto invalidStream{(numeric_limits<DWORD>::max)()};
    auto includeAudio{keepAudio && audioReader && audioMediaType && audioStreamIndex != invalidStream};
    uint32_t audioSampleRate{};
    uint32_t audioChannels{};
    if(includeAudio){
        GUID audioSubtype{GUID_NULL};
        checkHresult(audioMediaType->GetGUID(MF_MT_SUBTYPE, &audioSubtype));
        if(audioSubtype != MFAudioFormat_AAC){
            throwHresult(MF_E_INVALIDMEDIATYPE, L"MPEG-TS audio passthrough currently requires AAC audio");
        }
        (void)audioMediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &audioSampleRate);
        (void)audioMediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &audioChannels);
        if(audioSampleRate == 0 || audioChannels == 0){
            throwHresult(MF_E_INVALIDMEDIATYPE, L"MPEG-TS audio passthrough requires AAC sample rate and channel metadata");
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
            throwHresult(MF_E_INVALIDMEDIATYPE, L"H.264 sample is neither Annex B nor a valid length-prefixed sample");
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
                throwHresult(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
            }

            DWORD actualStream{};
            DWORD flags{};
            LONGLONG timestamp{};
            com_ptr<IMFSample> sample;
            const auto hr{reader->ReadSample(videoStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put())};
            if(FAILED(hr)){
                checkHresult(hr);
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
            checkHresult(sample->ConvertToContiguousBuffer(contiguousBuffer.put()));

            BYTE* data{};
            DWORD maxLength{};
            DWORD currentLength{};
            checkHresult(contiguousBuffer->Lock(&data, &maxLength, &currentLength));
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
                throwHresult(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
            }

            DWORD actualStream{};
            DWORD flags{};
            LONGLONG timestamp{};
            com_ptr<IMFSample> sample;
            const auto hr{audioReader->ReadSample(audioStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put())};
            if(FAILED(hr)){
                checkHresult(hr);
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
            checkHresult(sample->ConvertToContiguousBuffer(contiguousBuffer.put()));

            BYTE* data{};
            DWORD maxLength{};
            DWORD currentLength{};
            checkHresult(contiguousBuffer->Lock(&data, &maxLength, &currentLength));

            vector<uint8_t> outputData;
            if(aacSampleLooksAdts(data, currentLength)){
                outputData.assign(data, data + currentLength);
            }else{
                outputData = addAdtsHeaderToAacFrame(data, currentLength, audioSampleRate, audioChannels);
                if(outputData.empty()){
                    contiguousBuffer->Unlock();
                    throwHresult(MF_E_INVALIDMEDIATYPE, L"MPEG-TS AAC audio sample cannot be written as ADTS");
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
            throwHresult(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
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

void writeVideoContainerForExport(VideoContainerExportKind exportKind, const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const com_ptr<IMFMediaType>& sourceVideoType, GUID videoSubtype, uint32_t nalLengthFieldSize, const VideoContainerExportRequest& request){
    uint32_t width{};
    uint32_t height{};
    uint32_t fpsNum{};
    uint32_t fpsDen{};
    (void)MFGetAttributeSize(sourceVideoType.get(), MF_MT_FRAME_SIZE, &width, &height);
    (void)MFGetAttributeRatio(sourceVideoType.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);

    constexpr auto invalidStream{(numeric_limits<DWORD>::max)()};
    auto hasAudioForExport{false};
    DWORD writerAudioStreamIndex{};
    uint32_t audioChannels{};
    uint32_t audioSampleRate{};
    com_ptr<IMFMediaType> audioPcmType;
    com_ptr<IMFMediaType> audioNativeType;
    GUID audioSubtype{GUID_NULL};
    DWORD audioStreamIndex{invalidStream};

    if(request.allowAudio && request.keepAudio){
        com_ptr<IMFSourceReader> audioProbeReader;
        checkHresult(MFCreateSourceReaderFromURL(request.sourcePath.c_str(), nullptr, audioProbeReader.put()));
        for(DWORD streamIndex{};; ++streamIndex){
            com_ptr<IMFMediaType> type;
            const auto hr{audioProbeReader->GetNativeMediaType(streamIndex, 0, type.put())};
            if(hr == MF_E_INVALIDSTREAMNUMBER){
                break;
            }
            checkHresult(hr);
            GUID major{GUID_NULL};
            checkHresult(type->GetGUID(MF_MT_MAJOR_TYPE, &major));
            if(major == MFMediaType_Audio){
                checkHresult(type->GetGUID(MF_MT_SUBTYPE, &audioSubtype));
                audioStreamIndex = streamIndex;
                audioNativeType = type;
                (void)type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &audioChannels);
                (void)type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &audioSampleRate);
                if(audioChannels == 0){ audioChannels = 2; }
                if(audioSampleRate == 0){ audioSampleRate = 48000; }
                audioPcmType = createPcmFloatAudioType(audioSampleRate, audioChannels);
                hasAudioForExport = true;
                break;
            }
        }
    }

    if(exportKind == VideoContainerExportKind::WebmVp9){
        if(request.keepAudio && hasAudioForExport){
            throwHresult(MF_E_INVALIDMEDIATYPE, L"WebM export currently keeps VP9 video only; disable audio first.");
        }
        (void)writeWebmVp9VideoSamplesForExport(reader, videoStreamIndex, request.temporaryOutputPath, request.effectiveCutRanges100ns, videoSubtype, nalLengthFieldSize, request.sourceDuration100ns, request.outputDuration100ns, width, height, fpsNum, fpsDen, request.onVideoProgress, request.shouldCancel);
        return;
    }

    if(exportKind == VideoContainerExportKind::MpegTs){
        struct TemporaryFileCleanup final{
            wstring path{};
            ~TemporaryFileCleanup(){
                if(!path.empty()){
                    std::error_code ec;
                    filesystem::remove(path, ec);
                }
            }
        } audioTempCleanup{};

        com_ptr<IMFSourceReader> audioReader;
        auto tsAudioStreamIndex{audioStreamIndex};
        auto tsAudioType{audioNativeType};
        auto audioWasRenderedToTemp{false};

        if(request.keepAudio && hasAudioForExport){
            const auto canPassThroughAudio{audioSubtype == MFAudioFormat_AAC && request.audioCrossfadeMs == 0 && request.audioVolumePct == 100};
            if(canPassThroughAudio){
                checkHresult(MFCreateSourceReaderFromURL(request.sourcePath.c_str(), nullptr, audioReader.put()));
                checkHresult(audioReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
                checkHresult(audioReader->SetStreamSelection(tsAudioStreamIndex, TRUE));
                checkHresult(audioReader->SetCurrentMediaType(tsAudioStreamIndex, nullptr, tsAudioType.get()));
            }else{
                const filesystem::path audioTempPath{request.temporaryOutputPath + L".audio.mp4"};
                audioTempCleanup.path = audioTempPath.wstring();

                {
                    MediaFoundationSinkWriter audioWriter{audioTempCleanup.path};
                    DWORD renderedAudioStreamIndex{};
                    const auto aacOutputType{createAacOutputType(audioSampleRate, audioChannels)};
                    checkHresult(audioWriter.addStream(aacOutputType, audioPcmType, renderedAudioStreamIndex));
                    audioWriter.beginWriting();

                    com_ptr<IMFSourceReader> pcmAudioReader;
                    checkHresult(MFCreateSourceReaderFromURL(request.sourcePath.c_str(), nullptr, pcmAudioReader.put()));
                    checkHresult(pcmAudioReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
                    checkHresult(pcmAudioReader->SetStreamSelection(audioStreamIndex, TRUE));
                    checkHresult(pcmAudioReader->SetCurrentMediaType(audioStreamIndex, nullptr, audioPcmType.get()));
                    const auto keepRanges100ns{invertCutRanges100ns(request.effectiveCutRanges100ns, request.sourceDuration100ns)};
                    writeMixedAudioForKeepRanges(pcmAudioReader, audioStreamIndex, keepRanges100ns, audioWriter.writer(), renderedAudioStreamIndex, audioChannels, audioSampleRate, request.audioCrossfadeMs, request.audioVolumePct / 100.0f, request.onAudioProgress, request.shouldCancel);
                    audioWriter.finalize();
                }

                checkHresult(MFCreateSourceReaderFromURL(audioTempCleanup.path.c_str(), nullptr, audioReader.put()));
                tsAudioStreamIndex = invalidStream;
                tsAudioType = nullptr;
                for(DWORD streamIndex{};; ++streamIndex){
                    com_ptr<IMFMediaType> type;
                    const auto hr{audioReader->GetNativeMediaType(streamIndex, 0, type.put())};
                    if(hr == MF_E_INVALIDSTREAMNUMBER){
                        break;
                    }
                    checkHresult(hr);
                    GUID major{GUID_NULL};
                    checkHresult(type->GetGUID(MF_MT_MAJOR_TYPE, &major));
                    if(major == MFMediaType_Audio){
                        tsAudioStreamIndex = streamIndex;
                        tsAudioType = type;
                        break;
                    }
                }
                if(tsAudioStreamIndex == invalidStream || !tsAudioType){
                    throwHresult(MF_E_INVALIDMEDIATYPE, L"Could not read re-encoded MPEG-TS audio");
                }
                checkHresult(audioReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
                checkHresult(audioReader->SetStreamSelection(tsAudioStreamIndex, TRUE));
                checkHresult(audioReader->SetCurrentMediaType(tsAudioStreamIndex, nullptr, tsAudioType.get()));
                audioWasRenderedToTemp = true;
            }
        }

        (void)writeMpegTsH264VideoSamplesForExport(reader, videoStreamIndex, request.temporaryOutputPath, request.effectiveCutRanges100ns, videoSubtype, nalLengthFieldSize, sourceVideoType, request.sourceDuration100ns, audioReader, tsAudioStreamIndex, tsAudioType, request.keepAudio && hasAudioForExport, request.onVideoProgress, audioWasRenderedToTemp ? function<void(double)>{} : request.onAudioProgress, request.shouldCancel);
        return;
    }

    if(exportKind == VideoContainerExportKind::Wmv && request.keepAudio && hasAudioForExport){
        throwHresult(MF_E_INVALIDMEDIATYPE, L"WMV export currently keeps VC-1 video only; disable audio first.");
    }

    MediaFoundationSinkWriter writer{request.temporaryOutputPath};
    DWORD writerVideoStreamIndex{};
    auto configureWriter = [&]() -> HRESULT{
        writerVideoStreamIndex = 0;
        writerAudioStreamIndex = 0;
        checkHresult(writer.addStream(sourceVideoType, sourceVideoType, writerVideoStreamIndex));
        if(!hasAudioForExport){
            return S_OK;
        }
        const auto aacType{createAacOutputType(audioSampleRate, audioChannels)};
        return writer.addStream(aacType, audioPcmType, writerAudioStreamIndex);
    };

    const auto writerConfigHr{configureWriter()};
    if(FAILED(writerConfigHr)){
        if(exportKind == VideoContainerExportKind::MediaFoundation){
            throwHresult(writerConfigHr, L"System cannot mux H.264 into MP4 via Media Foundation.");
        }
        if(exportKind == VideoContainerExportKind::Wmv){
            throwHresult(writerConfigHr, L"System cannot mux VC-1 into WMV via Media Foundation on this machine.");
        }
        checkHresult(writerConfigHr);
    }

    writer.beginWriting();
    (void)writeMediaFoundationVideoSamplesForExport(reader, videoStreamIndex, writer.writer(), writerVideoStreamIndex, request.effectiveCutRanges100ns, videoSubtype, nalLengthFieldSize, request.sourceDuration100ns, request.onVideoProgress, request.shouldCancel);

    if(hasAudioForExport && audioStreamIndex != invalidStream){
        com_ptr<IMFSourceReader> audioReader;
        checkHresult(MFCreateSourceReaderFromURL(request.sourcePath.c_str(), nullptr, audioReader.put()));
        checkHresult(audioReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
        checkHresult(audioReader->SetStreamSelection(audioStreamIndex, TRUE));
        checkHresult(audioReader->SetCurrentMediaType(audioStreamIndex, nullptr, audioPcmType.get()));
        const auto keepRanges100ns{invertCutRanges100ns(request.effectiveCutRanges100ns, request.sourceDuration100ns)};
        writeMixedAudioForKeepRanges(audioReader, audioStreamIndex, keepRanges100ns, writer.writer(), writerAudioStreamIndex, audioChannels, audioSampleRate, request.audioCrossfadeMs, request.audioVolumePct / 100.0f, request.onAudioProgress, request.shouldCancel);
    }

    if(request.shouldCancel && request.shouldCancel()){
        throwHresult(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
    }
    writer.finalize();
}

}
