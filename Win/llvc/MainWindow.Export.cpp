#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.Helpers.h"

#include <filesystem>
#include <fstream>
#include <limits>

#include <algorithm>
#include <functional>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

#include <shobjidl_core.h>
#include <winrt/Windows.Storage.h>

namespace winrt::llvc::implementation{

using namespace std;
using namespace winrt;

using Control = MainWindow::Control;
using REArgs = MainWindow::REArgs;
using AAction = MainWindow::AAction;

wstring formatDurationFileTag(const int64_t duration100ns);
vector<pair<int64_t, int64_t>> invertCutRanges100ns(const vector<pair<int64_t, int64_t>>& cutRanges, const int64_t totalDuration100ns);
int64_t removedDurationBefore(const vector<pair<int64_t, int64_t>>& cutRanges, const int64_t time100ns);
com_ptr<IMFMediaType> chooseBestNativeVideoMediaType(IMFSourceReader* reader, const DWORD streamIndex);
uint32_t getNalLengthFieldSize(const com_ptr<IMFMediaType>& mediaType, const GUID& videoSubtype);
bool isTrueRandomAccessPointSample(const com_ptr<IMFSample>& sample, const GUID& videoSubtype, const uint32_t nalLengthFieldSize, const bool allowInconclusive);
bool isContainerSyncSample(const com_ptr<IMFSample>& sample);
com_ptr<IMFMediaType> createPcmFloatAudioType(const uint32_t sampleRate, const uint32_t channels);
com_ptr<IMFMediaType> createAacOutputType(const uint32_t sampleRate, const uint32_t channels);
vector<float> decodeAudioRangeToFloat(IMFSourceReader* reader, const DWORD audioStreamIndex, const int64_t rangeStart100ns, const int64_t rangeEnd100ns, const uint32_t channels, const uint32_t sampleRate);

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

wstring pickExportOutputPath(const std::filesystem::path& sourceFsPath, const wchar_t* defaultExt, const int64_t outputDuration100ns, const HWND ownerWindow){
    com_ptr<IFileSaveDialog> saveDialog;
    check_hresult(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(saveDialog.put())));

    DWORD options{};
    check_hresult(saveDialog->GetOptions(&options));
    check_hresult(saveDialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT));

    const COMDLG_FILTERSPEC fileTypes[]{
        {L"MP4 video", L"*.mp4"},
        {L"MOV video", L"*.mov"},
    };
    check_hresult(saveDialog->SetFileTypes(static_cast<UINT>(size(fileTypes)), fileTypes));

    const auto isMovDefault{_wcsicmp(defaultExt, L".mov") == 0};
    check_hresult(saveDialog->SetFileTypeIndex(isMovDefault ? 2U : 1U));
    check_hresult(saveDialog->SetDefaultExtension(isMovDefault ? L"mov" : L"mp4"));

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

void appendCrossfadedAudioSegment(vector<float>& mixedAudio, const vector<float>& segmentAudio, const uint32_t audioChannels, const size_t fadeFrames){
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
        const auto fadeOut{static_cast<float>(overlapFrames - frame) / static_cast<float>(overlapFrames)};
        const auto fadeIn{static_cast<float>(frame + 1) / static_cast<float>(overlapFrames)};
        for(uint32_t ch{0}; ch < audioChannels; ++ch){
            const auto dstIndex{(mixedFrames - overlapFrames + frame) * audioChannels + ch};
            const auto srcIndex{frame * audioChannels + ch};
            mixedAudio[dstIndex] = (mixedAudio[dstIndex] * fadeOut) + (segmentAudio[srcIndex] * fadeIn);
        }
    }

    mixedAudio.insert(mixedAudio.end(), segmentAudio.begin() + static_cast<std::ptrdiff_t>(overlapFrames * audioChannels), segmentAudio.end());
}

vector<float> buildMixedAudioForKeepRanges(
    IMFSourceReader* audioReader,
    const DWORD audioStreamIndex,
    const vector<pair<int64_t, int64_t>>& keepRanges100ns,
    const uint32_t audioChannels,
    const uint32_t audioSampleRate,
    const int crossfadeMs){

    vector<float> mixedAudio;
    const auto fadeMs{crossfadeMs > 0 ? crossfadeMs : 100};
    const auto fadeFrames{static_cast<size_t>((static_cast<int64_t>(audioSampleRate) * fadeMs) / 1000)};

    for(const auto& [keepStart, keepEnd] : keepRanges100ns){
        auto segmentAudio{decodeAudioRangeToFloat(audioReader, audioStreamIndex, keepStart, keepEnd, audioChannels, audioSampleRate)};
        appendCrossfadedAudioSegment(mixedAudio, segmentAudio, audioChannels, fadeFrames);
    }

    return mixedAudio;
}

