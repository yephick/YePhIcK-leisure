module;

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

export module llvc.Media;

import std;
import llvc.Export;
import llvc.Utils;

export namespace llvc{

using namespace ::std;
using namespace ::winrt;

enum class CapabilityState : uint8_t{
    Unknown,
    Supported,
    Unsupported,
    NotApplicable
};

class VideoSource{
public:
    struct Ratio final{
        uint32_t num{};
        uint32_t den{};
        constexpr operator double() const noexcept{ return den != 0 ? 1.0 * num / den : 0; }
    };

    struct InspectionResult final{
        bool isValid{false};
        wstring errorMessage{};
        wstring container{};
        wstring videoCodec{};
        wstring audioCodec{};
        wstring duration{};
        wstring fileSize{};
        wstring sourceCreated{};
        wstring sourceModified{};
        wstring sourceEncodedBy{};
        wstring sourceComment{};
        wstring resolution{};
        Ratio frameRate{};
        wstring videoBitrate{};
        wstring audioBitrate{};
        wstring keyFrameSummary{};
        wstring keyFrameInterval{};
        wstring allSamplesIndependent{};
        wstring maxKeyFrameSpacing{};
        CapabilityState openSupport{CapabilityState::Unsupported};
        CapabilityState previewSupport{CapabilityState::Unsupported};
        CapabilityState losslessExportSupport{CapabilityState::Unsupported};
        CapabilityState audioExportSupport{CapabilityState::NotApplicable};
        vector<wstring> supportedExportExtensions{};
        wstring defaultExportExtension{};
        bool audioDisabledForThisSource{false};
        wstring audioDisabledReason{};
    };

    struct ExportRequest final{
        wstring temporaryOutputPath{};
        int64_t sourceDuration100ns{};
        int64_t outputDuration100ns{};
        vector<pair<int64_t, int64_t>> effectiveCutRanges100ns{};
        bool keepAudio{};
        int32_t audioCrossfadeMs{};
        int32_t audioVolumePct{100};
        function<void(double)> onVideoProgress{};
        function<void(double)> onAudioProgress{};
        function<bool()> shouldCancel{};
    };

    explicit VideoSource(wstring sourcePath): m_sourcePath(std::move(sourcePath)) {}
    virtual ~VideoSource() = default;

    virtual InspectionResult inspect() const = 0;
    virtual vector<int64_t> collectRapTimes100ns(const vector<int64_t>& markerTimes100ns = {}, const function<void(double)>& progressCallback = {}, const function<bool()>& cancelRequested = {}) const = 0;
    virtual void exportLossless(const ExportRequest& request) const = 0;

    const wstring& sourcePath() const noexcept{ return m_sourcePath; }

protected:
    wstring m_sourcePath;
};

unique_ptr<VideoSource> createVideoSource(const wstring& sourcePath);

}

