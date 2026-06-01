module;

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <winrt/base.h>

export module llvc.AudioWaveform;

import std;
import llvc.Export;

export namespace llvc{

using namespace ::std;

inline constexpr double AudioWaveformThresholdLineRatio{0.8};

struct AudioWaveformChunkRange final{
    int first{};
    int last{};
};

wstring formatWaveformThresholdDb(double thresholdDb);
float clampAudioPeak(float value);
double audioWaveformThresholdAmplitude(double thresholdDb);
double audioWaveformHeightRatio(float peak, double thresholdDb);
bool audioWaveformPeakIsHot(float peak, double thresholdDb);
AudioWaveformChunkRange visibleAudioWaveformChunkRange(double viewportLeft, double viewportWidth, double chunkWidth, int chunkCount);
int chooseNextAudioWaveformChunkIndex(const vector<bool>& chunkBuilt, AudioWaveformChunkRange visibleRange, bool allowOffscreenExpansion);
size_t audioWaveformChunkBucketStart(size_t chunkIndex, size_t chunkCount, size_t bucketCount);
size_t audioWaveformChunkBucketEnd(size_t chunkIndex, size_t chunkCount, size_t bucketCount);
vector<float> analyzeAudioWaveformChunkPeaks(
    const wstring& sourcePath,
    int64_t duration100ns,
    size_t bucketCount,
    size_t chunkIndex,
    size_t chunkCount,
    const function<bool()>& shouldCancel = {});

}

namespace llvc{

using namespace ::winrt;

std::once_flag g_audioWaveformMfStartupOnce;
std::unique_ptr<MFLifetime> g_audioWaveformMfLifetime;

void ensureAudioWaveformMediaFoundationLifetime(){
    std::call_once(g_audioWaveformMfStartupOnce, []{
        g_audioWaveformMfLifetime = std::make_unique<MFLifetime>();
    });
}

std::optional<std::pair<DWORD, GUID>> tryGetFirstAudioStreamIndex(IMFSourceReader* reader){
    constexpr auto invalidStream{(std::numeric_limits<DWORD>::max)()};
    DWORD audioStreamIndex{invalidStream};
    GUID audioSubtype{GUID_NULL};
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
        GUID subtype{};
        if(SUCCEEDED(nativeType->GetGUID(MF_MT_MAJOR_TYPE, &major)) && major == MFMediaType_Audio){
            (void)nativeType->GetGUID(MF_MT_SUBTYPE, &subtype);
            audioStreamIndex = streamIndex;
            audioSubtype = subtype;
            break;
        }
    }

    if(audioStreamIndex == invalidStream){
        return std::nullopt;
    }
    return std::pair{audioStreamIndex, audioSubtype};
}

wstring formatWaveformThresholdDb(double thresholdDb){
    return std::format(L"{:.0f} dB", thresholdDb);
}

float clampAudioPeak(float value){
    return std::clamp(value, 0.0f, 1.0f);
}

double audioWaveformThresholdAmplitude(double thresholdDb){
    return std::pow(10.0, thresholdDb / 20.0);
}

double audioWaveformHeightRatio(float peak, double thresholdDb){
    const auto clampedPeak{clampAudioPeak(peak)};
    if(clampedPeak <= 0.0f){
        return 0.0;
    }

    const auto thresholdAmplitude{audioWaveformThresholdAmplitude(thresholdDb)};
    if(thresholdAmplitude <= 0.0){
        return std::clamp(static_cast<double>(clampedPeak), 0.0, 1.0);
    }

    if(clampedPeak <= thresholdAmplitude){
        return std::clamp((static_cast<double>(clampedPeak) / thresholdAmplitude) * AudioWaveformThresholdLineRatio, 0.0, 1.0);
    }

    const auto aboveRatio{(static_cast<double>(clampedPeak) - thresholdAmplitude) / std::max(0.000001, 1.0 - thresholdAmplitude)};
    return std::clamp(AudioWaveformThresholdLineRatio + (aboveRatio * (1.0 - AudioWaveformThresholdLineRatio)), 0.0, 1.0);
}

bool audioWaveformPeakIsHot(float peak, double thresholdDb){
    constexpr auto comparisonTolerance{0.000001};
    return static_cast<double>(clampAudioPeak(peak)) + comparisonTolerance >= audioWaveformThresholdAmplitude(thresholdDb);
}