void writePcmAudioToWriter(
    IMFSinkWriter* writer,
    const DWORD writerAudioStreamIndex,
    const vector<float>& mixedAudio,
    const uint32_t audioChannels,
    const uint32_t audioSampleRate){

    const size_t chunkFrames{1024};
    size_t frameOffset{};
    const auto totalFrames{mixedAudio.size() / audioChannels};
    while(frameOffset < totalFrames){
        const auto framesToWrite{min(chunkFrames, totalFrames - frameOffset)};
        const auto bytesToWrite{static_cast<DWORD>(framesToWrite * audioChannels * sizeof(float))};

        com_ptr<IMFSample> audioSample;
        check_hresult(MFCreateSample(audioSample.put()));
        com_ptr<IMFMediaBuffer> audioBuffer;
        check_hresult(MFCreateMemoryBuffer(bytesToWrite, audioBuffer.put()));

        BYTE* audioBytes{};
        DWORD audioMaxLen{};
        DWORD audioCurLen{};
        check_hresult(audioBuffer->Lock(&audioBytes, &audioMaxLen, &audioCurLen));
        memcpy(audioBytes, mixedAudio.data() + (frameOffset * audioChannels), bytesToWrite);
        check_hresult(audioBuffer->Unlock());
        check_hresult(audioBuffer->SetCurrentLength(bytesToWrite));
        check_hresult(audioSample->AddBuffer(audioBuffer.get()));

        const auto sampleTime100ns{static_cast<LONGLONG>((frameOffset * 10'000'000LL) / audioSampleRate)};
        const auto sampleDuration100ns{static_cast<LONGLONG>((framesToWrite * 10'000'000LL) / audioSampleRate)};
        check_hresult(audioSample->SetSampleTime(sampleTime100ns));
        check_hresult(audioSample->SetSampleDuration(sampleDuration100ns));
        check_hresult(writer->WriteSample(writerAudioStreamIndex, audioSample.get()));

        frameOffset += framesToWrite;
    }
}

struct VideoWriteStats{
    uint64_t readSampleCount{};
    uint64_t droppedByCutCount{};
    uint64_t droppedWaitingRapCount{};
    uint64_t writtenSampleCount{};
};