namespace llvc{

using namespace ::std;
using namespace ::winrt;

namespace{

constexpr int64_t HNS_PER_SECOND{10'000'000LL};

constexpr uint32_t makeFourCc(char a, char b, char c, char d){
    return static_cast<uint32_t>(static_cast<uint8_t>(a))
        | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8)
        | (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16)
        | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

constexpr GUID makeMfVideoSubtype(uint32_t fourcc){
    return GUID{
        fourcc,
        0x0000,
        0x0010,
        {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
}

constexpr GUID VC1_VIDEO_SUBTYPE{makeMfVideoSubtype(makeFourCc('W', 'V', 'C', '1'))};

enum class SourceFormatId : uint8_t{
    Unknown,
    Mp4,
    Mov,
    Avi,
    Webm,
    Wmv
};

enum class AudioExportPolicy : uint8_t{
    Disabled,
    Allowed
};

enum class ExportProbeKind : uint8_t{
    None,
    MediaFoundationSinkWriter,
    CustomWriter
};

bool hasPathExtension(const wstring& filePath, const wchar_t* extension){
    if(!extension){
        return false;
    }

    const wstring expected{extension};
    if(filePath.size() < expected.size()){
        return false;
    }

    const auto ext{filePath.substr(filePath.size() - expected.size())};
    return _wcsicmp(ext.c_str(), expected.c_str()) == 0;
}

struct FormatProfile final{
    SourceFormatId id{SourceFormatId::Unknown};
    const wchar_t* extension{};
    span<const GUID> allowedVideoSubtypes{};
    array<const wchar_t*, 2> candidateExportExtensions{};
    size_t candidateExportExtensionCount{};
    AudioExportPolicy audioExportPolicy{AudioExportPolicy::Disabled};
    ExportProbeKind exportProbeKind{ExportProbeKind::None};
    bool requiresAviValidation{false};
};

const array<GUID, 3> MP4_MOV_ALLOWED_VIDEO_SUBTYPES{MFVideoFormat_H264, MFVideoFormat_HEVC, MFVideoFormat_H265};
const array<GUID, 1> AVI_ALLOWED_VIDEO_SUBTYPES{MFVideoFormat_H264};
const array<GUID, 1> WEBM_ALLOWED_VIDEO_SUBTYPES{MFVideoFormat_VP90};
const array<GUID, 1> WMV_ALLOWED_VIDEO_SUBTYPES{VC1_VIDEO_SUBTYPE};

const array<FormatProfile, 5>& supportedFormatProfiles(){
    static const array<FormatProfile, 5> profiles{{
        {.id = SourceFormatId::Mp4, .extension = L".mp4", .allowedVideoSubtypes = MP4_MOV_ALLOWED_VIDEO_SUBTYPES, .candidateExportExtensions = {L".mp4", L".mov"}, .candidateExportExtensionCount = 2, .audioExportPolicy = AudioExportPolicy::Allowed, .exportProbeKind = ExportProbeKind::MediaFoundationSinkWriter},
        {.id = SourceFormatId::Mov, .extension = L".mov", .allowedVideoSubtypes = MP4_MOV_ALLOWED_VIDEO_SUBTYPES, .candidateExportExtensions = {L".mp4", L".mov"}, .candidateExportExtensionCount = 2, .audioExportPolicy = AudioExportPolicy::Allowed, .exportProbeKind = ExportProbeKind::MediaFoundationSinkWriter},
        {.id = SourceFormatId::Avi, .extension = L".avi", .allowedVideoSubtypes = AVI_ALLOWED_VIDEO_SUBTYPES, .candidateExportExtensions = {L".mp4", nullptr}, .candidateExportExtensionCount = 1, .audioExportPolicy = AudioExportPolicy::Disabled, .exportProbeKind = ExportProbeKind::MediaFoundationSinkWriter, .requiresAviValidation = true},
        {.id = SourceFormatId::Webm, .extension = L".webm", .allowedVideoSubtypes = WEBM_ALLOWED_VIDEO_SUBTYPES, .candidateExportExtensions = {L".webm", nullptr}, .candidateExportExtensionCount = 1, .audioExportPolicy = AudioExportPolicy::Disabled, .exportProbeKind = ExportProbeKind::CustomWriter},
        {.id = SourceFormatId::Wmv, .extension = L".wmv", .allowedVideoSubtypes = WMV_ALLOWED_VIDEO_SUBTYPES, .candidateExportExtensions = {L".wmv", nullptr}, .candidateExportExtensionCount = 1, .audioExportPolicy = AudioExportPolicy::Disabled, .exportProbeKind = ExportProbeKind::MediaFoundationSinkWriter},
    }};
    return profiles;
}

const FormatProfile* tryGetFormatProfileForPath(const wstring& filePath){
    for(const auto& profile: supportedFormatProfiles()){
        if(hasPathExtension(filePath, profile.extension)){
            return &profile;
        }
    }
    return nullptr;
}

bool formatAllowsVideoSubtype(const FormatProfile& profile, const GUID& subtype){
    return ranges::find(profile.allowedVideoSubtypes, subtype) != profile.allowedVideoSubtypes.end();
}

vector<GUID> getAllowedVideoSubtypeVector(const FormatProfile& profile){
    return vector<GUID>(profile.allowedVideoSubtypes.begin(), profile.allowedVideoSubtypes.end());
}

vector<wstring> getCandidateExportExtensions(const FormatProfile& profile){
    vector<wstring> extensions;
    extensions.reserve(profile.candidateExportExtensionCount);
    for(size_t i{}; i < profile.candidateExportExtensionCount; ++i){
        if(profile.candidateExportExtensions[i] && *profile.candidateExportExtensions[i]){
            extensions.emplace_back(profile.candidateExportExtensions[i]);
        }
    }
    return extensions;
}

wstring chooseDefaultExportExtension(const FormatProfile& profile, const wstring& sourcePath, const vector<wstring>& supportedExtensions){
    if(supportedExtensions.empty()){
        return {};
    }

    const auto sourceExtension{filesystem::path{sourcePath}.extension().wstring()};
    for(const auto& extension: supportedExtensions){
        if(_wcsicmp(extension.c_str(), sourceExtension.c_str()) == 0){
            return extension;
        }
    }

    if(profile.id == SourceFormatId::Mov){
        for(const auto& extension: supportedExtensions){
            if(_wcsicmp(extension.c_str(), L".mov") == 0){
                return extension;
            }
        }
    }

    return supportedExtensions.front();
}

wstring buildTemporaryProbeOutputPath(const wchar_t* extension){
    if(!extension || !*extension){
        return {};
    }

    wchar_t tempDirectory[MAX_PATH + 1]{};
    const auto tempDirectoryLen{GetTempPathW(MAX_PATH, tempDirectory)};
    if(tempDirectoryLen == 0 || tempDirectoryLen > MAX_PATH){
        return {};
    }

    wchar_t tempFilePath[MAX_PATH + 1]{};
    if(GetTempFileNameW(tempDirectory, L"llv", 0, tempFilePath) == 0){
        return {};
    }

    filesystem::path probePath{tempFilePath};
    std::error_code ec;
    filesystem::remove(probePath, ec);
    probePath.replace_extension(extension);
    return probePath.wstring();
}

bool probeMediaFoundationVideoSinkSupport(const com_ptr<IMFMediaType>& sourceVideoType, const wchar_t* outputExtension){
    if(!sourceVideoType || !outputExtension || !*outputExtension){
        return false;
    }

    const auto probeOutputPath{buildTemporaryProbeOutputPath(outputExtension)};
    if(probeOutputPath.empty()){
        return false;
    }

    const auto cleanupProbeOutput = [&](){
        std::error_code ec;
        filesystem::remove(probeOutputPath, ec);
    };

    try{
        com_ptr<IMFAttributes> writerAttributes;
        check_hresult(MFCreateAttributes(writerAttributes.put(), 1));
        check_hresult(writerAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));

        com_ptr<IMFSinkWriter> writer;
        check_hresult(MFCreateSinkWriterFromURL(probeOutputPath.c_str(), nullptr, writerAttributes.get(), writer.put()));

        DWORD writerVideoStreamIndex{};
        check_hresult(writer->AddStream(sourceVideoType.get(), &writerVideoStreamIndex));
        check_hresult(writer->SetInputMediaType(writerVideoStreamIndex, sourceVideoType.get(), nullptr));
        cleanupProbeOutput();
        return true;
    }catch(...){
        cleanupProbeOutput();
        return false;
    }
}

vector<wstring> probeSupportedExportExtensions(const FormatProfile& profile, const com_ptr<IMFMediaType>& sourceVideoType){
    const auto candidateExtensions{getCandidateExportExtensions(profile)};
    if(candidateExtensions.empty()){
        return {};
    }

    if(profile.exportProbeKind == ExportProbeKind::CustomWriter){
        return candidateExtensions;
    }

    vector<wstring> supportedExtensions;
    for(const auto& extension: candidateExtensions){
        if(probeMediaFoundationVideoSinkSupport(sourceVideoType, extension.c_str())){
            supportedExtensions.push_back(extension);
        }
    }
    return supportedExtensions;
}

wstring guidToVideoCodecName(const GUID& subtype){
    if(subtype == MFVideoFormat_H264){ return L"H.264"; }
    if(subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265){ return L"HEVC"; }
    if(subtype == MFVideoFormat_VP90){ return L"VP9"; }
    if(subtype == VC1_VIDEO_SUBTYPE){ return L"VC-1"; }
    return L"unknown codec";
}

wstring guidToCodecName(const GUID& subtype, bool isVideo){
    if(isVideo){
        if(subtype == MFVideoFormat_H264){
            return L"H.264";
        }
        if(subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265){
            return L"HEVC";
        }
        if(subtype == MFVideoFormat_VP90){
            return L"VP9";
        }
        if(subtype == VC1_VIDEO_SUBTYPE){
            return L"VC-1";
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

wstring BuildUnsupportedAviReason(const wstring& detail){
    return detail;
}

bool IsAviH264StreamCopyCandidate(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, com_ptr<IMFMediaType>& selectedVideoType, wstring& failureReason){
    if(!reader){
        failureReason = BuildUnsupportedAviReason(L"AVI is supported only for H.264 video in v1. This file uses unknown codec.");
        return false;
    }

    com_ptr<IMFMediaType> firstVideoType;
    GUID firstVideoSubtype{GUID_NULL};
    auto hasH264NativeType{false};
    for(DWORD mediaTypeIndex{};; ++mediaTypeIndex){
        com_ptr<IMFMediaType> type;
        const auto hr{reader->GetNativeMediaType(videoStreamIndex, mediaTypeIndex, type.put())};
        if(hr == MF_E_NO_MORE_TYPES){
            break;
        }
        if(FAILED(hr)){
            failureReason = BuildUnsupportedAviReason(L"AVI is supported only for H.264 video in v1. This file could not be inspected.");
            return false;
        }

        GUID major{GUID_NULL};
        GUID subtype{GUID_NULL};
        if(FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video){
            continue;
        }
        if(FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))){
            continue;
        }

        if(!firstVideoType){
            firstVideoType = type;
            firstVideoSubtype = subtype;
        }

        if(subtype == MFVideoFormat_H264){
            hasH264NativeType = true;
            selectedVideoType = type;
            break;
        }
    }

    if(!hasH264NativeType){
        const auto codec{firstVideoType ? guidToVideoCodecName(firstVideoSubtype) : wstring(L"unknown codec")};
        failureReason = BuildUnsupportedAviReason(L"AVI is supported only for H.264 video. This file uses " + codec);
        return false;
    }

    return true;
}

bool validateAviSampleTimesAndSyncFlags(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, const com_ptr<IMFMediaType>& videoType, wstring& failureReason){
    if(!reader){
        failureReason = BuildUnsupportedAviReason(L"AVI file lacks usable timestamps for robust cutting; convert to MP4 first");
        return false;
    }

    uint32_t fpsNum{};
    uint32_t fpsDen{};
    const auto hasFrameRate{videoType && SUCCEEDED(MFGetAttributeRatio(videoType.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen)) && fpsNum > 0 && fpsDen > 0};
    const auto saneFrameRate{hasFrameRate && fpsNum <= 240 * fpsDen};
    const auto fallbackDuration100ns{saneFrameRate ? static_cast<int64_t>((HNS_PER_SECOND * static_cast<int64_t>(fpsDen) + (fpsNum / 2)) / fpsNum) : 0LL};

    constexpr auto maxForwardJump100ns{10LL * HNS_PER_SECOND};
    int64_t previousTime100ns{-1};
    int64_t syntheticTime100ns{};
    bool hasAnySample{};
    bool hasAnyCleanPoint{};

    PROPVARIANT startPos{};
    startPos.vt = VT_I8;
    startPos.hVal.QuadPart = 0;
    check_hresult(reader->SetCurrentPosition(GUID_NULL, startPos));
    PropVariantClear(&startPos);

    for(;;){
        DWORD actualStream{};
        DWORD flags{};
        LONGLONG timestamp{};
        com_ptr<IMFSample> sample;
        const auto hr{reader->ReadSample(videoStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put())};
        if(FAILED(hr)){
            failureReason = BuildUnsupportedAviReason(L"AVI file lacks usable timestamps for robust cutting; convert to MP4 first");
            return false;
        }
        if(flags & MF_SOURCE_READERF_ENDOFSTREAM){
            break;
        }
        if(!sample){
            continue;
        }

        hasAnySample = true;
        int64_t sampleTime100ns{};
        if(FAILED(sample->GetSampleTime(&sampleTime100ns))){
            if(!saneFrameRate || fallbackDuration100ns <= 0){
                failureReason = BuildUnsupportedAviReason(L"AVI file lacks usable timestamps for robust cutting; convert to MP4 first");
                return false;
            }
            sampleTime100ns = syntheticTime100ns;
            syntheticTime100ns += fallbackDuration100ns;
        }else if(sampleTime100ns < 0){
            failureReason = BuildUnsupportedAviReason(L"AVI file lacks usable timestamps for robust cutting; convert to MP4 first");
            return false;
        }

        if(previousTime100ns >= 0){
            if(sampleTime100ns < previousTime100ns || sampleTime100ns - previousTime100ns > maxForwardJump100ns){
                failureReason = BuildUnsupportedAviReason(L"AVI file lacks usable timestamps for robust cutting; convert to MP4 first");
                return false;
            }
        }

        previousTime100ns = sampleTime100ns;
        UINT32 cleanPoint{};
        if(SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0){
            hasAnyCleanPoint = true;
        }
    }

    if(!hasAnySample || !hasAnyCleanPoint){
        failureReason = BuildUnsupportedAviReason(L"AVI keyframe markers are not reliable for robust cutting; convert to MP4 first");
        return false;
    }

    startPos.vt = VT_I8;
    startPos.hVal.QuadPart = 0;
    check_hresult(reader->SetCurrentPosition(GUID_NULL, startPos));
    PropVariantClear(&startPos);
    return true;
}

vector<int64_t> collectCleanPointTimes100ns(const wstring& filePath, const vector<int64_t>& markerTimes100ns = {}, const function<void(double)>& progressCallback = {}, const function<bool()>& cancelRequested = {}){
    vector<int64_t> rapTimes;
    if(filePath.empty()){
        return rapTimes;
    }

    com_ptr<IMFSourceReader> reader;
    check_hresult(MFCreateSourceReaderFromURL(filePath.c_str(), nullptr, reader.put()));

    DWORD videoStreamIndex{};
    bool foundVideo{};
    for(DWORD streamIndex{};; ++streamIndex){
        com_ptr<IMFMediaType> nativeType;
        const auto hr{reader->GetNativeMediaType(streamIndex, 0, nativeType.put())};
        if(hr == MF_E_NO_MORE_TYPES){
            break;
        }
        if(FAILED(hr) || !nativeType){
            continue;
        }

        GUID majorType{};
        if(SUCCEEDED(nativeType->GetGUID(MF_MT_MAJOR_TYPE, &majorType)) && majorType == MFMediaType_Video){
            videoStreamIndex = streamIndex;
            foundVideo = true;
            break;
        }
    }

    if(!foundVideo){
        return rapTimes;
    }

    check_hresult(reader->SetStreamSelection(videoStreamIndex, TRUE));
    size_t nextMarkerIndex{};

    for(;;){
        if(cancelRequested && cancelRequested()){
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
        }

        DWORD actualStream{};
        DWORD flags{};
        LONGLONG timestamp{};
        com_ptr<IMFSample> sample;
        check_hresult(reader->ReadSample(videoStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put()));

        if(flags & MF_SOURCE_READERF_ENDOFSTREAM){
            break;
        }
        if(!sample){
            continue;
        }

        UINT32 cleanPoint{};
        if(FAILED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) || cleanPoint == 0){
            continue;
        }

        LONGLONG sampleTime{};
        if(FAILED(sample->GetSampleTime(&sampleTime))){
            sampleTime = timestamp;
        }

        const auto cleanTime100ns{(std::max<int64_t>)(0, sampleTime)};
        rapTimes.push_back(cleanTime100ns);
        while(nextMarkerIndex < markerTimes100ns.size() && markerTimes100ns[nextMarkerIndex] <= cleanTime100ns){
            ++nextMarkerIndex;
            if(progressCallback){
                progressCallback((100.0 * static_cast<double>(nextMarkerIndex)) / static_cast<double>(markerTimes100ns.size()));
            }
        }
    }

    sort(rapTimes.begin(), rapTimes.end());
    rapTimes.erase(unique(rapTimes.begin(), rapTimes.end()), rapTimes.end());
    return rapTimes;
}

class ProfileMedia : public VideoSource{
public:
    ProfileMedia(wstring sourcePath, const FormatProfile& profile): VideoSource(std::move(sourcePath)), m_profile(profile) {}

    InspectionResult inspect() const override;
    vector<int64_t> collectRapTimes100ns(const vector<int64_t>& markerTimes100ns = {}, const function<void(double)>& progressCallback = {}, const function<bool()>& cancelRequested = {}) const override;
    void exportLossless(const ExportRequest& request) const override;

protected:
    virtual wstring containerName() const = 0;
    virtual void describeAudioSupport(bool hasAudio, VideoSource::InspectionResult& result) const;
    virtual void validateExportVideoType(const com_ptr<IMFMediaType>& sourceVideoType, const GUID& videoSubtype) const;
    virtual com_ptr<IMFMediaType> selectExportVideoType(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, wstring& failureReason) const;

    const FormatProfile& m_profile;
};

void ProfileMedia::describeAudioSupport(bool, VideoSource::InspectionResult&) const{}

com_ptr<IMFMediaType> ProfileMedia::selectExportVideoType(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, wstring& failureReason) const{
    if(!reader || videoStreamIndex == (std::numeric_limits<DWORD>::max)()){
        failureReason = L"No video stream found";
        return nullptr;
    }

    if(m_profile.requiresAviValidation){
        com_ptr<IMFMediaType> aviNativeH264Type;
        if(!IsAviH264StreamCopyCandidate(reader, videoStreamIndex, aviNativeH264Type, failureReason)){
            return nullptr;
        }
        if(!validateAviSampleTimesAndSyncFlags(reader, videoStreamIndex, aviNativeH264Type, failureReason)){
            return nullptr;
        }
        return aviNativeH264Type;
    }

    auto selectedVideoType{chooseBestNativeVideoMediaTypeForSubtypes(reader, videoStreamIndex, getAllowedVideoSubtypeVector(m_profile))};
    if(selectedVideoType){
        return selectedVideoType;
    }

    switch(m_profile.id){
    case SourceFormatId::Mp4:
    case SourceFormatId::Mov:
        failureReason = L"No stream-copy video media type found. Require H.264 or HEVC in MP4/MOV.";
        break;
    case SourceFormatId::Webm:
        failureReason = L"No stream-copy video media type found. Require VP9 in WebM.";
        break;
    case SourceFormatId::Wmv:
        failureReason = L"No stream-copy video media type found. Require VC-1 in WMV.";
        break;
    default:
        failureReason = L"No stream-copy video media type found.";
        break;
    }
    return nullptr;
}

void ProfileMedia::validateExportVideoType(const com_ptr<IMFMediaType>& sourceVideoType, const GUID& videoSubtype) const{
    if(m_profile.requiresAviValidation){
        if(videoSubtype != MFVideoFormat_H264){
            throw hresult_error(MF_E_INVALIDMEDIATYPE, L"AVI is supported only for H.264 video");
        }
        UINT32 sequenceHeaderSize{};
        const auto seqSizeHr{sourceVideoType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sequenceHeaderSize)};
        vector<uint8_t> sequenceHeader(sequenceHeaderSize);
        UINT32 bytesWritten{};
        const auto seqReadHr{SUCCEEDED(seqSizeHr) && sequenceHeaderSize > 0 ? sourceVideoType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader.data(), sequenceHeaderSize, &bytesWritten) : E_FAIL};
        if(FAILED(seqReadHr) || bytesWritten == 0){
            throw hresult_error(MF_E_INVALIDMEDIATYPE, L"AVI H.264 stream lacks codec configuration (SPS/PPS). Convert to MP4 first.");
        }
        return;
    }

