#include "pch.h"
#include "MainWindow.Helpers.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <numeric>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

import Utils;

using namespace llvc;
using namespace std;
using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::llvc::implementation{

namespace{
constexpr wchar_t RECENT_DELIMITER{0x1F};
constexpr auto TIMELINE_EDGE_SENTINEL{numeric_limits<uint32_t>::max()};
}

MFLifetime::MFLifetime(){
    check_hresult(MFStartup(MF_VERSION, MFSTARTUP_FULL));
}

MFLifetime::~MFLifetime(){
    MFShutdown();
}

wstring formatGuid(const GUID& guid){
    constexpr auto guidBufferLength{40};
    OLECHAR raw[guidBufferLength]{};
    StringFromGUID2(guid, raw, guidBufferLength);
    return wstring(raw);
}

wstring formatFileSize(uint64_t bytes){
    constexpr auto kb{1024ULL};
    constexpr auto mb{kb * 1024ULL};
    constexpr auto gb{mb * 1024ULL};

    auto text {std::format(L"{} bytes", bytes)};
    if(bytes >= gb){
        text += std::format(L" ({:.2f} GB)", (1.0 * bytes) / gb);
    }else if(bytes >= mb){
        text += std::format(L" ({:.2f} MB)", (1.0 * bytes) / mb);
    }
    return text;
}

wstring formatRatio(uint32_t num, uint32_t den, const wstring& suffix){
    if(den == 0){
        return L"-";
    }
    return std::format(L"{:.3f}{}", (1.0 * num) / den, suffix);
}

wstring joinRecentItems(const vector<hstring>& values){
    wstring out;
    for(size_t i = 0; i < values.size(); ++i){
        if(i > 0){
            out.push_back(RECENT_DELIMITER);
        }
        out += values[i].c_str();
    }
    return out;
}

vector<hstring> splitRecentItems(const wstring& source){
    vector<hstring> items;
    size_t start{};
    while(start <= source.size()){
        const auto pos {source.find(RECENT_DELIMITER, start)};
        const auto len {(pos == wstring::npos) ? (source.size() - start) : (pos - start)};
        if(len > 0){
            items.emplace_back(source.substr(start, len));
        }
        if(pos == wstring::npos){
            break;
        }
        start = pos + 1;
    }
    return items;
}

bool isInMenuSubtree(const DependencyObject& object){
    auto current{object};
    while(current){
        if(current.try_as<Controls::MenuBar>() || current.try_as<Controls::MenuBarItem>() || current.try_as<Controls::MenuFlyoutItem>() || current.try_as<Controls::MenuFlyoutSubItem>() || current.try_as<Controls::MenuFlyoutPresenter>()){
            return true;
        }
        current = Media::VisualTreeHelper::GetParent(current);
    }
    return false;
}

bool isInDialogSubtree(const DependencyObject& object){
    auto current{object};
    while(current){
        if(current.try_as<Controls::ContentDialog>() || current.try_as<Controls::Primitives::Popup>()){
            return true;
        }
        current = Media::VisualTreeHelper::GetParent(current);
    }
    return false;
}

vector<int64_t> buildCleanKeyframeTimes100ns(const vector<::llvc::IndexedFrameSample>& index){
    vector<int64_t> times;
    times.reserve(index.size());
    for(const auto& sample: index){
        if(sample.cleanPoint){
            times.push_back(sample.time100ns);
        }
    }
    return times;
}

vector<pair<uint32_t, uint32_t>> normalizeAndMergeIndexIntervals(vector<pair<uint32_t, uint32_t>> intervals, size_t keyframeCount){
    using RankedInterval = pair<int64_t, int64_t>;
    vector<RankedInterval> normalized;
    normalized.reserve(intervals.size());

    for(const auto& interval: intervals){
        if((interval.first == TIMELINE_EDGE_SENTINEL && interval.second == TIMELINE_EDGE_SENTINEL)
            || (interval.first == TIMELINE_EDGE_SENTINEL && interval.second >= keyframeCount)
            || (interval.second == TIMELINE_EDGE_SENTINEL && interval.first >= keyframeCount)
            || (interval.first != TIMELINE_EDGE_SENTINEL && interval.second != TIMELINE_EDGE_SENTINEL && (interval.first >= keyframeCount || interval.second >= keyframeCount))){
            continue;
        }

        const auto startRank {interval.first == TIMELINE_EDGE_SENTINEL ? static_cast<int64_t>(-1) : static_cast<int64_t>(interval.first)};
        const auto endRank {interval.second == TIMELINE_EDGE_SENTINEL ? static_cast<int64_t>(keyframeCount) : static_cast<int64_t>(interval.second)};
        if(startRank >= endRank){
            continue;
        }
        normalized.emplace_back(startRank, endRank);
    }

    sort(normalized.begin(), normalized.end());

    vector<RankedInterval> mergedRanks;
    for(const auto& interval: normalized){
        if(mergedRanks.empty() || interval.first > mergedRanks.back().second){
            mergedRanks.push_back(interval);
        }else{
            mergedRanks.back().second = max(mergedRanks.back().second, interval.second);
        }
    }

    vector<pair<uint32_t, uint32_t>> merged;
    merged.reserve(mergedRanks.size());
    for(const auto& interval: mergedRanks){
        const auto start {interval.first < 0 ? TIMELINE_EDGE_SENTINEL : static_cast<uint32_t>(interval.first)};
        const auto end {interval.second >= static_cast<int64_t>(keyframeCount) ? TIMELINE_EDGE_SENTINEL : static_cast<uint32_t>(interval.second)};
        merged.emplace_back(start, end);
    }

    return merged;
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

void analyzeKeyFrameCadence(IMFSourceReader* reader, DWORD videoStreamIndex, uint32_t fpsNum, uint32_t fpsDen, MediaInspectionResult& result){
    constexpr uint32_t maxSamplesToInspect{1500};
    constexpr LONGLONG maxSpan100ns{120LL * 10'000'000LL};

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
        if(FAILED(hr)){
            break;
        }
        if(flags & MF_SOURCE_READERF_ENDOFSTREAM){
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
        result.keyFrameSummary = L"unknown (no samples read)";
        result.keyFrameInterval = L"unknown";
        return;
    }

    if(!cleanPointSeen){
        result.keyFrameSummary = L"unknown (clean-point flags unavailable)";
        result.keyFrameInterval = L"unknown";
        return;
    }

    const auto ratio {(1.0 * keyFrames) / sampledFrames};
    result.keyFrameSummary = std::format(L"{} key frames / {} sampled frames ({:.2f}%)", keyFrames, sampledFrames, ratio * 100.0);

    if(keyIntervalsSec.empty()){
        result.keyFrameInterval = L"unknown (insufficient key frames sampled)";
        return;
    }

    const auto sum {accumulate(keyIntervalsSec.begin(), keyIntervalsSec.end(), 0.0)};
    const auto avg {sum / keyIntervalsSec.size()};
    const auto minIt {min_element(keyIntervalsSec.begin(), keyIntervalsSec.end())};
    const auto maxIt {max_element(keyIntervalsSec.begin(), keyIntervalsSec.end())};

    auto text {std::format(L"avg {:.3f} s, min {:.3f} s, max {:.3f} s", avg, *minIt, *maxIt)};

    if(fpsNum > 0 && fpsDen > 0){
        const auto fps{(1.0 * fpsNum) / fpsDen};
        text += std::format(L" (~{:.1f} frames avg)", avg * fps);
    }

    result.keyFrameInterval = text;
}

}