AudioWaveformChunkRange visibleAudioWaveformChunkRange(double viewportLeft, double viewportWidth, double chunkWidth, int chunkCount){
    if(chunkCount <= 0 || chunkWidth <= 0.0){
        return AudioWaveformChunkRange{};
    }

    const auto safeViewportLeft{std::max(0.0, viewportLeft)};
    const auto viewportRight{safeViewportLeft + std::max(0.0, viewportWidth)};
    return AudioWaveformChunkRange{
        .first = std::clamp(static_cast<int>(std::floor(safeViewportLeft / chunkWidth)), 0, chunkCount - 1),
        .last = std::clamp(static_cast<int>(std::floor(std::max(safeViewportLeft, viewportRight - 1.0) / chunkWidth)), 0, chunkCount - 1),
    };
}

int chooseNextAudioWaveformChunkIndex(const vector<bool>& chunkBuilt, AudioWaveformChunkRange visibleRange, bool allowOffscreenExpansion){
    const auto chunkCount{static_cast<int>(chunkBuilt.size())};
    if(chunkCount <= 0){
        return -1;
    }

    for(auto index{visibleRange.first}; index <= visibleRange.last; ++index){
        if(index >= 0 && index < chunkCount && !chunkBuilt[index]){
            return index;
        }
    }

    if(!allowOffscreenExpansion){
        return -1;
    }

    auto left{visibleRange.first - 1};
    auto right{visibleRange.last + 1};
    while(left >= 0 || right < chunkCount){
        if(right < chunkCount && !chunkBuilt[right]){
            return right;
        }
        ++right;

        if(left >= 0 && !chunkBuilt[left]){
            return left;
        }
        --left;
    }

    return -1;
}

size_t audioWaveformChunkBucketStart(size_t chunkIndex, size_t chunkCount, size_t bucketCount){
    if(chunkCount == 0 || bucketCount == 0 || chunkIndex >= chunkCount){
        return bucketCount;
    }

    return std::min(bucketCount, (bucketCount * chunkIndex) / chunkCount);
}

size_t audioWaveformChunkBucketEnd(size_t chunkIndex, size_t chunkCount, size_t bucketCount){
    if(chunkCount == 0 || bucketCount == 0 || chunkIndex >= chunkCount){
        return bucketCount;
    }

    return chunkIndex + 1 >= chunkCount
        ? bucketCount
        : std::min(bucketCount, (bucketCount * (chunkIndex + 1)) / chunkCount);
}