    if(m_profile.id == SourceFormatId::Webm && videoSubtype != MFVideoFormat_VP90){
        throw hresult_error(MF_E_INVALIDMEDIATYPE, L"WebM stream-copy export currently requires VP9 video");
    }
    if(m_profile.id == SourceFormatId::Wmv && videoSubtype != VC1_VIDEO_SUBTYPE){
        throw hresult_error(MF_E_INVALIDMEDIATYPE, L"WMV stream-copy export currently requires VC-1 video");
    }
}

VideoSource::InspectionResult ProfileMedia::inspect() const{
    VideoSource::InspectionResult result{.keyFrameSummary = L"unknown", .keyFrameInterval = L"unknown", .allSamplesIndependent = L"unknown", .maxKeyFrameSpacing = L"unknown"};
    MFLifetime mf{};

    com_ptr<IMFSourceReader> reader;
    check_hresult(MFCreateSourceReaderFromURL(m_sourcePath.c_str(), nullptr, reader.put()));

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
    constexpr auto invalidStreamIndex{(std::numeric_limits<DWORD>::max)()};
    auto videoStreamIndex{invalidStreamIndex};
    com_ptr<IMFMediaType> selectedVideoType;

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
        }else if(major == MFMediaType_Audio){
            ++audioCount;
            audioSubtype = subtype;
            (void)type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &audioBitrate);
        }else{
            hasText = true;
        }
    }

    selectedVideoType = selectExportVideoType(reader, videoStreamIndex, result.errorMessage);
    if(!result.errorMessage.empty()){
        return result;
    }
    if(videoCount != 1){
        result.errorMessage = L"Expected exactly one video stream";
        return result;
    }
    if(audioCount > 1){
        result.errorMessage = L"Multiple audio streams are not supported";
        return result;
    }
    if(hasText){
        result.errorMessage = L"Subtitle/text streams are not supported";
        return result;
    }
    if(!selectedVideoType){
        result.errorMessage = L"No stream-copy video media type found.";
        return result;
    }

    check_hresult(selectedVideoType->GetGUID(MF_MT_SUBTYPE, &videoSubtype));
    if(!formatAllowsVideoSubtype(m_profile, videoSubtype)){
        result.errorMessage = L"Video codec not supported for this container";
        return result;
    }

    result.container = containerName();
    if(videoSubtype == MFVideoFormat_HEVC || videoSubtype == MFVideoFormat_H265){
        if(!hasDecoderForSubtype(videoSubtype)){
            result.errorMessage = L"HEVC support missing (install HEVC Video Extensions)";
            return result;
        }
    }else if(!hasDecoderForSubtype(videoSubtype)){
        result.errorMessage = L"No decoder available";
        return result;
    }

    MFGetAttributeSize(selectedVideoType.get(), MF_MT_FRAME_SIZE, &width, &height);
    MFGetAttributeRatio(selectedVideoType.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
    (void)selectedVideoType->GetUINT32(MF_MT_AVG_BITRATE, &videoBitrate);

    PROPVARIANT duration{};
    PropVariantInit(&duration);
    if(SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) && duration.vt == VT_UI8){
        const auto seconds{duration.uhVal.QuadPart / 10'000'000.0};
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
        PROPVARIANT startPos{};
        startPos.vt = VT_I8;
        startPos.hVal.QuadPart = 0;
        check_hresult(reader->SetCurrentPosition(GUID_NULL, startPos));
        PropVariantClear(&startPos);
        const auto cadence{analyzeKeyFrameCadence(reader.get(), videoStreamIndex, fpsNum, fpsDen)};
        result.keyFrameSummary = cadence.summary;
        result.keyFrameInterval = cadence.interval;
    }

    result.openSupport = CapabilityState::Supported;
    result.previewSupport = CapabilityState::Supported;
    result.supportedExportExtensions = probeSupportedExportExtensions(m_profile, selectedVideoType);
    result.defaultExportExtension = chooseDefaultExportExtension(m_profile, m_sourcePath, result.supportedExportExtensions);
    result.losslessExportSupport = result.supportedExportExtensions.empty() ? CapabilityState::Unsupported : CapabilityState::Supported;
    result.audioExportSupport = audioCount == 0 ? CapabilityState::NotApplicable : (m_profile.audioExportPolicy == AudioExportPolicy::Allowed ? CapabilityState::Supported : CapabilityState::Unsupported);
    describeAudioSupport(audioCount > 0, result);
    result.isValid = true;
    return result;
}

