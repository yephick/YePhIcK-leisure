#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.Helpers.h"

#include <filesystem>
#include <limits>

#include <algorithm>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

import llvc.Export;

namespace winrt::llvc::implementation{

using namespace std;
using namespace winrt;

using Control = MainWindow::Control;
using REArgs = MainWindow::REArgs;
using AAction = MainWindow::AAction;

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

    m_isExportInProgress = true;
    m_resumeTimelineRenderAfterExport = m_prj.videoFile() && m_timelineDurationSeconds > 0;
    ++m_timelineRenderVersion;

    setStatusMessage(L"Exporting...");
    clearErrorMessage();
    setOperationInProgress(true, true);

    winrt::hstring exportErrorMessage{};
    auto exportSucceeded{false};

    winrt::apartment_context uiThread;
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

        auto sourceVideoType{chooseBestNativeVideoMediaType(reader, videoStreamIndex)};
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
            reader,
            videoStreamIndex,
            writer,
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
            const auto mixedAudio{buildMixedAudioForKeepRanges(audioReader, audioStreamIndex, keepRanges100ns, audioChannels, audioSampleRate, m_prj.audioXfadeMs())};
            writePcmAudioToWriter(writer, writerAudioStreamIndex, mixedAudio, audioChannels, audioSampleRate);
        }

        check_hresult(writer->Finalize());
        exportSucceeded = true;
    }catch(const hresult_error& ex){
        exportErrorMessage = ex.message();
    }

    co_await uiThread;

    if(exportSucceeded){
        setOperationProgress(100);
    }

    m_isExportInProgress = false;
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

    const auto shouldResumeTimeline{m_resumeTimelineRenderAfterExport && m_prj.videoFile() && m_timelineDurationSeconds > 0};
    m_resumeTimelineRenderAfterExport = false;
    if(shouldResumeTimeline){
        wstring status{L"Loaded: "};
        status += m_prj.videoFile().Name().c_str();
        status += L" (loading story line...)";
        setStatusMessage(status);
        clearErrorMessage();
        renderTimelineAsync();
    }
}

}
