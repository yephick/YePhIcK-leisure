module;

#include <Windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>

export module llvc.ExportCoordinator;

import std;
import llvc.Export;
import llvc.Media;
import llvc.Project;

export namespace llvc{

using namespace ::std;
using namespace ::winrt;

enum class ExportStage : uint8_t{
    Rap,
    Video,
    Audio,
    Finalize,
};

struct ExportCoordinatorRequest final{
    const Project* project{};
    const VideoSource* media{};
    const VideoSource::InspectionResult* mediaInfo{};
    wstring sourcePath{};
    wstring outputPath{};
    wstring temporaryOutputPath{};
    wstring exportComment{};
    int64_t sourceDuration100ns{};
    uint64_t sourceSizeBytes{};
    bool sourceHasAudio{};
    bool needsRapReevaluation{};

    function<winrt::Windows::Foundation::IAsyncOperation<bool>(const wstring&, const function<void(double)>&)> ensureRapMarkersAvailableAsync{};
    function<bool(bool)> reevaluateCutMarkers{};
    function<std::optional<EffectiveExportPlan>(const function<void(double)>&)> buildEffectiveExportPlan{};
    function<void(const wstring&)> onStatus{};
    function<void(double)> onOverallProgress{};
    function<void(ExportStage, wstring, optional<double>, bool)> onStageState{};
    function<bool()> shouldCancel{};
};

struct ExportCoordinatorResult final{
    bool succeeded{false};
    bool canceled{false};
    hstring errorMessage{};
    EffectiveExportPlan effectivePlan{};
    std::optional<chrono::milliseconds> rapStageDurationMs{};
    std::optional<chrono::milliseconds> videoStageDurationMs{};
    std::optional<chrono::milliseconds> audioStageDurationMs{};
    std::optional<chrono::milliseconds> finalizeStageDurationMs{};
};

winrt::Windows::Foundation::IAsyncAction runExportAsync(const ExportCoordinatorRequest& request, ExportCoordinatorResult& result);

}

namespace llvc{

using namespace ::std;
using namespace ::winrt;

namespace{

wstring buildTemporaryBackupPath(const wstring& targetPath){
    const filesystem::path targetFsPath{targetPath};
    const auto parent{targetFsPath.parent_path()};
    const auto filename{targetFsPath.filename().wstring()};

    for(uint32_t attempt{}; attempt < 1000; ++attempt){
        const auto ticks{static_cast<unsigned long long>(chrono::steady_clock::now().time_since_epoch().count())};
        const auto candidateName{
            std::format(L"{}.llvc-backup-{}-{}", filename.empty() ? L"backup" : filename, ::GetCurrentProcessId(), ticks + attempt)};
        const auto candidatePath{parent / candidateName};

        std::error_code existsEc;
        if(!filesystem::exists(candidatePath, existsEc)){
            return candidatePath.wstring();
        }
    }

    return {};
}

bool isCanceled(const ExportCoordinatorRequest& request){
    return request.shouldCancel && request.shouldCancel();
}

void throwIfCanceled(const ExportCoordinatorRequest& request){
    if(isCanceled(request)){
        throw hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Export canceled.");
    }
}

}

winrt::Windows::Foundation::IAsyncAction runExportAsync(const ExportCoordinatorRequest& request, ExportCoordinatorResult& result){
    if(!request.project || !request.media || !request.mediaInfo){
        result.errorMessage = L"Export coordinator received incomplete request data.";
        co_return;
    }

    const auto exportOverallStartedAt{chrono::steady_clock::now()};
    const auto requestedCutRanges100ns{request.project->buildCutRanges100ns()};
    const auto requestedOutputDuration100ns{request.project->outputDuration100ns()};

    if(request.onStatus){
        request.onStatus(request.needsRapReevaluation ? L"Reevaluating cut plan to RAP frames..." : L"Preparing export plan...");
    }
    if(request.onStageState){
        request.onStageState(ExportStage::Rap, L"In progress", 0.0, true);
    }
    if(request.onOverallProgress){
        request.onOverallProgress(0.0);
    }

    if(request.needsRapReevaluation){
        const auto rapReady{co_await request.ensureRapMarkersAvailableAsync(
            L"Reevaluating cut plan to RAP frames...",
            [onStageState = request.onStageState, onOverallProgress = request.onOverallProgress](double pct){
                if(onStageState){
                    onStageState(ExportStage::Rap, L"In progress", pct, true);
                }
                if(onOverallProgress){
                    onOverallProgress((pct * 5.0) / 100.0);
                }
            })};
        if(!rapReady){
            result.canceled = isCanceled(request);
            result.errorMessage = result.canceled ? hstring{} : hstring{L"Could not build a RAP-aligned cut plan for the current source."};
            co_return;
        }
    }

    if(request.needsRapReevaluation){
        if(isCanceled(request)){
            result.canceled = true;
            co_return;
        }

        if(!request.reevaluateCutMarkers(false)){
            result.canceled = isCanceled(request);
            result.errorMessage = result.canceled ? hstring{} : hstring{L"Could not build a RAP-aligned cut plan for the current source."};
            co_return;
        }
    }

    const auto effectivePlanOpt{request.buildEffectiveExportPlan({})};
    if(!effectivePlanOpt){
        result.errorMessage = L"The export plan is not ready yet. Wait for the RAP analysis to finish and try again.";
        co_return;
    }
    result.effectivePlan = *effectivePlanOpt;
    if(result.effectivePlan.emptyAfterAlignment){
        result.errorMessage = L"The current cut selection does not contain a safe RAP-aligned cut range. Adjust markers or reevaluate clear cut markers first.";
        co_return;
    }
    result.rapStageDurationMs = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - exportOverallStartedAt);

