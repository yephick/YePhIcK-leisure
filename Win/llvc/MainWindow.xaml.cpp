#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"


#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

#include <commctrl.h>

#include <microsoft.ui.xaml.window.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shobjidl_core.h>
#include <mmreg.h>
#include <robuffer.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Editing.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>

import std;
import llvc.AudioWaveform;
import llvc.Dialogs;
import llvc.EditorController;
import llvc.Export;
import llvc.Media;
import llvc.Utils;
import llvc.ExportCoordinator;

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")


using namespace std;
using namespace winrt;
using namespace ::llvc;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Input;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::Media::Playback;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;
using namespace winrt::Windows::System;

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
using WAVArgs = MainWindow::WAVArgs;
using MPSession = MainWindow::MPSession;
using SFile = MainWindow::SFile;
using FState = MainWindow::FState;
using IOpBool = MainWindow::IOpBool;
using TS = MainWindow::TS;

namespace{
constexpr array kPageJumpSecondsOptions{0.5, 1.0, 2.0, 3.0, 4.0, 5.0, 7.0, 10.0, 15.0, 20.0};
constexpr int kIntegerZoomStartIndex{4};
constexpr int32_t kMainWindowMinWidthDips{960};
constexpr int32_t kMainWindowMinHeightDips{640};
constexpr size_t kAudioWaveformBucketCount{4096};
constexpr size_t kAudioWaveformChunkCount{64};
constexpr size_t kAudioWaveformBucketsPerChunk{kAudioWaveformBucketCount / kAudioWaveformChunkCount};

struct AudioWaveformCacheEntry final{
    vector<float> peaks = vector<float>(kAudioWaveformBucketCount, 0.0f);
    vector<bool> chunksBuilt = vector<bool>(kAudioWaveformChunkCount, false);
    bool ready{false};
    bool failed{false};
    bool inProgress{false};
};

std::mutex g_audioWaveformCacheMutex;
std::unordered_map<std::wstring, std::shared_ptr<AudioWaveformCacheEntry>> g_audioWaveformCache;

double pageJumpSecondsFromSettings(const ::llvc::AppSettingsState& settings){
    return kPageJumpSecondsOptions[min<size_t>(settings.pageJumpDurationIndex, kPageJumpSecondsOptions.size() - 1)];
}

double clampTimelineZoomIndex(const Slider& slider, double value){
    return clamp(static_cast<double>(lround(value)), slider.Minimum(), slider.Maximum());
}

double timelineZoomValueFromIndex(double index){
    const auto zoomIndex{max(1, static_cast<int>(lround(index)))};
    if(zoomIndex < kIntegerZoomStartIndex){
        return 1.0 / static_cast<double>(kIntegerZoomStartIndex + 1 - zoomIndex);
    }
    return static_cast<double>(zoomIndex - (kIntegerZoomStartIndex - 1));
}

LRESULT CALLBACK MainWindowSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData){
    auto* self{reinterpret_cast<MainWindow*>(refData)};
    if(!self){
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    if(msg == WM_GETMINMAXINFO){
        auto* minmaxInfo{reinterpret_cast<MINMAXINFO*>(lParam)};
        if(minmaxInfo){
            const auto dpi{::GetDpiForWindow(hwnd)};
            minmaxInfo->ptMinTrackSize.x = dipsToPixels(kMainWindowMinWidthDips, dpi);
            minmaxInfo->ptMinTrackSize.y = dipsToPixels(kMainWindowMinHeightDips, dpi);
            return 0;
        }
    }

    if(msg == WM_CLOSE && self->isExportInProgressForClosePrompt()){
        const auto choice{MessageBoxW(hwnd,
            L"An export is still in progress. Cancel export and close the app?",
            L"Export in progress",
            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2)};
        if(choice != IDYES){
            return 0;
        }

        self->requestExportCancel();
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

std::optional<uint32_t> tryGetTimelineJumpPercentForKey(const VirtualKey key){
    switch(key){
    case VirtualKey::Number0:
    case VirtualKey::NumberPad0:
        return 0;
    case VirtualKey::Number1:
    case VirtualKey::NumberPad1:
        return 10;
    case VirtualKey::Number2:
    case VirtualKey::NumberPad2:
        return 20;
    case VirtualKey::Number3:
    case VirtualKey::NumberPad3:
        return 30;
    case VirtualKey::Number4:
    case VirtualKey::NumberPad4:
        return 40;
    case VirtualKey::Number5:
    case VirtualKey::NumberPad5:
        return 50;
    case VirtualKey::Number6:
    case VirtualKey::NumberPad6:
        return 60;
    case VirtualKey::Number7:
    case VirtualKey::NumberPad7:
        return 70;
    case VirtualKey::Number8:
    case VirtualKey::NumberPad8:
        return 80;
    case VirtualKey::Number9:
    case VirtualKey::NumberPad9:
        return 90;
    default:
        return std::nullopt;
    }
}

void addPathToShellRecentDocuments(const hstring& path){
    if(path.empty()){
        return;
    }

    ::SHAddToRecentDocs(SHARD_PATHW, path.c_str());
}

bool pathsMatchInsensitive(const std::wstring& left, const std::wstring& right){
    if(left.empty() || right.empty()){
        return false;
    }

    const auto normalizedLeft{filesystem::path(left).lexically_normal().wstring()};
    const auto normalizedRight{filesystem::path(right).lexically_normal().wstring()};
    return _wcsicmp(normalizedLeft.c_str(), normalizedRight.c_str()) == 0;
}

struct DecodedThumbnail final{
    int32_t width{};
    int32_t height{};
    vector<uint8_t> bgraPixels{};
};

std::optional<DecodedThumbnail> tryDecodeThumbnailWithSourceReader(const wstring& filePath, int64_t time100ns, uint32_t targetWidth, uint32_t targetHeight){
    if(filePath.empty() || targetWidth == 0 || targetHeight == 0){
        return nullopt;
    }

    const auto coinitHr{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    const auto shouldCoUninitialize{SUCCEEDED(coinitHr)};
    if(FAILED(coinitHr) && coinitHr != RPC_E_CHANGED_MODE){
        return nullopt;
    }

    struct CoUninitGuard final{
        bool active{};
        ~CoUninitGuard(){
            if(active){
                CoUninitialize();
            }
        }
    } coGuard{shouldCoUninitialize};

    try{
        ::llvc::MFLifetime mf{};

        com_ptr<IMFAttributes> attributes;
        check_hresult(MFCreateAttributes(attributes.put(), 2));
        check_hresult(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
        check_hresult(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE));

        com_ptr<IMFSourceReader> reader;
        check_hresult(MFCreateSourceReaderFromURL(filePath.c_str(), attributes.get(), reader.put()));

        constexpr auto invalidStream{(numeric_limits<DWORD>::max)()};
        auto videoStreamIndex{invalidStream};
        for(DWORD streamIndex{};; ++streamIndex){
            com_ptr<IMFMediaType> nativeType;
            const auto hr{reader->GetNativeMediaType(streamIndex, 0, nativeType.put())};
            if(hr == MF_E_INVALIDSTREAMNUMBER || hr == MF_E_NO_MORE_TYPES){
                break;
            }
            if(FAILED(hr) || !nativeType){
                continue;
            }

            GUID major{};
            if(SUCCEEDED(nativeType->GetGUID(MF_MT_MAJOR_TYPE, &major)) && major == MFMediaType_Video){
                videoStreamIndex = streamIndex;
                break;
            }
        }
        if(videoStreamIndex == invalidStream){
            return nullopt;
        }

        check_hresult(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
        check_hresult(reader->SetStreamSelection(videoStreamIndex, TRUE));

        com_ptr<IMFMediaType> outputType;
        check_hresult(MFCreateMediaType(outputType.put()));
        check_hresult(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        check_hresult(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
        check_hresult(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        check_hresult(reader->SetCurrentMediaType(videoStreamIndex, nullptr, outputType.get()));

        com_ptr<IMFMediaType> currentType;
        check_hresult(reader->GetCurrentMediaType(videoStreamIndex, currentType.put()));
        uint32_t actualWidth{targetWidth};
        uint32_t actualHeight{targetHeight};
        (void)MFGetAttributeSize(currentType.get(), MF_MT_FRAME_SIZE, &actualWidth, &actualHeight);
        if(actualWidth == 0 || actualHeight == 0 || actualWidth > 4096 || actualHeight > 4096){
            return nullopt;
        }

        PROPVARIANT startPosition{};
        startPosition.vt = VT_I8;
        startPosition.hVal.QuadPart = max<int64_t>(0, time100ns);
        check_hresult(reader->SetCurrentPosition(GUID_NULL, startPosition));
        PropVariantClear(&startPosition);

        com_ptr<IMFSample> selectedSample;
        LONGLONG selectedTime{};
        constexpr uint32_t maxSamplesToRead{120};
        for(uint32_t attempt{}; attempt < maxSamplesToRead; ++attempt){
            DWORD actualStream{};
            DWORD flags{};
            LONGLONG timestamp{};
            com_ptr<IMFSample> sample;
            const auto hr{reader->ReadSample(videoStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put())};
            if(FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)){
                break;
            }
            if(!sample){
                continue;
            }

            LONGLONG sampleTime{};
            if(FAILED(sample->GetSampleTime(&sampleTime))){
                sampleTime = timestamp;
            }
            selectedSample = sample;
            selectedTime = sampleTime;
            if(sampleTime >= time100ns){
                break;
            }
        }
        (void)selectedTime;
        if(!selectedSample){
            return nullopt;
        }

        com_ptr<IMFMediaBuffer> contiguousBuffer;
        check_hresult(selectedSample->ConvertToContiguousBuffer(contiguousBuffer.put()));

        BYTE* sourceBytes{};
        DWORD maxLength{};
        DWORD currentLength{};
        check_hresult(contiguousBuffer->Lock(&sourceBytes, &maxLength, &currentLength));

        LONG sourceStride{};
        if(FAILED(currentType->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&sourceStride))) || sourceStride == 0){
            sourceStride = static_cast<LONG>(actualWidth * 4);
        }
        const auto absStride{static_cast<size_t>(sourceStride < 0 ? -sourceStride : sourceStride)};
        if(absStride < static_cast<size_t>(actualWidth) * 4){
            contiguousBuffer->Unlock();
            return nullopt;
        }
        const auto requiredBytes{absStride * static_cast<size_t>(actualHeight)};
        if(!sourceBytes || currentLength < requiredBytes){
            contiguousBuffer->Unlock();
            return nullopt;
        }

        DecodedThumbnail thumbnail{
            .width = static_cast<int32_t>(targetWidth),
            .height = static_cast<int32_t>(targetHeight),
            .bgraPixels = vector<uint8_t>(static_cast<size_t>(targetWidth) * targetHeight * 4)};

        for(uint32_t y{}; y < targetHeight; ++y){
            const auto scaledY{static_cast<uint32_t>((static_cast<uint64_t>(y) * actualHeight) / targetHeight)};
            const auto sourceY{sourceStride < 0 ? (actualHeight - 1 - scaledY) : scaledY};
            const auto* sourceRow{sourceBytes + (static_cast<size_t>(sourceY) * absStride)};
            auto* destRow{thumbnail.bgraPixels.data() + (static_cast<size_t>(y) * targetWidth * 4)};
            for(uint32_t x{}; x < targetWidth; ++x){
                const auto sourceX{static_cast<uint32_t>((static_cast<uint64_t>(x) * actualWidth) / targetWidth)};
                const auto* sourcePixel{sourceRow + (static_cast<size_t>(sourceX) * 4)};
                auto* destPixel{destRow + (static_cast<size_t>(x) * 4)};
                destPixel[0] = sourcePixel[0];
                destPixel[1] = sourcePixel[1];
                destPixel[2] = sourcePixel[2];
                destPixel[3] = 0xFF;
            }
        }

        contiguousBuffer->Unlock();
        return thumbnail;
    }catch(...){
        return nullopt;
    }
}

Media::Imaging::WriteableBitmap createWriteableBitmapFromDecodedThumbnail(const DecodedThumbnail& thumbnail){
    Media::Imaging::WriteableBitmap bitmap{thumbnail.width, thumbnail.height};
    const auto pixelBuffer{bitmap.PixelBuffer()};
    auto byteAccess{pixelBuffer.as<::Windows::Storage::Streams::IBufferByteAccess>()};
    uint8_t* dest{};
    check_hresult(byteAccess->Buffer(&dest));
    if(dest && !thumbnail.bgraPixels.empty()){
        memcpy(dest, thumbnail.bgraPixels.data(), thumbnail.bgraPixels.size());
    }
    bitmap.Invalidate();
    return bitmap;
}

std::wstring buildTemporaryExportPath(const std::wstring& targetPath){
    const filesystem::path targetFsPath{targetPath};
    const auto parent{targetFsPath.parent_path()};
    const auto stem{targetFsPath.stem().wstring()};
    const auto extension{targetFsPath.extension().wstring()};

    for(uint32_t attempt{}; attempt < 1000; ++attempt){
        const auto ticks{static_cast<unsigned long long>(chrono::steady_clock::now().time_since_epoch().count())};
        const auto candidateName{
            std::format(L"{}.llvc-export-{}-{}{}", stem.empty() ? L"export" : stem, ::GetCurrentProcessId(), ticks + attempt, extension)};
        const auto candidatePath{parent / candidateName};

        std::error_code existsEc;
        if(!filesystem::exists(candidatePath, existsEc)){
            return candidatePath.wstring();
        }
    }

    return {};
}

std::wstring buildTemporaryBackupPath(const std::wstring& targetPath){
    const filesystem::path targetFsPath{targetPath};
    const auto parent{targetFsPath.parent_path()};
    const auto filename{targetFsPath.filename().wstring()};

    for(uint32_t attempt{}; attempt < 1000; ++attempt){
        const auto ticks{static_cast<unsigned long long>(chrono::steady_clock::now().time_since_epoch().count())};
        const auto candidateName{
            std::format(L"{}.llvc-backup-{}-{}", filename.empty() ? L"export" : filename, ::GetCurrentProcessId(), ticks + attempt)};
        const auto candidatePath{parent / candidateName};

        std::error_code existsEc;
        if(!filesystem::exists(candidatePath, existsEc)){
            return candidatePath.wstring();
        }
    }

    return {};
}

bool removePathFromShellRecentDocuments(const std::wstring& path){
    if(path.empty()){
        return false;
    }

    com_ptr<IShellItem> shellItem;
    const auto itemHr{SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(shellItem.put()))};
    if(FAILED(itemHr)){
        return false;
    }

    com_ptr<IApplicationDestinations> destinations;
    const auto destHr{CoCreateInstance(CLSID_ApplicationDestinations, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(destinations.put()))};
    if(FAILED(destHr)){
        return false;
    }

    return SUCCEEDED(destinations->RemoveDestination(shellItem.get()));
}

bool movePathToRecycleBin(const std::wstring& path, std::wstring& failureReason){
    failureReason.clear();
    if(path.empty()){
        failureReason = L"Path is empty.";
        return false;
    }

    com_ptr<IFileOperation> fileOperation;
    const auto createHr{CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(fileOperation.put()))};
    if(SUCCEEDED(createHr)){
        const auto flagsHr{fileOperation->SetOperationFlags(
            FOFX_ADDUNDORECORD | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION | FOF_SILENT | FOFX_SHOWELEVATIONPROMPT)};
        if(SUCCEEDED(flagsHr)){
            com_ptr<IShellItem> shellItem;
            const auto itemHr{SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(shellItem.put()))};
            if(SUCCEEDED(itemHr)){
                const auto deleteHr{fileOperation->DeleteItem(shellItem.get(), nullptr)};
                if(SUCCEEDED(deleteHr)){
                    const auto performHr{fileOperation->PerformOperations()};
                    if(SUCCEEDED(performHr)){
                        BOOL anyAborted{};
                        if(SUCCEEDED(fileOperation->GetAnyOperationsAborted(&anyAborted)) && !anyAborted){
                            return true;
                        }
                    }
                }
            }
        }
    }

    // Fall back to the classic shell delete path when IFileOperation is unavailable
    // in the current packaged/runtime context.
    std::wstring doubleNullPath{path};
    doubleNullPath.push_back(L'\0');
    doubleNullPath.push_back(L'\0');

    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = doubleNullPath.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_WANTNUKEWARNING;

    const int shellResult{::SHFileOperationW(&operation)};
    if(shellResult == 0 && !operation.fAnyOperationsAborted){
        return true;
    }

    // Some locations, especially removable media, do not support recycle-bin
    // semantics. Fall back to normal permanent delete paths in that case.
    std::error_code removeEc;
    if(filesystem::remove(path, removeEc)){
        return true;
    }

    if(::DeleteFileW(path.c_str())){
        return true;
    }
    const auto deleteFileError{::GetLastError()};

    if(deleteFileError != ERROR_SUCCESS){
        failureReason = winrt::to_hstring(winrt::hresult_error(HRESULT_FROM_WIN32(deleteFileError)).message()).c_str();
    }else if(removeEc){
        failureReason = winrt::to_hstring(removeEc.message()).c_str();
    }else if(shellResult != 0){
        failureReason = winrt::to_hstring(winrt::hresult_error(HRESULT_FROM_WIN32(shellResult)).message()).c_str();
    }else{
        failureReason = L"The delete operation was canceled or aborted.";
    }
    return false;
}

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
    Mkv,
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

const array<GUID, 3> MP4_MOV_ALLOWED_VIDEO_SUBTYPES{
    MFVideoFormat_H264,
    MFVideoFormat_HEVC,
    MFVideoFormat_H265};
const array<GUID, 1> AVI_ALLOWED_VIDEO_SUBTYPES{
    MFVideoFormat_H264};
const array<GUID, 1> WEBM_ALLOWED_VIDEO_SUBTYPES{
    MFVideoFormat_VP90};
const array<GUID, 1> WMV_ALLOWED_VIDEO_SUBTYPES{
    VC1_VIDEO_SUBTYPE};

const array<FormatProfile, 6>& supportedFormatProfiles(){
    static const array<FormatProfile, 6> profiles{{
        FormatProfile{
            .id = SourceFormatId::Mp4,
            .extension = L".mp4",
            .allowedVideoSubtypes = MP4_MOV_ALLOWED_VIDEO_SUBTYPES,
            .candidateExportExtensions = {L".mp4", L".mov"},
            .candidateExportExtensionCount = 2,
            .audioExportPolicy = AudioExportPolicy::Allowed,
            .exportProbeKind = ExportProbeKind::MediaFoundationSinkWriter},
        FormatProfile{
            .id = SourceFormatId::Mov,
            .extension = L".mov",
            .allowedVideoSubtypes = MP4_MOV_ALLOWED_VIDEO_SUBTYPES,
            .candidateExportExtensions = {L".mp4", L".mov"},
            .candidateExportExtensionCount = 2,
            .audioExportPolicy = AudioExportPolicy::Allowed,
            .exportProbeKind = ExportProbeKind::MediaFoundationSinkWriter},
        FormatProfile{
            .id = SourceFormatId::Mkv,
            .extension = L".mkv",
            .allowedVideoSubtypes = MP4_MOV_ALLOWED_VIDEO_SUBTYPES,
            .candidateExportExtensions = {L".mp4", nullptr},
            .candidateExportExtensionCount = 1,
            .audioExportPolicy = AudioExportPolicy::Allowed,
            .exportProbeKind = ExportProbeKind::MediaFoundationSinkWriter},
        FormatProfile{
            .id = SourceFormatId::Avi,
            .extension = L".avi",
            .allowedVideoSubtypes = AVI_ALLOWED_VIDEO_SUBTYPES,
            .candidateExportExtensions = {L".mp4", nullptr},
            .candidateExportExtensionCount = 1,
            .audioExportPolicy = AudioExportPolicy::Disabled,
            .exportProbeKind = ExportProbeKind::MediaFoundationSinkWriter,
            .requiresAviValidation = true},
        FormatProfile{
            .id = SourceFormatId::Webm,
            .extension = L".webm",
            .allowedVideoSubtypes = WEBM_ALLOWED_VIDEO_SUBTYPES,
            .candidateExportExtensions = {L".webm", nullptr},
            .candidateExportExtensionCount = 1,
            .audioExportPolicy = AudioExportPolicy::Disabled,
            .exportProbeKind = ExportProbeKind::CustomWriter},
        FormatProfile{
            .id = SourceFormatId::Wmv,
            .extension = L".wmv",
            .allowedVideoSubtypes = WMV_ALLOWED_VIDEO_SUBTYPES,
            .candidateExportExtensions = {L".wmv", nullptr},
            .candidateExportExtensionCount = 1,
            .audioExportPolicy = AudioExportPolicy::Disabled,
            .exportProbeKind = ExportProbeKind::MediaFoundationSinkWriter},
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

bool isSupportedMediaPath(const wstring& filePath){
    return static_cast<bool>(createVideoSource(filePath));
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

wstring capabilityStateToText(const CapabilityState state){
    switch(state){
    case CapabilityState::Supported:
        return L"yes";
    case CapabilityState::Unsupported:
        return L"no";
    case CapabilityState::NotApplicable:
        return L"n/a";
    default:
        return L"unknown";
    }
}

wstring joinExtensions(const vector<wstring>& extensions){
    if(extensions.empty()){
        return L"-";
    }

    wstring text;
    for(size_t i{}; i < extensions.size(); ++i){
        if(i > 0){
            text += L", ";
        }
        text += extensions[i];
    }
    return text;
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
        const auto hr{reader->ReadSample(videoStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put())};
        check_hresult(hr);

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

        const auto cleanTime100ns{max<int64_t>(0, sampleTime)};
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

wstring guidToVideoCodecName(const GUID& subtype){
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

    const auto hasMfSubtypeBase{
        subtype.Data2 == 0x0000
        && subtype.Data3 == 0x0010
        && subtype.Data4[0] == 0x80
        && subtype.Data4[1] == 0x00
        && subtype.Data4[2] == 0x00
        && subtype.Data4[3] == 0xAA
        && subtype.Data4[4] == 0x00
        && subtype.Data4[5] == 0x38
        && subtype.Data4[6] == 0x9B
        && subtype.Data4[7] == 0x71};
    if(hasMfSubtypeBase){
        const DWORD fcc{subtype.Data1};
        wchar_t fourcc[5]{};
        for(int i{}; i < 4; ++i){
            const auto c{static_cast<wchar_t>((fcc >> (i * 8)) & 0xFF)};
            if(c < 0x20 || c > 0x7E){
                fourcc[0] = L'\0';
                break;
            }
            fourcc[i] = static_cast<wchar_t>(towupper(c));
        }

        if(fourcc[0] != L'\0'){
            return fourcc;
        }
    }

    return L"unknown codec";
}

wstring BuildUnsupportedAviReason(const wstring& detail){
    return detail;
}

wstring decodeXmlEntities(wstring text){
    const array<pair<wstring, wstring>, 5> replacements{{
        {L"&apos;", L"'"},
        {L"&quot;", L"\""},
        {L"&amp;", L"&"},
        {L"&lt;", L"<"},
        {L"&gt;", L">"}}};

    for(const auto& [from, to] : replacements){
        size_t pos{};
        while((pos = text.find(from, pos)) != wstring::npos){
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    return text;
}

wstring tryReadVisualElementsDescriptionFromManifestPath(const filesystem::path& manifestPath){
    ifstream file{manifestPath, ios::binary};
    if(!file){
        return {};
    }

    string xmlBytes{istreambuf_iterator<char>{file}, istreambuf_iterator<char>{}};
    if(xmlBytes.empty()){
        return {};
    }

    static constexpr string_view key{"Description=\""};
    const auto visualElementsPos{xmlBytes.find("VisualElements")};
    if(visualElementsPos == string::npos){
        return {};
    }

    const auto descStart{xmlBytes.find(key, visualElementsPos)};
    if(descStart == string::npos){
        return {};
    }

    const auto valueStart{descStart + key.size()};
    const auto valueEnd{xmlBytes.find('"', valueStart)};
    if(valueEnd == string::npos || valueEnd <= valueStart){
        return {};
    }

    const string descriptionUtf8{xmlBytes.substr(valueStart, valueEnd - valueStart)};
    if(descriptionUtf8.empty()){
        return {};
    }

    const int wideLen{MultiByteToWideChar(CP_UTF8, 0, descriptionUtf8.c_str(), static_cast<int>(descriptionUtf8.size()), nullptr, 0)};
    if(wideLen <= 0){
        return {};
    }

    wstring description(static_cast<size_t>(wideLen), L'\0');
    const auto converted{MultiByteToWideChar(
        CP_UTF8,
        0,
        descriptionUtf8.c_str(),
        static_cast<int>(descriptionUtf8.size()),
        description.data(),
        wideLen)};
    if(converted <= 0){
        return {};
    }

    return decodeXmlEntities(move(description));
}

wstring getAppManifestVersionString(){
    try{
        const auto version{Package::Current().Id().Version()};
        return to_wstring(version.Major)
            + L"."
            + to_wstring(version.Minor)
            + L"."
            + to_wstring(version.Build)
            + L"."
            + to_wstring(version.Revision);
    }catch(...){
        return L"unknown";
    }
}

wstring getAppManifestDescriptionString(){
    try{
        const auto installedPath{filesystem::path{Package::Current().InstalledLocation().Path().c_str()}};
        return tryReadVisualElementsDescriptionFromManifestPath(installedPath / L"AppxManifest.xml");
    }catch(...){
        return {};
    }
}

bool IsAviH264StreamCopyCandidate(const com_ptr<IMFSourceReader>& reader, DWORD videoStreamIndex, com_ptr<IMFMediaType>& selectedVideoType, wstring& failureReason){
    if(!reader){
        failureReason = BuildUnsupportedAviReason(L"AVI is supported only for H.264 video. This file uses unknown codec.");
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
            failureReason = BuildUnsupportedAviReason(L"AVI is supported only for H.264 video. This file could not be inspected.");
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
            if(sampleTime100ns < previousTime100ns){
                failureReason = BuildUnsupportedAviReason(L"AVI file lacks usable timestamps for robust cutting; convert to MP4 first");
                return false;
            }
            if(sampleTime100ns - previousTime100ns > maxForwardJump100ns){
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

std::wstring readShellStringProperty(const std::wstring& filePath, const PROPERTYKEY& key){
    winrt::com_ptr<IPropertyStore> propertyStore;
    const auto hr{SHGetPropertyStoreFromParsingName(filePath.c_str(), nullptr, GPS_BESTEFFORT, IID_PPV_ARGS(propertyStore.put()))};
    if(FAILED(hr) || !propertyStore){
        return {};
    }

    PROPVARIANT value{};
    PropVariantInit(&value);

    std::wstring text;
    if(SUCCEEDED(propertyStore->GetValue(key, &value))){
        PWSTR converted{};
        if(SUCCEEDED(PropVariantToStringAlloc(value, &converted)) && converted){
            text = converted;
            ::CoTaskMemFree(converted);
        }
    }

    PropVariantClear(&value);
    return text;
}

}
constexpr auto P_FILE_PATH{L"file_path"};
constexpr auto P_STORYLINE_ZOOM{L"storyline_zoom"};
constexpr auto P_KEEP_AUDIO{L"keep_audio"};
constexpr auto P_AUDIO_CROSSFADE_MS{L"audio_crossfade_ms"};
constexpr auto P_RAP_MARKERS{L"rap_markers"};
constexpr auto P_CUT_SCENES{L"cut_scenes"};

constexpr auto PROJECT_KEY{L"llvc project"};
constexpr auto PROJECT_EXT{L".llvc"};

using CoreVirtualKeyStates = winrt::Windows::UI::Core::CoreVirtualKeyStates;
constexpr auto CORE_KEY_STATE_DOWN{static_cast<std::underlying_type_t<CoreVirtualKeyStates>>(CoreVirtualKeyStates::Down)};

bool isControlModifierActive(VirtualKeyModifiers modifiers){
    if((modifiers & VirtualKeyModifiers::Control) == VirtualKeyModifiers::Control){
        return true;
    }

    const auto state{InputKeyboardSource::GetKeyStateForCurrentThread(VirtualKey::Control)};
    return (static_cast<std::underlying_type_t<CoreVirtualKeyStates>>(state) & CORE_KEY_STATE_DOWN) != 0;
}

bool isModifierDown(VirtualKey key){
    const auto state{InputKeyboardSource::GetKeyStateForCurrentThread(key)};
    return (static_cast<std::underlying_type_t<CoreVirtualKeyStates>>(state) & CORE_KEY_STATE_DOWN) != 0;
}

wstring audioVolumeGlyph(bool keepAudio, int32_t volumePct){
    if(!keepAudio || volumePct <= 0){
        return L"\xD83D\xDD07"; // U+1F507
    }
    if(volumePct < 75){
        return L"\xD83D\xDD08"; // U+1F508
    }
    if(volumePct > 100){
        return L"\xD83D\xDD0A"; // U+1F50A
    }
    return L"\xD83D\xDD09"; // U+1F509
}

void MainWindow::clearUndoRedoHistory(){
    ::llvc::clearEditorHistory(m_editorHistory);
}

void MainWindow::refreshEditorUiState(){
    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();
    renderTimelineTicks();
    renderKeyframeTicks();
    renderCutOverlays();
    refreshVideoDetailsPanel();
    updateWindowTitle();
    refreshStatusInfoSection();
}

void MainWindow::applyEditorCommandResult(const ::llvc::EditorCommandResult& result){
    if(!result.changed){
        return;
    }

    if(result.refreshTimelineTicks){
        renderTimelineTicks();
    }
    if(result.refreshKeyframeTicks){
        renderKeyframeTicks();
    }
    if(result.refreshCutOverlays){
        renderCutOverlays();
    }
    if(result.refreshVideoDetails){
        refreshVideoDetailsPanel();
    }
    if(result.refreshWindowTitle){
        updateWindowTitle();
    }
}

bool MainWindow::undoLastEdit(){
    if(m_editorHistory.undoStack.empty()){
        setStatusMessage(L"Nothing to undo");
        return false;
    }

    const auto result{::llvc::undo(m_prj, m_editorHistory)};
    if(!result.changed){
        return false;
    }

    refreshEditorUiState();
    setStatusMessage(L"Undo applied");
    return true;
}

bool MainWindow::redoLastEdit(){
    if(m_editorHistory.redoStack.empty()){
        setStatusMessage(L"Nothing to redo");
        return false;
    }

    const auto result{::llvc::redo(m_prj, m_editorHistory)};
    if(!result.changed){
        return false;
    }

    refreshEditorUiState();
    setStatusMessage(L"Redo applied");
    return true;
}

MainWindow::MainWindow(const hstring& launchArguments){
    InitializeComponent();

    m_player = MediaPlayer();
    PreviewPlayer().SetMediaPlayer(m_player);
    updatePreviewPlaceholderVisibility();

    m_naturalDurationChangedRevoker = m_player.PlaybackSession().NaturalDurationChanged(auto_revoke, {this, &MainWindow::onNaturalDurationChanged});

    m_positionTimer = DispatcherTimer();
    m_positionTimer.Interval(chrono::milliseconds(80));
    m_positionTimerTickToken = m_positionTimer.Tick({this, &MainWindow::onPositionTimerTick});
    m_positionTimer.Start();

    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();
    refreshVideoDetailsPanel();
    setVideoDetailsPanelExpanded(false);

    restoreWindowPlacement();
    const auto hwnd{getWindowHandle()};
    SetWindowSubclass(hwnd, MainWindowSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    loadAppSettings();
    applyThemePreference();
    const auto initialZoomIndex{clampTimelineZoomIndex(TimelineZoomSlider(), m_appSettings.timelineZoom)};
    m_appSettings.timelineZoom = initialZoomIndex;
    m_prj.setZoomWithoutDirty(initialZoomIndex);
    TimelineZoomSlider().Value(initialZoomIndex);
    m_mainWindowClosedRevoker = Closed(auto_revoke, {this, &MainWindow::onClosed});
    m_mainWindowActivatedRevoker = Activated(auto_revoke, {this, &MainWindow::onWindowActivated});
    refreshRecentVideosMenu();
    refreshRecentProjectsMenu();
    updateWindowTitle();
    refreshStatusInfoSection();
    m_isUiReadyForEvents = true;

    if(m_appSettings.restorePreviewDetachedOnStartup){
        m_pendingSeparatePreviewRestoreOnStartup = true;
    }

    if(!launchArguments.empty()){
        (void)openFromLaunchArgumentsAsync(launchArguments);
    }
}

AAction MainWindow::openFromLaunchArgumentsAsync(const hstring& arguments){
    wstring launchPath{arguments.c_str()};
    if(launchPath.empty()){
        co_return;
    }

    const auto begin{launchPath.find_first_not_of(L" \t\r\n")};
    if(begin == wstring::npos){
        co_return;
    }
    const auto end{launchPath.find_last_not_of(L" \t\r\n")};
    launchPath = launchPath.substr(begin, end - begin + 1);

    if(launchPath.size() >= 2 && launchPath.front() == L'"' && launchPath.back() == L'"'){
        launchPath = launchPath.substr(1, launchPath.size() - 2);
    }

    wstring lowerExt{std::filesystem::path(launchPath).extension().wstring()};
    transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), ::towlower);

    if(lowerExt != PROJECT_EXT && !isSupportedMediaPath(launchPath)){
        setStatusMessage(L"Launch argument is not a supported media/project file");
        co_return;
    }

    SFile file{nullptr};
    try{
        file = co_await SFile::GetFileFromPathAsync(launchPath);
    }catch(const winrt::hresult_error&){
        setStatusMessage(L"File from launch argument was not found");
        co_return;
    }

    if(lowerExt == PROJECT_EXT){
        co_await openProjectFileAsync(file);
        co_return;
    }

    m_prj.clearTimeline();
    clearUndoRedoHistory();
    co_await loadVideoFileAsync(file);
}

HWND MainWindow::getWindowHandle() const{
    HWND hwnd{};
    const auto projected{const_cast<MainWindow*>(this)->get_strong()};
    check_hresult(projected.as<::IWindowNative>()->get_WindowHandle(&hwnd));
    return hwnd;
}


void MainWindow::restoreWindowPlacement(){
    const auto hwnd{getWindowHandle()};
    (void)::llvc::restoreWindowPlacementFromSettings(hwnd, hwnd, false);
}

void MainWindow::loadAppSettings(){
    m_appSettings = ::llvc::loadAppSettings();
}

std::shared_ptr<AudioWaveformCacheEntry> getOrCreateAudioWaveformEntry(const std::wstring& sourcePath){
    std::scoped_lock lock{g_audioWaveformCacheMutex};
    auto& entry{g_audioWaveformCache[sourcePath]};
    if(!entry){
        entry = std::make_shared<AudioWaveformCacheEntry>();
    }
    return entry;
}

void MainWindow::saveAppSettings() const{
    ::llvc::saveAppSettings(m_appSettings);
}

ElementTheme MainWindow::requestedElementTheme() const noexcept{
    switch(m_appSettings.appThemeMode){
    case ::llvc::AppThemeMode::Light:
        return ElementTheme::Light;
    case ::llvc::AppThemeMode::Dark:
        return ElementTheme::Dark;
    default:
        return ElementTheme::Default;
    }
}

void MainWindow::applyThemePreference(){
    const auto theme{requestedElementTheme()};
    if(RootGrid()){
        RootGrid().RequestedTheme(theme);
    }

    if(m_separatePreview.window){
        if(const auto previewRoot{m_separatePreview.window.Content().try_as<FrameworkElement>()}; previewRoot){
            previewRoot.RequestedTheme(theme);
        }
    }
}

void MainWindow::saveWindowPlacement() const{
    ::llvc::saveWindowPlacementToSettings(getWindowHandle());
}

void MainWindow::onClosed(const Control&, const WEArgs&){
    m_isClosing = true;
    m_appSettings.timelineZoom = clampTimelineZoomIndex(TimelineZoomSlider(), TimelineZoomSlider().Value());

    const auto hwnd{getWindowHandle()};
    RemoveWindowSubclass(hwnd, MainWindowSubclassProc, 1);

    if(m_positionTimer){
        m_positionTimer.Tick(m_positionTimerTickToken);
        m_positionTimer.Stop();
        m_positionTimer = nullptr;
    }

    m_naturalDurationChangedRevoker.revoke();
    m_mainWindowActivatedRevoker.revoke();
    m_mainWindowClosedRevoker.revoke();

    m_appSettings.restorePreviewDetachedOnStartup = m_separatePreview.isOpen;
    m_appSettings.restorePreviewFullscreenOnStartup = m_separatePreview.isOpen && m_separatePreview.isFullscreen;

    if(m_separatePreview.player){
        m_separatePreview.player.SetMediaPlayer(nullptr);
        m_separatePreview.player = nullptr;
    }

    if(PreviewPlayer()){
        PreviewPlayer().SetMediaPlayer(nullptr);
    }

    if(m_player){
        m_player.Pause();
        m_player.Source(nullptr);
        m_player = nullptr;
    }

    if(m_separatePreview.window){
        HWND previewHwnd{};
        if(SUCCEEDED(m_separatePreview.window.as<::IWindowNative>()->get_WindowHandle(&previewHwnd)) && previewHwnd){
            saveSeparatePreviewPlacement(previewHwnd);
        }

        m_separatePreview.closedRevoker.revoke();
        m_separatePreview.window.Close();
        m_separatePreview.window = nullptr;
    }

    m_separatePreview.splashImage = nullptr;
    saveWindowPlacement();
    saveAppSettings();
}

bool MainWindow::isExportInProgressForClosePrompt() const{
    return m_isExportInProgress;
}

void MainWindow::requestExportCancel(){
    m_cancelExportRequested = true;
    setStatusMessage(L"Canceling export...");
    setOperationInProgress(false);
}

void MainWindow::onWindowActivated(const Control&, const WAVArgs& args){
    if(args.WindowActivationState() == WindowActivationState::Deactivated){
        return;
    }

    const auto restoreSeparatePreviewOnStartup{exchange(m_pendingSeparatePreviewRestoreOnStartup, false)};
    const auto weakThis{get_weak()};
    DispatcherQueue().TryEnqueue([weakThis, restoreSeparatePreviewOnStartup]{
        if(const auto self{weakThis.get()}){
            if(restoreSeparatePreviewOnStartup && !self->setSeparatePreviewWindowOpen(true)){
                self->m_appSettings.restorePreviewDetachedOnStartup = false;
                self->m_appSettings.restorePreviewFullscreenOnStartup = false;
                self->saveAppSettings();
            }
            self->tryFocusTimelineCanvas(FocusState::Programmatic);
        }
    });
}

void MainWindow::startButton_Click(const Control&, const REArgs&){
    playPreviewFromCurrentPosition();
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


bool MainWindow::reevaluateAll(bool pushUndoState){
    if(!m_prj.hasVideoFile() || m_timelineDurationSeconds <= 0){
        setStatusMessage(L"Load a video before reevaluating clear cut markers");
        return false;
    }

    const auto originalMarkers{m_prj.frameIndex()};
    if(originalMarkers.empty()){
        setStatusMessage(L"No clear cut markers to reevaluate");
        return false;
    }

    vector<int64_t> rapTimes100ns;
    auto markerTimes100ns{::llvc::buildRapLookupTimesForExportAlignment(m_prj, m_prj.timelineDuration100ns())};
    if(markerTimes100ns.empty()){
        markerTimes100ns.reserve(originalMarkers.size());
        for(const auto& marker: originalMarkers){
            if(marker.time100ns > 0 && marker.time100ns < m_prj.timelineDuration100ns()){
                markerTimes100ns.push_back(marker.time100ns);
            }
        }
        sort(markerTimes100ns.begin(), markerTimes100ns.end());
        markerTimes100ns.erase(unique(markerTimes100ns.begin(), markerTimes100ns.end()), markerTimes100ns.end());
    }

    if(!tryGetRapTimes100ns(rapTimes100ns, !markerTimes100ns.empty(), &markerTimes100ns)){
        m_pendingReevaluateWithoutUndoAfterRapLookup = !pushUndoState;
        queueRapLookup(true, 0);
        return false;
    }

    const auto sourcePath{m_prj.videoFilePath()};
    const auto beforeSnapshot{::llvc::captureEditorSnapshot(m_prj)};
    if(m_lastReevaluatedEditorSnapshot
        && sourcePath == m_lastReevaluatedRapSourcePath
        && ::llvc::isSameEditorSnapshot(beforeSnapshot, *m_lastReevaluatedEditorSnapshot)){
        m_pendingReevaluateWithoutUndoAfterRapLookup = false;
        setStatusMessage(L"Cut markers are already reevaluated for the current RAP scan");
        return true;
    }

    const auto result{::llvc::reevaluateClearCutMarkers(m_prj, m_tl, rapTimes100ns, m_editorHistory, pushUndoState)};
    m_lastReevaluatedEditorSnapshot = ::llvc::captureEditorSnapshot(m_prj);
    m_lastReevaluatedRapSourcePath = sourcePath;
    m_cachedEffectiveExportPlanSnapshot = m_lastReevaluatedEditorSnapshot;
    m_cachedEffectiveExportPlanSourcePath = sourcePath;
    m_cachedEffectiveExportPlan = m_prj.buildEffectiveExportPlanWithRapPreroll(m_prj.timelineDuration100ns(), rapTimes100ns);

    renderTimelineTicks();
    renderKeyframeTicks();
    renderCutOverlays();
    refreshVideoDetailsPanel();
    updateWindowTitle();

    wstring status{L"Reevaluated cut markers against RAP frames"};
    m_pendingReevaluateWithoutUndoAfterRapLookup = false;
    if(result.replacedCount == 0){
        status += L" (no changes needed)";
    }else{
        status += std::format(L" (updated {})", result.replacedCount);
    }
    setStatusMessage(status);
    return true;
}

bool MainWindow::reevaluateClearCutMarkers(bool pushUndoState){
    return reevaluateAll(pushUndoState);
}

void MainWindow::reevaluateClearCutMarkersButton_Click(const Control&, const REArgs&){
    (void)reevaluateClearCutMarkers(true);
}

void MainWindow::timelineZoomSlider_ValueChanged(const Control&, const RBVArgs&){
    m_prj.setZoom(TimelineZoomSlider().Value());
    if(!m_isUiReadyForEvents){
        return;
    }

    if(!m_timelineInteraction.pendingWheelZoomAnchor && !m_timelineInteraction.pendingScrollbarAnchor){
        const auto bar{TimelineHorizontalScrollBar()};
        if(bar){
            m_timelineInteraction.pendingScrollbarAnchor = ::llvc::TimelineScrollbarAnchor::capture(bar.Value(), max(0.0, bar.Maximum()));
        }
    }

    if(m_prj.hasVideoFile() && m_timelineDurationSeconds > 0){
        renderTimelineAsync();
    }
    updateWindowTitle();
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

void MainWindow::timelineZoomSlider_PointerWheelChanged(const Control&, const PREArgs& args){
    const auto point{args.GetCurrentPoint(TimelineZoomSlider())};
    const auto delta{point.Properties().MouseWheelDelta()};
    if(delta == 0){
        return;
    }

    adjustTimelineZoomBy(delta > 0 ? 1 : -1);
    args.Handled(true);
}

void MainWindow::audioWaveformThresholdSlider_ValueChanged(const Control&, const RBVArgs& args){
    m_audioWaveformThresholdDb = std::clamp(args.NewValue(), -60.0, -3.0);
    const auto thresholdText{AudioWaveformThresholdValueText()};
    if(thresholdText){
        thresholdText.Text(formatWaveformThresholdDb(m_audioWaveformThresholdDb));
    }
    renderAudioWaveform();
}

void MainWindow::keepAudioCheckBox_Changed(const Control&, const REArgs&){
    if(m_editorHistory.isApplying){
        return;
    }
    (void)::llvc::pushUndoSnapshotIfChanged(m_prj, m_editorHistory);
    m_prj.keepAudio(KeepAudioCheckBox().IsChecked().GetBoolean());
    updateAudioUiAndPlaybackState();
    updateWindowTitle();
    refreshStatusInfoSection();
}

void MainWindow::audioCrossfadeComboBox_SelectionChanged(const Control&, const Control&){
    if(m_editorHistory.isApplying){
        return;
    }

    const auto selected{AudioCrossfadeComboBox().SelectedItem().try_as<Controls::ComboBoxItem>()};
    if(!selected || !selected.Tag()){
        return;
    }

    try{
        (void)::llvc::pushUndoSnapshotIfChanged(m_prj, m_editorHistory);
        const auto val{stoi(wstring(unbox_value<hstring>(selected.Tag())).c_str())};
        m_prj.audioXfadeMs(val);
    } catch(...){
        (void)::llvc::pushUndoSnapshotIfChanged(m_prj, m_editorHistory);
        m_prj.audioXfadeMs(0);
    }

    syncAudioCrossfadeComboSelection();
    updateWindowTitle();
    refreshStatusInfoSection();
}

void MainWindow::audioVolumeSlider_ValueChanged(const Control&, const RBVArgs& args){
    if(!m_player){
        return;
    }

    if(m_editorHistory.isApplying){
        return;
    }

    (void)::llvc::pushUndoSnapshotIfChanged(m_prj, m_editorHistory);
    m_prj.audioVolumePct(static_cast<int32_t>(lround(args.NewValue())));
    updateAudioUiAndPlaybackState();
    if(m_prj.keepAudio() && m_prj.audioVolumePct() > 100){
        setStatusMessage(L"Preview audio is capped at 100%; export still uses the full configured boost");
    }
    updateWindowTitle();
    refreshStatusInfoSection();
}

void MainWindow::timelineHorizontalScrollBar_ValueChanged(const Control&, const RBVArgs& args){
    if(m_isClosing || m_isSyncingTimelineScrollBar){
        return;
    }

    const auto scrollViewer{TimelineScrollViewer()};
    const auto offset{scrollViewer.HorizontalOffset()};
    if(fabs(offset - args.NewValue()) > 0.5){
        const auto targetOffset{box_value(args.NewValue()).as<IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }

    updateTimelineCursorFromViewportOffset(args.NewValue());
}

void MainWindow::timelineScrollViewer_ViewChanged(const Control&, const SVVCArgs&){
    syncTimelineHorizontalScrollBar();
    renderAudioWaveform();
    if(sourceHasAudio() && !m_audioWaveformAnalysisQueued){
        m_audioWaveformAnalysisQueued = true;
        ensureAudioWaveformAsync();
    }

    if(m_isExportInProgress && m_prj.hasVideoFile() && m_timelineDurationSeconds > 0){
        renderTimelineAsync();
    }else if(m_hasTimelineRenderCompleted){
        queueTimelineViewportRender();
    }
}

void MainWindow::timelineScrollViewer_SizeChanged(const Control&, const SCArgs&){
    syncTimelineHorizontalScrollBar();
    if(m_hasTimelineRenderCompleted){
        queueTimelineViewportRender();
    }
}

void MainWindow::timelineScrollViewer_PointerWheelChanged(const Control&, const PREArgs& args){
    if(!isControlModifierActive(args.KeyModifiers())){
        return;
    }

    const auto point{args.GetCurrentPoint(TimelineScrollViewer())};
    const auto delta{point.Properties().MouseWheelDelta()};
    if(delta == 0){
        return;
    }

    if(m_prj.timelineDuration100ns() > 0 && TimelineCanvas().Width() > 0){
        const auto pointerXOnCanvas{point.Position().X + TimelineScrollViewer().HorizontalOffset()};
        if(const auto time100ns{timelinePointToTime100ns(pointerXOnCanvas, TimelineCanvas().Width())}){
            m_timelineInteraction.pendingWheelZoomAnchor = ::llvc::TimelinePointerZoomAnchor{
                .time100ns = *time100ns,
                .viewportPointerX = point.Position().X,
            };
        }
    }

    adjustTimelineZoomBy(delta > 0 ? 1 : -1);
    args.Handled(true);
}

void MainWindow::timelineCanvas_PointerPressed(const Control&, const PREArgs& e){
    if(m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    if(!point.Properties().IsLeftButtonPressed()){
        return;
    }

    m_timelineInteraction.isDragging = true;
    m_timelineInteraction.dragMoved = false;
    m_timelineInteraction.dragPointerId = e.Pointer().PointerId();
    m_timelineInteraction.dragStartX = point.Position().X;
    m_timelineInteraction.dragStartOffset = TimelineScrollViewer().HorizontalOffset();

    tryFocusTimelineCanvas(FocusState::Programmatic);
    TimelineCanvas().CapturePointer(e.Pointer());
    e.Handled(true);
}

void MainWindow::timelineCanvas_PointerMoved(const Control&, const PREArgs& e){
    if(!m_timelineInteraction.isDragging || e.Pointer().PointerId() != m_timelineInteraction.dragPointerId){
        return;
    }

    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    const auto deltaX{point.Position().X - m_timelineInteraction.dragStartX};

    if(fabs(deltaX) > 4.0){
        m_timelineInteraction.dragMoved = true;
    }

    const auto scrollViewer{TimelineScrollViewer()};
    const auto viewportWidth{scrollViewer.ViewportWidth()};
    const auto target{m_tl.dragTargetOffset(m_timelineInteraction.dragStartOffset, deltaX, TimelineCanvas().Width(), viewportWidth)};
    const auto targetOffset{box_value(target).as<IReference<double>>()};
    scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    e.Handled(true);
}

std::optional<int64_t> MainWindow::timelinePointToTime100ns(double pointerX, double width) const{
    return m_tl.pointToTime100ns(pointerX, width, m_prj.timelineDuration100ns());
}

bool MainWindow::toggleSelectedKeyframeAtTime100ns(int64_t time100ns){
    const auto result{::llvc::executeToggleMarkerCommand(m_prj, m_mediaInfo.frameRate, m_editorHistory, time100ns)};
    if(!result.changed){
        return false;
    }

    applyEditorCommandResult(result);

    if(m_appSettings.autoReevaluateCutMarkersOnPlacement && result.markerCountIncreased){
        (void)evaluatePlacedMarkerAtTime100ns(time100ns);
    }
    return true;
}

bool MainWindow::evaluatePlacedMarkerAtTime100ns(int64_t time100ns){
    if(!m_prj.hasVideoFile() || m_timelineDurationSeconds <= 0){
        return false;
    }

    vector<int64_t> rapTimes100ns;
    vector<int64_t> markerTimes100ns{time100ns};
    if(!tryGetRapTimes100ns(rapTimes100ns, true, &markerTimes100ns)){
        if(find(m_pendingAutoEvaluateMarkerTimes100ns.begin(), m_pendingAutoEvaluateMarkerTimes100ns.end(), time100ns) == m_pendingAutoEvaluateMarkerTimes100ns.end()){
            m_pendingAutoEvaluateMarkerTimes100ns.push_back(time100ns);
        }
        queueRapLookup(false, 0);
        return false;
    }

    const auto result{::llvc::evaluatePlacedMarkerAgainstRap(m_prj, rapTimes100ns, time100ns)};
    if(!result.changed){
        return false;
    }

    m_lastReevaluatedEditorSnapshot.reset();
    m_lastReevaluatedRapSourcePath.clear();
    m_cachedEffectiveExportPlanSnapshot.reset();
    m_cachedEffectiveExportPlanSourcePath.clear();
    m_cachedEffectiveExportPlan.reset();
    renderTimelineTicks();
    renderKeyframeTicks();
    renderCutOverlays();
    refreshVideoDetailsPanel();
    updateWindowTitle();
    refreshStatusInfoSection();
    return true;
}

fire_and_forget MainWindow::toggleSelectedKeyframeAtTime100nsAsync(int64_t time100ns){
    (void)toggleSelectedKeyframeAtTime100ns(time100ns);
    co_return;
}


void MainWindow::timelineCanvas_PointerReleased(const Control&, const PREArgs& e){
    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    if(point.Properties().PointerUpdateKind() == PointerUpdateKind::RightButtonReleased){
        if(const auto time100ns{timelinePointToTime100ns(point.Position().X, TimelineCanvas().Width())}){
            (void)toggleSelectedKeyframeAtTime100nsAsync(*time100ns);
            e.Handled(true);
            return;
        }
    }

    if(!m_timelineInteraction.isDragging || e.Pointer().PointerId() != m_timelineInteraction.dragPointerId){
        return;
    }
    const auto dragged{m_timelineInteraction.dragMoved};

    m_timelineInteraction.isDragging = false;
    m_timelineInteraction.dragMoved = false;
    TimelineCanvas().ReleasePointerCapture(e.Pointer());

    if(!dragged){
        if(isControlModifierActive(e.KeyModifiers())){
            if(const auto time100ns{timelinePointToTime100ns(point.Position().X, TimelineCanvas().Width())}){
                toggleCutBlockAtTime100ns(*time100ns);
            }
        }else{
            seekTimelineToCanvasX(point.Position().X);
        }
    }

    e.Handled(true);
}

void MainWindow::timelineCanvas_PointerCanceled(const Control&, const PREArgs& e){
    if(m_timelineInteraction.isDragging && e.Pointer().PointerId() == m_timelineInteraction.dragPointerId){
        m_timelineInteraction.isDragging = false;
        m_timelineInteraction.dragMoved = false;
        TimelineCanvas().ReleasePointerCapture(e.Pointer());
        e.Handled(true);
    }
}

void MainWindow::timelineCanvas_PointerCaptureLost(const Control&, const PREArgs&){
    m_timelineInteraction.isDragging = false;
    m_timelineInteraction.dragMoved = false;
}

void MainWindow::timelineCanvas_Loaded(const Control&, const REArgs&){
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

void MainWindow::timelineTickCanvas_PointerReleased(const Control&, const PREArgs& e){
    const auto point{e.GetCurrentPoint(TimelineTickCanvas())};
    if(point.Properties().PointerUpdateKind() == PointerUpdateKind::RightButtonReleased){
        if(const auto time100ns{timelinePointToTime100ns(point.Position().X, TimelineTickCanvas().Width())}; time100ns && toggleSelectedKeyframeAtTime100ns(*time100ns)){
            tryFocusTimelineCanvas(FocusState::Programmatic);
            e.Handled(true);
            return;
        }
    }

    if(point.Properties().PointerUpdateKind() == PointerUpdateKind::LeftButtonReleased){
        if(isControlModifierActive(e.KeyModifiers())){
            if(const auto time100ns{timelinePointToTime100ns(point.Position().X, TimelineTickCanvas().Width())}; time100ns && toggleCutBlockAtTime100ns(*time100ns)){
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
    m_prj.timelineDuration100ns(max<int64_t>(0, duration.count()));

    if(m_prj.hasVideoFile() && m_timelineDurationSeconds > 0){
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
        return;
    }

    setOperationInProgress(false);
}

void MainWindow::onPositionTimerTick(const Control&, const Control&){
    if(m_isClosing){
        return;
    }

    (void)trySkipCurrentCutDuringPlayback();
    if(m_player && m_player.PlaybackSession().PlaybackState() == MediaPlaybackState::Playing){
        updateTimelineCursorFromPlayback();
    }
}

void MainWindow::updateTimelineCursorFromPlayback(){
    if(m_isClosing || !m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    updateTimelineCursorFromPosition(max<int64_t>(0, m_player.PlaybackSession().Position().count()));
}

void MainWindow::updateTimelineCursorFromPosition(int64_t position100ns){
    if(m_isClosing || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto seconds{max(0.0, position100ns / 10'000'000.0)};
    const auto ratio{clamp(seconds / m_timelineDurationSeconds, 0.0, 1.0)};
    const auto left{ratio * TimelineCanvas().Width()};
    Controls::Canvas::SetLeft(TimelineCursor(), left);

    if(m_player.PlaybackSession().PlaybackState() == MediaPlaybackState::Playing){
        ensureTimelineCursorVisible(left);
    }

    syncTimelineHorizontalScrollBar();
}

int64_t MainWindow::currentNavigationTime100ns(){
    if(const auto cursor100ns{timelinePointToTime100ns(Controls::Canvas::GetLeft(TimelineCursor()), TimelineCanvas().Width())}){
        return max<int64_t>(0, *cursor100ns);
    }
    if(m_player){
        return max<int64_t>(0, m_player.PlaybackSession().Position().count());
    }
    return 0;
}

void MainWindow::syncTimelineHorizontalScrollBar(){
    const auto scrollViewer{TimelineScrollViewer()};
    scrollViewer.UpdateLayout();

    const auto viewportWidth{max(1.0, scrollViewer.ViewportWidth())};
    const auto extentWidth{max(viewportWidth, scrollViewer.ExtentWidth())};
    const auto scrollableWidth{max(0.0, extentWidth - viewportWidth)};

    auto bar{TimelineHorizontalScrollBar()};
    const auto currentValue{bar.Value()};
    const auto clampedCurrentValue{clamp(currentValue, 0.0, scrollableWidth)};
    if(fabs(currentValue - clampedCurrentValue) > 0.5){
        m_isSyncingTimelineScrollBar = true;
        bar.Value(clampedCurrentValue);
        m_isSyncingTimelineScrollBar = false;
    }

    bar.Minimum(0.0);
    bar.Maximum(scrollableWidth);
    bar.LargeChange(max(32.0, viewportWidth * 0.8));
    bar.SmallChange(24.0);
    bar.IsEnabled(scrollableWidth > 0.0);
    bar.Visibility(Visibility::Visible);

    const auto offset{clamp(scrollViewer.HorizontalOffset(), 0.0, scrollableWidth)};
    if(fabs(clampedCurrentValue - offset) > 0.5){
        m_isSyncingTimelineScrollBar = true;
        bar.Value(offset);
        m_isSyncingTimelineScrollBar = false;
    }
}

void MainWindow::updateTimelineCursorFromViewportOffset(double offset){
    const auto width{TimelineCanvas().Width()};
    const auto viewportWidth{TimelineScrollViewer().ViewportWidth()};
    if(width <= 0 || m_timelineDurationSeconds <= 0){
        return;
    }

    const auto safeViewportWidth{max(0.0, viewportWidth)};
    const auto maxOffset{max(0.0, width - safeViewportWidth)};
    const auto clampedOffset{clamp(offset, 0.0, maxOffset)};
    const auto viewportRatio{maxOffset > 0.0 ? clamp(clampedOffset / maxOffset, 0.0, 1.0) : 0.5};
    const auto clampedCanvasX{clamp(clampedOffset + (viewportRatio * safeViewportWidth), 0.0, width)};
    const auto target100nsOpt{timelinePointToTime100ns(clampedCanvasX, width)};
    if(!target100nsOpt){
        return;
    }

    const auto target100ns{*target100nsOpt};
    if(m_player){
        m_player.PlaybackSession().Position(TimeSpan{target100ns});
        updateTimelineCursorFromPosition(target100ns);
        return;
    }

    Controls::Canvas::SetLeft(TimelineCursor(), clampedCanvasX);
    syncTimelineHorizontalScrollBar();
}

void MainWindow::seekTimelineToCanvasX(double pointerX){
    if(!m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto duration100ns{m_prj.timelineDuration100ns()};
    const auto target100nsOpt{m_tl.pointToTime100ns(pointerX, TimelineCanvas().Width(), duration100ns)};
    if(!target100nsOpt){
        return;
    }
    const auto target100ns{*target100nsOpt};

    const auto session{m_player.PlaybackSession()};
    const auto wasPlaying{session.PlaybackState() == MediaPlaybackState::Playing};
    session.Position(TimeSpan{target100ns});
    if(!wasPlaying){
        try{
            const auto frameStep100ns{m_tl.frameStep100ns(m_mediaInfo.frameRate.num, m_mediaInfo.frameRate.den)};
            if(target100ns + frameStep100ns < duration100ns){
                m_player.StepForwardOneFrame();
                m_player.StepBackwardOneFrame();
            }else if(target100ns >= frameStep100ns){
                m_player.StepBackwardOneFrame();
                m_player.StepForwardOneFrame();
            }
        }catch(...){
        }
    }
    updateTimelineCursorFromPosition(target100ns);
}

void MainWindow::ensureTimelineCursorVisible(double cursorLeft){
    const auto scrollViewer{TimelineScrollViewer()};
    if(!scrollViewer || !TimelineCanvas()){
        return;
    }

    const auto currentOffset{scrollViewer.HorizontalOffset()};
    const auto viewportWidth{scrollViewer.ViewportWidth()};

    if(viewportWidth <= 0){
        return;
    }

    if(const auto targetOffsetValue{m_tl.cursorOffsetToEnsureVisible(cursorLeft, currentOffset, viewportWidth, TimelineCanvas().Width())}){
        const auto targetOffset{box_value(*targetOffsetValue).as<IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }
}

void MainWindow::ensureCurrentTimelineCursorVisible(){
    if(!TimelineCursor() || !TimelineScrollViewer() || !TimelineCanvas()){
        return;
    }

    const auto cursorLeft{Controls::Canvas::GetLeft(TimelineCursor())};
    ensureTimelineCursorVisible(cursorLeft);
    syncTimelineHorizontalScrollBar();
}

void MainWindow::renderTimelineTicks(){
    const auto width{TimelineCanvas().Width()};
    if(m_timelineDurationSeconds <= 0 || width <= 0){
        TimelineTickCanvas().Children().Clear();
        TimelineTickCanvas().Width(width);
        refreshStatusInfoSection();
        return;
    }

    const auto visibleWidth{max(1.0, TimelineScrollViewer().ViewportWidth())};
    ::llvc::renderTimelineMajorTicks(m_tl, TimelineTickCanvas(), width, m_timelineDurationSeconds, visibleWidth);
}

void MainWindow::renderKeyframeTicks(){
    if(m_prj.frameIndex().empty() || m_timelineDurationSeconds <= 0 || TimelineTickCanvas().Width() <= 0){
        return;
    }

    ::llvc::renderTimelineKeyframeTicks(TimelineTickCanvas(), m_prj.frameIndex(), m_prj.timelineDuration100ns());
}

void MainWindow::renderCutOverlays(){
    const auto width{TimelineCanvas().Width()};
    if(m_timelineDurationSeconds <= 0 || width <= 0){
        CutOverlayLayer().Children().Clear();
        CutOverlayLayer().Width(width);
        refreshStatusInfoSection();
        return;
    }

    ::llvc::renderTimelineCutOverlays(m_tl, CutOverlayLayer(), m_prj.buildCutRanges100ns(), width, m_timelineDurationSeconds);

    refreshStatusInfoSection();
}


bool MainWindow::toggleCutBlockAtTime100ns(int64_t time100ns){
    const auto result{::llvc::executeToggleCutBlockCommand(m_prj, m_editorHistory, time100ns)};
    if(!result.changed){
        return false;
    }

    applyEditorCommandResult(result);
    return true;
}

bool MainWindow::setCutBlockAtTime100ns(int64_t time100ns, bool cutScene){
    const auto result{::llvc::executeSetCutBlockCommand(m_prj, m_editorHistory, time100ns, cutScene)};
    if(!result.changed){
        return false;
    }

    applyEditorCommandResult(result);
    return true;
}

bool MainWindow::toggleCutMarkerAtCursor(){
    if(const auto time100ns{timelinePointToTime100ns(Controls::Canvas::GetLeft(TimelineCursor()), TimelineTickCanvas().Width())}){
        (void)toggleSelectedKeyframeAtTime100nsAsync(*time100ns);
        return true;
    }
    return false;
}

bool MainWindow::markSceneAtCursor(bool cutScene){
    if(const auto time100ns{timelinePointToTime100ns(Controls::Canvas::GetLeft(TimelineCursor()), TimelineCanvas().Width())}){
        return setCutBlockAtTime100ns(*time100ns, cutScene);
    }
    return false;
}

bool MainWindow::nudgeCurrentSceneBoundaryToNearestRap(bool expandScene){
    if(!m_prj.hasVideoFile() || m_timelineDurationSeconds <= 0){
        return false;
    }

    vector<int64_t> rapTimes100ns;
    if(!tryGetRapTimes100ns(rapTimes100ns)){
        queueRapLookup(false, expandScene ? 1 : -1);
        return false;
    }

    const auto cursorTime100ns{timelinePointToTime100ns(Controls::Canvas::GetLeft(TimelineCursor()), TimelineCanvas().Width())};
    if(!cursorTime100ns){
        return false;
    }
    const auto result{::llvc::executeRapNudgeCommand(m_prj, m_tl, rapTimes100ns, m_editorHistory, *cursorTime100ns, expandScene)};
    if(!result.changed){
        return false;
    }

    applyEditorCommandResult(result);
    return true;
}



bool MainWindow::trySkipCurrentCutDuringPlayback(){
    if(!m_player || m_prj.cutScenes().empty() || m_timelineDurationSeconds <= 0){
        return false;
    }

    const auto state{m_player.PlaybackSession().PlaybackState()};
    if(state != MediaPlaybackState::Playing){
        return false;
    }

    const auto cutRanges100ns{m_prj.buildCutRanges100ns()};
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
    if(delta == 0){
        return;
    }

    const auto durationFromProject100ns{m_prj.timelineDuration100ns()};
    const auto durationFromTimeline100ns{static_cast<int64_t>(max(0.0, m_timelineDurationSeconds) * Timeline::HnsPerSecond)};
    const auto duration100ns{max(durationFromProject100ns, durationFromTimeline100ns)};
    if(duration100ns <= 0){
        return;
    }

    const auto current{currentNavigationTime100ns()};

    const auto frameStep100ns{m_tl.frameStep100ns(m_mediaInfo.frameRate.num, m_mediaInfo.frameRate.den)};
    const auto target{m_tl.applyFrameStep(current, delta, duration100ns, frameStep100ns)};

    if(m_player){
        m_player.PlaybackSession().Position(TimeSpan{target});
        updateTimelineCursorFromPosition(target);
        ensureCurrentTimelineCursorVisible();
        return;
    }

    const auto width{TimelineCanvas().Width()};
    if(width <= 0){
        return;
    }

    const auto left{m_tl.timeToCanvasX(target, duration100ns, width)};
    Controls::Canvas::SetLeft(TimelineCursor(), left);
    ensureTimelineCursorVisible(left);
    syncTimelineHorizontalScrollBar();
}

bool MainWindow::seekBySeconds(int deltaSeconds){
    if(deltaSeconds == 0){
        return false;
    }

    const auto durationFromProject100ns{m_prj.timelineDuration100ns()};
    const auto durationFromTimeline100ns{static_cast<int64_t>(max(0.0, m_timelineDurationSeconds) * Timeline::HnsPerSecond)};
    const auto duration100ns{max(durationFromProject100ns, durationFromTimeline100ns)};
    if(duration100ns <= 0){
        return false;
    }

    const auto current100ns{currentNavigationTime100ns()};

    const auto delta100ns{static_cast<int64_t>(deltaSeconds) * HNS_PER_SECOND};
    const auto target100ns{clamp(current100ns + delta100ns, int64_t{0}, duration100ns)};

    if(m_player){
        m_player.PlaybackSession().Position(TimeSpan{target100ns});
        updateTimelineCursorFromPosition(target100ns);
        ensureCurrentTimelineCursorVisible();
        return true;
    }

    const auto width{TimelineCanvas().Width()};
    if(width <= 0){
        return false;
    }

    const auto left{m_tl.timeToCanvasX(target100ns, duration100ns, width)};
    Controls::Canvas::SetLeft(TimelineCursor(), left);
    ensureTimelineCursorVisible(left);
    syncTimelineHorizontalScrollBar();
    return true;
}

bool MainWindow::seekBySeconds(double deltaSeconds){
    if(deltaSeconds == 0.0){
        return false;
    }

    const auto durationFromProject100ns{m_prj.timelineDuration100ns()};
    const auto durationFromTimeline100ns{static_cast<int64_t>(max(0.0, m_timelineDurationSeconds) * Timeline::HnsPerSecond)};
    const auto duration100ns{max(durationFromProject100ns, durationFromTimeline100ns)};
    if(duration100ns <= 0){
        return false;
    }

    const auto current100ns{currentNavigationTime100ns()};

    const auto delta100ns{static_cast<int64_t>(llround(deltaSeconds * static_cast<double>(HNS_PER_SECOND)))};
    const auto target100ns{clamp(current100ns + delta100ns, int64_t{0}, duration100ns)};

    if(m_player){
        m_player.PlaybackSession().Position(TimeSpan{target100ns});
        updateTimelineCursorFromPosition(target100ns);
        ensureCurrentTimelineCursorVisible();
        return true;
    }

    const auto width{TimelineCanvas().Width()};
    if(width <= 0){
        return false;
    }

    const auto left{m_tl.timeToCanvasX(target100ns, duration100ns, width)};
    Controls::Canvas::SetLeft(TimelineCursor(), left);
    ensureTimelineCursorVisible(left);
    syncTimelineHorizontalScrollBar();
    return true;
}

bool MainWindow::jumpToTimelinePercent(uint32_t percent){
    if(percent > 100){
        return false;
    }

    const auto durationFromProject100ns{m_prj.timelineDuration100ns()};
    const auto durationFromTimeline100ns{static_cast<int64_t>(max(0.0, m_timelineDurationSeconds) * Timeline::HnsPerSecond)};
    const auto duration100ns{max(durationFromProject100ns, durationFromTimeline100ns)};
    if(duration100ns <= 0){
        return false;
    }

    const auto target100ns{clamp<int64_t>((duration100ns * static_cast<int64_t>(percent)) / 100, 0, duration100ns)};

    if(m_player){
        m_player.PlaybackSession().Position(TimeSpan{target100ns});
        updateTimelineCursorFromPosition(target100ns);
        ensureCurrentTimelineCursorVisible();
        return true;
    }

    const auto width{TimelineCanvas().Width()};
    if(width <= 0){
        return false;
    }

    const auto left{m_tl.timeToCanvasX(target100ns, duration100ns, width)};
    Controls::Canvas::SetLeft(TimelineCursor(), left);
    ensureTimelineCursorVisible(left);
    syncTimelineHorizontalScrollBar();
    return true;
}


bool MainWindow::moveCursorToMarker(int direction){
    if(direction == 0){
        return false;
    }

    const auto duration100ns{m_prj.timelineDuration100ns()};
    if(duration100ns <= 0){
        return false;
    }

    const auto& markers{m_prj.frameIndex()};
    if(markers.empty()){
        return false;
    }

    const auto current100ns{currentNavigationTime100ns()};

    const auto target100ns{m_tl.markerNavigationTarget100ns(markers, current100ns, direction)};
    if(!target100ns){
        return false;
    }

    if(m_player){
        m_player.PlaybackSession().Position(TimeSpan{*target100ns});
        updateTimelineCursorFromPosition(*target100ns);
        ensureCurrentTimelineCursorVisible();
        return true;
    }

    const auto width{TimelineCanvas().Width()};
    if(width > 0){
        const auto left{m_tl.timeToCanvasX(*target100ns, duration100ns, width)};
        Controls::Canvas::SetLeft(TimelineCursor(), left);
        ensureTimelineCursorVisible(left);
        syncTimelineHorizontalScrollBar();
        return true;
    }

    return false;
}

void MainWindow::tryFocusTimelineCanvas(FState focusState){
    const auto canvas{TimelineCanvas()};
    if(canvas && canvas.XamlRoot()){
        canvas.Focus(focusState);
    }
}

bool MainWindow::tryGetRapTimes100ns(vector<int64_t>& rapTimes100ns, bool allowPartial, const vector<int64_t>* requiredPartialTargets100ns) const{
    rapTimes100ns.clear();

    if(!m_prj.hasVideoFile()){
        return false;
    }

    const hstring sourcePath{m_prj.videoFilePath()};
    if(sourcePath != m_cachedRapSourcePath || !m_cachedRapLookupAttempted || !m_cachedRapLookupSucceeded){
        return false;
    }
    if(m_cachedRapTimesPartial){
        if(!allowPartial){
            return false;
        }
        if(requiredPartialTargets100ns){
            auto requestedTargets{*requiredPartialTargets100ns};
            sort(requestedTargets.begin(), requestedTargets.end());
            requestedTargets.erase(unique(requestedTargets.begin(), requestedTargets.end()), requestedTargets.end());
            if(!includes(m_cachedRapLookupTargetTimes100ns.begin(), m_cachedRapLookupTargetTimes100ns.end(), requestedTargets.begin(), requestedTargets.end())){
                return false;
            }
        }
    }

    rapTimes100ns = m_cachedRapTimes100ns;
    return !rapTimes100ns.empty();
}

std::optional<::llvc::EffectiveExportPlan> MainWindow::tryBuildEffectiveExportPlan(const std::function<void(double)>& progressCallback) const{
    const auto sourceDuration100ns{m_prj.timelineDuration100ns()};
    if(sourceDuration100ns <= 0){
        return std::nullopt;
    }

    const auto sourcePath{m_prj.hasVideoFile() ? m_prj.videoFilePath() : hstring{}};
    const auto currentSnapshot{::llvc::captureEditorSnapshot(m_prj)};
    if(m_cachedEffectiveExportPlan
        && sourcePath == m_cachedEffectiveExportPlanSourcePath
        && m_cachedEffectiveExportPlanSnapshot
        && ::llvc::isSameEditorSnapshot(currentSnapshot, *m_cachedEffectiveExportPlanSnapshot)){
        if(progressCallback){
            progressCallback(100.0);
        }
        return m_cachedEffectiveExportPlan;
    }

    if(!::llvc::effectiveExportPlanNeedsRapAlignment(m_prj)){
        auto plan{::llvc::buildDirectEffectiveExportPlan(m_prj, sourceDuration100ns)};
        if(progressCallback){
            progressCallback(100.0);
        }
        m_cachedEffectiveExportPlanSnapshot = currentSnapshot;
        m_cachedEffectiveExportPlanSourcePath = sourcePath;
        m_cachedEffectiveExportPlan = plan;
        return plan;
    }

    vector<int64_t> rapTimes100ns;
    const auto rapLookupTargets100ns{::llvc::buildRapLookupTimesForExportAlignment(m_prj, sourceDuration100ns)};

    if(!tryGetRapTimes100ns(rapTimes100ns, true, &rapLookupTargets100ns)){
        return std::nullopt;
    }

    auto plan{m_prj.buildEffectiveExportPlanWithRapPreroll(sourceDuration100ns, rapTimes100ns, progressCallback)};
    m_cachedEffectiveExportPlanSnapshot = currentSnapshot;
    m_cachedEffectiveExportPlanSourcePath = sourcePath;
    m_cachedEffectiveExportPlan = plan;
    return plan;
}

void MainWindow::queueRapLookup(bool queueReevaluate, int nudgeDirection){
    if(!m_prj.hasVideoFile() || m_prj.videoFilePath().empty()){
        return;
    }

    const hstring sourcePath{m_prj.videoFilePath()};
    if(m_cachedRapLookupAttempted && sourcePath == m_cachedRapSourcePath && !m_cachedRapLookupSucceeded){
        setStatusMessage(L"Could not read RAP markers from the current source");
        return;
    }

    if(queueReevaluate){
        m_pendingReevaluateAfterRapLookup = true;
    }
    if(nudgeDirection != 0){
        m_pendingNudgeDirectionAfterRapLookup = nudgeDirection;
    }

    if(m_isRapLookupInProgress){
        setStatusMessage(L"Scanning source for RAP markers...");
        return;
    }

    runRapLookupAsync();
}

IOpBool MainWindow::ensureRapMarkersAvailableAsync(const wstring& statusMessage, const std::function<void(double)>& progressCallback){
    if(!m_prj.hasVideoFile() || m_prj.videoFilePath().empty()){
        co_return false;
    }

    if(m_isExportInProgress && m_cancelExportRequested.load()){
        co_return false;
    }

    MFLifetime mf{};
    const hstring sourcePath{m_prj.videoFilePath()};
    vector<int64_t> rapTimes100ns;
    auto markerTimes100ns{::llvc::buildRapLookupTimesForExportAlignment(m_prj, m_prj.timelineDuration100ns())};
    if(markerTimes100ns.empty()){
        const auto sourceDuration100ns{m_prj.timelineDuration100ns()};
        const auto& markers{m_prj.frameIndex()};
        markerTimes100ns.reserve(markers.size());
        for(const auto& marker: markers){
            if(marker.time100ns > 0 && marker.time100ns < sourceDuration100ns){
                markerTimes100ns.push_back(marker.time100ns);
            }
        }
        sort(markerTimes100ns.begin(), markerTimes100ns.end());
        markerTimes100ns.erase(unique(markerTimes100ns.begin(), markerTimes100ns.end()), markerTimes100ns.end());
    }
    const auto allowPartialRapTimes{!markerTimes100ns.empty()};
    if(tryGetRapTimes100ns(rapTimes100ns, allowPartialRapTimes, &markerTimes100ns)){
        if(progressCallback && !markerTimes100ns.empty()){
            progressCallback(100.0);
        }
        co_return true;
    }

    while(m_isRapLookupInProgress){
        if(m_isExportInProgress && m_cancelExportRequested.load()){
            co_return false;
        }
        if(tryGetRapTimes100ns(rapTimes100ns, allowPartialRapTimes, &markerTimes100ns)){
            co_return true;
        }
        if(!m_prj.hasVideoFile() || m_prj.videoFilePath() != sourcePath){
            co_return false;
        }

        setOperationInProgress(true, m_isExportInProgress ? false : true);
        if(m_isExportInProgress && ExportOverlayProgressBar().Value() <= 0.0){
            setOperationProgress(0);
            setExportOverlayStageState(ExportOverlayStage::Rap, L"In progress", 0.0, true);
        }
        setStatusMessage(statusMessage);
        co_await winrt::resume_after(std::chrono::milliseconds(50));
    }

    if(tryGetRapTimes100ns(rapTimes100ns, allowPartialRapTimes, &markerTimes100ns)){
        co_return true;
    }
    if(m_cachedRapLookupAttempted && sourcePath == m_cachedRapSourcePath && !m_cachedRapLookupSucceeded){
        co_return false;
    }

    winrt::apartment_context uiThread;
    m_isRapLookupInProgress = true;
    setOperationInProgress(true, m_isExportInProgress ? false : true);
    if(m_isExportInProgress && ExportOverlayProgressBar().Value() <= 0.0){
        setOperationProgress(0);
        setExportOverlayStageState(ExportOverlayStage::Rap, L"In progress", 0.0, true);
    }
    setStatusMessage(statusMessage);

    auto lookupSucceeded{false};
    auto lookupCanceled{false};
    co_await resume_background();
    try{
        if(m_cancelExportRequested.load()){
            lookupSucceeded = false;
        }else{
            rapTimes100ns = m_media ? m_media->collectRapTimes100ns(markerTimes100ns, progressCallback, [weak = get_weak()](){
                if(const auto strong = weak.get()){
                    return strong->m_cancelExportRequested.load();
                }
                return false;
            }) : vector<int64_t>{};
            sort(rapTimes100ns.begin(), rapTimes100ns.end());
            rapTimes100ns.erase(unique(rapTimes100ns.begin(), rapTimes100ns.end()), rapTimes100ns.end());
            lookupSucceeded = !rapTimes100ns.empty();
        }
    }catch(const hresult_error& ex){
        if(ex.code() == HRESULT_FROM_WIN32(ERROR_CANCELLED)){
            lookupCanceled = true;
        }
        rapTimes100ns.clear();
        lookupSucceeded = false;
    }catch(...){
        rapTimes100ns.clear();
        lookupSucceeded = false;
    }

    co_await uiThread;

    const auto sameSourceLoaded{m_prj.hasVideoFile() && m_prj.videoFilePath() == sourcePath};
    if(sameSourceLoaded && !lookupCanceled){
        m_cachedRapSourcePath = sourcePath;
        m_cachedRapTimes100ns = lookupSucceeded ? rapTimes100ns : vector<int64_t>{};
        m_cachedRapLookupTargetTimes100ns = lookupSucceeded ? markerTimes100ns : vector<int64_t>{};
        m_cachedRapLookupAttempted = true;
        m_cachedRapLookupSucceeded = lookupSucceeded;
        m_cachedRapTimesPartial = lookupSucceeded && !markerTimes100ns.empty();
    }

    m_isRapLookupInProgress = false;
    if(!m_isExportInProgress){
        setOperationInProgress(false);
    }
    if(sameSourceLoaded){
        refreshStatusInfoSection();
        refreshVideoDetailsPanel();
    }

    co_return sameSourceLoaded && lookupSucceeded;
}

fire_and_forget MainWindow::runRapLookupAsync(){
    const auto weakSelf{get_weak()};
    const auto lookupSucceeded{co_await ensureRapMarkersAvailableAsync(L"Scanning source for RAP markers and aligning cut plan...")};

    if(const auto self{weakSelf.get()}){
        const auto runReevaluate{self->m_pendingReevaluateAfterRapLookup};
        const auto runReevaluateWithoutUndo{self->m_pendingReevaluateWithoutUndoAfterRapLookup};
        const auto nudgeDirection{self->m_pendingNudgeDirectionAfterRapLookup};
        self->m_pendingReevaluateAfterRapLookup = false;
        self->m_pendingReevaluateWithoutUndoAfterRapLookup = false;
        self->m_pendingNudgeDirectionAfterRapLookup = 0;

        if(!self->m_prj.hasVideoFile() || self->m_prj.videoFilePath().empty()){
            co_return;
        }

        if(!lookupSucceeded){
            self->setStatusMessage(L"Could not read RAP markers from the current source");
            co_return;
        }

        self->setStatusMessage(L"RAP markers ready");
        self->refreshStatusInfoSection();
        self->refreshVideoDetailsPanel();
        for(const auto markerTime100ns: self->m_pendingAutoEvaluateMarkerTimes100ns){
            (void)self->evaluatePlacedMarkerAtTime100ns(markerTime100ns);
        }
        self->m_pendingAutoEvaluateMarkerTimes100ns.clear();
        if(runReevaluate){
            (void)self->reevaluateClearCutMarkers(!runReevaluateWithoutUndo);
        }
        if(nudgeDirection < 0){
            (void)self->nudgeCurrentSceneBoundaryToNearestRap(false);
        }else if(nudgeDirection > 0){
            (void)self->nudgeCurrentSceneBoundaryToNearestRap(true);
        }
    }
}

bool MainWindow::handleStorylineKeyDownImpl(const KRArgs& args){
    const auto focused{Input::FocusManager::GetFocusedElement(Content().XamlRoot()).try_as<DependencyObject>()};
    const auto focusOnMenu{focused && isInMenuSubtree(focused)};
    const auto focusInDialog{focused && isInDialogSubtree(focused)};

    if(!focusInDialog && (args.Key() == VirtualKey::Tab || args.Key() == VirtualKey::Escape)){
        if(focusOnMenu){
            const auto weakThis{get_weak()};
            DispatcherQueue().TryEnqueue([weakThis]{
                if(const auto self{weakThis.get()}){ 
                    self->tryFocusTimelineCanvas(FocusState::Programmatic);
                }
            });
            return false;
        }

        tryFocusTimelineCanvas(FocusState::Programmatic);
        args.Handled(true);
        return true;
    }

    const auto ctrlDown{isModifierDown(VirtualKey::Control)};
    const auto shiftDown{isModifierDown(VirtualKey::Shift)};

    if(!focusInDialog && ctrlDown){
        if(args.Key() == VirtualKey::Z){
            (void)undoLastEdit();
            args.Handled(true);
            return true;
        }
        if(args.Key() == VirtualKey::Y){
            (void)redoLastEdit();
            args.Handled(true);
            return true;
        }
        if(args.Key() == VirtualKey::O){
            (void)openProjectMenuItem_Click(nullptr, {});
            args.Handled(true);
            return true;
        }
        if(args.Key() == VirtualKey::L){
            (void)loadVideoMenuItem_Click(nullptr, {});
            args.Handled(true);
            return true;
        }
        if(args.Key() == VirtualKey::S){
            if(shiftDown){
                (void)saveProjectAsMenuItem_Click(nullptr, {});
            }else{
                (void)saveProjectMenuItem_Click(nullptr, {});
            }
            args.Handled(true);
            return true;
        }
        if(args.Key() == VirtualKey::N){
            (void)newProjectMenuItem_Click(nullptr, {});
            args.Handled(true);
            return true;
        }
        if(args.Key() == VirtualKey::M){
            (void)toggleCutMarkerAtCursor();
            args.Handled(true);
            return true;
        }
        if(args.Key() == VirtualKey::Add || args.Key() == static_cast<VirtualKey>(187)){
            adjustTimelineZoomBy(1);
            args.Handled(true);
            return true;
        }
        if(args.Key() == VirtualKey::Subtract || args.Key() == static_cast<VirtualKey>(189)){
            adjustTimelineZoomBy(-1);
            args.Handled(true);
            return true;
        }
        if(args.Key() == VirtualKey::R){
            reevaluateClearCutMarkersButton_Click(nullptr, {});
            args.Handled(true);
            return true;
        }
        if(args.Key() == static_cast<VirtualKey>(188)){
            (void)nudgeCurrentSceneBoundaryToNearestRap(false);
            args.Handled(true);
            return true;
        }
        if(args.Key() == static_cast<VirtualKey>(190)){
            (void)nudgeCurrentSceneBoundaryToNearestRap(true);
            args.Handled(true);
            return true;
        }
    }

    if(focusInDialog){
        return false;
    }

    tryFocusTimelineCanvas(FocusState::Programmatic);

    switch(args.Key()){
    case VirtualKey::Space:
        if(m_player){
            const auto state {m_player.PlaybackSession().PlaybackState()};
            if(state == MediaPlaybackState::Playing){
                m_player.Pause();
            }else{
                playPreviewFromCurrentPosition();
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
    case VirtualKey::Up:
        (void)moveCursorToMarker(-1);
        args.Handled(true);
        return true;
    case VirtualKey::Down:
        (void)moveCursorToMarker(1);
        args.Handled(true);
        return true;
    case VirtualKey::PageUp:
        (void)seekBySeconds(-pageJumpSecondsFromSettings(m_appSettings));
        args.Handled(true);
        return true;
    case VirtualKey::PageDown:
        (void)seekBySeconds(pageJumpSecondsFromSettings(m_appSettings));
        args.Handled(true);
        return true;
    case VirtualKey::Delete:
        (void)markSceneAtCursor(true);
        args.Handled(true);
        return true;
    case VirtualKey::Insert:
        (void)markSceneAtCursor(false);
        args.Handled(true);
        return true;
    case VirtualKey::F11:
        (void)toggleSeparatePreviewFullscreen();
        args.Handled(true);
        return true;
    case VirtualKey::F7:
        (void)exportVideoMenuItem_Click(Control{}, REArgs{});
        args.Handled(true);
        return true;
    default:
        if(const auto jumpPercent{tryGetTimelineJumpPercentForKey(args.Key())}){
            (void)jumpToTimelinePercent(*jumpPercent);
            args.Handled(true);
            return true;
        }
        return false;
    }
}

bool MainWindow::handleStorylineKeyDown(const KRArgs& args){
    return handleStorylineKeyDownImpl(args);
}

void MainWindow::window_PreviewKeyDown(const Control&, const KRArgs& args){
    if(args.Handled()){
        return;
    }
    (void)handleStorylineKeyDown(args);
}

void MainWindow::window_KeyDown(const Control&, const KRArgs&){
    // Keyboard shortcuts are dispatched from PreviewKeyDown to avoid duplicate handling.
}

void MainWindow::separatePreviewWindowMenuItem_Click(const Control&, const REArgs&){
    const auto targetOpen{!m_separatePreview.isOpen};
    if(!setSeparatePreviewWindowOpen(targetOpen)){
        SeparatePreviewWindowMenuItem().IsChecked(m_separatePreview.isOpen);
    }
}

void MainWindow::toggleSeparatePreviewFullscreenMenuItem_Click(const Control&, const REArgs&){
    (void)toggleSeparatePreviewFullscreen();
}

void MainWindow::zoomInTimelineMenuItem_Click(const Control&, const REArgs&){
    adjustTimelineZoomBy(1);
}

void MainWindow::zoomOutTimelineMenuItem_Click(const Control&, const REArgs&){
    adjustTimelineZoomBy(-1);
}

void MainWindow::adjustTimelineZoomBy(int delta){
    auto slider{TimelineZoomSlider()};
    if(!slider){
        return;
    }

    const auto bar{TimelineHorizontalScrollBar()};
    m_timelineInteraction.pendingScrollbarAnchor = ::llvc::TimelineScrollbarAnchor::capture(bar.Value(), max(0.0, bar.Maximum()));

    const auto target{clampTimelineZoomIndex(slider, slider.Value() + delta)};
    slider.Value(target);
}

bool MainWindow::setSeparatePreviewWindowOpen(bool open){
    if(open == m_separatePreview.isOpen){
        SeparatePreviewWindowMenuItem().IsChecked(open);
        return true;
    }

    if(open){
        auto previewWindow{Window()};
        previewWindow.Title(L"ClipRazor: Lossless Video Cutter - video preview");

        Controls::Grid root{};
        root.RequestedTheme(requestedElementTheme());
        if(const auto backgroundBrush{Application::Current().Resources().Lookup(box_value(L"PreviewWindowBackgroundBrush")).try_as<Media::Brush>()}; backgroundBrush){
            root.Background(backgroundBrush);
        }

        Controls::Image detachedSplash{};
        detachedSplash.Source(Media::Imaging::BitmapImage(Uri{L"ms-appx:///Assets/SplashScreen.png"}));
        detachedSplash.Opacity(0.22);
        detachedSplash.Stretch(Media::Stretch::UniformToFill);
        detachedSplash.IsHitTestVisible(false);
        detachedSplash.HorizontalAlignment(HorizontalAlignment::Stretch);
        detachedSplash.VerticalAlignment(VerticalAlignment::Stretch);

        Controls::MediaPlayerElement detachedPreview{};
        detachedPreview.AreTransportControlsEnabled(true);
        detachedPreview.HorizontalAlignment(HorizontalAlignment::Stretch);
        detachedPreview.VerticalAlignment(VerticalAlignment::Stretch);
        detachedPreview.SetMediaPlayer(m_player);

        const auto weakSelf{get_weak()};
        const auto detachedKeyHandler{[weakSelf](const auto&, const KRArgs& e){
            if(const auto self{weakSelf.get()}){
                self->onSeparatePreviewWindowKeyDown(e);
            }
        }};
        const auto detachedDoubleTapHandler{[weakSelf](const auto&, const auto&){
            if(const auto self{weakSelf.get()}){
                (void)self->toggleSeparatePreviewFullscreen();
            }
        }};
        detachedPreview.PreviewKeyDown(detachedKeyHandler);
        detachedPreview.KeyDown(detachedKeyHandler);
        detachedPreview.DoubleTapped(detachedDoubleTapHandler);

        root.IsTabStop(true);
        root.PreviewKeyDown(detachedKeyHandler);
        root.KeyDown(detachedKeyHandler);

        root.Children().Append(detachedSplash);
        root.Children().Append(detachedPreview);

        previewWindow.Content(root);
        previewWindow.Activate();
        root.Focus(FocusState::Programmatic);

        HWND previewHwnd{};
        if(SUCCEEDED(previewWindow.as<::IWindowNative>()->get_WindowHandle(&previewHwnd)) && previewHwnd){
            restoreSeparatePreviewPlacement(previewHwnd);
        }

        Activate();
        const auto weakThis{get_weak()};
        DispatcherQueue().TryEnqueue([weakThis]{
            if(const auto self{weakThis.get()}){
                self->tryFocusTimelineCanvas(FocusState::Programmatic);
            }
        });

        m_separatePreview.closedRevoker = previewWindow.Closed(auto_revoke, {this, &MainWindow::onSeparatePreviewWindowClosed});
        m_separatePreview.window = previewWindow;
        m_separatePreview.player = detachedPreview;
        m_separatePreview.splashImage = detachedSplash;
        m_separatePreview.isOpen = true;
        m_separatePreview.isFullscreen = false;
    ::llvc::applySeparatePreviewOpened(m_appSettings);
        PreviewPlayer().SetMediaPlayer(nullptr);
        updatePreviewPlaceholderVisibility();

        if(m_appSettings.restorePreviewFullscreenOnStartup){
            (void)toggleSeparatePreviewFullscreen();
        }

        SeparatePreviewWindowMenuItem().IsChecked(true);
        setStatusMessage(L"Preview opened in separate window");
        return true;
    }

    if(m_separatePreview.window){
        HWND previewHwnd{};
        if(SUCCEEDED(m_separatePreview.window.as<::IWindowNative>()->get_WindowHandle(&previewHwnd)) && previewHwnd){
            saveSeparatePreviewPlacement(previewHwnd);
        }

        m_separatePreview.closedRevoker.revoke();
        m_separatePreview.window.Close();
        m_separatePreview.window = nullptr;
    }

    PreviewPlayer().SetMediaPlayer(m_player);
    m_separatePreview.player = nullptr;
    m_separatePreview.splashImage = nullptr;
    m_separatePreview.isOpen = false;
    m_separatePreview.isFullscreen = false;
    ::llvc::applySeparatePreviewClosed(m_appSettings);
    updatePreviewPlaceholderVisibility();
    SeparatePreviewWindowMenuItem().IsChecked(false);
    setStatusMessage(L"Preview restored to main window");
    return true;
}

void MainWindow::onSeparatePreviewWindowClosed(const Control&, const WEArgs&){
    if(m_separatePreview.window){
        HWND previewHwnd{};
        if(SUCCEEDED(m_separatePreview.window.as<::IWindowNative>()->get_WindowHandle(&previewHwnd)) && previewHwnd){
            saveSeparatePreviewPlacement(previewHwnd);
        }
    }

    m_separatePreview.closedRevoker.revoke();
    m_separatePreview.window = nullptr;
    m_separatePreview.isOpen = false;
    m_separatePreview.isFullscreen = false;
    if(!m_isClosing){
    ::llvc::applySeparatePreviewClosed(m_appSettings);
    }
    PreviewPlayer().SetMediaPlayer(m_player);
    m_separatePreview.player = nullptr;
    m_separatePreview.splashImage = nullptr;
    updatePreviewPlaceholderVisibility();
    SeparatePreviewWindowMenuItem().IsChecked(false);
    setStatusMessage(L"Preview restored to main window");
}

void MainWindow::onSeparatePreviewWindowKeyDown(const KRArgs& args){
    (void)handleStorylineKeyDown(args);
}

void MainWindow::saveSeparatePreviewPlacement(HWND previewHwnd){
    if(!previewHwnd){
        return;
    }

    RECT bounds{};
    if(!::GetWindowRect(previewHwnd, &bounds)){
        return;
    }

    if(m_separatePreview.isFullscreen && (m_separatePreview.restoreRect.right > m_separatePreview.restoreRect.left) && (m_separatePreview.restoreRect.bottom > m_separatePreview.restoreRect.top)){
        bounds = m_separatePreview.restoreRect;
    }

    const auto dpi{::GetDpiForWindow(previewHwnd)};
    m_appSettings.separatePreviewPlacement = ::llvc::WindowPlacementState{
        .left = static_cast<int32_t>(bounds.left),
        .top = static_cast<int32_t>(bounds.top),
        .widthDips = pixelsToDips(static_cast<int32_t>(bounds.right - bounds.left), dpi),
        .heightDips = pixelsToDips(static_cast<int32_t>(bounds.bottom - bounds.top), dpi),
        .dpi = static_cast<int32_t>(dpi),
    };
}

void MainWindow::restoreSeparatePreviewPlacement(HWND previewHwnd){
    if(!previewHwnd || !m_appSettings.separatePreviewPlacement){
        return;
    }

    (void)applyWindowPlacement(previewHwnd, *m_appSettings.separatePreviewPlacement, getWindowHandle(), false);

}

bool MainWindow::toggleSeparatePreviewFullscreen(){
    if(!m_separatePreview.isOpen || !m_separatePreview.window){
        setStatusMessage(L"Open the separate preview window first");
        return false;
    }

    HWND previewHwnd{};
    check_hresult(m_separatePreview.window.as<::IWindowNative>()->get_WindowHandle(&previewHwnd));
    if(!previewHwnd){
        setErrorMessage(L"Could not resolve preview window handle");
        return false;
    }

    if(!m_separatePreview.isFullscreen){
        RECT currentRect{};
        if(!::GetWindowRect(previewHwnd, &currentRect)){
            setErrorMessage(L"Could not read preview window bounds");
            return false;
        }

        m_separatePreview.restoreRect = currentRect;
        m_separatePreview.restoreStyle = ::GetWindowLongPtrW(previewHwnd, GWL_STYLE);
        m_separatePreview.restoreExStyle = ::GetWindowLongPtrW(previewHwnd, GWL_EXSTYLE);

        const auto fullscreenStyle{m_separatePreview.restoreStyle & ~(WS_OVERLAPPEDWINDOW)};
        ::SetWindowLongPtrW(previewHwnd, GWL_STYLE, fullscreenStyle);

        MONITORINFO monitorInfo{.cbSize = sizeof(monitorInfo)};
        const auto monitor{::MonitorFromWindow(previewHwnd, MONITOR_DEFAULTTONEAREST)};
        if(!monitor || !::GetMonitorInfoW(monitor, &monitorInfo)){
            setErrorMessage(L"Could not resolve preview monitor bounds");
            return false;
        }

        ::SetWindowPos(
            previewHwnd,
            HWND_TOP,
            monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.top,
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);

        m_separatePreview.isFullscreen = true;
            ::llvc::applySeparatePreviewFullscreen(m_appSettings, true);
        setStatusMessage(L"Separate preview: full-screen on");
        return true;
    }

    ::SetWindowLongPtrW(previewHwnd, GWL_STYLE, m_separatePreview.restoreStyle);
    ::SetWindowLongPtrW(previewHwnd, GWL_EXSTYLE, m_separatePreview.restoreExStyle);
    ::SetWindowPos(
        previewHwnd,
        HWND_NOTOPMOST,
        m_separatePreview.restoreRect.left,
        m_separatePreview.restoreRect.top,
        m_separatePreview.restoreRect.right - m_separatePreview.restoreRect.left,
        m_separatePreview.restoreRect.bottom - m_separatePreview.restoreRect.top,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    m_separatePreview.isFullscreen = false;
            ::llvc::applySeparatePreviewFullscreen(m_appSettings, false);
    setStatusMessage(L"Separate preview: full-screen off");
    return true;
}

void MainWindow::rootGrid_PointerReleased(const Control&, const PREArgs& args){
    const auto source {args.OriginalSource().try_as<DependencyObject>()};
    if(!source){
        return;
    }
    if(isInMenuSubtree(source) || isInDialogSubtree(source)){
        return;
    }

    if(VideoDetailsPanel().Visibility() == Visibility::Visible){
        auto current{source};
        auto clickedInsideDetailsPanel{false};
        while(current){
            if(current == VideoDetailsPanel()){
                clickedInsideDetailsPanel = true;
                break;
            }
            current = Media::VisualTreeHelper::GetParent(current);
        }

        if(!clickedInsideDetailsPanel){
            setVideoDetailsPanelExpanded(false);
        }
    }

    tryFocusTimelineCanvas(FocusState::Programmatic);
}

void MainWindow::videoDetailsOpenMarker_Click(const Control&, const REArgs&){
    setVideoDetailsPanelExpanded(true);
}

void MainWindow::videoDetailsCollapseMarker_Click(const Control&, const REArgs&){
    setVideoDetailsPanelExpanded(false);
}

void MainWindow::cheatSheetOpenMarker_Click(const Control&, const REArgs&){
    CheatSheetPanel().Visibility(Visibility::Visible);
    CheatSheetOpenMarker().Visibility(Visibility::Collapsed);
}

void MainWindow::cheatSheetCollapseMarker_Click(const Control&, const REArgs&){
    CheatSheetPanel().Visibility(Visibility::Collapsed);
    CheatSheetOpenMarker().Visibility(Visibility::Visible);
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

TS MainWindow::secondsToTimeSpan(double seconds){
    return chrono::duration_cast<TimeSpan>(chrono::duration<double>(seconds));
}

AAction MainWindow::undoMenuItem_Click(const Control&, const REArgs&){
    (void)undoLastEdit();
    co_return;
}

AAction MainWindow::redoMenuItem_Click(const Control&, const REArgs&){
    (void)redoLastEdit();
    co_return;
}

AAction MainWindow::newProjectMenuItem_Click(const Control&, const REArgs&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    resetProjectState();
    setStatusMessage(L"New project created");
    clearErrorMessage();
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
        picker.SuggestedFileName(hstring(currentProjectDisplayName()));

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
    picker.SuggestedFileName(hstring(currentProjectDisplayName()));

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

    resetProjectState();
    setStatusMessage(L"Project closed");
    clearErrorMessage();
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
        m_prj.clearTimeline();

        co_await loadVideoFileAsync(file);
    }catch(const winrt::hresult_error&){
        openFailed = true;
    }

    if(openFailed){
        co_await ::llvc::showInfoDialogAsync(Content().XamlRoot(), L"Open failed", L"Could not open selected recent video.");
    }

    tryFocusTimelineCanvas(FocusState::Programmatic);
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
        co_await ::llvc::showInfoDialogAsync(Content().XamlRoot(), L"Open failed", L"Could not open selected recent project.");
    }

    tryFocusTimelineCanvas(FocusState::Programmatic);
}


AAction MainWindow::exitMenuItem_Click(const Control&, const REArgs&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    Close();
}

AAction MainWindow::manualMenuItem_Click(const Control& sender, const REArgs& args){
    co_await ::llvc::showQuickManualDialogAsync(Content().XamlRoot());
}

AAction MainWindow::aboutMenuItem_Click(const Control&, const REArgs&){
    const auto manifestVersion{getAppManifestVersionString()};
    const auto manifestDescription{getAppManifestDescriptionString()};
    co_await ::llvc::showAboutDialogAsync(Content().XamlRoot(), manifestVersion.c_str(), manifestDescription.c_str());
}

AAction MainWindow::optionsMenuItem_Click(const Control&, const REArgs&){
    co_await showOptionsDialogAsync();
}

AAction MainWindow::toggleCutMarkerAtCursorMenuItem_Click(const Control&, const REArgs&){
    (void)toggleCutMarkerAtCursor();
    co_return;
}

AAction MainWindow::markSceneCutAtCursorMenuItem_Click(const Control&, const REArgs&){
    (void)markSceneAtCursor(true);
    co_return;
}

AAction MainWindow::markSceneKeptAtCursorMenuItem_Click(const Control&, const REArgs&){
    (void)markSceneAtCursor(false);
    co_return;
}

AAction MainWindow::shrinkSceneToRapMenuItem_Click(const Control&, const REArgs&){
    (void)nudgeCurrentSceneBoundaryToNearestRap(false);
    co_return;
}

AAction MainWindow::expandSceneToRapMenuItem_Click(const Control&, const REArgs&){
    (void)nudgeCurrentSceneBoundaryToNearestRap(true);
    co_return;
}

AAction MainWindow::pickAndLoadVideoAsync(){
    FileOpenPicker picker{};
    picker.SuggestedStartLocation(PickerLocationId::VideosLibrary);
    for(const auto& profile: supportedFormatProfiles()){
        picker.FileTypeFilter().Append(profile.extension);
    }

    const auto hwnd{getWindowHandle()};
    auto initWithWindow{picker.as<IInitializeWithWindow>()};
    check_hresult(initWithWindow->Initialize(hwnd));

    if(const auto file{co_await picker.PickSingleFileAsync()}){
        m_prj.clearTimeline();

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
    _refreshRecentFilesMenu(RecentVideosMenu(), m_appSettings.recentVideos, {this, &MainWindow::recentVideoMenuItem_Click});
}

void MainWindow::refreshRecentProjectsMenu(){
    _refreshRecentFilesMenu(RecentProjectsMenu(), m_appSettings.recentProjects, {this, &MainWindow::recentProjectMenuItem_Click});
}

void MainWindow::addRecentVideo(const hstring& path){
    if(path.empty()){
        return;
    }

    ::llvc::pushRecentItemFront(m_appSettings.recentVideos, m_appSettings.maxRecentVideos, path);
    addPathToShellRecentDocuments(path);
    refreshRecentVideosMenu();
    saveAppSettings();
}

void MainWindow::addRecentProject(const hstring& path){
    if(path.empty()){
        return;
    }

    ::llvc::pushRecentItemFront(m_appSettings.recentProjects, m_appSettings.maxRecentProjects, path);
    addPathToShellRecentDocuments(path);
    refreshRecentProjectsMenu();
    saveAppSettings();
}

void MainWindow::removeRecentPath(const hstring& path){
    if(path.empty()){
        return;
    }

    ::llvc::removeRecentItem(m_appSettings.recentVideos, path);
    ::llvc::removeRecentItem(m_appSettings.recentProjects, path);
    (void)removePathFromShellRecentDocuments(path.c_str());
    refreshRecentVideosMenu();
    refreshRecentProjectsMenu();
    saveAppSettings();
}

void MainWindow::resetProjectStateImpl(){
    const auto preservedZoomIndex{clampTimelineZoomIndex(TimelineZoomSlider(), TimelineZoomSlider().Value())};
    if(m_player){
        m_player.Pause();
    }
    m_player.Source(nullptr);
    updatePreviewPlaceholderVisibility();

    ++m_timelineRenderVersion;
    m_projectPath.clear();
    m_prj.reset();
    m_media.reset();
    clearUndoRedoHistory();
    m_mediaInfo = {};
    m_cachedRapSourcePath.clear();
    m_cachedRapTimes100ns.clear();
    m_cachedRapLookupTargetTimes100ns.clear();
    m_cachedRapLookupAttempted = false;
    m_cachedRapLookupSucceeded = false;
    m_cachedRapTimesPartial = false;
    m_lastReevaluatedEditorSnapshot.reset();
    m_lastReevaluatedRapSourcePath.clear();
    m_cachedEffectiveExportPlanSnapshot.reset();
    m_cachedEffectiveExportPlanSourcePath.clear();
    m_cachedEffectiveExportPlan.reset();
    m_isRapLookupInProgress = false;
    m_hasTimelineRenderCompleted = false;
    m_pendingReevaluateAfterRapLookup = false;
    m_pendingReevaluateWithoutUndoAfterRapLookup = false;
    m_pendingAutoEvaluateMarkerTimes100ns.clear();
    m_pendingNudgeDirectionAfterRapLookup = 0;
    m_exportEtaText.clear();
    m_lastExportEtaProgress.reset();
    m_timelineInteraction.pendingScrollbarAnchor.reset();
    m_timelineInteraction.pendingWheelZoomAnchor.reset();
    m_timelineDurationSeconds = 0;
    m_prj.setZoomWithoutDirty(preservedZoomIndex);
    TimelineZoomSlider().Value(preservedZoomIndex);
    refreshVideoDetailsPanel();
    setVideoDetailsPanelExpanded(false);
    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();

    ThumbnailLayer().Children().Clear();
    CutOverlayLayer().Children().Clear();
    TimelineTickCanvas().Children().Clear();
    TimelineCanvas().Width(640.0);
    TimelineTickCanvas().Width(640.0);
    Controls::Canvas::SetLeft(TimelineCursor(), 0);
    syncTimelineHorizontalScrollBar();

    setStatusMessage(L"Load or drag-and-drop a .llvc/.mp4/.mov/.mkv/.avi/.webm/.wmv file to begin.");
    clearErrorMessage();
    refreshStatusInfoSection();
    updateWindowTitle();
}

void MainWindow::resetProjectState(){
    resetProjectStateImpl();
}

std::wstring MainWindow::currentProjectDisplayName() const{
    if(!m_projectPath.empty()){
        const auto projectPath{filesystem::path(m_projectPath.c_str())};
        const auto stem{projectPath.stem().wstring()};
        const auto fromProjectPath{stem.empty() ? projectPath.filename().wstring() : stem};
        if(!fromProjectPath.empty()){
            return fromProjectPath;
        }
    }

    if(m_prj.hasVideoFile() && !m_prj.videoFileName().empty()){
        const auto videoPath{filesystem::path(m_prj.videoFileName().c_str())};
        const auto stem{videoPath.stem().wstring()};
        const auto fromVideoPath{stem.empty() ? videoPath.filename().wstring() : stem};
        if(!fromVideoPath.empty()){
            return fromVideoPath;
        }
    }

    return L"Untitled";
}

void MainWindow::updateWindowTitle(){
    auto projectName{currentProjectDisplayName()};

    if(m_prj.isDirty()){
        projectName += L"*";
    }

    const wstring loadedFile{m_prj.hasVideoFile() ? m_prj.videoFilePath().c_str() : L"No file"};
    Title(hstring(std::format(L"ClipRazor: Lossless Video Cutter - {} - {}", projectName, loadedFile)));
}

IOpBool MainWindow::ensureProjectSavedBeforeContinuingAsync(){
    if(!m_prj.isDirty()){
        co_return true;
    }

    const auto choice{co_await ::llvc::showUnsavedProjectPromptAsync(Content().XamlRoot())};
    if(choice == Controls::ContentDialogResult::Primary){
        co_await saveProjectMenuItem_Click(nullptr, RoutedEventArgs{});
        co_return !m_prj.isDirty();
    }
    if(choice == Controls::ContentDialogResult::Secondary){
        co_return true;
    }

    co_return false;
}

AAction MainWindow::openProjectFileAsync(const SFile& file){
    resetProjectState();
    bool referencedVideoCouldNotBeLoaded{};

    co_await m_prj.open(file);

    const auto projectZoomIndex{clampTimelineZoomIndex(TimelineZoomSlider(), m_prj.zoom())};
    m_prj.setZoomWithoutDirty(projectZoomIndex);
    TimelineZoomSlider().Value(projectZoomIndex);

    if(!m_prj.videoFilePath().empty()){
        try{
            const auto videoFile{co_await StorageFile::GetFileFromPathAsync(m_prj.videoFilePath())};
            co_await loadVideoFileAsync(videoFile);
        }catch(...){
            referencedVideoCouldNotBeLoaded = true;
            setStatusMessage(::llvc::buildProjectLoadedStatus(true, true));
        }
    }

    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();

    m_projectPath = file.Path();
    addRecentProject(m_projectPath);

    if(const auto projectStatus{::llvc::buildProjectLoadedStatus(referencedVideoCouldNotBeLoaded, !m_prj.videoFilePath().empty())};
        !projectStatus.empty()){
        setStatusMessage(projectStatus);
        clearErrorMessage();
    }
    updateWindowTitle();
    refreshStatusInfoSection();
}

AAction MainWindow::saveProjectFileAsync(const SFile& file){
    co_await m_prj.save(file);
    m_projectPath = file.Path();
    addRecentProject(file.Path());
    setStatusMessage(L"Project saved");
    clearErrorMessage();
    updateWindowTitle();
    refreshStatusInfoSection();
}

IOpBool MainWindow::confirmAdjustedExportPlanAsync(const ::llvc::EffectiveExportPlan& plan){
    if(!plan.hasRequestedCuts || !plan.materiallyDifferent || plan.emptyAfterAlignment){
        co_return true;
    }

    wstring message{
        L"Your requested cuts need to be narrowed to safe random-access points before lossless export.\n\n"
        L"Requested output: "
        + formatTimelineDurationText(plan.requestedOutputDuration100ns)
        + L"\nRAP-aligned output: "
        + formatTimelineDurationText(plan.effectiveOutputDuration100ns)
        + L"\n\nContinue with the RAP-aligned export plan?"};

    Controls::ContentDialog dialog{};
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(L"Export will be RAP-aligned"));
    dialog.Content(box_value(message));
    dialog.PrimaryButtonText(L"Continue export");
    dialog.CloseButtonText(L"Cancel");

    const auto choice{co_await dialog.ShowAsync()};
    co_return choice == Controls::ContentDialogResult::Primary;
}


wstring MainWindow::buildSourcePropertiesTextImpl() const{
    if(!m_prj.hasVideoFile() || !m_mediaInfo.isValid){
        return L"No video is currently loaded.";
    }

    const auto sourceDuration100ns{m_prj.timelineDuration100ns()};
    const auto requestedOutputDuration100ns{m_prj.outputDuration100ns()};
    const auto effectivePlan{tryBuildEffectiveExportPlan()};

    wstring content;
    content += L"File: "; content += m_prj.videoFilePath().c_str(); content += L"\n";
    content += L"Container: "; content += m_mediaInfo.container; content += L"\n";
    content += L"Duration: "; content += m_mediaInfo.duration; content += L"\n";
    content += L"Size: "; content += m_mediaInfo.fileSize; content += L"\n";
    content += L"Created: "; content += m_mediaInfo.sourceCreated; content += L"\n";
    content += L"Modified: "; content += m_mediaInfo.sourceModified; content += L"\n";
    content += L"Encoded by: "; content += (m_mediaInfo.sourceEncodedBy.empty() ? L"-" : m_mediaInfo.sourceEncodedBy); content += L"\n";
    content += L"Comment: "; content += (m_mediaInfo.sourceComment.empty() ? L"-" : m_mediaInfo.sourceComment); content += L"\n";
    content += L"Video codec: "; content += m_mediaInfo.videoCodec; content += L"\n";
    content += L"Resolution: "; content += m_mediaInfo.resolution; content += L"\n";
    content += L"FPS: "; content += formatRatio(m_mediaInfo.frameRate.num, m_mediaInfo.frameRate.den, L" fps"); content += L"\n";
    content += L"Video bitrate: "; content += m_mediaInfo.videoBitrate; content += L"\n";
    content += L"Random access points: "; content += m_mediaInfo.keyFrameSummary; content += L"\n";
    content += L"Random access interval: "; content += m_mediaInfo.keyFrameInterval; content += L"\n";
    content += L"All samples independent: "; content += m_mediaInfo.allSamplesIndependent; content += L"\n";
    content += L"Max random access spacing: "; content += m_mediaInfo.maxKeyFrameSpacing; content += L"\n";
    content += L"Openable: "; content += capabilityStateToText(m_mediaInfo.openSupport); content += L"\n";
    content += L"Previewable: "; content += capabilityStateToText(m_mediaInfo.previewSupport); content += L"\n";
    content += L"Lossless export here: "; content += capabilityStateToText(m_mediaInfo.losslessExportSupport); content += L"\n";
    content += L"Audio on export: "; content += capabilityStateToText(m_mediaInfo.audioExportSupport); content += L"\n";
    content += L"Export containers here: "; content += joinExtensions(m_mediaInfo.supportedExportExtensions); content += L"\n";
    content += L"Audio codec: "; content += m_mediaInfo.audioCodec; content += L"\n";
    content += L"Audio bitrate: "; content += m_mediaInfo.audioBitrate;
    if(m_mediaInfo.audioDisabledForThisSource && !m_mediaInfo.audioDisabledReason.empty()){
        content += L"\nAudio note: "; content += m_mediaInfo.audioDisabledReason;
    }
    content += L"\nRequested output: ";
    if(sourceDuration100ns > 0){
        content += MainWindow::formatTimelineDurationText(requestedOutputDuration100ns);
    }else{
        content += L"waiting for story line";
    }
    content += L"\nRAP-aligned output: ";
    if(sourceDuration100ns <= 0){
        content += L"waiting for story line";
    }else if(effectivePlan){
        if(effectivePlan->emptyAfterAlignment){
            content += L"no safe cut range";
        }else{
            content += MainWindow::formatTimelineDurationText(effectivePlan->effectiveOutputDuration100ns);
            if(effectivePlan->materiallyDifferent){
                content += L" (adjusted)";
            }
        }
    }else if(::llvc::projectHasRequestedCuts(m_prj)){
        if(m_isRapLookupInProgress){
            content += L"analyzing...";
        }else if(m_cachedRapLookupAttempted && !m_cachedRapLookupSucceeded){
            content += L"unavailable";
        }else{
            content += L"pending analysis";
        }
    }else{
        content += MainWindow::formatTimelineDurationText(sourceDuration100ns);
    }
    return content;
}

wstring MainWindow::buildSourcePropertiesText() const{
    return buildSourcePropertiesTextImpl();
}

void MainWindow::setVideoDetailsPanelExpanded(bool expanded){
    VideoDetailsPanel().Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
    VideoDetailsOpenMarker().Visibility(expanded ? Visibility::Collapsed : Visibility::Visible);
}

void MainWindow::refreshVideoDetailsPanel(){
    VideoDetailsText().Text(buildSourcePropertiesText());
}

wstring MainWindow::formatTimelineDurationText(int64_t duration100ns){
    return ::llvc::formatDuration100ns(duration100ns);
}


wstring MainWindow::formatDateTimeText(const winrt::Windows::Foundation::DateTime& value){
    const auto asTimeT{winrt::clock::to_time_t(value)};
    tm localTime{};
    if(localtime_s(&localTime, &asTimeT) != 0){
        return L"-";
    }

    wchar_t timestamp[64]{};
    if(wcsftime(timestamp, size(timestamp), L"%Y-%m-%d %H:%M:%S", &localTime) == 0){
        return L"-";
    }

    return timestamp;
}

void MainWindow::updatePreviewPlaceholderVisibility(){
    const auto hasLoadedVideo{m_prj.hasVideoFile() && !m_prj.videoFilePath().empty()};

    if(m_separatePreview.player && m_separatePreview.splashImage){
        m_separatePreview.player.Visibility(hasLoadedVideo ? Visibility::Visible : Visibility::Collapsed);
        m_separatePreview.splashImage.Visibility(hasLoadedVideo ? Visibility::Collapsed : Visibility::Visible);
    }
}

void MainWindow::setStatusMessage(const wstring& message){
    StatusText().Text(message);
    if(ExportOverlayStatusText()){
        ExportOverlayStatusText().Text(message);
    }
}

void MainWindow::setErrorMessage(const wstring& message){
    ErrorText().Text(message);
    ErrorText().Visibility(message.empty() ? Visibility::Collapsed : Visibility::Visible);
}

void MainWindow::clearErrorMessage(){
    ErrorText().Text(L"");
    ErrorText().Visibility(Visibility::Collapsed);
}

wstring MainWindow::buildEstimatedOutputText(
    const ::llvc::Project& project,
    const MediaInspectionResult& mediaInfo,
    bool hasTimelineRenderCompleted,
    bool isRapLookupInProgress,
    bool cachedRapLookupAttempted,
    bool cachedRapLookupSucceeded,
    const std::optional<::llvc::EffectiveExportPlan>& effectivePlan){
    if(!project.hasVideoFile() || !mediaInfo.isValid){
        return L"Estimated output: --";
    }

    const auto sourceDuration100ns{project.timelineDuration100ns()};
    if(sourceDuration100ns <= 0){
        return L"Estimated output: waiting for story line...";
    }

    wstring text{L"Estimated output: "};
    if(effectivePlan){
        if(effectivePlan->emptyAfterAlignment){
            text += L"no safe RAP-aligned cut range";
        }else{
            text += formatTimelineDurationText(effectivePlan->effectiveOutputDuration100ns);
            if(effectivePlan->materiallyDifferent){
                text += L" (requested ";
                text += formatTimelineDurationText(effectivePlan->requestedOutputDuration100ns);
                text += L")";
            }
        }
        return text;
    }

    if(::llvc::effectiveExportPlanNeedsRapAlignment(project)){
        if(isRapLookupInProgress){
            text += L"analyzing RAP-aligned cut plan...";
        }else if(cachedRapLookupAttempted && !cachedRapLookupSucceeded){
            text += L"unavailable until RAP analysis succeeds";
        }else if(hasTimelineRenderCompleted){
            text += L"pending RAP analysis";
        }else{
            text += L"waiting for story line...";
        }
        return text;
    }

    text += formatTimelineDurationText(sourceDuration100ns);
    return text;
}

void MainWindow::refreshStatusInfoSection(){
    if(!InfoText()){
        return;
    }

    const auto effectivePlan{tryBuildEffectiveExportPlan()};
    InfoText().Text(buildEstimatedOutputText(
        m_prj,
        m_mediaInfo,
        m_hasTimelineRenderCompleted,
        m_isRapLookupInProgress,
        m_cachedRapLookupAttempted,
        m_cachedRapLookupSucceeded,
        effectivePlan));

    if(!effectivePlan && ::llvc::effectiveExportPlanNeedsRapAlignment(m_prj)){
        if(m_hasTimelineRenderCompleted && !m_isRapLookupInProgress && !m_cachedRapLookupAttempted){
            const auto weakSelf{get_weak()};
            DispatcherQueue().TryEnqueue([weakSelf]{
                if(const auto self{weakSelf.get()}; self && ::llvc::effectiveExportPlanNeedsRapAlignment(self->m_prj)){
                    self->queueRapLookup(false, 0);
                }
            });
        }
    }
}

void MainWindow::configureExportOverlay(const wstring& outputPath, int64_t sourceDuration100ns, int64_t outputDuration100ns, uint64_t sourceSizeBytes, bool adjustedPlan, size_t cutBlockCount, const ::llvc::EffectiveExportPlan* effectivePlan){
    const auto formatOverlaySize{[](uint64_t bytes) -> wstring{
        if(bytes >= (1024ull * 1024ull * 1024ull)){
            return std::format(L"{:.2f} GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
        }
        if(bytes >= (1024ull * 1024ull)){
            return std::format(L"{:.2f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        }
        if(bytes >= 1024ull){
            return std::format(L"{:.2f} KB", static_cast<double>(bytes) / 1024.0);
        }
        return std::format(L"{} bytes", bytes);
    }};
    const auto parseAudioBitrateBytesPerSecond{[this]() -> uint64_t{
        const auto bitrateText{m_mediaInfo.audioBitrate};
        if(bitrateText.empty() || bitrateText == L"none" || bitrateText == L"-"){
            return 0;
        }

        size_t consumed{};
        try{
            const auto kbps{stoull(bitrateText, &consumed)};
            return (kbps * 1000ull) / 8ull;
        }catch(...){
            return 0;
        }
    }};
    const auto formatOverlayShrinkSummary{[&](const ::llvc::EffectiveExportPlan& plan) -> pair<size_t, wstring>{
        const auto summary{::llvc::summarizeEffectiveExportPlan(plan)};
        return {
            summary.repositionedMarkers,
            std::format(
                L"{} cut scene{} shrunk by {} total",
                summary.shrunkCutScenes,
                summary.shrunkCutScenes == 1 ? L"" : L"s",
                formatTimelineDurationText(summary.shrunkTotal100ns))};
    }};

    m_currentExportSourceSizeBytes = sourceSizeBytes;
    m_currentExportSourceDuration100ns = sourceDuration100ns;
    m_currentExportOutputDuration100ns = outputDuration100ns;
    m_currentExportPlanAdjusted = adjustedPlan;
    m_currentExportCutBlockCount = cutBlockCount;
    m_exportOverlayHasFinalState = false;
    m_lastExportSucceeded = false;
    m_currentExportSourcePath = m_prj.hasVideoFile() ? m_prj.videoFilePath() : hstring{};
    m_currentExportProjectPath = m_projectPath;
    m_currentExportOutputPath = outputPath;

    ExportOverlayTargetPathText().Text(outputPath);
    ExportOverlaySourceDurationText().Text(formatTimelineDurationText(sourceDuration100ns));
    ExportOverlayTargetDurationText().Text(formatTimelineDurationText(outputDuration100ns));

    const auto removedDuration100ns{max<int64_t>(0, sourceDuration100ns - outputDuration100ns)};
    if(sourceDuration100ns > 0){
        const auto removedPct{(100.0 * removedDuration100ns) / sourceDuration100ns};
        ExportOverlayDurationDeltaText().Text(
            removedDuration100ns > 0
                ? std::format(L"{} removed ({:.1f}% shorter)", formatTimelineDurationText(removedDuration100ns), removedPct)
                : L"No duration change");
    }else{
        ExportOverlayDurationDeltaText().Text(L"-");
    }

    ExportOverlaySourceSizeText().Text(sourceSizeBytes > 0 ? (formatOverlaySize(sourceSizeBytes) + std::format(L" ({} bytes)", sourceSizeBytes)) : L"-");
    const auto overlayEstimates{::llvc::buildExportOverlayEstimates(
        sourceSizeBytes,
        sourceDuration100ns,
        outputDuration100ns,
        m_prj.keepAudio(),
        sourceHasAudio(),
        parseAudioBitrateBytesPerSecond())};
    if(overlayEstimates.estimatedTargetBytes > 0){
        ExportOverlayEstimatedTargetSizeText().Text(formatOverlaySize(overlayEstimates.estimatedTargetBytes) + L" (rough estimate)");
        if(overlayEstimates.estimatedSavingsBytes > 0){
            wstring savingsText{std::format(L"{} less", formatOverlaySize(overlayEstimates.estimatedSavingsBytes))};
            if(overlayEstimates.estimatedDroppedAudioBytes > 0){
                savingsText += std::format(
                    L" (of which {} is dropped audio size)",
                    formatOverlaySize(overlayEstimates.estimatedDroppedAudioBytes));
            }
            ExportOverlayEstimatedSavingsText().Text(savingsText);
        }else{
            ExportOverlayEstimatedSavingsText().Text(L"No size reduction estimate available");
        }
    }else{
        ExportOverlayEstimatedTargetSizeText().Text(L"-");
        ExportOverlayEstimatedSavingsText().Text(L"-");
    }

    wstring planDetails{
        std::format(
            L"{} export, {} cut block{}, audio {}",
            adjustedPlan ? L"RAP-adjusted lossless" : L"Lossless",
            cutBlockCount,
            cutBlockCount == 1 ? L"" : L"s",
            m_prj.keepAudio() ? L"kept" : L"removed")};
    if(effectivePlan){
        const auto [repositionedMarkers, shrinkSummary]{formatOverlayShrinkSummary(*effectivePlan)};
        planDetails += std::format(L", {} markers repositioned, {}", repositionedMarkers, shrinkSummary);
    }
    if(m_prj.keepAudio() && sourceHasAudio()){
        planDetails += std::format(L", cross-fade {} ms, volume {}%", m_prj.audioXfadeMs(), m_prj.audioVolumePct());
    }
    ExportOverlayPlanDetailsText().Text(planDetails);

    setExportOverlayStagePending(ExportOverlayStage::Video);
    setExportOverlayStagePending(ExportOverlayStage::Rap);
    if(m_prj.keepAudio() && sourceHasAudio()){
        setExportOverlayStagePending(ExportOverlayStage::Audio);
    }else{
        setExportOverlayStageSkipped(ExportOverlayStage::Audio, L"Not needed");
    }
    setExportOverlayStagePending(ExportOverlayStage::Finalize);

    updateExportOverlayStatus();
    updateExportOverlayActionButtons();
}

void MainWindow::setExportOverlayStageState(ExportOverlayStage stage, const wstring& label, std::optional<double> progressPercent, bool active){
    Controls::ProgressBar progressBar{nullptr};
    Controls::TextBlock stageLabel{nullptr};
    switch(stage){
    case ExportOverlayStage::Rap:
        progressBar = ExportOverlayRapStageProgressBar();
        stageLabel = ExportOverlayRapStageLabel();
        break;
    case ExportOverlayStage::Video:
        progressBar = ExportOverlayVideoStageProgressBar();
        stageLabel = ExportOverlayVideoStageLabel();
        break;
    case ExportOverlayStage::Audio:
        progressBar = ExportOverlayAudioStageProgressBar();
        stageLabel = ExportOverlayAudioStageLabel();
        break;
    case ExportOverlayStage::Finalize:
        progressBar = ExportOverlayFinalizeStageProgressBar();
        stageLabel = ExportOverlayFinalizeStageLabel();
        break;
    }

    if(!progressBar || !stageLabel){
        return;
    }

    m_exportStageActive[static_cast<size_t>(stage)] = active;
    m_exportStageProgress[static_cast<size_t>(stage)] = progressPercent;

    if(active){
        if(!m_activeExportStage || *m_activeExportStage != stage){
            m_activeExportStage = stage;
            m_activeExportStageStartedAt = chrono::steady_clock::now();
        }
        m_activeExportStageProgress = progressPercent;
    }else if(m_activeExportStage && *m_activeExportStage == stage){
        m_activeExportStage.reset();
        m_activeExportStageProgress.reset();
    }

    stageLabel.Text(label);
    if(progressPercent.has_value()){
        progressBar.IsIndeterminate(false);
        progressBar.Value(clamp(*progressPercent, 0.0, 100.0));
    }else{
        progressBar.IsIndeterminate(active);
        if(!active){
            progressBar.Value(0);
        }
    }

    if(m_isExportInProgress){
        updateExportStageEta();
        updateExportOverlayStatus();
    }
}

void MainWindow::setExportOverlayStagePending(ExportOverlayStage stage){
    setExportOverlayStageState(stage, L"Pending", 0.0, false);
}

void MainWindow::setExportOverlayStageSkipped(ExportOverlayStage stage, const wstring& reason){
    setExportOverlayStageState(stage, reason, 0.0, false);
}

void MainWindow::setExportOverlayStageComplete(ExportOverlayStage stage, const wstring& label){
    setExportOverlayStageState(stage, label, 100.0, false);
}

void MainWindow::updateExportOverlayStatus(){
    if(!ExportOverlayProgressBar() || !ExportOverlayProgressLabel() || !ExportOverlayEtaLabel()){
        return;
    }

    if(ExportOverlayProgressBar().IsIndeterminate()){
        ExportOverlayProgressLabel().Text(L"Working...");
        ExportOverlayEtaLabel().Text(L"");
        return;
    }

    const auto value{clamp(ExportOverlayProgressBar().Value(), 0.0, 100.0)};
    ExportOverlayProgressLabel().Text(std::format(L"{:.1f}% complete", value));
    ExportOverlayEtaLabel().Text(m_exportEtaText);
}

void MainWindow::updateExportOverlayActionButtons(){
    const std::wstring sourcePath{m_currentExportSourcePath.c_str()};
    const std::wstring projectPath{m_currentExportProjectPath.c_str()};
    const std::wstring liveProjectPath{m_projectPath.c_str()};
    const std::wstring outputPath{m_currentExportOutputPath.c_str()};
    std::error_code sourceEc;
    std::error_code projectEc;
    std::error_code outputEc;
    const bool sourceExists{!sourcePath.empty() && filesystem::exists(sourcePath, sourceEc)};
    const bool projectMatchesCurrent{
        !projectPath.empty()
        && !liveProjectPath.empty()
        && pathsMatchInsensitive(projectPath, liveProjectPath)};
    const bool projectExists{projectMatchesCurrent && filesystem::exists(projectPath, projectEc)};
    const bool outputExists{!outputPath.empty() && filesystem::exists(outputPath, outputEc)};
    const bool sourceCanBeDeleted{
        sourceExists && !(m_lastExportSucceeded && !outputPath.empty() && pathsMatchInsensitive(outputPath, sourcePath))};
    const bool projectCanBeDeleted{
        projectExists && !(m_lastExportSucceeded && !outputPath.empty() && pathsMatchInsensitive(outputPath, projectPath))};

    ExportOverlayProgressActionsPanel().Visibility(m_exportOverlayHasFinalState ? Visibility::Collapsed : Visibility::Visible);
    ExportOverlayFinishedActionsPanel().Visibility(m_exportOverlayHasFinalState ? Visibility::Visible : Visibility::Collapsed);
    ExportOverlayDeleteSourceAndProjectButton().IsEnabled(sourceCanBeDeleted);
    ExportOverlayDeleteProjectButton().IsEnabled(projectCanBeDeleted);
    ExportOverlayOpenExportButton().IsEnabled(outputExists);

    if(!sourceExists && !projectExists){
        ExportOverlayDeleteSourceAndProjectButton().Content(box_value(L"Source and project deleted"));
    }else if(sourceCanBeDeleted && projectCanBeDeleted){
        ExportOverlayDeleteSourceAndProjectButton().Content(box_value(L"Delete source and project file"));
    }else if(!sourceExists && projectExists){
        ExportOverlayDeleteSourceAndProjectButton().Content(box_value(L"Source file deleted"));
    }else if(sourceExists && !projectExists){
        ExportOverlayDeleteSourceAndProjectButton().Content(box_value(L"Delete source file"));
    }else{
        ExportOverlayDeleteSourceAndProjectButton().Content(box_value(L"Delete source and project file"));
    }

    if(!projectExists){
        ExportOverlayDeleteProjectButton().Content(box_value(L"Project file deleted"));
    }else{
        ExportOverlayDeleteProjectButton().Content(box_value(L"Delete project file"));
    }
}

void MainWindow::ensureExportEtaTimer(){
    if(!m_exportEtaTimer){
        m_exportEtaTimer = DTS{};
        const TS interval{std::chrono::seconds{2}};
        m_exportEtaTimer.Interval(interval);
        m_exportEtaTimer.Tick({this, &MainWindow::exportEtaTimer_Tick});
    }

    if(!m_exportEtaTimer.IsEnabled()){
        m_exportEtaTimer.Start();
    }
}

void MainWindow::stopExportEtaTimer(){
    if(m_exportEtaTimer && m_exportEtaTimer.IsEnabled()){
        m_exportEtaTimer.Stop();
    }
}

void MainWindow::exportEtaTimer_Tick(const Control&, const Control&){
    if(!m_isExportInProgress){
        stopExportEtaTimer();
        return;
    }

    updateExportStageEta();
    updateExportOverlayStatus();
}

const wchar_t* MainWindow::exportStageDisplayName(ExportOverlayStage stage) noexcept{
    switch(stage){
    case ExportOverlayStage::Rap: return L"RAP";
    case ExportOverlayStage::Video: return L"Video";
    case ExportOverlayStage::Audio: return L"Audio";
    case ExportOverlayStage::Finalize: return L"Finalize";
    }
    return L"Export";
}

std::chrono::seconds MainWindow::estimateStageDuration(ExportOverlayStage stage) const{
    const auto sourceSeconds{std::max(0.0, static_cast<double>(m_currentExportSourceDuration100ns) / 10'000'000.0)};
    const auto outputSeconds{std::max(0.0, static_cast<double>(m_currentExportOutputDuration100ns) / 10'000'000.0)};
    const auto sourceSizeMb{std::max(1.0, static_cast<double>(m_currentExportSourceSizeBytes) / (1024.0 * 1024.0))};
    const auto hasAudioExport{m_prj.keepAudio() && sourceHasAudio()};
    const auto isWmv{m_mediaInfo.container == L"WMV"};

    double estimateSeconds{};
    switch(stage){
    case ExportOverlayStage::Rap:
        if(m_currentExportCutBlockCount == 0){
            estimateSeconds = 0.0;
        }else if(isWmv){
            estimateSeconds = std::max(1.0, sourceSeconds * 0.0035);
        }else{
            estimateSeconds = std::max(1.0, sourceSeconds * (m_currentExportPlanAdjusted ? 0.023 : 0.0165));
            if(sourceSizeMb < 512.0){
                estimateSeconds *= 0.25;
            }
        }
        break;
    case ExportOverlayStage::Video:
        if(isWmv){
            estimateSeconds = std::max(1.0, outputSeconds * 0.011);
        }else if(hasAudioExport){
            estimateSeconds = std::max(1.0, outputSeconds * 0.0165);
        }else{
            estimateSeconds = std::max(1.0, outputSeconds * 0.032);
        }
        break;
    case ExportOverlayStage::Audio:
        if(!hasAudioExport){
            estimateSeconds = 0.0;
        }else{
            estimateSeconds = std::max(1.0, outputSeconds * 0.042);
            if(m_prj.audioXfadeMs() > 0){
                estimateSeconds *= 1.03;
            }
            if(m_prj.audioVolumePct() > 100){
                estimateSeconds *= 1.05;
            }
        }
        break;
    case ExportOverlayStage::Finalize:
        if(isWmv){
            estimateSeconds = 1.0;
        }else if(sourceSizeMb < 512.0){
            estimateSeconds = 1.0;
        }else{
            estimateSeconds = 1.0;
        }
        break;
    }

    return chrono::seconds{static_cast<int64_t>(std::max(0.0, ceil(estimateSeconds)))};
}

void MainWindow::updateExportStageEta(){
    if(!m_isExportInProgress){
        m_exportEtaText.clear();
        return;
    }

    if(!m_activeExportStage){
        m_exportEtaText = L"Estimating remaining time...";
        return;
    }

    const auto now{chrono::steady_clock::now()};
    const auto elapsed{chrono::duration_cast<chrono::seconds>(now - m_activeExportStageStartedAt)};
    chrono::seconds remaining{};
    if(m_activeExportStageProgress && *m_activeExportStageProgress > 0.0 && *m_activeExportStageProgress < 100.0){
        const auto totalEstimateSeconds{static_cast<double>(elapsed.count()) / (*m_activeExportStageProgress / 100.0)};
        remaining = chrono::seconds{(std::max<int64_t>)(0, static_cast<int64_t>(llround(totalEstimateSeconds - elapsed.count())))};
    }else{
        const auto estimatedStageDuration{estimateStageDuration(*m_activeExportStage)};
        remaining = estimatedStageDuration > elapsed ? (estimatedStageDuration - elapsed) : chrono::seconds::zero();
    }

    m_lastExportEtaRefreshAt = now;
    wstring stageSummary{exportStageDisplayName(*m_activeExportStage)};
    const auto videoActive{m_exportStageActive[static_cast<size_t>(ExportOverlayStage::Video)]};
    const auto audioActive{m_exportStageActive[static_cast<size_t>(ExportOverlayStage::Audio)]};
    if(videoActive && audioActive){
        stageSummary = L"Video + audio";
    }

    m_exportEtaText = stageSummary + L" stage: about " + formatRemainingDurationText(remaining) + L" remaining";
}

AAction MainWindow::deleteExportArtifactsAsync(bool deleteSource, bool deleteProject){
    const std::wstring sourcePath{m_currentExportSourcePath.c_str()};
    const std::wstring projectPath{m_currentExportProjectPath.c_str()};
    const std::wstring liveProjectPath{m_projectPath.c_str()};
    const std::wstring outputPath{m_currentExportOutputPath.c_str()};

    std::error_code sourceEc;
    std::error_code projectEc;
    const bool projectMatchesCurrent{
        !projectPath.empty()
        && !liveProjectPath.empty()
        && pathsMatchInsensitive(projectPath, liveProjectPath)};
    const bool canDeleteSource{
        deleteSource
        && !sourcePath.empty()
        && filesystem::exists(sourcePath, sourceEc)
        && !(m_lastExportSucceeded && !outputPath.empty() && pathsMatchInsensitive(outputPath, sourcePath))};
    const bool canDeleteProject{
        deleteProject
        && projectMatchesCurrent
        && filesystem::exists(projectPath, projectEc)
        && !(m_lastExportSucceeded && !outputPath.empty() && pathsMatchInsensitive(outputPath, projectPath))};

    if(!canDeleteSource && !canDeleteProject){
        co_return;
    }

    resetProjectState();

    std::vector<std::wstring> failures;
    if(canDeleteSource){
        std::wstring failureReason;
        if(movePathToRecycleBin(sourcePath, failureReason)){
            removeRecentPath(sourcePath.c_str());
            m_currentExportSourcePath.clear();
        }else{
            failures.push_back(wstring{L"Could not delete source video:\n"} + sourcePath + L"\n\n" + failureReason);
        }
    }
    if(canDeleteProject){
        std::wstring failureReason;
        if(movePathToRecycleBin(projectPath, failureReason)){
            removeRecentPath(projectPath.c_str());
            m_currentExportProjectPath.clear();
        }else{
            failures.push_back(wstring{L"Could not delete project file:\n"} + projectPath + L"\n\n" + failureReason);
        }
    }

    updateExportOverlayActionButtons();

    if(failures.empty()){
        if(canDeleteSource && canDeleteProject){
            setStatusMessage(L"Source video and project file deleted");
        }else{
            setStatusMessage(L"Project file deleted");
        }
        clearErrorMessage();
        co_return;
    }

    setStatusMessage(L"One or more files could not be deleted");
    clearErrorMessage();

    std::wstring failureText;
    for(size_t i{}; i < failures.size(); ++i){
        if(i != 0){
            failureText += L"\n\n";
        }
        failureText += failures[i];
    }
    co_await ::llvc::showInfoDialogAsync(Content().XamlRoot(), L"Delete failed", failureText.c_str());
}

void MainWindow::setOperationInProgress(bool active, bool indeterminate){
    const auto exportOverlayActive{active && m_isExportInProgress};
    if(active){
        if(exportOverlayActive){
            ensureExportEtaTimer();
        }
        OperationProgressBar().IsIndeterminate(indeterminate);
        if(!indeterminate){
            OperationProgressBar().Value(0);
        }
        if(exportOverlayActive){
            ExportOverlayProgressBar().IsIndeterminate(indeterminate);
            if(!indeterminate){
                ExportOverlayProgressBar().Value(0);
            }
            updateExportOverlayStatus();
        }
    }else{
        OperationProgressBar().IsIndeterminate(false);
        ExportOverlayProgressBar().IsIndeterminate(false);
        stopExportEtaTimer();
        m_activeExportStage.reset();
        m_activeExportStageProgress.reset();
        m_exportStageActive.fill(false);
        m_exportStageProgress.fill(std::nullopt);
        m_exportEtaText.clear();
        m_lastExportEtaProgress.reset();
    }
    OperationProgressBar().Visibility((active && !m_isExportInProgress) ? Visibility::Visible : Visibility::Collapsed);
    CancelExportButton().Visibility(Visibility::Collapsed);
    CancelExportButton().IsEnabled(false);
    ExportOverlay().Visibility((exportOverlayActive || m_exportOverlayHasFinalState) ? Visibility::Visible : Visibility::Collapsed);
    ExportOverlayCancelButton().IsEnabled(active && m_isExportInProgress && !m_cancelExportRequested);
    updateExportOverlayActionButtons();
}

void MainWindow::setOperationProgress(double percent){
    const auto clampedPercent{clamp(percent, 0.0, 100.0)};
    OperationProgressBar().IsIndeterminate(false);
    OperationProgressBar().Value(clampedPercent);
    if(m_isExportInProgress){
        updateExportStageEta();
        ExportOverlayProgressBar().IsIndeterminate(false);
        ExportOverlayProgressBar().Value(clampedPercent);
        updateExportOverlayStatus();
    }
}

std::wstring MainWindow::formatRemainingDurationText(std::chrono::seconds remaining){
    const auto totalSeconds{(std::max<int64_t>)(0, remaining.count())};
    const auto hours{totalSeconds / 3600};
    const auto minutes{(totalSeconds % 3600) / 60};
    const auto seconds{totalSeconds % 60};

    if(hours > 0){
        return std::format(L"{}h {}m", hours, minutes);
    }
    if(minutes > 0){
        return std::format(L"{}m {}s", minutes, seconds);
    }
    return std::format(L"{}s", seconds);
}

void MainWindow::cancelExportButton_Click(const Control&, const REArgs&){
    if(!m_isExportInProgress){
        return;
    }

    m_cancelExportRequested = true;
    setStatusMessage(L"Canceling export...");
    ExportOverlayCancelButton().IsEnabled(false);
}

void MainWindow::exportOverlayCloseButton_Click(const Control&, const REArgs&){
    m_exportOverlayHasFinalState = false;
    setOperationInProgress(false);
}

AAction MainWindow::exportOverlayOpenExportButton_Click(const Control&, const REArgs&){
    if(m_currentExportOutputPath.empty()){
        co_return;
    }

    StorageFile file{nullptr};
    bool fileMissing{};
    try{
        file = co_await StorageFile::GetFileFromPathAsync(m_currentExportOutputPath);
    }catch(...){
        fileMissing = true;
    }

    if(fileMissing || !file){
        co_await ::llvc::showInfoDialogAsync(Content().XamlRoot(), L"Open exported file", L"The exported file could not be found.");
        co_return;
    }

    const auto launched{co_await Launcher::LaunchFileAsync(file)};
    if(!launched){
        co_await ::llvc::showInfoDialogAsync(Content().XamlRoot(), L"Open exported file", L"Windows could not open the exported file with a default app.");
    }
}

AAction MainWindow::exportOverlayDeleteSourceAndProjectButton_Click(const Control&, const REArgs&){
    co_await deleteExportArtifactsAsync(true, true);
}

AAction MainWindow::exportOverlayDeleteProjectButton_Click(const Control&, const REArgs&){
    co_await deleteExportArtifactsAsync(false, true);
}

AAction MainWindow::showOptionsDialogAsync(){
    auto updatedSettings{m_appSettings};
    if(!co_await ::llvc::showOptionsDialogAsync(Content().XamlRoot(), updatedSettings)){
        co_return;
    }

    m_appSettings.maxRecentVideos = updatedSettings.maxRecentVideos;
    m_appSettings.maxRecentProjects = updatedSettings.maxRecentProjects;
    m_appSettings.pageJumpDurationIndex = updatedSettings.pageJumpDurationIndex;
    m_appSettings.appThemeMode = updatedSettings.appThemeMode;
    m_appSettings.deleteSourceAndProjectAfterExport = updatedSettings.deleteSourceAndProjectAfterExport;
    m_appSettings.autoReevaluateCutMarkersOnPlacement = updatedSettings.autoReevaluateCutMarkersOnPlacement;
    m_appSettings.generateExportTimeReport = updatedSettings.generateExportTimeReport;

    if(m_appSettings.recentVideos.size() > m_appSettings.maxRecentVideos){
        m_appSettings.recentVideos.resize(m_appSettings.maxRecentVideos);
    }
    if(m_appSettings.recentProjects.size() > m_appSettings.maxRecentProjects){
        m_appSettings.recentProjects.resize(m_appSettings.maxRecentProjects);
    }

    refreshRecentVideosMenu();
    refreshRecentProjectsMenu();
    applyThemePreference();
    saveAppSettings();
}

AAction MainWindow::promptDeleteSourceAndProjectAfterExportAsync(const std::wstring& exportedPath){
    const std::wstring sourcePath{m_currentExportSourcePath.c_str()};
    const std::wstring projectPath{m_currentExportProjectPath.c_str()};
    const std::wstring liveProjectPath{m_projectPath.c_str()};
    const std::wstring outputPath{m_currentExportOutputPath.c_str()};
    const std::wstring effectiveOutputPath{!exportedPath.empty() ? exportedPath : outputPath};

    std::error_code sourceEc;
    std::error_code projectEc;
    const bool projectMatchesCurrent{
        !projectPath.empty()
        && !liveProjectPath.empty()
        && pathsMatchInsensitive(projectPath, liveProjectPath)};
    const bool canDeleteSource{
        !sourcePath.empty()
        && filesystem::exists(sourcePath, sourceEc)
        && !(m_lastExportSucceeded && !effectiveOutputPath.empty() && pathsMatchInsensitive(effectiveOutputPath, sourcePath))};
    const bool canDeleteProject{
        projectMatchesCurrent
        && filesystem::exists(projectPath, projectEc)
        && !(m_lastExportSucceeded && !effectiveOutputPath.empty() && pathsMatchInsensitive(effectiveOutputPath, projectPath))};

    if(!canDeleteSource && !canDeleteProject){
        co_return;
    }

    if(!co_await ::llvc::showDeleteAfterExportPromptAsync(Content().XamlRoot(), canDeleteSource, canDeleteProject)){
        co_return;
    }

    co_await deleteExportArtifactsAsync(canDeleteSource, canDeleteProject);
}

wstring MainWindow::guidToCodecName(const GUID& subtype, bool isVideo){
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

MediaInspectionResult MainWindow::inspectMediaFile(const wstring& filePath){
    auto media{createVideoSource(filePath)};
    MediaInspectionResult result{};
    if(!media){
        result.errorMessage = L"Container not supported. Only MP4, MOV, AVI (H.264 only), WEBM (VP9 only), and WMV (VC-1 only) are allowed";
        return result;
    }

    result = media->inspect();
    result.sourceEncodedBy = readShellStringProperty(filePath, PKEY_Media_EncodedBy);
    result.sourceComment = readShellStringProperty(filePath, PKEY_Comment);
    return result;
}

void MainWindow::window_DragOver(const Control&, const DEArgs& e){
    e.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
}

AAction MainWindow::window_Drop(const Control&, const DEArgs& e){
    const auto view{e.DataView()};
    if(!view.Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems())){
        setStatusMessage(L"Dropped content is not a file");
        co_return;
    }

    const auto items{co_await view.GetStorageItemsAsync()};
    if(items.Size() != 1){
        setStatusMessage(L"Only support a single .llvc/.mp4/.mov/.mkv/.avi/.webm/.wmv file");
        co_return;
    }

    const auto file{items.GetAt(0).try_as<StorageFile>()};
    if(!file){
        setStatusMessage(L"Dropped content is not a file");
        __debugbreak(); // this should have been caught earlier in this function!
        co_return;
    }

    const auto ext{file.FileType()};
    wstring lower{ext.c_str()};
    transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

    if(lower == PROJECT_EXT){
        co_await openProjectFileAsync(file);
        co_return;
    }

    if(!isSupportedMediaPath(file.Path().c_str())){
        setStatusMessage(L"Only .llvc, .mp4, .mov, .mkv (H.264/HEVC), .avi (H.264), .webm (VP9), and .wmv (VC-1) files are supported");
        co_return;
    }

    m_prj.clearTimeline();
    clearUndoRedoHistory();

    co_await loadVideoFileAsync(file);
}

AAction MainWindow::loadVideoFileAsync(const SFile& file){
    setOperationInProgress(true, true);

    MediaInspectionResult inspected{};
    const auto filePath{std::filesystem::path(file.Path().c_str())};
    try{
        inspected = inspectMediaFile(filePath.c_str());
    }catch(const winrt::hresult_error& ex){
        inspected.errorMessage = L"No decoder available";
        if(ex.code() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)){
            inspected.errorMessage = L"File not found";
        }
    }

    if(!inspected.isValid){
        wstring status{L"Open rejected: "};
        status += inspected.errorMessage;
        setStatusMessage(status);
        setErrorMessage(inspected.errorMessage);
        setOperationInProgress(false);
        co_await ::llvc::showInfoDialogAsync(Content().XamlRoot(), L"Unsupported media", hstring(status));
        co_return;
    }

    std::error_code metadataEc;
    const auto sourceSizeBytes{std::filesystem::file_size(filePath, metadataEc)};
    inspected.fileSize = metadataEc ? L"-" : formatFileSize(sourceSizeBytes);
    inspected.sourceCreated = formatDateTimeText(file.DateCreated());
    if(const auto lastWriteTime{std::filesystem::last_write_time(filePath, metadataEc)}; !metadataEc){
        const auto systemTime{
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                lastWriteTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now())};
        inspected.sourceModified = formatDateTimeText(winrt::clock::from_sys(systemTime));
    }else{
        inspected.sourceModified = L"-";
    }
    m_mediaInfo = inspected;
    m_media = createVideoSource(filePath.c_str());
    m_cachedRapSourcePath.clear();
    m_cachedRapTimes100ns.clear();
    m_cachedRapLookupTargetTimes100ns.clear();
    m_cachedRapLookupAttempted = false;
    m_cachedRapLookupSucceeded = false;
    m_cachedRapTimesPartial = false;
    m_lastReevaluatedEditorSnapshot.reset();
    m_lastReevaluatedRapSourcePath.clear();
    m_cachedEffectiveExportPlanSnapshot.reset();
    m_cachedEffectiveExportPlanSourcePath.clear();
    m_cachedEffectiveExportPlan.reset();
    m_isRapLookupInProgress = false;
    m_hasTimelineRenderCompleted = false;
    m_pendingReevaluateAfterRapLookup = false;
    m_pendingReevaluateWithoutUndoAfterRapLookup = false;
    m_pendingAutoEvaluateMarkerTimes100ns.clear();
    m_pendingNudgeDirectionAfterRapLookup = 0;
    m_timelineInteraction.pendingScrollbarAnchor.reset();
    m_timelineInteraction.pendingWheelZoomAnchor.reset();
    m_projectPath.clear();

    setStatusMessage(::llvc::buildTimelineLoadingStatus(file.Name().c_str()));
    clearErrorMessage();

    const auto source{winrt::Windows::Media::Core::MediaSource::CreateFromStorageFile(file)};
    m_player.Source(source);
    updatePreviewPlaceholderVisibility();
    m_player.IsMuted(false);
    m_prj.videoFilePath(file.Path());
    refreshVideoDetailsPanel();
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
    refreshStatusInfoSection();
    refreshVideoDetailsPanel();
}

bool MainWindow::sourceHasAudio() const{
    return m_mediaInfo.isValid && m_mediaInfo.audioCodec != L"none";
}

void MainWindow::syncAudioCrossfadeComboSelection(){
    const auto previousGuard{m_editorHistory.isApplying};
    m_editorHistory.isApplying = true;

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
            if(stoi(wstring(tag.c_str())) == m_prj.audioXfadeMs()){
                combo.SelectedIndex(static_cast<int32_t>(i));
                m_editorHistory.isApplying = previousGuard;
                return;
            }
        }catch(...){ }
    }

    combo.SelectedIndex(0);
    if(const auto firstItem{combo.SelectedItem().try_as<Controls::ComboBoxItem>()}){
        if(firstItem.Tag()){
            try{
                const auto tag{unbox_value<hstring>(firstItem.Tag())};
                m_prj.audioXfadeMs(stoi(wstring(tag.c_str())));
            }catch(...){
                m_prj.audioXfadeMs(0);
            }
        }
    }

    m_editorHistory.isApplying = previousGuard;
}

void MainWindow::applyAudioSettingsToPlayer(){
    if(!m_player){
        return;
    }

    const auto allowAudio{sourceHasAudio() && m_prj.keepAudio()};
    m_player.IsMuted(!allowAudio);
    // WinRT MediaPlayer preview volume is 0..1, so preview boost is capped at 100%.
    // Export path still applies full configured gain above 100%.
    m_player.Volume(allowAudio ? clamp(m_prj.audioVolumePct() / 100.0, 0.0, 1.0) : 0.0);
}

void MainWindow::playPreviewFromCurrentPosition(){
    if(!m_player){
        return;
    }

    applyAudioSettingsToPlayer();
    const auto session{m_player.PlaybackSession()};
    const auto duration{session.NaturalDuration().count()};
    if(duration > 0 && session.Position().count() >= max<int64_t>(0, duration - 100'000)){
        session.Position(TimeSpan{0});
        updateTimelineCursorFromPosition(0);
    }
    m_player.Play();
}

void MainWindow::updateAudioUiAndPlaybackState(){
    if(!m_prj.hasVideoFile() || m_prj.videoFilePath().empty()){
        if(AudioWaveformCanvas()){
            AudioWaveformCanvas().Visibility(Visibility::Collapsed);
            AudioWaveformCanvas().Children().Clear();
        }
        if(AudioWaveformThresholdLabel()){
            AudioWaveformThresholdLabel().Visibility(Visibility::Collapsed);
        }
        if(AudioWaveformThresholdSlider()){
            AudioWaveformThresholdSlider().Visibility(Visibility::Collapsed);
        }
        if(AudioWaveformThresholdValueText()){
            AudioWaveformThresholdValueText().Visibility(Visibility::Collapsed);
        }
        m_audioWaveformAnalysisQueued = false;
        return;
    }
    const auto hasAudio{sourceHasAudio()};
    const auto audioHardDisabled{m_mediaInfo.audioDisabledForThisSource};
    if(!hasAudio){
        m_prj.keepAudio(false);
    }
    if(audioHardDisabled){
        m_prj.keepAudio(false);
    }

    const auto previousGuard{m_editorHistory.isApplying};
    m_editorHistory.isApplying = true;
    KeepAudioCheckBox().IsEnabled(hasAudio && !audioHardDisabled);
    KeepAudioCheckBox().IsChecked(box_value(hasAudio && !audioHardDisabled && m_prj.keepAudio()).as<IReference<bool>>());
    AudioCrossfadeComboBox().IsEnabled(hasAudio && !audioHardDisabled && m_prj.keepAudio());
    AudioVolumeSlider().IsEnabled(hasAudio && !audioHardDisabled && m_prj.keepAudio());
    AudioVolumeSlider().Value(static_cast<double>(m_prj.audioVolumePct()));
    AudioVolumeIconText().Text(audioVolumeGlyph(hasAudio && !audioHardDisabled && m_prj.keepAudio(), m_prj.audioVolumePct()));

    const auto showPreviewBoostHint{hasAudio && !audioHardDisabled && m_prj.keepAudio() && m_prj.audioVolumePct() > 100};
    AudioPreviewBoostHintText().Visibility(showPreviewBoostHint ? Visibility::Visible : Visibility::Collapsed);

    m_editorHistory.isApplying = previousGuard;
    applyAudioSettingsToPlayer();
    updateAudioWaveformUi();
}

void MainWindow::updateAudioWaveformUi(){
    const auto hasAudio{m_prj.hasVideoFile() && sourceHasAudio()};
    const auto visibility{hasAudio ? Visibility::Visible : Visibility::Collapsed};
    if(!AudioWaveformCanvas() || !AudioWaveformThresholdLabel() || !AudioWaveformThresholdSlider() || !AudioWaveformThresholdValueText()){
        return;
    }
    AudioWaveformCanvas().Visibility(visibility);
    AudioWaveformThresholdLabel().Visibility(visibility);
    AudioWaveformThresholdSlider().Visibility(visibility);
    AudioWaveformThresholdValueText().Visibility(visibility);
    AudioWaveformThresholdSlider().Value(m_audioWaveformThresholdDb);
    AudioWaveformThresholdValueText().Text(formatWaveformThresholdDb(m_audioWaveformThresholdDb));
    if(!hasAudio){
        AudioWaveformCanvas().Children().Clear();
        m_audioWaveformAnalysisQueued = false;
        return;
    }
    renderAudioWaveform();
}

void MainWindow::renderAudioWaveform(){
    const auto canvas{AudioWaveformCanvas()};
    if(!canvas){
        return;
    }
    canvas.Children().Clear();

    if(canvas.Visibility() != Visibility::Visible || !m_prj.hasVideoFile() || !sourceHasAudio()){
        return;
    }

    const auto width{TimelineCanvas().Width()};
    const auto height{canvas.Height()};
    canvas.Width(width);
    if(width <= 0 || height <= 0){
        return;
    }

    const auto sourcePath{std::wstring{m_prj.videoFilePath().c_str()}};
    const auto entry{getOrCreateAudioWaveformEntry(sourcePath)};
    vector<float> peaks;
    vector<bool> chunksBuilt;
    {
        std::scoped_lock lock{g_audioWaveformCacheMutex};
        if(entry){
            peaks = entry->peaks;
            chunksBuilt = entry->chunksBuilt;
        }
    }
    if(!entry || peaks.empty() || chunksBuilt.empty() || std::none_of(chunksBuilt.begin(), chunksBuilt.end(), [](bool built){ return built; })){
        return;
    }

    namespace Shapes = winrt::Microsoft::UI::Xaml::Shapes;
    namespace Media = winrt::Microsoft::UI::Xaml::Media;
    namespace Controls = winrt::Microsoft::UI::Xaml::Controls;
    const auto guideY{height * (1.0 - AudioWaveformThresholdLineRatio)};
    const auto coolBrush{Media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 76, 163, 224))};
    const auto hotBrush{Media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 224, 96, 96))};
    const auto guideBrush{Media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(220, 214, 170, 64))};

    Shapes::Line guideLine{};
    guideLine.X1(0.0);
    guideLine.X2(width);
    guideLine.Y1(guideY);
    guideLine.Y2(guideY);
    guideLine.Stroke(guideBrush);
    guideLine.StrokeThickness(1.0);
    Media::DoubleCollection dashPattern{};
    dashPattern.Append(2.0);
    dashPattern.Append(3.0);
    guideLine.StrokeDashArray(dashPattern);
    guideLine.IsHitTestVisible(false);
    canvas.Children().Append(guideLine);

    const auto columnCount{std::max(1, static_cast<int>(std::ceil(width)))};
    for(int column{}; column < columnCount; ++column){
        const auto firstBucket{
            std::min<size_t>(peaks.size() - 1, static_cast<size_t>((static_cast<double>(column) / columnCount) * peaks.size()))};
        const auto lastBucket{
            std::min<size_t>(peaks.size() - 1, static_cast<size_t>((static_cast<double>(column + 1) / columnCount) * peaks.size()))};
        const auto firstChunk{std::min<size_t>(chunksBuilt.size() - 1, firstBucket / kAudioWaveformBucketsPerChunk)};
        const auto lastChunk{std::min<size_t>(chunksBuilt.size() - 1, lastBucket / kAudioWaveformBucketsPerChunk)};
        auto hasReadyChunk{false};
        for(auto chunk{firstChunk}; chunk <= lastChunk; ++chunk){
            hasReadyChunk = hasReadyChunk || chunksBuilt[chunk];
        }
        if(!hasReadyChunk){
            continue;
        }

        float peak{};
        for(size_t bucket{firstBucket}; bucket <= lastBucket; ++bucket){
            const auto chunk{std::min<size_t>(chunksBuilt.size() - 1, bucket / kAudioWaveformBucketsPerChunk)};
            if(chunksBuilt[chunk]){
                peak = std::max(peak, peaks[bucket]);
            }
        }
        const auto barHeight{std::clamp(audioWaveformHeightRatio(peak, m_audioWaveformThresholdDb) * height, 0.0, height)};
        if(barHeight <= 0.5){
            continue;
        }

        Shapes::Rectangle bar{};
        bar.Width(1.0);
        bar.Height(barHeight);
        bar.Fill(audioWaveformPeakIsHot(peak, m_audioWaveformThresholdDb) ? hotBrush : coolBrush);
        bar.IsHitTestVisible(false);
        Controls::Canvas::SetLeft(bar, static_cast<double>(column));
        Controls::Canvas::SetTop(bar, height - barHeight);
        canvas.Children().Append(bar);
    }
}

winrt::fire_and_forget MainWindow::ensureAudioWaveformAsync(){
    const auto lifetime{get_strong()};
    if(m_isClosing || !m_prj.hasVideoFile() || !sourceHasAudio() || m_prj.timelineDuration100ns() <= 0){
        co_return;
    }

    const auto sourcePath{std::wstring{m_prj.videoFilePath().c_str()}};
    const auto duration100ns{m_prj.timelineDuration100ns()};
    const auto entry{getOrCreateAudioWaveformEntry(sourcePath)};
    if(!entry || entry->ready || entry->failed){
        renderAudioWaveform();
        co_return;
    }
    if(entry->inProgress){
        co_return;
    }

    const apartment_context uiThread{};
    co_await resume_after(std::chrono::milliseconds{750});
    co_await uiThread;
    if(m_isClosing || std::wstring{m_prj.videoFilePath().c_str()} != sourcePath || !sourceHasAudio()){
        co_return;
    }
    if(entry->ready || entry->failed){
        renderAudioWaveform();
        co_return;
    }
    if(entry->inProgress){
        co_return;
    }

    entry->inProgress = true;
    while(true){
        co_await uiThread;
        if(m_isClosing || std::wstring{m_prj.videoFilePath().c_str()} != sourcePath || !sourceHasAudio()){
            break;
        }

        vector<bool> chunksBuilt;
        {
            std::scoped_lock lock{g_audioWaveformCacheMutex};
            chunksBuilt = entry->chunksBuilt;
        }

        if(chunksBuilt.empty() || std::all_of(chunksBuilt.begin(), chunksBuilt.end(), [](bool built){ return built; })){
            std::scoped_lock lock{g_audioWaveformCacheMutex};
            entry->ready = true;
            break;
        }

        const auto timelineWidth{std::max(1.0, TimelineCanvas().Width())};
        const auto chunkWidth{timelineWidth / static_cast<double>(chunksBuilt.size())};
        const auto scrollViewer{TimelineScrollViewer()};
        const auto visibleRange{visibleAudioWaveformChunkRange(
            scrollViewer.HorizontalOffset(),
            std::max(0.0, scrollViewer.ViewportWidth()),
            chunkWidth,
            static_cast<int>(chunksBuilt.size()))};
        const auto nextChunk{chooseNextAudioWaveformChunkIndex(chunksBuilt, visibleRange, true)};
        if(nextChunk < 0){
            std::scoped_lock lock{g_audioWaveformCacheMutex};
            entry->ready = true;
            break;
        }

        co_await resume_background();
        const auto chunkPeaks{analyzeAudioWaveformChunkPeaks(sourcePath, duration100ns, kAudioWaveformBucketCount, static_cast<size_t>(nextChunk), chunksBuilt.size(), [weak = get_weak(), sourcePath](){
            if(const auto self{weak.get()}){
                return self->m_isClosing || std::wstring{self->m_prj.videoFilePath().c_str()} != sourcePath;
            }
            return true;
        })};

        {
            std::scoped_lock lock{g_audioWaveformCacheMutex};
            if(chunkPeaks.empty()){
                entry->failed = true;
                break;
            }

            const auto bucketStart{audioWaveformChunkBucketStart(static_cast<size_t>(nextChunk), chunksBuilt.size(), entry->peaks.size())};
            const auto bucketEnd{std::min(entry->peaks.size(), bucketStart + chunkPeaks.size())};
            for(size_t bucket{bucketStart}; bucket < bucketEnd; ++bucket){
                entry->peaks[bucket] = chunkPeaks[bucket - bucketStart];
            }
            if(static_cast<size_t>(nextChunk) < entry->chunksBuilt.size()){
                entry->chunksBuilt[static_cast<size_t>(nextChunk)] = true;
            }
        }

        co_await uiThread;
        if(m_isClosing || std::wstring{m_prj.videoFilePath().c_str()} != sourcePath){
            break;
        }
        renderAudioWaveform();
    }

    co_await uiThread;
    {
        std::scoped_lock lock{g_audioWaveformCacheMutex};
        entry->inProgress = false;
        entry->ready = entry->ready || (!entry->chunksBuilt.empty() && std::all_of(entry->chunksBuilt.begin(), entry->chunksBuilt.end(), [](bool built){ return built; }));
    }
    m_audioWaveformAnalysisQueued = false;
    if(m_isClosing || std::wstring{m_prj.videoFilePath().c_str()} != sourcePath){
        co_return;
    }
    renderAudioWaveform();
}

void MainWindow::queueTimelineViewportRender(){
    if(m_isClosing || m_isExportInProgress || !m_prj.hasVideoFile()){
        return;
    }

    renderTimelineViewportAfterDelayAsync(++m_timelineViewportRenderRequestVersion);
}

winrt::fire_and_forget MainWindow::renderTimelineViewportAfterDelayAsync(uint64_t requestVersion){
    const auto lifetime{get_strong()};
    const apartment_context uiThread{};
    co_await resume_after(std::chrono::milliseconds{120});
    co_await uiThread;
    if(m_isClosing || requestVersion != m_timelineViewportRenderRequestVersion || !m_hasTimelineRenderCompleted){
        co_return;
    }

    renderTimelineAsync(true);
}

winrt::fire_and_forget MainWindow::renderTimelineAsync(bool viewportOnly){
    const auto lifetime{get_strong()};

    if(m_isClosing || !m_prj.hasVideoFile() || m_timelineDurationSeconds <= 0){
        if(!m_isExportInProgress){
            setOperationInProgress(false);
        }
        co_return;
    }

    if(!DispatcherQueue().HasThreadAccess()){
        const auto weak{get_weak()};
        DispatcherQueue().TryEnqueue([weak, viewportOnly](){
            if(const auto self{weak.get()}){
                self->renderTimelineAsync(viewportOnly);
            }
        });
        co_return;
    }

    try{
        const auto renderDuringExport{m_isExportInProgress};
        if(!renderDuringExport && !viewportOnly){
            m_hasTimelineRenderCompleted = false;
            setOperationInProgress(true, true);
        }

        const auto renderVersion{++m_timelineRenderVersion};
        const auto stripPlan{m_tl.buildThumbnailStripPlan(m_timelineDurationSeconds, timelineZoomValueFromIndex(TimelineZoomSlider().Value()))};
        constexpr auto thumbnailImageHeight{86.0};
        const auto totalWidth{stripPlan.totalWidth};
        const auto thumbnailCount{stripPlan.thumbnailCount};
        const auto thumbnailWidth{stripPlan.thumbnailWidth};
        const auto sourcePath{m_prj.videoFilePath()};
        const auto thumbnailPlanChanged{
            m_thumbnailPlanSourcePath != sourcePath
            || m_thumbnailPlanCount != thumbnailCount
            || fabs(m_thumbnailPlanTotalWidth - totalWidth) > 0.5
            || fabs(m_thumbnailPlanWidth - thumbnailWidth) > 0.5};

        TimelineCanvas().Width(totalWidth);
        AudioWaveformCanvas().Width(totalWidth);
        CutOverlayLayer().Width(totalWidth);
        if(thumbnailPlanChanged){
            ThumbnailLayer().Children().Clear();
            m_thumbnailBuilt.assign(static_cast<size_t>(thumbnailCount), false);
            m_thumbnailPlanSourcePath = sourcePath;
            m_thumbnailPlanCount = thumbnailCount;
            m_thumbnailPlanTotalWidth = totalWidth;
            m_thumbnailPlanWidth = thumbnailWidth;
        }
        if(thumbnailPlanChanged || !viewportOnly){
            renderTimelineTicks();
            renderAudioWaveform();
            renderKeyframeTicks();
            renderCutOverlays();
        }
        syncTimelineHorizontalScrollBar();
        if(sourceHasAudio() && !m_audioWaveformAnalysisQueued){
            m_audioWaveformAnalysisQueued = true;
            ensureAudioWaveformAsync();
        }

        if(m_timelineInteraction.pendingWheelZoomAnchor){
            const auto scrollViewer{TimelineScrollViewer()};
            const auto viewportWidth{max(1.0, scrollViewer.ViewportWidth())};
            const auto targetOffsetValue{
                m_timelineInteraction.pendingWheelZoomAnchor->restoreOffset(m_tl, m_prj.timelineDuration100ns(), totalWidth, viewportWidth)};
            const auto targetOffset{box_value(targetOffsetValue).as<IReference<double>>()};
            scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
            syncTimelineHorizontalScrollBar();
            m_timelineInteraction.pendingWheelZoomAnchor.reset();
            m_timelineInteraction.pendingScrollbarAnchor.reset();
        }else if(m_timelineInteraction.pendingScrollbarAnchor){
            const auto scrollViewer{TimelineScrollViewer()};
            const auto barMaximum{max(0.0, TimelineHorizontalScrollBar().Maximum())};
            const auto targetOffsetValue{m_timelineInteraction.pendingScrollbarAnchor->restoreOffset(barMaximum)};
            const auto targetOffset{box_value(targetOffsetValue).as<IReference<double>>()};
            scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
            syncTimelineHorizontalScrollBar();
            m_timelineInteraction.pendingScrollbarAnchor.reset();
        }

        const auto scrollViewer{TimelineScrollViewer()};
        const auto visibleRange{m_tl.visibleThumbnailRange(
            scrollViewer.HorizontalOffset(),
            max(0.0, scrollViewer.ViewportWidth()),
            thumbnailWidth,
            thumbnailCount)};
        const auto bufferedRange{m_tl.expandThumbnailRange(visibleRange, 1, thumbnailCount)};
        const auto allowOffscreenThumbnailExpansion{viewportOnly};
        auto nextIndex{m_tl.chooseNextThumbnailIndex(m_thumbnailBuilt, bufferedRange, allowOffscreenThumbnailExpansion)};

        const auto useSourceReaderThumbnails{m_mediaInfo.container == L"MPEG-TS"};
        winrt::Windows::Media::Editing::MediaComposition composition{};
        if(nextIndex >= 0 && !useSourceReaderThumbnails){
            const auto clipFile{co_await StorageFile::GetFileFromPathAsync(m_prj.videoFilePath())};
            const auto clip{co_await winrt::Windows::Media::Editing::MediaClip::CreateFromFileAsync(clipFile)};
            composition.Clips().Append(clip);
        }

        if(renderVersion != m_timelineRenderVersion){
            co_return;
        }

        while(nextIndex >= 0){
            if(renderVersion != m_timelineRenderVersion || m_isClosing){
                if(m_isClosing){
                    setOperationInProgress(false);
                }
                co_return;
            }

            const auto t{(nextIndex + 0.5) / thumbnailCount};
            Controls::Image image{};
            image.Width(thumbnailWidth);
            image.Height(thumbnailImageHeight);
            image.Stretch(Media::Stretch::UniformToFill);

            auto thumbnailCreated{false};
            if(useSourceReaderThumbnails){
                const wstring sourceFilePath{m_prj.videoFilePath().c_str()};
                const auto thumbnailTime100ns{static_cast<int64_t>(max(0.0, t * m_timelineDurationSeconds) * Timeline::HnsPerSecond)};
                const apartment_context uiThread{};
                co_await resume_background();
                const auto decodedThumbnail{tryDecodeThumbnailWithSourceReader(sourceFilePath, thumbnailTime100ns, 180, 96)};
                co_await uiThread;
                if(renderVersion != m_timelineRenderVersion || m_isClosing){
                    if(m_isClosing){
                        setOperationInProgress(false);
                    }
                    co_return;
                }
                if(decodedThumbnail){
                    image.Source(createWriteableBitmapFromDecodedThumbnail(*decodedThumbnail));
                    thumbnailCreated = true;
                }
            }else{
                const auto stream{co_await composition.GetThumbnailAsync(secondsToTimeSpan(t * m_timelineDurationSeconds), 180, 96, winrt::Windows::Media::Editing::VideoFramePrecision::NearestFrame)};
                if(renderVersion != m_timelineRenderVersion || m_isClosing){
                    if(m_isClosing){
                        setOperationInProgress(false);
                    }
                    co_return;
                }

                Media::Imaging::BitmapImage bitmap{};
                co_await bitmap.SetSourceAsync(stream);
                image.Source(bitmap);
                thumbnailCreated = true;
            }

            m_thumbnailBuilt[static_cast<size_t>(nextIndex)] = true;
            if(thumbnailCreated){
                Controls::Canvas::SetLeft(image, nextIndex * thumbnailWidth);
                ThumbnailLayer().Children().Append(image);
            }
            nextIndex = m_tl.chooseNextThumbnailIndex(m_thumbnailBuilt, bufferedRange, allowOffscreenThumbnailExpansion);
        }

        if(!renderDuringExport && m_player && m_player.PlaybackSession().PlaybackState() == MediaPlaybackState::Playing){
            updateTimelineCursorFromPlayback();
            ensureTimelineCursorVisible(Controls::Canvas::GetLeft(TimelineCursor()));
        }
        syncTimelineHorizontalScrollBar();

        if(!renderDuringExport && !viewportOnly){
            m_hasTimelineRenderCompleted = true;
            m_audioWaveformAnalysisQueued = false;
            const auto postActions{m_tl.buildRenderPostActions(
                m_prj.videoFileName().c_str(),
                ::llvc::projectHasRequestedCuts(m_prj),
                m_cachedRapLookupAttempted,
                m_isRapLookupInProgress)};
            setStatusMessage(postActions.readyStatus);
            refreshStatusInfoSection();
            setOperationInProgress(false);
            if(postActions.shouldQueueRapLookup){
                queueRapLookup(false, 0);
            }
            renderTimelineAsync(true);
        }
    }catch(const winrt::hresult_error& ex){
        if(!m_isExportInProgress && !viewportOnly){
            m_hasTimelineRenderCompleted = false;
            m_audioWaveformAnalysisQueued = false;
            wstring status{L"Failed to render story line: "};
            status += ex.message().c_str();
            setStatusMessage(status);
            setErrorMessage(ex.message().c_str());
            setOperationInProgress(false);
        }
    }
}
AAction MainWindow::exportVideoMenuItem_Click(const Control&, const REArgs&){
    if(!m_prj.hasVideoFile()){
        co_await ::llvc::showInfoDialogAsync(Content().XamlRoot(), L"Export video", L"Load a video before exporting.");
        co_return;
    }

    const wstring sourcePath{m_prj.videoFilePath().c_str()};
    const auto preflight{::llvc::buildExportPreflightState(
        m_prj,
        m_media != nullptr,
        m_mediaInfo.losslessExportSupport == CapabilityState::Supported,
        !m_mediaInfo.supportedExportExtensions.empty(),
        sourceHasAudio(),
        m_mediaInfo.audioExportSupport == CapabilityState::Supported)};
    if(!preflight.canExport){
        co_await ::llvc::showInfoDialogAsync(Content().XamlRoot(), L"Export video", hstring{preflight.blockMessage});
        co_return;
    }
    const auto sourceDuration100ns{m_prj.timelineDuration100ns()};
    const filesystem::path sourceFsPath{sourcePath};
    std::error_code sourceSizeEc;
    const auto sourceSizeBytes{filesystem::file_size(sourceFsPath, sourceSizeEc)};
    const auto requestedOutputDuration100ns{preflight.requestedOutputDuration100ns};
    const auto requestedCutBlockCount{preflight.requestedCutBlockCount};

    auto makeExportComment = [&](const filesystem::path& sourcePathForComment) -> wstring {
        const auto now{chrono::system_clock::now()};
        const auto nowTimeT{chrono::system_clock::to_time_t(now)};
        tm localTime{};
        const auto CREATED_BY{L"created by ClipRazor from "};
        if(localtime_s(&localTime, &nowTimeT) != 0){
            return CREATED_BY + sourcePathForComment.filename().wstring();
        }

        wchar_t timestamp[64]{};
        if(wcsftime(timestamp, size(timestamp), L"%Y-%m-%d %H:%M:%S", &localTime) == 0){
            return CREATED_BY + sourcePathForComment.filename().wstring();
        }

        return CREATED_BY + sourcePathForComment.filename().wstring() + L" on " + timestamp;
    };
    const auto outputPath{
        pickExportOutputPath(
            sourceFsPath,
            m_mediaInfo.supportedExportExtensions,
            m_mediaInfo.defaultExportExtension.c_str(),
            requestedOutputDuration100ns,
            getWindowHandle())};
    if(outputPath.empty()){
        co_return;
    }

    if(pathsMatchInsensitive(outputPath, sourcePath)){
        if(!co_await ::llvc::showOverwriteSourcePromptAsync(Content().XamlRoot())){
            co_return;
        }
    }

    const auto temporaryOutputPath{buildTemporaryExportPath(outputPath)};
    if(temporaryOutputPath.empty()){
        co_await ::llvc::showInfoDialogAsync(Content().XamlRoot(), L"Export failed", L"Could not create a temporary export file path.");
        co_return;
    }

    m_isExportInProgress = true;
    m_cancelExportRequested = false;
    m_currentExportStartedAt = chrono::steady_clock::now();
    m_lastExportEtaRefreshAt = {};
    m_lastExportEtaProgress.reset();
    m_exportEtaText = L"Estimating remaining time...";
    ++m_timelineRenderVersion;

    configureExportOverlay(
        outputPath,
        sourceDuration100ns,
        requestedOutputDuration100ns,
        sourceSizeEc ? 0 : sourceSizeBytes,
        false,
        requestedCutBlockCount,
        nullptr);
    const auto needsRapReevaluation{preflight.needsRapReevaluation};
    setStatusMessage(needsRapReevaluation ? L"Reevaluating cut plan to RAP frames..." : L"Preparing export plan...");
    clearErrorMessage();
    setOperationInProgress(true, false);
    setOperationProgress(0);
    ExportOverlayProgressBar().IsIndeterminate(false);
    ExportOverlayProgressBar().Value(0);
    updateExportOverlayStatus();
    setExportOverlayStageState(ExportOverlayStage::Rap, L"In progress", 0.0, true);
    const auto exportOverallStartedAt{chrono::steady_clock::now()};

    if(needsRapReevaluation){
        const auto rapReady{co_await ensureRapMarkersAvailableAsync(
            L"Reevaluating cut plan to RAP frames...",
            [weak = get_weak()](double pct){
                if(const auto strong = weak.get()){
                    strong->DispatcherQueue().TryEnqueue([uiWeak = strong->get_weak(), pct](){
                        if(const auto ui = uiWeak.get()){
                            const auto overallPct{(pct * 5.0) / 100.0};
                            ui->setExportOverlayStageState(ExportOverlayStage::Rap, L"In progress", pct, true);
                            ui->OperationProgressBar().IsIndeterminate(false);
                            ui->OperationProgressBar().Value(overallPct);
                            ui->ExportOverlayProgressBar().IsIndeterminate(false);
                            ui->ExportOverlayProgressBar().Value(overallPct);
                            ui->updateExportOverlayStatus();
                        }
                    });
                }
            })};
        if(!rapReady){
            if(m_cancelExportRequested){
                m_isExportInProgress = false;
                m_lastExportSucceeded = false;
                m_exportOverlayHasFinalState = true;
                setExportOverlayStageState(ExportOverlayStage::Rap, L"Canceled", 0.0, false);
                setStatusMessage(L"Export canceled");
                clearErrorMessage();
                setOperationInProgress(false);
                updateExportOverlayActionButtons();
                co_return;
            }
            m_isExportInProgress = false;
            setExportOverlayStageState(ExportOverlayStage::Rap, L"Failed", 0.0, false);
            setOperationInProgress(false);
            setStatusMessage(L"Could not build a RAP-aligned cut plan for the current source");
            refreshStatusInfoSection();
            refreshVideoDetailsPanel();
            co_await ::llvc::showInfoDialogAsync(Content().XamlRoot(), L"Export blocked", L"Could not build a RAP-aligned cut plan for the current source.");
            co_return;
        }
    }

    const auto effectivePlanOpt{tryBuildEffectiveExportPlan()};
    if(!effectivePlanOpt){
        m_isExportInProgress = false;
        setExportOverlayStageState(ExportOverlayStage::Rap, L"Failed", 0.0, false);
        setOperationInProgress(false);
        co_await ::llvc::showInfoDialogAsync(Content().XamlRoot(), L"Export blocked", L"The export plan is not ready yet. Wait for the RAP analysis to finish and try again.");
        co_return;
    }
    const auto effectivePlan{*effectivePlanOpt};
    if(effectivePlan.emptyAfterAlignment){
        m_isExportInProgress = false;
        setExportOverlayStageState(ExportOverlayStage::Rap, L"No safe range", 0.0, false);
        setOperationInProgress(false);
        setStatusMessage(L"Export blocked: no safe RAP-aligned cut range");
        refreshStatusInfoSection();
        refreshVideoDetailsPanel();
        co_await ::llvc::showInfoDialogAsync(
            Content().XamlRoot(),
            L"Export blocked",
            L"The current cut selection does not contain a safe RAP-aligned cut range. Adjust markers or reevaluate clear cut markers first.");
        co_return;
    }
    const auto outputDuration100ns{effectivePlan.effectiveOutputDuration100ns};
    const auto effectiveCutRanges100ns{effectivePlan.effectiveCutRanges100ns};
    configureExportOverlay(
        outputPath,
        sourceDuration100ns,
        outputDuration100ns,
        sourceSizeEc ? 0 : sourceSizeBytes,
        effectivePlan.materiallyDifferent,
        effectiveCutRanges100ns.size(),
        &effectivePlan);

    ::llvc::ExportCoordinatorResult exportResult{};
    co_await ::llvc::runExportAsync(::llvc::ExportCoordinatorRequest{
        .project = &m_prj,
        .media = m_media.get(),
        .mediaInfo = &m_mediaInfo,
        .sourcePath = sourcePath,
        .outputPath = outputPath,
        .temporaryOutputPath = temporaryOutputPath,
        .exportComment = makeExportComment(sourceFsPath),
        .sourceDuration100ns = sourceDuration100ns,
        .sourceSizeBytes = sourceSizeEc ? 0 : sourceSizeBytes,
        .sourceHasAudio = sourceHasAudio(),
        .needsRapReevaluation = needsRapReevaluation,
        .ensureRapMarkersAvailableAsync = [this](const wstring& statusMessage, const function<void(double)>& progressCallback) -> winrt::Windows::Foundation::IAsyncOperation<bool>{
            co_return co_await ensureRapMarkersAvailableAsync(statusMessage, progressCallback);
        },
        .reevaluateCutMarkers = [this](bool pushUndoState){
            return reevaluateClearCutMarkers(pushUndoState);
        },
        .buildEffectiveExportPlan = [this](const function<void(double)>& progressCallback) -> std::optional<::llvc::EffectiveExportPlan>{
            return tryBuildEffectiveExportPlan(progressCallback);
        },
        .onStatus = [weak = get_weak()](const wstring& message){
            if(const auto ui = weak.get()){
                if(ui->DispatcherQueue().HasThreadAccess()){
                    ui->setStatusMessage(message);
                    return;
                }

                ui->DispatcherQueue().TryEnqueue([uiWeak = weak, message](){
                    if(const auto ui = uiWeak.get()){
                        ui->setStatusMessage(message);
                    }
                });
            }
        },
        .onOverallProgress = [weak = get_weak()](double pct){
            if(const auto ui = weak.get()){
                if(ui->DispatcherQueue().HasThreadAccess()){
                    ui->setOperationProgress(pct);
                    return;
                }

                ui->DispatcherQueue().TryEnqueue([uiWeak = weak, pct](){
                    if(const auto ui = uiWeak.get()){
                        ui->setOperationProgress(pct);
                    }
                });
            }
        },
        .onStageState = [weak = get_weak()](::llvc::ExportStage stage, wstring label, optional<double> progressPercent, bool active){
            const auto mapStage = [](const ::llvc::ExportStage exportStage){
                switch(exportStage){
                case ::llvc::ExportStage::Rap: return ExportOverlayStage::Rap;
                case ::llvc::ExportStage::Video: return ExportOverlayStage::Video;
                case ::llvc::ExportStage::Audio: return ExportOverlayStage::Audio;
                case ::llvc::ExportStage::Finalize: return ExportOverlayStage::Finalize;
                }
                return ExportOverlayStage::Finalize;
            };

            if(const auto ui = weak.get()){
                if(ui->DispatcherQueue().HasThreadAccess()){
                    ui->setExportOverlayStageState(mapStage(stage), label, progressPercent, active);
                    return;
                }

                ui->DispatcherQueue().TryEnqueue([uiWeak = weak, stage, label = std::move(label), progressPercent, active](){
                    if(const auto ui = uiWeak.get()){
                        const auto mapStage = [](const ::llvc::ExportStage exportStage){
                            switch(exportStage){
                            case ::llvc::ExportStage::Rap: return ExportOverlayStage::Rap;
                            case ::llvc::ExportStage::Video: return ExportOverlayStage::Video;
                            case ::llvc::ExportStage::Audio: return ExportOverlayStage::Audio;
                            case ::llvc::ExportStage::Finalize: return ExportOverlayStage::Finalize;
                            }
                            return ExportOverlayStage::Finalize;
                        };
                        ui->setExportOverlayStageState(mapStage(stage), label, progressPercent, active);
                    }
                });
            }
        },
        .shouldCancel = [weak = get_weak()](){
            if(const auto ui = weak.get()){
                return ui->m_cancelExportRequested.load();
            }
            return false;
        },
    }, exportResult);

    winrt::hstring exportErrorMessage{exportResult.errorMessage};
    const auto exportSucceeded{exportResult.succeeded};
    const auto exportCanceled{exportResult.canceled};
    const auto rapStageDurationMs{exportResult.rapStageDurationMs};
    const auto videoStageDurationMs{exportResult.videoStageDurationMs};
    const auto audioStageDurationMs{exportResult.audioStageDurationMs};
    const auto finalizeStageDurationMs{exportResult.finalizeStageDurationMs};

    if(exportSucceeded){
        setOperationProgress(100);
    }

    m_isExportInProgress = false;
    m_lastExportSucceeded = exportSucceeded;
    m_exportOverlayHasFinalState = true;
    setOperationInProgress(false);
    ExportOverlayProgressActionsPanel().Visibility(Visibility::Collapsed);
    ExportOverlayFinishedActionsPanel().Visibility(Visibility::Visible);
    ExportOverlayCancelButton().IsEnabled(false);
    if(exportSucceeded){
        setExportOverlayStageComplete(ExportOverlayStage::Rap, effectivePlan.materiallyDifferent ? L"Adjusted" : L"Done");
        setExportOverlayStageComplete(ExportOverlayStage::Video);
        if(m_prj.keepAudio() && sourceHasAudio()){
            setExportOverlayStageComplete(ExportOverlayStage::Audio);
        }
        setExportOverlayStageComplete(ExportOverlayStage::Finalize);
        setStatusMessage(L"Export completed");
        clearErrorMessage();
        refreshStatusInfoSection();
    }else if(exportCanceled){
        if(ExportOverlayFinalizeStageProgressBar().IsIndeterminate()){
            setExportOverlayStageState(ExportOverlayStage::Finalize, L"Canceled", 0.0, false);
        }
        setStatusMessage(L"Export canceled");
        clearErrorMessage();
    }else{
        if(ExportOverlayFinalizeStageProgressBar().IsIndeterminate()){
            setExportOverlayStageState(ExportOverlayStage::Finalize, L"Failed", 0.0, false);
        }
        setStatusMessage(L"Export failed");
        setErrorMessage(exportErrorMessage.c_str());
    }
    updateExportOverlayActionButtons();

    if(m_appSettings.generateExportTimeReport){
        const auto totalDurationMs{chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - exportOverallStartedAt)};
        const auto formatMs{[](const std::optional<chrono::milliseconds>& value) -> wstring{
            if(!value){
                return L"n/a";
            }

            const auto totalMs{value->count()};
            const auto totalSeconds{totalMs / 1000};
            const auto msRemainder{totalMs % 1000};
            const auto minutes{totalSeconds / 60};
            const auto seconds{totalSeconds % 60};
            if(minutes > 0){
                return std::format(L"{}m {}.{:03d}s", minutes, seconds, static_cast<int>(msRemainder));
            }
            return std::format(L"{}.{:03d}s", seconds, static_cast<int>(msRemainder));
        }};

        const auto report{std::format(
            L"llvc export timing report\r\n"
            L"status: {}\r\n"
            L"source file: {}\r\n"
            L"output file: {}\r\n"
            L"project file: {}\r\n"
            L"container: {}\r\n"
            L"video codec: {}\r\n"
            L"audio codec: {}\r\n"
            L"source size: {} bytes\r\n"
            L"source duration: {}\r\n"
            L"target duration: {}\r\n"
            L"requested cut blocks: {}\r\n"
            L"effective cut blocks: {}\r\n"
            L"plan adjusted: {}\r\n"
            L"empty after alignment: {}\r\n"
            L"keep audio: {}\r\n"
            L"audio export support: {}\r\n"
            L"audio crossfade ms: {}\r\n"
            L"audio volume pct: {}\r\n"
            L"report mode: clipboard auto-copy\r\n"
            L"stage rap: {}\r\n"
            L"stage video: {}\r\n"
            L"stage audio: {}\r\n"
            L"stage finalize: {}\r\n"
            L"total export: {}\r\n",
            exportSucceeded ? L"success" : (exportCanceled ? L"canceled" : L"failed"),
            sourcePath,
            outputPath,
            m_projectPath.empty() ? L"(unsaved project)" : wstring(m_projectPath.c_str()),
            m_mediaInfo.container,
            m_mediaInfo.videoCodec,
            m_mediaInfo.audioCodec,
            sourceSizeEc ? 0ull : sourceSizeBytes,
            formatTimelineDurationText(sourceDuration100ns),
            formatTimelineDurationText(outputDuration100ns),
            requestedCutBlockCount,
            effectiveCutRanges100ns.size(),
            effectivePlan.materiallyDifferent ? L"yes" : L"no",
            effectivePlan.emptyAfterAlignment ? L"yes" : L"no",
            m_prj.keepAudio() ? L"yes" : L"no",
            capabilityStateToText(m_mediaInfo.audioExportSupport),
            m_prj.audioXfadeMs(),
            m_prj.audioVolumePct(),
            formatMs(rapStageDurationMs),
            formatMs(videoStageDurationMs),
            formatMs(audioStageDurationMs),
            formatMs(finalizeStageDurationMs),
            formatMs(std::optional<chrono::milliseconds>{totalDurationMs}))};

        winrt::Windows::ApplicationModel::DataTransfer::DataPackage package{};
        package.SetText(report);
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
        setStatusMessage(exportSucceeded ? L"Export completed and timing report copied to clipboard" : L"Export timing report copied to clipboard");
    }

    if(exportSucceeded && m_appSettings.deleteSourceAndProjectAfterExport){
        co_await promptDeleteSourceAndProjectAfterExportAsync(outputPath);
    }

    if(!exportErrorMessage.empty()){
        co_await ::llvc::showInfoDialogAsync(Content().XamlRoot(), L"Export failed", exportErrorMessage);
    }
}



}
