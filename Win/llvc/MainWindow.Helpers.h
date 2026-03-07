#pragma once

#include "MainWindow.xaml.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct _GUID;
struct IMFSourceReader;

namespace winrt::Microsoft::UI::Xaml{
struct DependencyObject;
}

namespace winrt::llvc::implementation{

using namespace ::std;

wstring formatGuid(const _GUID& guid);
wstring formatFileSize(uint64_t bytes);
wstring formatRatio(uint32_t num, uint32_t den, const wstring& suffix);

wstring joinRecentItems(const vector<winrt::hstring>& values);
vector<winrt::hstring> splitRecentItems(const wstring& source);


vector<int64_t> buildCleanKeyframeTimes100ns(const vector<::llvc::IndexedFrameSample>& index);
vector<pair<uint32_t, uint32_t>> normalizeAndMergeIndexIntervals(
    vector<pair<uint32_t, uint32_t>> intervals,
    size_t keyframeCount);

bool hasDecoderForSubtype(const _GUID& subtype);
void analyzeKeyFrameCadence(IMFSourceReader* reader, DWORD videoStreamIndex, uint32_t fpsNum, uint32_t fpsDen, MediaInspectionResult& result);

struct MFLifetime{
    MFLifetime();
    ~MFLifetime();
};

}