vector<int64_t> ProfileMedia::collectRapTimes100ns(const vector<int64_t>& markerTimes100ns, const function<void(double)>& progressCallback, const function<bool()>& cancelRequested) const{
    MFLifetime mf{};
    return collectCleanPointTimes100ns(m_sourcePath, markerTimes100ns, progressCallback, cancelRequested);
}

void ProfileMedia::exportLossless(const ExportRequest& request) const{
    MFLifetime mf{};

    com_ptr<IMFAttributes> readerAttributes;
    check_hresult(MFCreateAttributes(readerAttributes.put(), 1));
    check_hresult(readerAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE));

    com_ptr<IMFSourceReader> reader;
    check_hresult(MFCreateSourceReaderFromURL(m_sourcePath.c_str(), readerAttributes.get(), reader.put()));

    constexpr auto invalidStream{(std::numeric_limits<DWORD>::max)()};
    auto videoStreamIndex{invalidStream};
    for(DWORD streamIndex{0};; ++streamIndex){
        com_ptr<IMFMediaType> type;
        const auto hr{reader->GetNativeMediaType(streamIndex, 0, type.put())};
        if(hr == MF_E_INVALIDSTREAMNUMBER){
            break;
        }
        check_hresult(hr);

        GUID major{GUID_NULL};
        check_hresult(type->GetGUID(MF_MT_MAJOR_TYPE, &major));
        if(major == MFMediaType_Video){
            videoStreamIndex = streamIndex;
            break;
        }
    }
    if(videoStreamIndex == invalidStream){
        throw hresult_error(MF_E_INVALIDMEDIATYPE, L"No video stream found");
    }

    check_hresult(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
    check_hresult(reader->SetStreamSelection(videoStreamIndex, TRUE));

    wstring failureReason;
    auto sourceVideoType{selectExportVideoType(reader, videoStreamIndex, failureReason)};
    if(!sourceVideoType){
        throw hresult_error(MF_E_INVALIDMEDIATYPE, failureReason.empty() ? L"No stream-copy video media type found" : failureReason);
    }
    check_hresult(reader->SetCurrentMediaType(videoStreamIndex, nullptr, sourceVideoType.get()));

    GUID videoSubtype{GUID_NULL};
    check_hresult(sourceVideoType->GetGUID(MF_MT_SUBTYPE, &videoSubtype));
    const auto nalLengthFieldSize{getNalLengthFieldSize(sourceVideoType, videoSubtype)};
    validateExportVideoType(sourceVideoType, videoSubtype);

    uint32_t width{};
    uint32_t height{};
    uint32_t fpsNum{};
    uint32_t fpsDen{};
    (void)MFGetAttributeSize(sourceVideoType.get(), MF_MT_FRAME_SIZE, &width, &height);
    (void)MFGetAttributeRatio(sourceVideoType.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);

    auto hasAudioForExport{false};
    DWORD writerAudioStreamIndex{};
    uint32_t audioChannels{};
    uint32_t audioSampleRate{};
    com_ptr<IMFMediaType> audioPcmType;
    DWORD audioStreamIndex{invalidStream};

    if(m_profile.audioExportPolicy == AudioExportPolicy::Allowed && request.keepAudio){
        com_ptr<IMFSourceReader> audioProbeReader;
        check_hresult(MFCreateSourceReaderFromURL(m_sourcePath.c_str(), nullptr, audioProbeReader.put()));
        for(DWORD streamIndex{};; ++streamIndex){
            com_ptr<IMFMediaType> type;
            const auto hr{audioProbeReader->GetNativeMediaType(streamIndex, 0, type.put())};
            if(hr == MF_E_INVALIDSTREAMNUMBER){
                break;
            }
            check_hresult(hr);
            GUID major{GUID_NULL};
            check_hresult(type->GetGUID(MF_MT_MAJOR_TYPE, &major));
            if(major == MFMediaType_Audio){
                audioStreamIndex = streamIndex;
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

    if(m_profile.id == SourceFormatId::Webm){
        if(request.keepAudio && hasAudioForExport){
            throw hresult_error(MF_E_INVALIDMEDIATYPE, L"WebM export currently keeps VP9 video only; disable audio first.");
        }
        (void)writeWebmVp9VideoSamplesForExport(reader, videoStreamIndex, request.temporaryOutputPath, request.effectiveCutRanges100ns, videoSubtype, nalLengthFieldSize, request.sourceDuration100ns, request.outputDuration100ns, width, height, fpsNum, fpsDen, request.onVideoProgress, request.shouldCancel);
        return;
    }

    if(m_profile.id == SourceFormatId::Wmv && request.keepAudio && hasAudioForExport){
        throw hresult_error(MF_E_INVALIDMEDIATYPE, L"WMV export currently keeps VC-1 video only; disable audio first.");
    }

    com_ptr<IMFSinkWriter> writer;
    DWORD writerVideoStreamIndex{};
    auto configureWriter = [&]() -> HRESULT{
        writer = nullptr;
        writerVideoStreamIndex = 0;
        writerAudioStreamIndex = 0;
        com_ptr<IMFAttributes> writerAttributes;
        check_hresult(MFCreateAttributes(writerAttributes.put(), 1));
        check_hresult(writerAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
        check_hresult(MFCreateSinkWriterFromURL(request.temporaryOutputPath.c_str(), nullptr, writerAttributes.get(), writer.put()));
        check_hresult(writer->AddStream(sourceVideoType.get(), &writerVideoStreamIndex));
        check_hresult(writer->SetInputMediaType(writerVideoStreamIndex, sourceVideoType.get(), nullptr));
        if(!hasAudioForExport){
            return S_OK;
        }
        const auto aacType{createAacOutputType(audioSampleRate, audioChannels)};
        auto hr = writer->AddStream(aacType.get(), &writerAudioStreamIndex);
        if(SUCCEEDED(hr)){
            hr = writer->SetInputMediaType(writerAudioStreamIndex, audioPcmType.get(), nullptr);
        }
        return hr;
    };

    const auto writerConfigHr{configureWriter()};
    if(FAILED(writerConfigHr)){
        if(m_profile.id == SourceFormatId::Avi){
            throw hresult_error(writerConfigHr, L"System cannot mux H.264 into MP4 via Media Foundation.");
        }
        if(m_profile.id == SourceFormatId::Wmv){
            throw hresult_error(writerConfigHr, L"System cannot mux VC-1 into WMV via Media Foundation on this machine.");
        }
        check_hresult(writerConfigHr);
    }

    check_hresult(writer->BeginWriting());
    (void)writeVideoSamplesForExport(reader, videoStreamIndex, writer, writerVideoStreamIndex, request.effectiveCutRanges100ns, videoSubtype, nalLengthFieldSize, request.sourceDuration100ns, request.onVideoProgress, request.shouldCancel);

    if(hasAudioForExport && audioStreamIndex != invalidStream){
        com_ptr<IMFSourceReader> audioReader;
        check_hresult(MFCreateSourceReaderFromURL(m_sourcePath.c_str(), nullptr, audioReader.put()));
        check_hresult(audioReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
        check_hresult(audioReader->SetStreamSelection(audioStreamIndex, TRUE));
        check_hresult(audioReader->SetCurrentMediaType(audioStreamIndex, nullptr, audioPcmType.get()));
        const auto keepRanges100ns{invertCutRanges100ns(request.effectiveCutRanges100ns, request.sourceDuration100ns)};
        writeMixedAudioForKeepRanges(audioReader, audioStreamIndex, keepRanges100ns, writer, writerAudioStreamIndex, audioChannels, audioSampleRate, request.audioCrossfadeMs, request.audioVolumePct / 100.0f, request.onAudioProgress, request.shouldCancel);
    }

    if(request.shouldCancel && request.shouldCancel()){
        throw hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
    }
    check_hresult(writer->Finalize());
}

class Mp4Media final : public ProfileMedia{
public:
    explicit Mp4Media(wstring sourcePath): ProfileMedia(std::move(sourcePath), supportedFormatProfiles()[0]) {}
protected:
    wstring containerName() const override{ return L"MP4"; }
};

class MovMedia final : public ProfileMedia{
public:
    explicit MovMedia(wstring sourcePath): ProfileMedia(std::move(sourcePath), supportedFormatProfiles()[1]) {}
protected:
    wstring containerName() const override{ return L"MOV"; }
};

class AviMedia final : public ProfileMedia{
public:
    explicit AviMedia(wstring sourcePath): ProfileMedia(std::move(sourcePath), supportedFormatProfiles()[2]) {}
protected:
    wstring containerName() const override{ return L"AVI"; }
    void describeAudioSupport(bool hasAudio, VideoSource::InspectionResult& result) const override{
        if(hasAudio){
            result.audioDisabledForThisSource = true;
            result.audioDisabledReason = L"AVI audio passthrough is disabled in v1. Export will keep video only";
        }
    }
};

class WebmMedia final : public ProfileMedia{
public:
    explicit WebmMedia(wstring sourcePath): ProfileMedia(std::move(sourcePath), supportedFormatProfiles()[3]) {}
protected:
    wstring containerName() const override{ return L"WEBM"; }
    void describeAudioSupport(bool hasAudio, VideoSource::InspectionResult& result) const override{
        if(hasAudio){
            result.audioDisabledForThisSource = true;
            result.audioDisabledReason = L"WebM export currently keeps VP9 video only; audio passthrough is not implemented yet";
        }
    }
};

class WmvMedia final : public ProfileMedia{
public:
    explicit WmvMedia(wstring sourcePath): ProfileMedia(std::move(sourcePath), supportedFormatProfiles()[4]) {}
protected:
    wstring containerName() const override{ return L"WMV"; }
    void describeAudioSupport(bool hasAudio, VideoSource::InspectionResult& result) const override{
        if(hasAudio){
            result.audioDisabledForThisSource = true;
            result.audioDisabledReason = L"WMV export currently keeps VC-1 video only; audio passthrough is disabled for now";
        }
    }
};

}

unique_ptr<VideoSource> createVideoSource(const wstring& sourcePath){
    const auto* profile{tryGetFormatProfileForPath(sourcePath)};
    if(!profile){
        return nullptr;
    }

    switch(profile->id){
    case SourceFormatId::Mp4: return make_unique<Mp4Media>(sourcePath);
    case SourceFormatId::Mov: return make_unique<MovMedia>(sourcePath);
    case SourceFormatId::Avi: return make_unique<AviMedia>(sourcePath);
    case SourceFormatId::Webm: return make_unique<WebmMedia>(sourcePath);
    case SourceFormatId::Wmv: return make_unique<WmvMedia>(sourcePath);
    default: return nullptr;
    }
}

}