    if(request.onStageState){
        request.onStageState(ExportStage::Rap, result.effectivePlan.materiallyDifferent ? L"Adjusted" : L"Done", 100.0, false);
    }
    if(request.onOverallProgress){
        request.onOverallProgress(5.0);
    }
    if(request.onStatus){
        request.onStatus(L"Exporting...");
    }

    const auto outputDuration100ns{result.effectivePlan.effectiveOutputDuration100ns};
    const auto effectiveCutRanges100ns{result.effectivePlan.effectiveCutRanges100ns};
    const auto hasAudioForExport{
        request.project->keepAudio()
        && request.sourceHasAudio
        && request.mediaInfo->audioExportSupport == CapabilityState::Supported};

    apartment_context uiThread;
    MFLifetime mf{};

    co_await resume_background();
    try{
        const auto overallRapShare{5.0};
        const auto overallVideoShare{hasAudioForExport ? 65.0 : 80.0};
        const auto overallAudioShare{hasAudioForExport ? 20.0 : 0.0};
        const auto overallFinalizeBase{overallRapShare + overallVideoShare + overallAudioShare};
        std::optional<chrono::steady_clock::time_point> videoStageStartedAt;
        std::optional<chrono::steady_clock::time_point> audioStageStartedAt;

        VideoSource::ExportRequest mediaRequest{
            .temporaryOutputPath = request.temporaryOutputPath,
            .sourceDuration100ns = request.sourceDuration100ns,
            .outputDuration100ns = outputDuration100ns,
            .effectiveCutRanges100ns = effectiveCutRanges100ns,
            .keepAudio = request.project->keepAudio(),
            .audioCrossfadeMs = request.project->audioXfadeMs(),
            .audioVolumePct = request.project->audioVolumePct(),
            .onVideoProgress = [&, onStageState = request.onStageState, onOverallProgress = request.onOverallProgress](double pct){
                const auto now{chrono::steady_clock::now()};
                if(!videoStageStartedAt){
                    videoStageStartedAt = now;
                }
                if(pct >= 100.0 && videoStageStartedAt && !result.videoStageDurationMs){
                    result.videoStageDurationMs = chrono::duration_cast<chrono::milliseconds>(now - *videoStageStartedAt);
                }
                if(onStageState){
                    onStageState(ExportStage::Video, L"In progress", pct, true);
                }
                if(onOverallProgress){
                    onOverallProgress(overallRapShare + ((pct * overallVideoShare) / 100.0));
                }
            },
            .onAudioProgress = [&, onStageState = request.onStageState, onOverallProgress = request.onOverallProgress, onStatus = request.onStatus](double pct){
                const auto now{chrono::steady_clock::now()};
                if(!audioStageStartedAt){
                    audioStageStartedAt = now;
                }
                if(pct >= 100.0 && audioStageStartedAt && !result.audioStageDurationMs){
                    result.audioStageDurationMs = chrono::duration_cast<chrono::milliseconds>(now - *audioStageStartedAt);
                }
                if(onStatus){
                    onStatus(L"Writing audio...");
                }
                if(onStageState){
                    onStageState(ExportStage::Audio, L"In progress", pct, true);
                }
                if(onOverallProgress){
                    onOverallProgress(overallRapShare + overallVideoShare + ((pct * overallAudioShare) / 100.0));
                }
            },
            .shouldCancel = [shouldCancel = request.shouldCancel](){
                return shouldCancel && shouldCancel();
            }};

        request.media->exportLossless(mediaRequest);

        if(videoStageStartedAt && !result.videoStageDurationMs){
            result.videoStageDurationMs = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - *videoStageStartedAt);
        }
        if(request.onStageState){
            request.onStageState(ExportStage::Video, L"Done", 100.0, false);
        }
        if(hasAudioForExport){
            if(audioStageStartedAt && !result.audioStageDurationMs){
                result.audioStageDurationMs = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - *audioStageStartedAt);
            }
            if(request.onStageState){
                request.onStageState(ExportStage::Audio, L"Done", 100.0, false);
            }
        }else if(request.onStageState){
            request.onStageState(ExportStage::Audio, L"Not needed", 0.0, false);
        }

        throwIfCanceled(request);
        if(request.onStatus){
            request.onStatus(L"Building indices and metadata...");
        }
        if(request.onStageState){
            request.onStageState(ExportStage::Finalize, L"In progress", nullopt, true);
        }
        if(request.onOverallProgress){
            request.onOverallProgress(min(99.0, overallFinalizeBase + 5.0));
        }

        const auto finalizeStageStartedAt{chrono::steady_clock::now()};
        if(request.onStageState){
            request.onStageState(ExportStage::Finalize, L"Applying metadata", nullopt, true);
        }
        applyExportFileMetadata(request.sourcePath, request.temporaryOutputPath, request.exportComment);

        throwIfCanceled(request);
        if(request.onStageState){
            request.onStageState(ExportStage::Finalize, L"Replacing final file", nullopt, true);
        }
        std::error_code outputExistsEc;
        if(filesystem::exists(request.outputPath, outputExistsEc)){
            const auto backupPath{buildTemporaryBackupPath(request.outputPath)};
            if(backupPath.empty()){
                throw hresult_error(E_FAIL, L"Could not create a backup path for the final export target.");
            }

            throwIfCanceled(request);
            if(!::ReplaceFileW(request.outputPath.c_str(), request.temporaryOutputPath.c_str(), backupPath.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)){
                const auto replaceError{::GetLastError()};
                std::error_code cleanupEc;
                filesystem::remove(backupPath, cleanupEc);
                throw hresult_error(HRESULT_FROM_WIN32(replaceError), L"Could not replace the final export target.");
            }

            std::error_code cleanupEc;
            filesystem::remove(backupPath, cleanupEc);
        }else if(outputExistsEc){
            throw hresult_error(HRESULT_FROM_WIN32(outputExistsEc.value()), winrt::to_hstring(outputExistsEc.message()));
        }else{
            throwIfCanceled(request);
            if(!::MoveFileExW(request.temporaryOutputPath.c_str(), request.outputPath.c_str(), MOVEFILE_WRITE_THROUGH)){
                throw hresult_error(HRESULT_FROM_WIN32(::GetLastError()), L"Could not move the final export target into place.");
            }
        }

        result.finalizeStageDurationMs = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - finalizeStageStartedAt);
        if(request.onStageState){
            request.onStageState(ExportStage::Finalize, L"Done", 100.0, false);
        }
        result.succeeded = true;
    }catch(const hresult_error& ex){
        if(ex.code() == HRESULT_FROM_WIN32(ERROR_CANCELLED)){
            result.canceled = true;
        }else{
            result.errorMessage = ex.message();
        }
        std::error_code ec;
        filesystem::remove(request.temporaryOutputPath, ec);
    }

    co_await uiThread;
    co_return;
}

}