VideoWriteStats writeVideoSamplesForExport(
    IMFSourceReader* reader,
    const DWORD videoStreamIndex,
    IMFSinkWriter* writer,
    const DWORD writerVideoStreamIndex,
    const vector<pair<int64_t, int64_t>>& effectiveCutRanges100ns,
    const GUID videoSubtype,
    const uint32_t nalLengthFieldSize,
    const int64_t sourceDuration100ns,
    const function<void(double)>& progressCallback){

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


AAction MainWindow::exportVideoMenuItem_Click(const Control&, const REArgs&){
    if(!m_prj.videoFile()){
        co_await showInfoDialogAsync(L"Export video", L"Load a video before exporting.");
        co_return;
    }

    MFLifetime mf{};

    const wstring sourcePath{m_prj.videoFile().Path().c_str()};
    const auto sourceDuration100ns{max<int64_t>(0, static_cast<int64_t>(llround(max(0.0, m_timelineDurationSeconds) * 10'000'000.0)))};
    const auto outputDuration100ns{m_prj.outputDuration100ns(m_timelineDurationSeconds)};

    const filesystem::path sourceFsPath{sourcePath};
    const auto sourceExt{sourceFsPath.extension().wstring()};
    const auto defaultExt{_wcsicmp(sourceExt.c_str(), L".mov") == 0 ? L".mov" : L".mp4"};

    const auto outputPath{pickExportOutputPath(sourceFsPath, defaultExt, outputDuration100ns, getWindowHandle())};
    if(outputPath.empty()){
        co_return;
    }

    setStatusMessage(L"Exporting...");
    clearErrorMessage();
    setOperationInProgress(true, true);

    winrt::hstring exportErrorMessage{};
    auto exportSucceeded{false};

    co_await winrt::resume_background();

    try{
        com_ptr<IMFAttributes> readerAttributes;
        check_hresult(MFCreateAttributes(readerAttributes.put(), 1));
        check_hresult(readerAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE));

        com_ptr<IMFSourceReader> reader;
        check_hresult(MFCreateSourceReaderFromURL(sourcePath.c_str(), readerAttributes.get(), reader.put()));

        constexpr auto invalidStream{numeric_limits<DWORD>::max()};
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

        auto sourceVideoType{chooseBestNativeVideoMediaType(reader.get(), videoStreamIndex)};
        if(!sourceVideoType){
            throw hresult_error(MF_E_INVALIDMEDIATYPE, L"No native video media type found");
        }
        check_hresult(reader->SetCurrentMediaType(videoStreamIndex, nullptr, sourceVideoType.get()));

        GUID videoSubtype{GUID_NULL};
        check_hresult(sourceVideoType->GetGUID(MF_MT_SUBTYPE, &videoSubtype));
        const auto nalLengthFieldSize{getNalLengthFieldSize(sourceVideoType, videoSubtype)};

        vector<int64_t> rapTimes100ns;
        rapTimes100ns.reserve(2048);
        for(;;){
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

            LONGLONG sampleTime100ns{};
            if(FAILED(sample->GetSampleTime(&sampleTime100ns))){
                sampleTime100ns = timestamp;
            }

            if(isContainerSyncSample(sample) && isTrueRandomAccessPointSample(sample, videoSubtype, nalLengthFieldSize, false)){
                rapTimes100ns.push_back(max<int64_t>(0, sampleTime100ns));
            }
        }
        sort(rapTimes100ns.begin(), rapTimes100ns.end());
        rapTimes100ns.erase(unique(rapTimes100ns.begin(), rapTimes100ns.end()), rapTimes100ns.end());

        const auto effectiveCutRanges100ns{m_prj.buildEffectiveCutRangesWithRapPreroll(sourceDuration100ns, rapTimes100ns)};

        PROPVARIANT startPos{};
        startPos.vt = VT_I8;
        startPos.hVal.QuadPart = 0;
        check_hresult(reader->SetCurrentPosition(GUID_NULL, startPos));
        PropVariantClear(&startPos);

        auto hasAudioForExport{false};
        DWORD writerAudioStreamIndex{};
        uint32_t audioChannels{};
        uint32_t audioSampleRate{};
        com_ptr<IMFMediaType> audioPcmType;
        com_ptr<IMFMediaType> sourceAudioNativeType;
        DWORD audioStreamIndex{};
        constexpr auto invalidAudioStream{numeric_limits<DWORD>::max()};

        if(m_prj.keepAudio() && sourceHasAudio()){
            com_ptr<IMFSourceReader> audioProbeReader;
            check_hresult(MFCreateSourceReaderFromURL(sourcePath.c_str(), nullptr, audioProbeReader.put()));

            audioStreamIndex = invalidAudioStream;
            for(DWORD streamIndex = 0;; ++streamIndex){
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
                    sourceAudioNativeType = type;
                    break;
                }
            }

            if(audioStreamIndex != invalidAudioStream){
                (void)sourceAudioNativeType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &audioChannels);
                (void)sourceAudioNativeType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &audioSampleRate);
                if(audioChannels == 0){
                    audioChannels = 2;
                }
                if(audioSampleRate == 0){
                    audioSampleRate = 48000;
                }

                audioPcmType = createPcmFloatAudioType(audioSampleRate, audioChannels);
                hasAudioForExport = true;
            }
        }

        com_ptr<IMFSinkWriter> writer;
        DWORD writerVideoStreamIndex{};
        auto configureWriter = [&]() -> HRESULT {
            writer = nullptr;
            writerVideoStreamIndex = 0;
            writerAudioStreamIndex = 0;

            com_ptr<IMFAttributes> writerAttributes;
            check_hresult(MFCreateAttributes(writerAttributes.put(), 1));
            check_hresult(writerAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
            check_hresult(MFCreateSinkWriterFromURL(outputPath.c_str(), nullptr, writerAttributes.get(), writer.put()));
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

        check_hresult(configureWriter());

        check_hresult(writer->BeginWriting());

        const auto videoStats{writeVideoSamplesForExport(
            reader.get(),
            videoStreamIndex,
            writer.get(),
            writerVideoStreamIndex,
            effectiveCutRanges100ns,
            videoSubtype,
            nalLengthFieldSize,
            sourceDuration100ns,
            [self = get_weak()](double pct){
                if(const auto strong = self.get()){
                    strong->DispatcherQueue().TryEnqueue([weak = strong->get_weak(), pct](){
                        if(const auto ui = weak.get()){
                            ui->setOperationProgress(pct);
                        }
                    });
                }
            })};
        (void)videoStats;

        if(hasAudioForExport && audioStreamIndex != numeric_limits<DWORD>::max()){
            com_ptr<IMFSourceReader> audioReader;
            check_hresult(MFCreateSourceReaderFromURL(sourcePath.c_str(), nullptr, audioReader.put()));
            check_hresult(audioReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
            check_hresult(audioReader->SetStreamSelection(audioStreamIndex, TRUE));
            check_hresult(audioReader->SetCurrentMediaType(audioStreamIndex, nullptr, audioPcmType.get()));

            const auto keepRanges100ns{invertCutRanges100ns(effectiveCutRanges100ns, sourceDuration100ns)};
            const auto mixedAudio{buildMixedAudioForKeepRanges(audioReader.get(), audioStreamIndex, keepRanges100ns, audioChannels, audioSampleRate, m_prj.audioXfadeMs())};
            writePcmAudioToWriter(writer.get(), writerAudioStreamIndex, mixedAudio, audioChannels, audioSampleRate);
        }

        check_hresult(writer->Finalize());
        exportSucceeded = true;
    }catch(const hresult_error& ex){
        exportErrorMessage = ex.message();
    }

    co_await winrt::resume_foreground(DispatcherQueue());

    if(exportSucceeded){
        setOperationProgress(100);
    }

    setOperationInProgress(false);
    if(exportSucceeded){
        setStatusMessage(L"Export completed");
        clearErrorMessage();
        refreshStatusInfoSection();
    }else{
        setStatusMessage(L"Export failed");
        setErrorMessage(exportErrorMessage.c_str());
    }

    if(!exportErrorMessage.empty()){
        co_await showInfoDialogAsync(L"Export failed", exportErrorMessage);
    }
}



} // namespace winrt::llvc::implementation