vector<float> analyzeAudioWaveformChunkPeaks(
    const wstring& sourcePath,
    int64_t duration100ns,
    size_t bucketCount,
    size_t chunkIndex,
    size_t chunkCount,
    const function<bool()>& shouldCancel){
    if(sourcePath.empty() || duration100ns <= 0 || bucketCount == 0 || chunkCount == 0 || chunkIndex >= chunkCount){
        return {};
    }

    const auto coinitHr{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    const auto shouldCoUninitialize{SUCCEEDED(coinitHr)};
    if(FAILED(coinitHr) && coinitHr != RPC_E_CHANGED_MODE){
        return {};
    }
    struct CoUninitGuard final{
        bool active{};
        ~CoUninitGuard(){ if(active){ CoUninitialize(); } }
    } coGuard{shouldCoUninitialize};

    ensureAudioWaveformMediaFoundationLifetime();

    const auto bucketStart{audioWaveformChunkBucketStart(chunkIndex, chunkCount, bucketCount)};
    const auto bucketEnd{audioWaveformChunkBucketEnd(chunkIndex, chunkCount, bucketCount)};
    if(bucketStart >= bucketEnd){
        return {};
    }

    vector<float> peaks(bucketEnd - bucketStart, 0.0f);
    const auto chunkStart100ns{static_cast<int64_t>((static_cast<long double>(duration100ns) * chunkIndex) / chunkCount)};
    const auto chunkEnd100ns{chunkIndex + 1 >= chunkCount
        ? duration100ns
        : static_cast<int64_t>((static_cast<long double>(duration100ns) * (chunkIndex + 1)) / chunkCount)};
    if(chunkEnd100ns <= chunkStart100ns){
        return peaks;
    }

    com_ptr<IMFAttributes> attributes;
    check_hresult(MFCreateAttributes(attributes.put(), 1));
    check_hresult(attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE));

    com_ptr<IMFSourceReader> reader;
    check_hresult(MFCreateSourceReaderFromURL(sourcePath.c_str(), attributes.get(), reader.put()));

    const auto audioStreamInfo{tryGetFirstAudioStreamIndex(reader.get())};
    if(!audioStreamInfo){
        return {};
    }
    const auto [audioStreamIndex, audioSubtype] = *audioStreamInfo;
    (void)audioSubtype;

    check_hresult(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
    check_hresult(reader->SetStreamSelection(audioStreamIndex, TRUE));

    com_ptr<IMFMediaType> outputType;
    check_hresult(MFCreateMediaType(outputType.put()));
    check_hresult(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    check_hresult(outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float));
    check_hresult(reader->SetCurrentMediaType(audioStreamIndex, nullptr, outputType.get()));

    com_ptr<IMFMediaType> currentType;
    check_hresult(reader->GetCurrentMediaType(audioStreamIndex, currentType.put()));

    uint32_t sampleRate{};
    uint32_t channelCount{};
    check_hresult(currentType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate));
    check_hresult(currentType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channelCount));
    if(sampleRate == 0 || channelCount == 0){
        return {};
    }

    PROPVARIANT seekPosition{};
    seekPosition.vt = VT_I8;
    seekPosition.hVal.QuadPart = chunkStart100ns;
    check_hresult(reader->SetCurrentPosition(GUID_NULL, seekPosition));

    while(true){
        if(shouldCancel && shouldCancel()){
            return {};
        }

        DWORD actualStream{};
        DWORD flags{};
        LONGLONG timestamp{};
        com_ptr<IMFSample> sample;
        const auto hr{reader->ReadSample(audioStreamIndex, 0, &actualStream, &flags, &timestamp, sample.put())};
        if(FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)){
            break;
        }
        if(!sample){
            continue;
        }

        LONGLONG sampleDuration{};
        if(FAILED(sample->GetSampleDuration(&sampleDuration))){
            sampleDuration = 0;
        }

        com_ptr<IMFMediaBuffer> contiguousBuffer;
        check_hresult(sample->ConvertToContiguousBuffer(contiguousBuffer.put()));

        BYTE* sourceBytes{};
        DWORD maxLength{};
        DWORD currentLength{};
        check_hresult(contiguousBuffer->Lock(&sourceBytes, &maxLength, &currentLength));

        const auto* floatSamples{reinterpret_cast<const float*>(sourceBytes)};
        const auto frameCount{static_cast<size_t>(currentLength / (sizeof(float) * channelCount))};
        const auto estimatedDuration{sampleDuration > 0
            ? sampleDuration
            : static_cast<LONGLONG>((frameCount * 10'000'000LL) / sampleRate)};

        for(size_t frameIndex{}; frameIndex < frameCount; ++frameIndex){
            float monoPeak{};
            for(uint32_t channel{}; channel < channelCount; ++channel){
                monoPeak = std::max(monoPeak, std::abs(floatSamples[(frameIndex * channelCount) + channel]));
            }

            const auto frameTime100ns{
                timestamp + static_cast<LONGLONG>((estimatedDuration * static_cast<LONGLONG>(frameIndex)) / std::max<size_t>(1, frameCount))};
            if(frameTime100ns >= chunkEnd100ns){
                break;
            }
            if(frameTime100ns < chunkStart100ns){
                continue;
            }
            const auto clampedTime100ns{std::clamp<int64_t>(static_cast<int64_t>(frameTime100ns), 0, duration100ns - 1)};
            const auto bucketIndex{
                std::min<size_t>(bucketCount - 1, static_cast<size_t>((clampedTime100ns * static_cast<int64_t>(bucketCount)) / duration100ns))};
            if(bucketIndex >= bucketStart && bucketIndex < bucketEnd){
                peaks[bucketIndex - bucketStart] = std::max(peaks[bucketIndex - bucketStart], clampAudioPeak(monoPeak));
            }
        }

        contiguousBuffer->Unlock();
        const auto sampleEnd100ns{timestamp + estimatedDuration};
        if(sampleEnd100ns >= chunkEnd100ns){
            break;
        }
    }

    return peaks;
}

}
