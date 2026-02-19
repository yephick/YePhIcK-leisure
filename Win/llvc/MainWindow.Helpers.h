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

std::wstring formatGuid(const _GUID& guid);
std::wstring formatFileSize(std::uint64_t bytes);
std::wstring formatRatio(std::uint32_t num, std::uint32_t den);

std::wstring joinRecentItems(const std::vector<winrt::hstring>& values);
std::vector<winrt::hstring> splitRecentItems(const std::wstring& source);

bool isInMenuSubtree(const winrt::Microsoft::UI::Xaml::DependencyObject& object);
bool isInDialogSubtree(const winrt::Microsoft::UI::Xaml::DependencyObject& object);

std::vector<std::uint32_t> parseIndexList(const std::wstring& text);
std::vector<std::pair<std::uint32_t, std::uint32_t>> parseIndexPairs(const std::wstring& text);
std::wstring serializeIndexList(const std::vector<std::uint32_t>& values);
std::wstring serializeIndexPairs(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& values);

std::vector<std::int64_t> buildCleanKeyframeTimes100ns(const std::vector<IndexedFrameSample>& index);
std::vector<std::pair<std::uint32_t, std::uint32_t>> normalizeAndMergeIndexIntervals(
    std::vector<std::pair<std::uint32_t, std::uint32_t>> intervals,
    size_t keyframeCount);

std::vector<IndexedFrameSample> parseKeyframeVector(const std::wstring& text);
std::wstring serializeKeyframeVector(const std::vector<IndexedFrameSample>& index);

bool hasDecoderForSubtype(const _GUID& subtype);
void analyzeKeyFrameCadence(IMFSourceReader* reader, DWORD videoStreamIndex, std::uint32_t fpsNum, std::uint32_t fpsDen, MediaInspectionResult& result);

struct MFLifetime{
    MFLifetime();
    ~MFLifetime();
};

}
