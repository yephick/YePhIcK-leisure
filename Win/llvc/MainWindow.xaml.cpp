#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"
#include "MainWindow.Helpers.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <functional>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

#include <microsoft.ui.xaml.window.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shobjidl_core.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Editing.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>

import llvc.Export;
import llvc.Utils;

#pragma comment(lib, "Shell32.lib")

using namespace std;
using namespace winrt;
using namespace ::llvc;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Input;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Input;
using namespace Windows::Foundation;
using namespace Windows::Media::Playback;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Windows::System;
using namespace Windows::UI::Core;

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

constexpr auto W_POS_L{L"WindowLeft"};

namespace{

constexpr int64_t HNS_PER_SECOND{10'000'000LL};

bool isAviPath(const wstring& filePath){
    if(filePath.size() < 4){
        return false;
    }
    const auto ext{filePath.substr(filePath.size() - 4)};
    return _wcsicmp(ext.c_str(), L".avi") == 0;
}


vector<int64_t> collectCleanPointTimes100ns(const wstring& filePath){
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

    for(;;){
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

        rapTimes.push_back(max<int64_t>(0, sampleTime));
    }

    sort(rapTimes.begin(), rapTimes.end());
    rapTimes.erase(unique(rapTimes.begin(), rapTimes.end()), rapTimes.end());
    return rapTimes;
}

bool isTimeInsideRanges(int64_t time100ns, const vector<pair<int64_t, int64_t>>& ranges){
    for(const auto& [start100ns, end100ns]: ranges){
        if(time100ns < start100ns){
            return false;
        }
        if(time100ns < end100ns){
            return true;
        }
    }

    return false;
}

wstring guidToVideoCodecName(const GUID& subtype){
    if(subtype == MFVideoFormat_H264){
        return L"H.264";
    }
    if(subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265){
        return L"HEVC";
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
constexpr auto W_POS_T{L"WindowTop"};
constexpr auto W_POS_W{L"WindowWidth"};
constexpr auto W_POS_H{L"WindowHeight"};
constexpr auto W_POS_DPI{L"WindowDpi"};
constexpr auto S_RECENT_VIDEOS{L"RecentVideos"};
constexpr auto S_RECENT_PROJECTS{L"RecentProjects"};
constexpr auto S_MAX_RECENT_VIDEOS{L"MaxRecentVideos"};
constexpr auto S_MAX_RECENT_PROJECTS{L"MaxRecentProjects"};
constexpr auto S_DEFAULT_MAX_RECENT{5};
constexpr auto S_SEPARATE_PREVIEW_DETACHED{L"SeparatePreviewDetached"};
constexpr auto S_SEPARATE_PREVIEW_L{L"SeparatePreviewLeft"};
constexpr auto S_SEPARATE_PREVIEW_T{L"SeparatePreviewTop"};
constexpr auto S_SEPARATE_PREVIEW_W{L"SeparatePreviewWidth"};
constexpr auto S_SEPARATE_PREVIEW_H{L"SeparatePreviewHeight"};
constexpr auto S_SEPARATE_PREVIEW_DPI{L"SeparatePreviewDpi"};
constexpr auto S_SEPARATE_PREVIEW_FULLSCREEN{L"SeparatePreviewFullscreen"};

constexpr auto P_FILE_PATH{L"file_path"};
constexpr auto P_STORYLINE_ZOOM{L"storyline_zoom"};
constexpr auto P_KEEP_AUDIO{L"keep_audio"};
constexpr auto P_AUDIO_CROSSFADE_MS{L"audio_crossfade_ms"};
constexpr auto P_RAP_MARKERS{L"rap_markers"};
constexpr auto P_CUT_SCENES{L"cut_scenes"};

constexpr auto PROJECT_KEY{L"llvc project"};
constexpr auto UNNAMED_PROJECT{L"Untitled video cut"};
constexpr auto PROJECT_EXT{L".llvc"};

bool isControlModifierActive(VirtualKeyModifiers modifiers){
    if((modifiers & VirtualKeyModifiers::Control) == VirtualKeyModifiers::Control){
        return true;
    }

    const auto ctrlState{InputKeyboardSource::GetKeyStateForCurrentThread(VirtualKey::Control)};
    return (ctrlState & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
}

bool MainWindow::isSameUndoRedoState(const UndoRedoState& a, const UndoRedoState& b) const{
    if(a.keepAudio != b.keepAudio || a.audioCrossfadeMs != b.audioCrossfadeMs || a.cutScenes != b.cutScenes || a.frameIndex.size() != b.frameIndex.size()){
        return false;
    }

    for(size_t i{}; i < a.frameIndex.size(); ++i){
        const auto& l{a.frameIndex[i]};
        const auto& r{b.frameIndex[i]};
        if(l.time100ns != r.time100ns || l.duration100ns != r.duration100ns || l.cleanPoint != r.cleanPoint || l.sampleIndex != r.sampleIndex){
            return false;
        }
    }

    return true;
}

MainWindow::UndoRedoState MainWindow::captureUndoRedoState() const{
    return UndoRedoState{
        .frameIndex = m_prj.frameIndex(),
        .cutScenes = m_prj.cutScenes(),
        .keepAudio = m_prj.keepAudio(),
        .audioCrossfadeMs = m_prj.audioXfadeMs(),
    };
}

void MainWindow::clearUndoRedoHistory(){
    m_undoStack.clear();
    m_redoStack.clear();
}

bool MainWindow::pushUndoStateIfChanged(){
    if(m_isApplyingUndoRedoState){
        return false;
    }

    const auto current{captureUndoRedoState()};
    if(!m_undoStack.empty() && isSameUndoRedoState(m_undoStack.back(), current)){
        return false;
    }

    m_undoStack.push_back(current);
    m_redoStack.clear();
    return true;
}

bool MainWindow::applyUndoRedoState(const UndoRedoState& state, bool fromUndo){
    const auto current{captureUndoRedoState()};
    if(isSameUndoRedoState(current, state)){
        return false;
    }

    m_isApplyingUndoRedoState = true;

    m_prj.frameIndex(state.frameIndex);
    m_prj.refreshSelectedMarkers();
    m_prj.cutScenes(state.cutScenes);
    m_prj.keepAudio(state.keepAudio);
    m_prj.audioXfadeMs(state.audioCrossfadeMs);

    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();
    renderTimelineTicks();
    renderKeyframeTicks();
    renderCutOverlays();
    updateWindowTitle();
    refreshStatusInfoSection();

    m_isApplyingUndoRedoState = false;

    setStatusMessage(fromUndo ? L"Undo applied" : L"Redo applied");
    return true;
}

bool MainWindow::undoLastEdit(){
    if(m_undoStack.empty()){
        setStatusMessage(L"Nothing to undo");
        return false;
    }

    const auto previous{m_undoStack.back()};
    m_undoStack.pop_back();
    m_redoStack.push_back(captureUndoRedoState());
    return applyUndoRedoState(previous, true);
}

bool MainWindow::redoLastEdit(){
    if(m_redoStack.empty()){
        setStatusMessage(L"Nothing to redo");
        return false;
    }

    const auto next{m_redoStack.back()};
    m_redoStack.pop_back();
    m_undoStack.push_back(captureUndoRedoState());
    return applyUndoRedoState(next, false);
}

MainWindow::MainWindow(){
    InitializeComponent();

    m_player = MediaPlayer();
    PreviewPlayer().SetMediaPlayer(m_player);

    m_naturalDurationChangedRevoker = m_player.PlaybackSession().NaturalDurationChanged(auto_revoke, {this, &MainWindow::onNaturalDurationChanged});

    m_positionTimer = DispatcherTimer();
    m_positionTimer.Interval(chrono::milliseconds(80));
    m_positionTimer.Tick({this, &MainWindow::onPositionTimerTick});
    m_positionTimer.Start();

    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();
    refreshVideoDetailsPanel();
    setVideoDetailsPanelExpanded(false);

    restoreWindowPlacement();
    loadAppSettings();
    Closed({this, &MainWindow::onClosed});
    m_mainWindowActivatedRevoker = Activated(auto_revoke, {this, &MainWindow::onWindowActivated});
    refreshRecentVideosMenu();
    refreshRecentProjectsMenu();
    updateWindowTitle();
    refreshStatusInfoSection();

    if(m_restorePreviewDetachedOnStartup){
        (void)setSeparatePreviewWindowOpen(true);
    }
}

HWND MainWindow::getWindowHandle() const{
    HWND hwnd{};
    const auto projected{const_cast<MainWindow*>(this)->get_strong()};
    check_hresult(projected.as<::IWindowNative>()->get_WindowHandle(&hwnd));
    return hwnd;
}


void MainWindow::restoreWindowPlacement(){
    const auto localSettings{ApplicationData::Current().LocalSettings()};
    const auto values{localSettings.Values()};

    if(!values.HasKey(W_POS_L) || !values.HasKey(W_POS_T) || !values.HasKey(W_POS_W) || !values.HasKey(W_POS_H)){
        return;
    }

    ::llvc::WindowPlacementState state{};
    state.left = unbox_value<int32_t>(values.Lookup(W_POS_L));
    state.top = unbox_value<int32_t>(values.Lookup(W_POS_T));
    state.widthDips = unbox_value<int32_t>(values.Lookup(W_POS_W));
    state.heightDips = unbox_value<int32_t>(values.Lookup(W_POS_H));
    state.dpi = values.HasKey(W_POS_DPI) ? unbox_value<int32_t>(values.Lookup(W_POS_DPI)) : 96;

    const auto hwnd{getWindowHandle()};
    if(!applyWindowPlacement(hwnd, state, hwnd, false)){
        values.Remove(W_POS_L);
        values.Remove(W_POS_T);
        values.Remove(W_POS_W);
        values.Remove(W_POS_H);
        values.Remove(W_POS_DPI);
    }
}

void MainWindow::loadAppSettings(){
    const auto values{ApplicationData::Current().LocalSettings().Values()};

    m_maxRecentVideos = S_DEFAULT_MAX_RECENT;
    m_maxRecentProjects = S_DEFAULT_MAX_RECENT;

    if(values.HasKey(S_MAX_RECENT_VIDEOS)){
        const auto parsed{unbox_value<int32_t>(values.Lookup(S_MAX_RECENT_VIDEOS))};
        m_maxRecentVideos = static_cast<uint32_t>(clamp(parsed, 1, 20));
    }
    if(values.HasKey(S_MAX_RECENT_PROJECTS)){
        const auto parsed{unbox_value<int32_t>(values.Lookup(S_MAX_RECENT_PROJECTS))};
        m_maxRecentProjects = static_cast<uint32_t>(clamp(parsed, 1, 20));
    }

    if(values.HasKey(S_RECENT_VIDEOS)){
        m_recentVideos = splitRecentItems(unbox_value<hstring>(values.Lookup(S_RECENT_VIDEOS)).c_str());
    }
    if(values.HasKey(S_RECENT_PROJECTS)){
        m_recentProjects = splitRecentItems(unbox_value<hstring>(values.Lookup(S_RECENT_PROJECTS)).c_str());
    }
    if(m_recentVideos.size() > m_maxRecentVideos){
        m_recentVideos.resize(m_maxRecentVideos);
    }
    if(m_recentProjects.size() > m_maxRecentProjects){
        m_recentProjects.resize(m_maxRecentProjects);
    }

    m_restorePreviewDetachedOnStartup = values.HasKey(S_SEPARATE_PREVIEW_DETACHED)
        && unbox_value<bool>(values.Lookup(S_SEPARATE_PREVIEW_DETACHED));

    if(values.HasKey(S_SEPARATE_PREVIEW_L) && values.HasKey(S_SEPARATE_PREVIEW_T) && values.HasKey(S_SEPARATE_PREVIEW_W) && values.HasKey(S_SEPARATE_PREVIEW_H)){
        m_separatePreviewLeft = unbox_value<int32_t>(values.Lookup(S_SEPARATE_PREVIEW_L));
        m_separatePreviewTop = unbox_value<int32_t>(values.Lookup(S_SEPARATE_PREVIEW_T));
        m_separatePreviewWidthDips = max<int32_t>(320, unbox_value<int32_t>(values.Lookup(S_SEPARATE_PREVIEW_W)));
        m_separatePreviewHeightDips = max<int32_t>(200, unbox_value<int32_t>(values.Lookup(S_SEPARATE_PREVIEW_H)));
        m_separatePreviewDpi = values.HasKey(S_SEPARATE_PREVIEW_DPI) ? max<int32_t>(96, unbox_value<int32_t>(values.Lookup(S_SEPARATE_PREVIEW_DPI))) : 96;
        m_hasSeparatePreviewPlacement = true;
    }

    m_restorePreviewFullscreenOnStartup = values.HasKey(S_SEPARATE_PREVIEW_FULLSCREEN)
        && unbox_value<bool>(values.Lookup(S_SEPARATE_PREVIEW_FULLSCREEN));
}

void MainWindow::saveAppSettings() const{
    const auto values{ApplicationData::Current().LocalSettings().Values()};
    values.Insert(S_MAX_RECENT_VIDEOS, box_value(static_cast<int32_t>(m_maxRecentVideos)));
    values.Insert(S_MAX_RECENT_PROJECTS, box_value(static_cast<int32_t>(m_maxRecentProjects)));
    values.Insert(S_RECENT_VIDEOS, box_value(hstring(joinRecentItems(m_recentVideos))));
    values.Insert(S_RECENT_PROJECTS, box_value(hstring(joinRecentItems(m_recentProjects))));
    values.Insert(S_SEPARATE_PREVIEW_DETACHED, box_value(m_restorePreviewDetachedOnStartup));
    values.Insert(S_SEPARATE_PREVIEW_FULLSCREEN, box_value(m_restorePreviewFullscreenOnStartup));

    if(m_hasSeparatePreviewPlacement){
        values.Insert(S_SEPARATE_PREVIEW_L, box_value(m_separatePreviewLeft));
        values.Insert(S_SEPARATE_PREVIEW_T, box_value(m_separatePreviewTop));
        values.Insert(S_SEPARATE_PREVIEW_W, box_value(m_separatePreviewWidthDips));
        values.Insert(S_SEPARATE_PREVIEW_H, box_value(m_separatePreviewHeightDips));
        values.Insert(S_SEPARATE_PREVIEW_DPI, box_value(m_separatePreviewDpi));
    }
}

void MainWindow::saveWindowPlacement() const{
    const auto hwnd{getWindowHandle()};
    const auto captured{::llvc::captureWindowPlacement(hwnd)};
    if(!captured){
        return;
    }

    const auto localSettings{ApplicationData::Current().LocalSettings()};
    const auto values{localSettings.Values()};
    values.Insert(W_POS_L, box_value(captured->left));
    values.Insert(W_POS_T, box_value(captured->top));
    values.Insert(W_POS_W, box_value(captured->widthDips));
    values.Insert(W_POS_H, box_value(captured->heightDips));
    values.Insert(W_POS_DPI, box_value(captured->dpi));
}

void MainWindow::onClosed(const Control&, const WEArgs&){
    m_isClosing = true;

    if(m_positionTimer){
        m_positionTimer.Stop();
    }

    m_naturalDurationChangedRevoker.revoke();
    m_mainWindowActivatedRevoker.revoke();

    m_restorePreviewDetachedOnStartup = m_isSeparatePreviewWindowOpen;
    m_restorePreviewFullscreenOnStartup = m_isSeparatePreviewWindowOpen && m_isSeparatePreviewFullscreen;

    if(m_separatePreviewWindow){
        HWND previewHwnd{};
        if(SUCCEEDED(m_separatePreviewWindow.as<::IWindowNative>()->get_WindowHandle(&previewHwnd)) && previewHwnd){
            saveSeparatePreviewPlacement(previewHwnd);
        }

        m_separatePreviewClosedRevoker.revoke();
        m_separatePreviewWindow.Close();
        m_separatePreviewWindow = nullptr;
    }

    saveWindowPlacement();
    saveAppSettings();
}

void MainWindow::onWindowActivated(const Control&, const WAVArgs& args){
    if(args.WindowActivationState() == WindowActivationState::Deactivated){
        return;
    }

    const auto weakThis{get_weak()};
    DispatcherQueue().TryEnqueue([weakThis]{
        if(const auto self{weakThis.get()}){
            self->tryFocusTimelineCanvas(FocusState::Programmatic);
        }
    });
}

void MainWindow::startButton_Click(const Control&, const REArgs&){
    if(m_player){
        applyAudioSettingsToPlayer();
        m_player.Play();
    }
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


void MainWindow::reevaluateClearCutMarkersButton_Click(const Control&, const REArgs&){
    if(!m_prj.videoFile() || m_timelineDurationSeconds <= 0){
        setStatusMessage(L"Load a video before reevaluating clear cut markers");
        return;
    }

    vector<int64_t> rapTimes100ns;
    try{
        rapTimes100ns = collectCleanPointTimes100ns(m_prj.videoFile().Path().c_str());
    }catch(const winrt::hresult_error&){
        setStatusMessage(L"Could not read RAP markers from the current source");
        return;
    }

    if(rapTimes100ns.empty()){
        setStatusMessage(L"Could not detect RAP markers in the current source");
        return;
    }

    const auto originalMarkers{m_prj.frameIndex()};
    if(originalMarkers.empty()){
        setStatusMessage(L"No clear cut markers to reevaluate");
        return;
    }

    const auto previousCutRanges{m_prj.buildCutRanges100ns()};
    const auto beforeState{captureUndoRedoState()};
    const auto hasUndoSnapshot{pushUndoStateIfChanged()};
    vector<IndexedFrameSample> updatedMarkers;
    updatedMarkers.reserve(originalMarkers.size() * 2);

    auto replacedCount{0u};
    for(const auto& marker: originalMarkers){
        if(binary_search(rapTimes100ns.begin(), rapTimes100ns.end(), marker.time100ns)){
            updatedMarkers.push_back(marker);
            continue;
        }

        ++replacedCount;
        const auto nextIt{lower_bound(rapTimes100ns.begin(), rapTimes100ns.end(), marker.time100ns)};
        if(nextIt != rapTimes100ns.begin()){
            const auto previousRapTime{*(nextIt - 1)};
            updatedMarkers.push_back(IndexedFrameSample{.time100ns = previousRapTime, .duration100ns = 0, .cleanPoint = true, .sampleIndex = 0});
        }
        if(nextIt != rapTimes100ns.end()){
            const auto nextRapTime{*nextIt};
            updatedMarkers.push_back(IndexedFrameSample{.time100ns = nextRapTime, .duration100ns = 0, .cleanPoint = true, .sampleIndex = 0});
        }
    }

    sort(updatedMarkers.begin(), updatedMarkers.end(), [](const auto& a, const auto& b){ return a.time100ns < b.time100ns; });
    updatedMarkers.erase(unique(updatedMarkers.begin(), updatedMarkers.end(), [](const auto& a, const auto& b){ return a.time100ns == b.time100ns; }), updatedMarkers.end());

    m_prj.frameIndex(std::move(updatedMarkers));
    m_prj.refreshSelectedMarkers();

    const auto sceneBoundaries{m_prj.buildSceneBoundaries100ns()};
    vector<uint32_t> scenes;
    for(size_t i{}; i + 1 < sceneBoundaries.size(); ++i){
        const auto midpoint{sceneBoundaries[i] + (sceneBoundaries[i + 1] - sceneBoundaries[i]) / 2};
        if(isTimeInsideRanges(midpoint, previousCutRanges)){
            scenes.push_back(static_cast<uint32_t>(i));
        }
    }
    m_prj.cutScenes(std::move(scenes));

    renderTimelineTicks();
    renderKeyframeTicks();
    renderCutOverlays();
    updateWindowTitle();

    wstring status{L"Reevaluated clear cut markers against RAP frames"};
    const auto didChange{!isSameUndoRedoState(beforeState, captureUndoRedoState())};
    if(!didChange && hasUndoSnapshot && !m_undoStack.empty()){
        m_undoStack.pop_back();
    }
    if(replacedCount == 0){
        status += L" (no changes needed)";
    }else{
        status += std::format(L" (updated {})", replacedCount);
    }
    setStatusMessage(status);
}

void MainWindow::timelineZoomSlider_ValueChanged(const Control&, const RBVArgs&){
    if(m_prj.videoFile() && m_timelineDurationSeconds > 0){
        renderTimelineAsync();
    }
    //ensureCurrentTimelineCursorVisible(); XXX: for some reason this line causes a terminating exception
    updateWindowTitle();
    tryFocusTimelineCanvas(FocusState::Programmatic);
}

void MainWindow::timelineZoomSlider_PointerWheelChanged(const Control&, const PREArgs& args){
    const auto point{args.GetCurrentPoint(TimelineZoomSlider())};
    const auto delta{point.Properties().MouseWheelDelta()};
    if(delta == 0){
        return;
    }

    auto slider{TimelineZoomSlider()};
    const auto direction{delta > 0 ? 1.0 : -1.0};
    const auto step{max(0.1, slider.SmallChange())};
    slider.Value(clamp(slider.Value() + (direction * step), slider.Minimum(), slider.Maximum()));
    args.Handled(true);
}

void MainWindow::keepAudioCheckBox_Changed(const Control&, const REArgs&){
    if(m_isApplyingUndoRedoState){
        return;
    }
    (void)pushUndoStateIfChanged();
    m_prj.keepAudio(KeepAudioCheckBox().IsChecked().GetBoolean());
    updateAudioUiAndPlaybackState();
    updateWindowTitle();
    refreshStatusInfoSection();
}

void MainWindow::audioCrossfadeComboBox_SelectionChanged(const Control&, const Control&){
    if(m_isApplyingUndoRedoState){
        return;
    }

    const auto selected{AudioCrossfadeComboBox().SelectedItem().try_as<Controls::ComboBoxItem>()};
    if(!selected || !selected.Tag()){
        return;
    }

    try{
        (void)pushUndoStateIfChanged();
        const auto val{stoi(wstring(unbox_value<hstring>(selected.Tag())).c_str())};
        m_prj.audioXfadeMs(val);
    } catch(...){
        (void)pushUndoStateIfChanged();
        m_prj.audioXfadeMs(0);
    }

    syncAudioCrossfadeComboSelection();
    updateWindowTitle();
    refreshStatusInfoSection();
}

void MainWindow::timelineHorizontalScrollBar_ValueChanged(const Control&, const RBVArgs& args){
    if(m_isClosing){
        return;
    }

    const auto scrollViewer{TimelineScrollViewer()};
    const auto offset{scrollViewer.HorizontalOffset()};
    if(fabs(offset - args.NewValue()) > 0.5){
        const auto targetOffset{box_value(args.NewValue()).as<IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }
}

void MainWindow::timelineScrollViewer_ViewChanged(const Control&, const SVVCArgs&){
    syncTimelineHorizontalScrollBar();

    if(m_isExportInProgress && m_prj.videoFile() && m_timelineDurationSeconds > 0){
        renderTimelineAsync();
    }
}

void MainWindow::timelineScrollViewer_SizeChanged(const Control&, const SCArgs&){
    syncTimelineHorizontalScrollBar();
}

void MainWindow::timelineCanvas_PointerPressed(const Control&, const PREArgs& e){
    if(m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    if(!point.Properties().IsLeftButtonPressed()){
        return;
    }

    m_isTimelineDragging = true;
    m_timelineDragMoved = false;
    m_timelineDragPointerId = e.Pointer().PointerId();
    m_timelineDragStartX = point.Position().X;
    m_timelineDragStartOffset = TimelineScrollViewer().HorizontalOffset();

    tryFocusTimelineCanvas(FocusState::Programmatic);
    TimelineCanvas().CapturePointer(e.Pointer());
    e.Handled(true);
}

void MainWindow::timelineCanvas_PointerMoved(const Control&, const PREArgs& e){
    if(!m_isTimelineDragging || e.Pointer().PointerId() != m_timelineDragPointerId){
        return;
    }

    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    const auto deltaX{point.Position().X - m_timelineDragStartX};

    if(fabs(deltaX) > 4.0){
        m_timelineDragMoved = true;
    }

    const auto scrollViewer{TimelineScrollViewer()};
    const auto viewportWidth{scrollViewer.ViewportWidth()};
    const auto maxOffset{max(0.0, TimelineCanvas().Width() - viewportWidth)};
    const auto target{clamp(m_timelineDragStartOffset - deltaX, 0.0, maxOffset)};
    const auto targetOffset{box_value(target).as<IReference<double>>()};
    scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    e.Handled(true);
}

std::optional<int64_t> MainWindow::timelinePointToTime100ns(double pointerX, double width) const{
    const auto duration100ns{m_prj.timelineDuration100ns()};
    if(duration100ns <= 0 || width <= 0){
        return std::nullopt;
    }

    const auto clampedX{clamp(pointerX, 0.0, width)};
    return static_cast<int64_t>((clampedX / width) * duration100ns);
}

bool MainWindow::toggleSelectedKeyframeAtTime100ns(int64_t time100ns){
    (void)pushUndoStateIfChanged();
    if(!m_prj.toggleSelectedKeyframeAtTime100ns(time100ns, m_mediaInfo.frameRate)){
        return false;
    }

    renderTimelineTicks();
    renderKeyframeTicks();
    renderCutOverlays();
    updateWindowTitle();
    return true;
}


void MainWindow::timelineCanvas_PointerReleased(const Control&, const PREArgs& e){
    const auto point{e.GetCurrentPoint(TimelineCanvas())};
    if(point.Properties().PointerUpdateKind() == PointerUpdateKind::RightButtonReleased){
        if(const auto time100ns{timelinePointToTime100ns(point.Position().X, TimelineCanvas().Width())}; time100ns && toggleSelectedKeyframeAtTime100ns(*time100ns)){
            e.Handled(true);
            return;
        }
    }

    if(!m_isTimelineDragging || e.Pointer().PointerId() != m_timelineDragPointerId){
        return;
    }
    const auto dragged{m_timelineDragMoved};

    m_isTimelineDragging = false;
    m_timelineDragMoved = false;
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
    if(m_isTimelineDragging && e.Pointer().PointerId() == m_timelineDragPointerId){
        m_isTimelineDragging = false;
        m_timelineDragMoved = false;
        TimelineCanvas().ReleasePointerCapture(e.Pointer());
        e.Handled(true);
    }
}

void MainWindow::timelineCanvas_PointerCaptureLost(const Control&, const PREArgs&){
    m_isTimelineDragging = false;
    m_timelineDragMoved = false;
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

    if(m_prj.videoFile() && m_timelineDurationSeconds > 0){
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
    updateTimelineCursorFromPlayback();
}

void MainWindow::updateTimelineCursorFromPlayback(){
    if(m_isClosing || !m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto current{m_player.PlaybackSession().Position()};
    const auto seconds{max(0.0, current.count() / 10'000'000.0)};
    const auto ratio{clamp(seconds / m_timelineDurationSeconds, 0.0, 1.0)};
    const auto left{ratio * TimelineCanvas().Width()};
    Controls::Canvas::SetLeft(TimelineCursor(), left);

    if(m_player.PlaybackSession().PlaybackState() == MediaPlaybackState::Playing){
        ensureTimelineCursorVisible(left);
    }

    syncTimelineHorizontalScrollBar();
}

void MainWindow::syncTimelineHorizontalScrollBar(){
    const auto scrollViewer{TimelineScrollViewer()};
    scrollViewer.UpdateLayout();

    const auto viewportWidth{max(1.0, scrollViewer.ViewportWidth())};
    const auto extentWidth{max(viewportWidth, scrollViewer.ExtentWidth())};
    const auto scrollableWidth{max(0.0, extentWidth - viewportWidth)};

    auto bar{TimelineHorizontalScrollBar()};
    bar.Minimum(0.0);
    bar.Maximum(max(1.0, scrollableWidth));
    bar.LargeChange(max(32.0, viewportWidth * 0.8));
    bar.SmallChange(24.0);
    bar.IsEnabled(scrollableWidth > 0.0);
    bar.Visibility(Visibility::Visible);

    const auto currentValue{bar.Value()};
    const auto offset{clamp(scrollViewer.HorizontalOffset(), 0.0, bar.Maximum())};
    if(fabs(currentValue - offset) > 0.5){
        bar.Value(offset);
    }
}

void MainWindow::seekTimelineToCanvasX(double pointerX){
    if(!m_player || m_timelineDurationSeconds <= 0 || TimelineCanvas().Width() <= 0){
        return;
    }

    const auto x{clamp(pointerX, 0.0, TimelineCanvas().Width())};
    const auto ratio{x / TimelineCanvas().Width()};
    const auto target100ns{static_cast<int64_t>(ratio * (m_timelineDurationSeconds * 10'000'000.0))};

    m_player.PlaybackSession().Position(TimeSpan{target100ns});
    updateTimelineCursorFromPlayback();
}

void MainWindow::ensureTimelineCursorVisible(double cursorLeft){
    const auto scrollViewer{TimelineScrollViewer()};
    const auto currentOffset{scrollViewer.HorizontalOffset()};
    const auto viewportWidth{scrollViewer.ViewportWidth()};

    if(viewportWidth <= 0){
        return;
    }

    constexpr auto cursorPadding{48.0};
    const auto minVisible{currentOffset + cursorPadding};
    const auto maxVisible{currentOffset + viewportWidth - cursorPadding};
    const auto maxOffset{max(0.0, TimelineCanvas().Width() - viewportWidth)};

    if(cursorLeft < minVisible){
        const auto targetOffset{box_value(clamp(cursorLeft - cursorPadding, 0.0, maxOffset)).as<IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }else if(cursorLeft > maxVisible){
        const auto targetOffset{box_value(clamp(cursorLeft + cursorPadding - viewportWidth, 0.0, maxOffset)).as<IReference<double>>()};
        scrollViewer.ChangeView(targetOffset, nullptr, nullptr, true);
    }
}

void MainWindow::ensureCurrentTimelineCursorVisible(){
    const auto cursorLeft{Controls::Canvas::GetLeft(TimelineCursor())};
    ensureTimelineCursorVisible(cursorLeft);
    syncTimelineHorizontalScrollBar();
}

void MainWindow::renderTimelineTicks(){
    TimelineTickCanvas().Children().Clear();

    const auto width{TimelineCanvas().Width()};
    TimelineTickCanvas().Width(width);
    if(m_timelineDurationSeconds <= 0 || width <= 0){
        refreshStatusInfoSection();
        return;
    }

    const auto majorTickCount{clamp(static_cast<int>(ceil(width / 120.0)), 6, 36)};

    for(int i{0}; i <= majorTickCount; ++i){
        const auto ratio{static_cast<double>(i) / majorTickCount};
        const auto x{ratio * width};

        Shapes::Line majorTick{};
        majorTick.X1(x);
        majorTick.X2(x);
        majorTick.Y1(7);
        majorTick.Y2(23);
        majorTick.Stroke(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 180, 180, 180)));
        majorTick.StrokeThickness(1.0);
        TimelineTickCanvas().Children().Append(majorTick);

        Controls::TextBlock label{};
        const auto totalSeconds{static_cast<int>(ratio * m_timelineDurationSeconds + 0.5)};
        const auto minutes{totalSeconds / 60};
        const auto seconds{totalSeconds % 60};
        auto text{to_wstring(minutes)};
        text += L":";
        if(seconds < 10){
            text += L"0";
        }
        text += to_wstring(seconds);
        label.Text(text);
        label.FontSize(11);
        label.Foreground(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 200, 200, 200)));
        Controls::Canvas::SetLeft(label, max(0.0, x + 3.0));
        Controls::Canvas::SetTop(label, 0);
        TimelineTickCanvas().Children().Append(label);
    }
}

void MainWindow::renderKeyframeTicks(){
    if(m_prj.frameIndex().empty() || m_timelineDurationSeconds <= 0 || TimelineTickCanvas().Width() <= 0){
        return;
    }

    const auto width {TimelineTickCanvas().Width()};
    const auto total100ns {m_timelineDurationSeconds * 10'000'000.0};

    for(const auto& frame: m_prj.frameIndex()){
        if(!frame.cleanPoint){
            continue;
        }

        const auto x {clamp((frame.time100ns / total100ns) * width, 0.0, width)};

        Shapes::Line tick{};
        tick.X1(x);
        tick.X2(x);
        tick.Y1(0);
        tick.Y2(8.0);
        tick.Stroke(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 255, 80, 80)));
        tick.StrokeThickness(2.0);
        TimelineTickCanvas().Children().Append(tick);
    }
}

void MainWindow::renderCutOverlays(){
    CutOverlayLayer().Children().Clear();

    const auto width{TimelineCanvas().Width()};
    CutOverlayLayer().Width(width);
    if(m_timelineDurationSeconds <= 0 || width <= 0){
        refreshStatusInfoSection();
        return;
    }

    const auto cutRanges100ns{m_prj.buildCutRanges100ns()};
    const auto overlayColor {Windows::UI::ColorHelper::FromArgb(180, 0, 0, 0)};
    for(const auto& [startTime100ns, endTime100ns]: cutRanges100ns){
        const auto start{clamp((static_cast<double>(startTime100ns) / 10'000'000.0) / m_timelineDurationSeconds, 0.0, 1.0)};
        const auto end{clamp((static_cast<double>(endTime100ns) / 10'000'000.0) / m_timelineDurationSeconds, 0.0, 1.0)};
        if(end <= start){
            continue;
        }

        Shapes::Rectangle block{};
        const auto left{start * width};
        block.Width(max(1.0, (end - start) * width));
        block.Height(86.0);
        block.Fill(Media::SolidColorBrush(overlayColor));
        block.IsHitTestVisible(false);
        Controls::Canvas::SetLeft(block, left);
        Controls::Canvas::SetTop(block, 0.0);
        CutOverlayLayer().Children().Append(block);
    }

    refreshStatusInfoSection();
}


bool MainWindow::toggleCutBlockAtTime100ns(int64_t time100ns){
    (void)pushUndoStateIfChanged();
    if(!m_prj.toggleCutBlockAtTime100ns(time100ns)){
        return false;
    }

    renderCutOverlays();
    updateWindowTitle();
    return true;
}

bool MainWindow::setCutBlockAtTime100ns(int64_t time100ns, bool cutScene){
    (void)pushUndoStateIfChanged();
    if(!m_prj.setCutBlockAtTime100ns(time100ns, cutScene)){
        return false;
    }

    renderCutOverlays();
    updateWindowTitle();
    return true;
}

bool MainWindow::toggleCutMarkerAtCursor(){
    if(const auto time100ns{timelinePointToTime100ns(Controls::Canvas::GetLeft(TimelineCursor()), TimelineTickCanvas().Width())}){
        return toggleSelectedKeyframeAtTime100ns(*time100ns);
    }
    return false;
}

bool MainWindow::markSceneAtCursor(bool cutScene){
    if(const auto time100ns{timelinePointToTime100ns(Controls::Canvas::GetLeft(TimelineCursor()), TimelineCanvas().Width())}){
        return setCutBlockAtTime100ns(*time100ns, cutScene);
    }
    return false;
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
    if(!m_player || delta == 0){
        return;
    }

    const auto current {m_player.PlaybackSession().Position().count()};
    constexpr auto fallbackFrameDuration100ns{333'667LL}; // ~29.97 fps

    vector<int64_t> frameDurations;
    frameDurations.reserve(m_prj.frameIndex().size());
    for(const auto& sample: m_prj.frameIndex()){
        if(sample.duration100ns > 0){
            frameDurations.push_back(sample.duration100ns);
        }
    }

    if(frameDurations.empty()){
        for(size_t i{1}; i < m_prj.frameIndex().size(); ++i){
            const auto sampleCount {static_cast<int64_t>(m_prj.frameIndex()[i].sampleIndex) - static_cast<int64_t>(m_prj.frameIndex()[i - 1].sampleIndex)};
            const auto timeDelta {m_prj.frameIndex()[i].time100ns - m_prj.frameIndex()[i - 1].time100ns};
            if(sampleCount > 0 && timeDelta > 0){
                frameDurations.push_back(timeDelta / sampleCount);
            }
        }
    }

    auto frameStep100ns{fallbackFrameDuration100ns};
    if(!frameDurations.empty()){
        nth_element(frameDurations.begin(), frameDurations.begin() + frameDurations.size() / 2, frameDurations.end());
        frameStep100ns = max(1LL, frameDurations[frameDurations.size() / 2]);
    }

    const auto direction {delta < 0 ? -1 : 1};
    const auto duration100ns {static_cast<int64_t>(max(0.0, m_timelineDurationSeconds) * 10'000'000.0)};
    const auto target {clamp(current + (direction * frameStep100ns), 0LL, duration100ns)};
    m_player.PlaybackSession().Position(TimeSpan{target});
    updateTimelineCursorFromPlayback();
    ensureCurrentTimelineCursorVisible();
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

    int64_t current100ns{};
    if(m_player){
        current100ns = max<int64_t>(0, m_player.PlaybackSession().Position().count());
    }else if(const auto cursor100ns{timelinePointToTime100ns(Controls::Canvas::GetLeft(TimelineCursor()), TimelineCanvas().Width())}){
        current100ns = *cursor100ns;
    }

    int64_t target100ns{current100ns};
    if(direction < 0){
        auto candidateFound{false};
        for(const auto& marker: markers){
            if(marker.time100ns >= current100ns){
                break;
            }
            target100ns = marker.time100ns;
            candidateFound = true;
        }
        if(!candidateFound){
            return false;
        }
    }else{
        const auto it{find_if(markers.begin(), markers.end(), [current100ns](const auto& marker){
            return marker.time100ns > current100ns;
        })};
        if(it == markers.end()){
            return false;
        }
        target100ns = it->time100ns;
    }

    if(m_player){
        m_player.PlaybackSession().Position(TimeSpan{target100ns});
        updateTimelineCursorFromPlayback();
        ensureCurrentTimelineCursorVisible();
        return true;
    }

    const auto width{TimelineCanvas().Width()};
    if(width > 0){
        const auto ratio{clamp(static_cast<double>(target100ns) / duration100ns, 0.0, 1.0)};
        const auto left{ratio * width};
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

bool MainWindow::handleStorylineKeyDown(const KRArgs& args){
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

    const auto ctrlState{InputKeyboardSource::GetKeyStateForCurrentThread(VirtualKey::Control)};
    const auto ctrlDown{(ctrlState & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down};

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
        if(args.Key() == VirtualKey::S){
            (void)saveProjectMenuItem_Click(nullptr, {});
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
    }

    if(focusOnMenu || focusInDialog){
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
                m_player.Play();
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
    default:
        return false;
    }
}

void MainWindow::window_PreviewKeyDown(const Control&, const KRArgs& args){
    (void)handleStorylineKeyDown(args);
}

void MainWindow::window_KeyDown(const Control&, const KRArgs& args){
    (void)handleStorylineKeyDown(args);
}

void MainWindow::separatePreviewWindowMenuItem_Click(const Control&, const REArgs&){
    const auto targetOpen{!m_isSeparatePreviewWindowOpen};
    if(!setSeparatePreviewWindowOpen(targetOpen)){
        SeparatePreviewWindowMenuItem().IsChecked(m_isSeparatePreviewWindowOpen);
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

    const auto target{clamp(slider.Value() + delta, slider.Minimum(), slider.Maximum())};
    slider.Value(target);
}

bool MainWindow::setSeparatePreviewWindowOpen(bool open){
    if(open == m_isSeparatePreviewWindowOpen){
        SeparatePreviewWindowMenuItem().IsChecked(open);
        return true;
    }

    if(open){
        auto previewWindow{Window()};
        previewWindow.Title(L"llvc - Video preview");

        Controls::Grid root{};
        root.Background(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(0xFF, 0x08, 0x08, 0x08)));

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
        detachedPreview.PreviewKeyDown(detachedKeyHandler);
        detachedPreview.KeyDown(detachedKeyHandler);

        root.IsTabStop(true);
        root.PreviewKeyDown(detachedKeyHandler);
        root.KeyDown(detachedKeyHandler);

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

        m_separatePreviewClosedRevoker = previewWindow.Closed(auto_revoke, {this, &MainWindow::onSeparatePreviewWindowClosed});
        m_separatePreviewWindow = previewWindow;
        m_isSeparatePreviewWindowOpen = true;
        m_isSeparatePreviewFullscreen = false;
        m_restorePreviewDetachedOnStartup = true;
        PreviewPlayer().SetMediaPlayer(nullptr);

        if(m_restorePreviewFullscreenOnStartup){
            (void)toggleSeparatePreviewFullscreen();
        }

        SeparatePreviewWindowMenuItem().IsChecked(true);
        setStatusMessage(L"Preview opened in separate window");
        return true;
    }

    if(m_separatePreviewWindow){
        HWND previewHwnd{};
        if(SUCCEEDED(m_separatePreviewWindow.as<::IWindowNative>()->get_WindowHandle(&previewHwnd)) && previewHwnd){
            saveSeparatePreviewPlacement(previewHwnd);
        }

        m_separatePreviewClosedRevoker.revoke();
        m_separatePreviewWindow.Close();
        m_separatePreviewWindow = nullptr;
    }

    PreviewPlayer().SetMediaPlayer(m_player);
    m_isSeparatePreviewWindowOpen = false;
    m_isSeparatePreviewFullscreen = false;
    m_restorePreviewDetachedOnStartup = false;
    SeparatePreviewWindowMenuItem().IsChecked(false);
    setStatusMessage(L"Preview restored to main window");
    return true;
}

void MainWindow::onSeparatePreviewWindowClosed(const Control&, const WEArgs&){
    if(m_separatePreviewWindow){
        HWND previewHwnd{};
        if(SUCCEEDED(m_separatePreviewWindow.as<::IWindowNative>()->get_WindowHandle(&previewHwnd)) && previewHwnd){
            saveSeparatePreviewPlacement(previewHwnd);
        }
    }

    m_separatePreviewClosedRevoker.revoke();
    m_separatePreviewWindow = nullptr;
    m_isSeparatePreviewWindowOpen = false;
    m_isSeparatePreviewFullscreen = false;
    if(!m_isClosing){
        m_restorePreviewDetachedOnStartup = false;
    }
    PreviewPlayer().SetMediaPlayer(m_player);
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

    if(m_isSeparatePreviewFullscreen && (m_separatePreviewRestoreRect.right > m_separatePreviewRestoreRect.left) && (m_separatePreviewRestoreRect.bottom > m_separatePreviewRestoreRect.top)){
        bounds = m_separatePreviewRestoreRect;
    }

    const auto dpi{::GetDpiForWindow(previewHwnd)};
    m_separatePreviewLeft = static_cast<int32_t>(bounds.left);
    m_separatePreviewTop = static_cast<int32_t>(bounds.top);
    m_separatePreviewWidthDips = pixelsToDips(static_cast<int32_t>(bounds.right - bounds.left), dpi);
    m_separatePreviewHeightDips = pixelsToDips(static_cast<int32_t>(bounds.bottom - bounds.top), dpi);
    m_separatePreviewDpi = static_cast<int32_t>(dpi);
    m_hasSeparatePreviewPlacement = true;
}

void MainWindow::restoreSeparatePreviewPlacement(HWND previewHwnd){
    if(!previewHwnd || !m_hasSeparatePreviewPlacement){
        return;
    }

    ::llvc::WindowPlacementState state{};
    state.left = m_separatePreviewLeft;
    state.top = m_separatePreviewTop;
    state.widthDips = m_separatePreviewWidthDips;
    state.heightDips = m_separatePreviewHeightDips;
    state.dpi = m_separatePreviewDpi;

    (void)applyWindowPlacement(previewHwnd, state, getWindowHandle(), false);

}

bool MainWindow::toggleSeparatePreviewFullscreen(){
    if(!m_isSeparatePreviewWindowOpen || !m_separatePreviewWindow){
        setStatusMessage(L"Open the separate preview window first");
        return false;
    }

    HWND previewHwnd{};
    check_hresult(m_separatePreviewWindow.as<::IWindowNative>()->get_WindowHandle(&previewHwnd));
    if(!previewHwnd){
        setErrorMessage(L"Could not resolve preview window handle");
        return false;
    }

    if(!m_isSeparatePreviewFullscreen){
        RECT currentRect{};
        if(!::GetWindowRect(previewHwnd, &currentRect)){
            setErrorMessage(L"Could not read preview window bounds");
            return false;
        }

        m_separatePreviewRestoreRect = currentRect;
        m_separatePreviewRestoreStyle = ::GetWindowLongPtrW(previewHwnd, GWL_STYLE);
        m_separatePreviewRestoreExStyle = ::GetWindowLongPtrW(previewHwnd, GWL_EXSTYLE);

        const auto fullscreenStyle{m_separatePreviewRestoreStyle & ~(WS_OVERLAPPEDWINDOW)};
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

        m_isSeparatePreviewFullscreen = true;
        m_restorePreviewFullscreenOnStartup = true;
        setStatusMessage(L"Separate preview: full-screen on");
        return true;
    }

    ::SetWindowLongPtrW(previewHwnd, GWL_STYLE, m_separatePreviewRestoreStyle);
    ::SetWindowLongPtrW(previewHwnd, GWL_EXSTYLE, m_separatePreviewRestoreExStyle);
    ::SetWindowPos(
        previewHwnd,
        HWND_NOTOPMOST,
        m_separatePreviewRestoreRect.left,
        m_separatePreviewRestoreRect.top,
        m_separatePreviewRestoreRect.right - m_separatePreviewRestoreRect.left,
        m_separatePreviewRestoreRect.bottom - m_separatePreviewRestoreRect.top,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    m_isSeparatePreviewFullscreen = false;
    m_restorePreviewFullscreenOnStartup = false;
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
        picker.SuggestedFileName(UNNAMED_PROJECT);

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
    picker.SuggestedFileName(UNNAMED_PROJECT);

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
        co_await showInfoDialogAsync(L"Open failed", L"Could not open selected recent video.");
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
        co_await showInfoDialogAsync(L"Open failed", L"Could not open selected recent project.");
    }

    tryFocusTimelineCanvas(FocusState::Programmatic);
}

AAction MainWindow::propertiesMenuItem_Click(const Control&, const REArgs&){
    co_await showPropertiesDialogAsync();
}

AAction MainWindow::exitMenuItem_Click(const Control&, const REArgs&){
    if(!co_await ensureProjectSavedBeforeContinuingAsync()){
        co_return;
    }

    Close();
}

AAction MainWindow::manualMenuItem_Click(const Control& sender, const REArgs& args){
    co_await showInfoDialogAsync(
        L"llvc Manual",
        L"Functions:\n"
        L"• Load video: Open .mp4/.mov/.avi source footage for timeline editing.\n"
        L"• Cut markers: Right-Click on the timeline/tick bar to toggle a marker at the desired frame. Markers split the video into scenes.\n"
        L"• Cut scene toggling: Ctrl+Left-Click a scene block to mark/unmark that whole scene for cutting; dark overlays indicate sections that will be removed.\n"
        L"• Preview start/pause/stop skipping cut scenes.\n"
        L"• Preview window: Tools → Preview in separate window opens a movable second window; use F11 to toggle full-screen.\n"
        L"• Audio controls: Keep/remove audio and configure cross-fade for segment transitions.\n"
        L"• Project files: Save and reopen .llvc projects with timeline state.\n"
        L"• Export: Render a lossless cut based on your selected ranges (auto-adjusting to proper cut points if necessary).\n\n"
        L"Usage workflow:\n"
        L"1) File → Load video (or drag and drop a supported file).\n"
        L"2) Right-click to place boundary markers around scenes you may want to remove.\n"
        L"3) Reevaluate cut markers to land on proper RAP frames.\n"
        L"4) Ctrl+Left-Click scene blocks to toggle which scenes are cut (dark = cut, clear = kept).\n"
        L"5) Optionally adjust Keep audio and Audio cross-fade settings, then preview playback.\n"
        L"6) Use File → Save project, then File → Export video to generate the final cut.");
}

AAction MainWindow::aboutMenuItem_Click(const Control&, const REArgs&){
    co_await showInfoDialogAsync(L"About llvc", L"llvc - Lossless Video Cut\nv0.1 - still in alpha\n\xA9 02'2026 YePhIcK");
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

AAction MainWindow::pickAndLoadVideoAsync(){
    FileOpenPicker picker{};
    picker.SuggestedStartLocation(PickerLocationId::VideosLibrary);
    picker.FileTypeFilter().Append(L".mp4");
    picker.FileTypeFilter().Append(L".mov");
    picker.FileTypeFilter().Append(L".avi");

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
    _refreshRecentFilesMenu(RecentVideosMenu(), m_recentVideos, {this, &MainWindow::recentVideoMenuItem_Click});
}

void MainWindow::refreshRecentProjectsMenu(){
    _refreshRecentFilesMenu(RecentProjectsMenu(), m_recentProjects, {this, &MainWindow::recentProjectMenuItem_Click});
}

void _addRecentVideo(vector<hstring>& recent, size_t maxCount, const hstring& path){
    recent.erase(remove(recent.begin(), recent.end(), path), recent.end());
    recent.insert(recent.begin(), path);
    if(recent.size() > maxCount){
        recent.resize(maxCount);
    }
}

void MainWindow::addRecentVideo(const hstring& path){
    if(path.empty()){
        return;
    }

    _addRecentVideo(m_recentVideos, m_maxRecentVideos, path);
    refreshRecentVideosMenu();
    saveAppSettings();
}

void MainWindow::addRecentProject(const hstring& path){
    if(path.empty()){
        return;
    }

    _addRecentVideo(m_recentProjects, m_maxRecentProjects, path);
    refreshRecentProjectsMenu();
    saveAppSettings();
}

void MainWindow::resetProjectState(){
    if(m_player){
        m_player.Pause();
    }
    m_player.Source(nullptr);

    ++m_timelineRenderVersion;
    m_projectPath.clear();
    m_prj.reset();
    clearUndoRedoHistory();
    m_mediaInfo = {};
    m_timelineDurationSeconds = 0;
    TimelineZoomSlider().Value(m_prj.zoom());
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

    setStatusMessage(L"Load or drag-and-drop an .mp4/.mov/.avi (H.264 only) file to preview.");
    clearErrorMessage();
    refreshStatusInfoSection();
    updateWindowTitle();
}

void MainWindow::updateWindowTitle(){
    wstring projectName{L"Untitled"};
    if(!m_projectPath.empty()){
        const auto projectPath{filesystem::path(m_projectPath.c_str())};
        const auto stem{projectPath.stem().wstring()};
        projectName = stem.empty() ? projectPath.filename().wstring() : stem;
        if(projectName.empty()){
            projectName = L"Untitled";
        }
    }

    if(m_prj.isDirty()){
        projectName += L"*";
    }

    const wstring loadedFile{m_prj.videoFile() ? m_prj.videoFile().Path().c_str() : L"No file"};
    Title(hstring(std::format(L"llvc - Lossless Video Cut - {} - {}", projectName, loadedFile)));
}

IOpBool MainWindow::ensureProjectSavedBeforeContinuingAsync(){
    if(!m_prj.isDirty()){
        co_return true;
    }

    Controls::ContentDialog dialog{};
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(L"Unsaved changes"));
    dialog.Content(box_value(L"Current project has unsaved changes. Save before continuing?"));
    dialog.PrimaryButtonText(L"Save");
    dialog.SecondaryButtonText(L"Don't save");
    dialog.CloseButtonText(L"Cancel");

    const auto choice{co_await dialog.ShowAsync()};
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

    co_await m_prj.open(file);

    m_prj.setZoom(clamp(m_prj.zoom(), TimelineZoomSlider().Minimum(), TimelineZoomSlider().Maximum()));
    TimelineZoomSlider().Value(m_prj.zoom());

    if(!m_prj.videoFile().Path().empty()){
        try{
            const auto videoFile{co_await StorageFile::GetFileFromPathAsync(m_prj.videoFile().Path())};
            co_await loadVideoFileAsync(videoFile);
        }catch(...){
            setStatusMessage(L"Project opened, but referenced video could not be loaded");
        }
    }

    syncAudioCrossfadeComboSelection();
    updateAudioUiAndPlaybackState();

    m_projectPath = file.Path();
    addRecentProject(m_projectPath);

    if(m_prj.videoFile().Path().empty()){
        setStatusMessage(L"Project loaded");
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

AAction MainWindow::showInfoDialogAsync(const hstring& title, const hstring& message){
    Controls::ContentDialog dialog{};
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(title));
    dialog.Content(box_value(message));
    dialog.CloseButtonText(L"OK");
    co_await dialog.ShowAsync();
}

AAction MainWindow::showPropertiesDialogAsync(){
    if(!m_prj.videoFile() || !m_mediaInfo.isValid){
        co_await showInfoDialogAsync(L"Properties", L"No video is currently loaded.");
        co_return;
    }

    co_await showInfoDialogAsync(L"Properties", hstring(buildSourcePropertiesText()));
}

wstring MainWindow::buildSourcePropertiesText() const{
    if(!m_prj.videoFile() || !m_mediaInfo.isValid){
        return L"No video is currently loaded.";
    }

    wstring content;
    content += L"File: "; content += m_prj.videoFile().Path().c_str(); content += L"\n";
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
    content += L"Audio codec: "; content += m_mediaInfo.audioCodec; content += L"\n";
    content += L"Audio bitrate: "; content += m_mediaInfo.audioBitrate;
    if(m_mediaInfo.audioDisabledForThisSource && !m_mediaInfo.audioDisabledReason.empty()){
        content += L"\nAudio note: "; content += m_mediaInfo.audioDisabledReason;
    }
    return content;
}

void MainWindow::setVideoDetailsPanelExpanded(bool expanded){
    VideoDetailsPanel().Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
    VideoDetailsOpenMarker().Visibility(expanded ? Visibility::Collapsed : Visibility::Visible);
}

void MainWindow::refreshVideoDetailsPanel(){
    VideoDetailsText().Text(buildSourcePropertiesText());
}

wstring MainWindow::formatTimelineDurationText(int64_t duration100ns){
    const auto clamped{max<int64_t>(0, duration100ns)};
    const auto totalMs{(clamped + 5'000) / 10'000};
    const auto minutes{totalMs / 60'000};
    const auto seconds{(totalMs / 1'000) % 60};
    const auto millis{totalMs % 1'000};
    return std::format(L"{:02}:{:02}.{:03}", minutes, seconds, millis);
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

void MainWindow::setStatusMessage(const wstring& message){
    StatusText().Text(message);
}

void MainWindow::setErrorMessage(const wstring& message){
    ErrorText().Text(message);
    ErrorText().Visibility(message.empty() ? Visibility::Collapsed : Visibility::Visible);
}

void MainWindow::clearErrorMessage(){
    ErrorText().Text(L"");
    ErrorText().Visibility(Visibility::Collapsed);
}

void MainWindow::refreshStatusInfoSection(){
    const auto outputDuration100ns{m_prj.outputDuration100ns()};
    wstring text{L"Estimated output: "};
    text += formatTimelineDurationText(outputDuration100ns);
    InfoText().Text(text);
}

void MainWindow::setOperationInProgress(bool active, bool indeterminate){
    if(active){
        OperationProgressBar().IsIndeterminate(indeterminate);
        if(!indeterminate){
            OperationProgressBar().Value(0);
        }
    }else{
        OperationProgressBar().IsIndeterminate(false);
    }
    OperationProgressBar().Visibility(active ? Visibility::Visible : Visibility::Collapsed);
}

void MainWindow::setOperationProgress(double percent){
    OperationProgressBar().IsIndeterminate(false);
    OperationProgressBar().Value(clamp(percent, 0.0, 100.0));
}

AAction MainWindow::showOptionsDialogAsync(){
    Controls::StackPanel panel{};
    panel.Spacing(10);

    Controls::TextBlock videosLabel{};
    videosLabel.Text(L"Recent videos to keep (1-20)");
    Controls::NumberBox videosCount{};
    videosCount.Minimum(1);
    videosCount.Maximum(20);
    videosCount.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
    videosCount.Value(m_maxRecentVideos);

    Controls::TextBlock projectsLabel{};
    projectsLabel.Text(L"Recent projects to keep (1-20)");
    Controls::NumberBox projectsCount{};
    projectsCount.Minimum(1);
    projectsCount.Maximum(20);
    projectsCount.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
    projectsCount.Value(m_maxRecentProjects);

    panel.Children().Append(videosLabel);
    panel.Children().Append(videosCount);
    panel.Children().Append(projectsLabel);
    panel.Children().Append(projectsCount);

    Controls::ContentDialog dialog{};
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(L"Options"));
    dialog.Content(panel);
    dialog.PrimaryButtonText(L"Save");
    dialog.CloseButtonText(L"Cancel");

    const auto dialogResult{co_await dialog.ShowAsync()};
    if(dialogResult != Controls::ContentDialogResult::Primary){
        co_return;
    }

    m_maxRecentVideos = static_cast<uint32_t>(clamp(static_cast<int>(lround(videosCount.Value())), 1, 20));
    m_maxRecentProjects = static_cast<uint32_t>(clamp(static_cast<int>(lround(projectsCount.Value())), 1, 20));

    if(m_recentVideos.size() > m_maxRecentVideos){
        m_recentVideos.resize(m_maxRecentVideos);
    }
    if(m_recentProjects.size() > m_maxRecentProjects){
        m_recentProjects.resize(m_maxRecentProjects);
    }

    refreshRecentVideosMenu();
    refreshRecentProjectsMenu();
    saveAppSettings();
}

bool MainWindow::isSupportedVideoSubtype(const GUID& subtype){
    return subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265;
}

wstring MainWindow::guidToCodecName(const GUID& subtype, bool isVideo){
    if(isVideo){
        if(subtype == MFVideoFormat_H264){
            return L"H.264";
        }
        if(subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_H265){
            return L"HEVC";
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
    MediaInspectionResult result{ .keyFrameSummary       = L"unknown",
                                  .keyFrameInterval      = L"unknown",
                                  .allSamplesIndependent = L"unknown",
                                  .maxKeyFrameSpacing    = L"unknown" };
    MFLifetime mf{};

    com_ptr<IMFSourceReader> reader;
    check_hresult(MFCreateSourceReaderFromURL(filePath.c_str(), nullptr, reader.put()));

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
    constexpr auto invalidStreamIndex{numeric_limits<DWORD>::max()};
    auto videoStreamIndex{invalidStreamIndex};
    uint32_t allSamplesIndependent{};
    uint32_t maxKeyFrameSpacing{};
    const auto sourceIsAvi{isAviPath(filePath)};
    wstring lowerPath{filePath};
    transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::towlower);
    const auto sourceIsMp4{lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == L".mp4"};
    const auto sourceIsMov{lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == L".mov"};

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
            if(SUCCEEDED(type->GetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, &allSamplesIndependent))){
                result.allSamplesIndependent = allSamplesIndependent != 0 ? L"yes" : L"no";
            }
            if(SUCCEEDED(type->GetUINT32(MF_MT_MAX_KEYFRAME_SPACING, &maxKeyFrameSpacing))){
                result.maxKeyFrameSpacing = to_wstring(maxKeyFrameSpacing) + L" frames";
            }
        }else if(major == MFMediaType_Audio){
            ++audioCount;
            audioSubtype = subtype;
            (void)type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &audioBitrate);
        }else{
            hasText = true;
        }
    }

    com_ptr<IMFMediaType> aviNativeH264Type;
    if(sourceIsAvi){
        if(videoCount != 1){
            result.errorMessage = BuildUnsupportedAviReason(L"Expected exactly one video stream");
            return result;
        }
        if(!IsAviH264StreamCopyCandidate(reader, videoStreamIndex, aviNativeH264Type, result.errorMessage)){
            return result;
        }

        GUID aviSubtype{GUID_NULL};
        check_hresult(aviNativeH264Type->GetGUID(MF_MT_SUBTYPE, &aviSubtype));
        if(aviSubtype != MFVideoFormat_H264){
            if(aviSubtype == MFVideoFormat_HEVC || aviSubtype == MFVideoFormat_H265){
                result.errorMessage = BuildUnsupportedAviReason(L"AVI is supported only for H.264 video. This file uses HEVC");
            }else{
                result.errorMessage = BuildUnsupportedAviReason(L"AVI is supported only for H.264 video. This file uses " + guidToVideoCodecName(aviSubtype));
            }
            return result;
        }

        if(!validateAviSampleTimesAndSyncFlags(reader, videoStreamIndex, aviNativeH264Type, result.errorMessage)){
            return result;
        }

        check_hresult(reader->SetCurrentMediaType(videoStreamIndex, nullptr, aviNativeH264Type.get()));
        videoSubtype = MFVideoFormat_H264;
        MFGetAttributeSize(aviNativeH264Type.get(), MF_MT_FRAME_SIZE, &width, &height);
        MFGetAttributeRatio(aviNativeH264Type.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        (void)aviNativeH264Type->GetUINT32(MF_MT_AVG_BITRATE, &videoBitrate);
    }

    if(videoStreamIndex != invalidStreamIndex){
        vector<GUID> allowedVideoSubtypes;
        if(sourceIsMp4 || sourceIsMov){
            allowedVideoSubtypes = {MFVideoFormat_H264, MFVideoFormat_HEVC, MFVideoFormat_H265};
        }

        if(auto bestVideoType{sourceIsAvi ? aviNativeH264Type : chooseBestNativeVideoMediaTypeForSubtypes(reader, videoStreamIndex, allowedVideoSubtypes)}){
            (void)reader->SetCurrentMediaType(videoStreamIndex, nullptr, bestVideoType.get());

            MFGetAttributeSize(bestVideoType.get(), MF_MT_FRAME_SIZE, &width, &height);

            check_hresult(bestVideoType->GetGUID(MF_MT_SUBTYPE, &videoSubtype));
            MFGetAttributeRatio(bestVideoType.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
            (void)bestVideoType->GetUINT32(MF_MT_AVG_BITRATE, &videoBitrate);
            if(SUCCEEDED(bestVideoType->GetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, &allSamplesIndependent))){
                result.allSamplesIndependent = allSamplesIndependent != 0 ? L"yes" : L"no";
            }
            if(SUCCEEDED(bestVideoType->GetUINT32(MF_MT_MAX_KEYFRAME_SPACING, &maxKeyFrameSpacing))){
                result.maxKeyFrameSpacing = to_wstring(maxKeyFrameSpacing) + L" frames";
            }
        }else if(sourceIsMp4 || sourceIsMov){
            result.errorMessage = L"No stream-copy video media type found. Require H.264 or HEVC in MP4/MOV.";
            return result;
        }
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
    if(!isSupportedVideoSubtype(videoSubtype)){
        result.errorMessage = L"Video codec not supported. Only H.264 and HEVC are allowed";
        return result;
    }

    if(sourceIsMp4){
        result.container = L"MP4";
    }else if(sourceIsMov){
        result.container = L"MOV";
    }else if(sourceIsAvi){
        result.container = L"AVI";
    }else{
        result.errorMessage = L"Container not supported. Only MP4, MOV, and AVI (H.264 only) are allowed";
        return result;
    }

    if(videoSubtype == MFVideoFormat_HEVC || videoSubtype == MFVideoFormat_H265){
        if(!hasDecoderForSubtype(videoSubtype)){
            result.errorMessage = L"HEVC support missing (install HEVC Video Extensions)";
            return result;
        }
    }else if(!hasDecoderForSubtype(videoSubtype)){
        result.errorMessage = L"No decoder available";
        return result;
    }

    PROPVARIANT duration{};
    PropVariantInit(&duration);
    if(SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) && duration.vt == VT_UI8){
        const auto seconds {duration.uhVal.QuadPart / 10'000'000.0};
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
        analyzeKeyFrameCadence(reader.get(), videoStreamIndex, fpsNum, fpsDen, result);
    }

    if(sourceIsAvi && audioCount > 0){
        result.audioDisabledForThisSource = true;
        result.audioDisabledReason = L"AVI audio passthrough is disabled in v1. Export will keep video only";
    }

    result.sourceEncodedBy = readShellStringProperty(filePath, PKEY_Media_EncodedBy);
    result.sourceComment = readShellStringProperty(filePath, PKEY_Comment);
    result.isValid = true;
    return result;
}

void MainWindow::window_DragOver(const Control&, const DEArgs& e){
    e.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
}

AAction MainWindow::window_Drop(const Control&, const DEArgs& e){
    const auto view{e.DataView()};
    if(!view.Contains(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems())){
        setStatusMessage(L"Dropped content is not a file");
        co_return;
    }

    const auto items{co_await view.GetStorageItemsAsync()};
    if(items.Size() != 1){
        setStatusMessage(L"Only support a single .mp4/.mov/.avi (H.264) file");
        co_return;
    }

    const auto file{items.GetAt(0).try_as<StorageFile>()};
    if(!file){
        setStatusMessage(L"Dropped content is not a file");
        __debugbreak(); // this should have been caught earlier in this function!
        co_return;
    }

    {
        const auto ext{file.FileType()};
        wstring lower{ext.c_str()};
        transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        if(lower != L".mp4" && lower != L".mov" && lower != L".avi"){
            setStatusMessage(L"Only .mp4, .mov, and .avi (H.264) files are supported");
            co_return;
        }
    }

    m_prj.clearTimeline();
    clearUndoRedoHistory();

    co_await loadVideoFileAsync(file);
}

AAction MainWindow::loadVideoFileAsync(const SFile& file){
    setOperationInProgress(true, true);

    MediaInspectionResult inspected{};
    try{
        inspected = inspectMediaFile(file.Path().c_str());
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
        co_await showInfoDialogAsync(L"Unsupported media", hstring(status));
        co_return;
    }

    const auto basicProperties{co_await file.GetBasicPropertiesAsync()};
    inspected.fileSize = formatFileSize(basicProperties.Size());
    inspected.sourceCreated = formatDateTimeText(file.DateCreated());
    inspected.sourceModified = formatDateTimeText(basicProperties.DateModified());
    m_mediaInfo = inspected;

    wstring status{L"Loaded: "};
    status += file.Name().c_str();
    status += L" (loading story line...)";
    setStatusMessage(status);
    clearErrorMessage();

    const auto source{Windows::Media::Core::MediaSource::CreateFromStorageFile(file)};
    m_player.Source(source);
    m_player.IsMuted(false);
    m_prj.videoFile(file);
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
}

bool MainWindow::sourceHasAudio() const{
    return m_mediaInfo.isValid && m_mediaInfo.audioCodec != L"none";
}

void MainWindow::syncAudioCrossfadeComboSelection(){
    const auto previousGuard{m_isApplyingUndoRedoState};
    m_isApplyingUndoRedoState = true;

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
                m_isApplyingUndoRedoState = previousGuard;
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

    m_isApplyingUndoRedoState = previousGuard;
}

void MainWindow::applyAudioSettingsToPlayer(){
    if(!m_player){
        return;
    }

    const auto allowAudio{sourceHasAudio() && m_prj.keepAudio()};
    m_player.IsMuted(!allowAudio);
}

void MainWindow::updateAudioUiAndPlaybackState(){
    if(!m_prj.videoFile() || m_prj.videoFile().Path().empty()){
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

    const auto previousGuard{m_isApplyingUndoRedoState};
    m_isApplyingUndoRedoState = true;
    KeepAudioCheckBox().IsEnabled(hasAudio && !audioHardDisabled);
    KeepAudioCheckBox().IsChecked(box_value(hasAudio && !audioHardDisabled && m_prj.keepAudio()).as<IReference<bool>>());
    AudioCrossfadeComboBox().IsEnabled(hasAudio && !audioHardDisabled && m_prj.keepAudio());
    m_isApplyingUndoRedoState = previousGuard;
    applyAudioSettingsToPlayer();
}

winrt::fire_and_forget MainWindow::renderTimelineAsync(){
    const auto lifetime{get_strong()};

    if(m_isClosing || !m_prj.videoFile() || m_timelineDurationSeconds <= 0){
        if(!m_isExportInProgress){
            setOperationInProgress(false);
        }
        co_return;
    }

    if(!DispatcherQueue().HasThreadAccess()){
        const auto weak{get_weak()};
        DispatcherQueue().TryEnqueue([weak](){
            if(const auto self{weak.get()}){
                self->renderTimelineAsync();
            }
        });
        co_return;
    }

    try{
        const auto renderDuringExport{m_isExportInProgress};
        if(!renderDuringExport){
            setOperationInProgress(true, true);
        }

        const auto renderVersion{++m_timelineRenderVersion};
        const auto zoomSetting{TimelineZoomSlider().Value()};
        const auto zoomScale{zoomSetting / 4.0};
        const auto totalWidth{max(800.0, m_timelineDurationSeconds * 14.0 * zoomScale)};
        const auto thumbnailCount{clamp(static_cast<int>(totalWidth / 150.0), 8, 96)};
        const auto thumbnailWidth{totalWidth / thumbnailCount};

        TimelineCanvas().Width(totalWidth);
        ThumbnailLayer().Children().Clear();
        CutOverlayLayer().Width(totalWidth);
        renderTimelineTicks();
        renderKeyframeTicks();
        renderCutOverlays();
        syncTimelineHorizontalScrollBar();

        const auto clip{co_await Windows::Media::Editing::MediaClip::CreateFromFileAsync(m_prj.videoFile())};
        Windows::Media::Editing::MediaComposition composition{};
        composition.Clips().Append(clip);

        if(renderVersion != m_timelineRenderVersion){
            co_return;
        }

        vector<bool> thumbnailBuilt(thumbnailCount, false);

        for(int builtCount{0}; builtCount < thumbnailCount; ++builtCount){
            if(renderVersion != m_timelineRenderVersion || m_isClosing){
                if(m_isClosing){
                    setOperationInProgress(false);
                }
                co_return;
            }

            const auto scrollViewer{TimelineScrollViewer()};
            const auto viewportWidth{max(0.0, scrollViewer.ViewportWidth())};
            const auto viewportLeft{scrollViewer.HorizontalOffset()};
            const auto viewportRight{viewportLeft + viewportWidth};
            const auto firstVisibleIndex{clamp(static_cast<int>(floor(viewportLeft / thumbnailWidth)), 0, thumbnailCount - 1)};
            const auto lastVisibleIndex{clamp(static_cast<int>(floor(max(viewportLeft, viewportRight - 1.0) / thumbnailWidth)), 0, thumbnailCount - 1)};

            auto nextIndex{-1};
            for(auto i{firstVisibleIndex}; i <= lastVisibleIndex; ++i){
                if(!thumbnailBuilt[i]){
                    nextIndex = i;
                    break;
                }
            }

            if(nextIndex < 0 && !renderDuringExport){
                auto left{firstVisibleIndex - 1};
                auto right{lastVisibleIndex + 1};
                while(nextIndex < 0 && (left >= 0 || right < thumbnailCount)){
                    if(right < thumbnailCount && !thumbnailBuilt[right]){
                        nextIndex = right;
                        break;
                    }
                    ++right;

                    if(left >= 0 && !thumbnailBuilt[left]){
                        nextIndex = left;
                        break;
                    }
                    --left;
                }
            }

            if(nextIndex < 0){
                break;
            }

            const auto t{(nextIndex + 0.5) / thumbnailCount};
            const auto stream{co_await composition.GetThumbnailAsync(secondsToTimeSpan(t * m_timelineDurationSeconds), 180, 96, Windows::Media::Editing::VideoFramePrecision::NearestFrame)};
            if(renderVersion != m_timelineRenderVersion || m_isClosing){
                if(m_isClosing){
                    setOperationInProgress(false);
                }
                co_return;
            }

            Controls::Image image{};
            image.Width(max(8.0, thumbnailWidth - 2.0));
            image.Height(86);
            image.Stretch(Media::Stretch::UniformToFill);

            Media::Imaging::BitmapImage bitmap{};
            co_await bitmap.SetSourceAsync(stream);
            image.Source(bitmap);

            Controls::Canvas::SetLeft(image, nextIndex * thumbnailWidth);
            ThumbnailLayer().Children().Append(image);
            thumbnailBuilt[nextIndex] = true;
            renderCutOverlays();
        }

        updateTimelineCursorFromPlayback();
        if(!renderDuringExport && m_player && m_player.PlaybackSession().PlaybackState() == MediaPlaybackState::Playing){
            ensureTimelineCursorVisible(Controls::Canvas::GetLeft(TimelineCursor()));
        }
        syncTimelineHorizontalScrollBar();

        if(!renderDuringExport){
            wstring status{L"Loaded: "};
            status += m_prj.videoFile().Name().c_str();
            status += L" (story line ready)";
            setStatusMessage(status);
            refreshStatusInfoSection();
            setOperationInProgress(false);
        }
    }catch(const winrt::hresult_error& ex){
        if(!m_isExportInProgress){
            wstring status{L"Failed to render story line: "};
            status += ex.message().c_str();
            setStatusMessage(status);
            setErrorMessage(ex.message().c_str());
            setOperationInProgress(false);
        }
    }
}

}
