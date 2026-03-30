module;

#include <Windows.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

export module llvc.Utils;

import std;

export namespace llvc{

using namespace std;

bool isInMenuSubtree(const winrt::Microsoft::UI::Xaml::DependencyObject& object);
bool isInDialogSubtree(const winrt::Microsoft::UI::Xaml::DependencyObject& object);

wstring formatGuid(const _GUID& guid);
wstring formatFileSize(uint64_t bytes);
wstring formatRatio(uint32_t num, uint32_t den, const wstring& suffix);
wstring joinRecentItems(const vector<winrt::hstring>& values);
vector<winrt::hstring> splitRecentItems(const wstring& source);

struct WindowPlacementState final{
    int32_t left{};
    int32_t top{};
    int32_t widthDips{};
    int32_t heightDips{};
    int32_t dpi{96};
    bool maximized{false};
};

struct AppSettingsState final{
    vector<winrt::hstring> recentVideos{};
    vector<winrt::hstring> recentProjects{};
    uint32_t maxRecentVideos{5};
    uint32_t maxRecentProjects{5};
    bool deleteSourceAndProjectAfterExport{false};
    bool autoReevaluateCutMarkersOnPlacement{false};
    bool generateExportTimeReport{false};
    bool restorePreviewDetachedOnStartup{false};
    bool restorePreviewFullscreenOnStartup{false};
    std::optional<WindowPlacementState> separatePreviewPlacement{};
};

wstring trim(wstring value);
wstring serializeIndexList(const vector<uint32_t>& values);
wstring serializeIndexPairs(const vector<pair<uint32_t, uint32_t>>& values);
vector<uint32_t> parseIndexList(const wstring& text);
vector<pair<uint32_t, uint32_t>> parseIndexPairs(const wstring& text);
wstring serializeInt64List(const vector<int64_t>& values);
vector<int64_t> parseInt64List(const wstring& text);
int32_t pixelsToDips(int32_t pixelValue, uint32_t dpi);
int32_t dipsToPixels(int32_t dipValue, uint32_t dpi);
bool isRectVisibleOnAnyMonitor(const RECT& rect);
std::optional<WindowPlacementState> captureWindowPlacement(HWND hwnd);
bool applyWindowPlacement(HWND hwnd, const WindowPlacementState& state, HWND fallbackWindow = nullptr, bool restoreMaximized = false);
bool restoreWindowPlacementFromSettings(HWND hwnd, HWND fallbackWindow = nullptr, bool restoreMaximized = false);
void saveWindowPlacementToSettings(HWND hwnd);
AppSettingsState loadAppSettings();
void saveAppSettings(const AppSettingsState& state);

}

namespace llvc{

using namespace std;
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Windows::Storage;

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
        if(current.try_as<Controls::ContentDialog>()){
            return true;
        }
        current = Media::VisualTreeHelper::GetParent(current);
    }
    return false;
}

namespace{
constexpr wchar_t RECENT_DELIMITER{0x1F};
constexpr auto W_POS_L{L"WindowLeft"};
constexpr auto W_POS_T{L"WindowTop"};
constexpr auto W_POS_W{L"WindowWidth"};
constexpr auto W_POS_H{L"WindowHeight"};
constexpr auto W_POS_DPI{L"WindowDpi"};
constexpr auto S_RECENT_VIDEOS{L"RecentVideos"};
constexpr auto S_RECENT_PROJECTS{L"RecentProjects"};
constexpr auto S_MAX_RECENT_VIDEOS{L"MaxRecentVideos"};
constexpr auto S_MAX_RECENT_PROJECTS{L"MaxRecentProjects"};
constexpr auto S_DELETE_SOURCE_AND_PROJECT_AFTER_EXPORT{L"DeleteSourceAndProjectAfterExport"};
constexpr auto S_AUTO_REEVALUATE_CUT_MARKERS_ON_PLACEMENT{L"AutoReevaluateCutMarkersOnPlacement"};
constexpr auto S_GENERATE_EXPORT_TIME_REPORT{L"GenerateExportTimeReport"};
constexpr auto S_SEPARATE_PREVIEW_DETACHED{L"SeparatePreviewDetached"};
constexpr auto S_SEPARATE_PREVIEW_L{L"SeparatePreviewLeft"};
constexpr auto S_SEPARATE_PREVIEW_T{L"SeparatePreviewTop"};
constexpr auto S_SEPARATE_PREVIEW_W{L"SeparatePreviewWidth"};
constexpr auto S_SEPARATE_PREVIEW_H{L"SeparatePreviewHeight"};
constexpr auto S_SEPARATE_PREVIEW_DPI{L"SeparatePreviewDpi"};
constexpr auto S_SEPARATE_PREVIEW_FULLSCREEN{L"SeparatePreviewFullscreen"};
}

wstring formatGuid(const _GUID& guid){
    constexpr auto guidBufferLength{40};
    OLECHAR raw[guidBufferLength]{};
    StringFromGUID2(guid, raw, guidBufferLength);
    return wstring(raw);
}

wstring formatFileSize(uint64_t bytes){
    constexpr auto kb{1024ULL};
    constexpr auto mb{kb * 1024ULL};
    constexpr auto gb{mb * 1024ULL};

    auto text{std::format(L"{} bytes", bytes)};
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

wstring joinRecentItems(const vector<winrt::hstring>& values){
    wstring out;
    for(size_t i{}; i < values.size(); ++i){
        if(i > 0){
            out.push_back(RECENT_DELIMITER);
        }
        out += values[i].c_str();
    }
    return out;
}

vector<winrt::hstring> splitRecentItems(const wstring& source){
    vector<winrt::hstring> items;
    size_t start{};
    while(start <= source.size()){
        const auto pos{source.find(RECENT_DELIMITER, start)};
        const auto len{(pos == wstring::npos) ? (source.size() - start) : (pos - start)};
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

wstring trim(wstring value){
    const auto first{value.find_first_not_of(L" \t\r\n")};
    if(first == wstring::npos){
        return L"";
    }
    const auto last{value.find_last_not_of(L" \t\r\n")};
    return value.substr(first, last - first + 1);
}

wstring serializeIndexList(const vector<uint32_t>& values){
    wstring out;
    for(size_t i{0}; i < values.size(); ++i){
        if(i > 0){ out += L","; }
        out += to_wstring(values[i]);
    }
    return out;
}

wstring serializeIndexPairs(const vector<pair<uint32_t, uint32_t>>& values){
    wstring out;
    for(size_t i{0}; i < values.size(); ++i){
        if(i > 0){ out += L";"; }
        out += std::format(L"{},{}", values[i].first, values[i].second);
    }
    return out;
}

vector<uint32_t> parseIndexList(const wstring& text){
    vector<uint32_t> values;
    size_t start{};
    while(start <= text.size()){
        const auto pos{text.find(L',', start)};
        auto token{trim(text.substr(start, pos == wstring::npos ? wstring::npos : pos - start))};
        if(!token.empty()){
            try{ values.push_back(static_cast<uint32_t>(stoul(token))); } catch(...){}
        }
        if(pos == wstring::npos){ break; }
        start = pos + 1;
    }
    return values;
}

vector<pair<uint32_t, uint32_t>> parseIndexPairs(const wstring& text){
    vector<pair<uint32_t, uint32_t>> pairs;
    size_t start{};
    while(start <= text.size()){
        const auto sep{text.find(L';', start)};
        const auto chunk{trim(text.substr(start, sep == wstring::npos ? wstring::npos : sep - start))};
        if(!chunk.empty()){
            const auto comma{chunk.find(L',')};
            if(comma != wstring::npos){
                try{
                    const auto a{stoul(trim(chunk.substr(0, comma)))};
                    const auto b{stoul(trim(chunk.substr(comma + 1)))};
                    pairs.emplace_back(a, b);
                } catch(...){}
            }
        }
        if(sep == wstring::npos){ break; }
        start = sep + 1;
    }
    return pairs;
}

wstring serializeInt64List(const vector<int64_t>& values){
    wstring out;
    for(size_t i{0}; i < values.size(); ++i){
        if(i > 0){ out += L","; }
        out += to_wstring(values[i]);
    }
    return out;
}

vector<int64_t> parseInt64List(const wstring& text){
    vector<int64_t> values;
    size_t start{};
    while(start <= text.size()){
        const auto pos{text.find(L',', start)};
        auto token{trim(text.substr(start, pos == wstring::npos ? wstring::npos : pos - start))};
        if(!token.empty()){
            try{ values.push_back(stoll(token)); } catch(...){}
        }
        if(pos == wstring::npos){ break; }
        start = pos + 1;
    }
    sort(values.begin(), values.end());
    values.erase(unique(values.begin(), values.end()), values.end());
    return values;
}

int32_t pixelsToDips(int32_t pixelValue, uint32_t dpi){
    return static_cast<int32_t>(lround((pixelValue * 96.0) / (dpi == 0 ? 96 : dpi)));
}

int32_t dipsToPixels(int32_t dipValue, uint32_t dpi){
    return static_cast<int32_t>(lround((dipValue * (dpi == 0 ? 96U : dpi)) / 96));
}

bool isRectVisibleOnAnyMonitor(const RECT& rect){
    const auto monitor{::MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)};
    if(!monitor){
        return false;
    }

    MONITORINFO monitorInfo{.cbSize = sizeof(monitorInfo)};
    if(!::GetMonitorInfoW(monitor, &monitorInfo)){
        return false;
    }

    RECT intersection{};
    return ::IntersectRect(&intersection, &rect, &monitorInfo.rcWork) != FALSE;
}

std::optional<WindowPlacementState> captureWindowPlacement(HWND hwnd){
    if(!hwnd){
        return nullopt;
    }

    RECT bounds{};
    if(!::GetWindowRect(hwnd, &bounds)){
        return nullopt;
    }

    WindowPlacementState state{};
    WINDOWPLACEMENT placement{.length = sizeof(placement)};
    if(::GetWindowPlacement(hwnd, &placement) && placement.showCmd == SW_SHOWMAXIMIZED){
        state.maximized = true;
        bounds = placement.rcNormalPosition;
    }

    const auto dpi{::GetDpiForWindow(hwnd)};
    state.left = static_cast<int32_t>(bounds.left);
    state.top = static_cast<int32_t>(bounds.top);
    state.widthDips = pixelsToDips(static_cast<int32_t>(bounds.right - bounds.left), dpi);
    state.heightDips = pixelsToDips(static_cast<int32_t>(bounds.bottom - bounds.top), dpi);
    state.dpi = static_cast<int32_t>(dpi);
    return state;
}

bool applyWindowPlacement(HWND hwnd, const WindowPlacementState& state, HWND fallbackWindow, bool restoreMaximized){
    if(!hwnd){
        return false;
    }

    const auto currentDpi{::GetDpiForWindow(hwnd)};
    const auto width{dipsToPixels(state.widthDips, currentDpi)};
    const auto height{dipsToPixels(state.heightDips, currentDpi)};
    RECT targetRect{state.left, state.top, state.left + width, state.top + height};

    if(!isRectVisibleOnAnyMonitor(targetRect) && fallbackWindow){
        MONITORINFO monitorInfo{.cbSize = sizeof(monitorInfo)};
        const auto monitor{::MonitorFromWindow(fallbackWindow, MONITOR_DEFAULTTONEAREST)};
        if(monitor && ::GetMonitorInfoW(monitor, &monitorInfo)){
            const auto fallbackWidth{min(width, static_cast<int32_t>(monitorInfo.rcWork.right - monitorInfo.rcWork.left))};
            const auto fallbackHeight{min(height, static_cast<int32_t>(monitorInfo.rcWork.bottom - monitorInfo.rcWork.top))};
            targetRect.left = monitorInfo.rcWork.left;
            targetRect.top = monitorInfo.rcWork.top;
            targetRect.right = targetRect.left + fallbackWidth;
            targetRect.bottom = targetRect.top + fallbackHeight;
        }
    }

    if(!isRectVisibleOnAnyMonitor(targetRect)){
        return false;
    }

    ::SetWindowPos(
        hwnd,
        nullptr,
        targetRect.left,
        targetRect.top,
        targetRect.right - targetRect.left,
        targetRect.bottom - targetRect.top,
        SWP_NOACTIVATE | SWP_NOZORDER);

    if(restoreMaximized && state.maximized){
        ::ShowWindow(hwnd, SW_MAXIMIZE);
    }

    return true;
}

bool restoreWindowPlacementFromSettings(HWND hwnd, HWND fallbackWindow, bool restoreMaximized){
    const auto values{ApplicationData::Current().LocalSettings().Values()};
    if(!values.HasKey(W_POS_L) || !values.HasKey(W_POS_T) || !values.HasKey(W_POS_W) || !values.HasKey(W_POS_H)){
        return false;
    }

    WindowPlacementState state{};
    state.left = unbox_value<int32_t>(values.Lookup(W_POS_L));
    state.top = unbox_value<int32_t>(values.Lookup(W_POS_T));
    state.widthDips = unbox_value<int32_t>(values.Lookup(W_POS_W));
    state.heightDips = unbox_value<int32_t>(values.Lookup(W_POS_H));
    state.dpi = values.HasKey(W_POS_DPI) ? unbox_value<int32_t>(values.Lookup(W_POS_DPI)) : 96;

    if(applyWindowPlacement(hwnd, state, fallbackWindow, restoreMaximized)){
        return true;
    }

    values.Remove(W_POS_L);
    values.Remove(W_POS_T);
    values.Remove(W_POS_W);
    values.Remove(W_POS_H);
    values.Remove(W_POS_DPI);
    return false;
}

void saveWindowPlacementToSettings(HWND hwnd){
    const auto captured{captureWindowPlacement(hwnd)};
    if(!captured){
        return;
    }

    const auto values{ApplicationData::Current().LocalSettings().Values()};
    values.Insert(W_POS_L, box_value(captured->left));
    values.Insert(W_POS_T, box_value(captured->top));
    values.Insert(W_POS_W, box_value(captured->widthDips));
    values.Insert(W_POS_H, box_value(captured->heightDips));
    values.Insert(W_POS_DPI, box_value(captured->dpi));
}

AppSettingsState loadAppSettings(){
    AppSettingsState state{};
    const auto values{ApplicationData::Current().LocalSettings().Values()};

    if(values.HasKey(S_MAX_RECENT_VIDEOS)){
        const auto parsed{unbox_value<int32_t>(values.Lookup(S_MAX_RECENT_VIDEOS))};
        state.maxRecentVideos = static_cast<uint32_t>(clamp(parsed, 1, 20));
    }
    if(values.HasKey(S_MAX_RECENT_PROJECTS)){
        const auto parsed{unbox_value<int32_t>(values.Lookup(S_MAX_RECENT_PROJECTS))};
        state.maxRecentProjects = static_cast<uint32_t>(clamp(parsed, 1, 20));
    }

    if(values.HasKey(S_RECENT_VIDEOS)){
        state.recentVideos = splitRecentItems(unbox_value<hstring>(values.Lookup(S_RECENT_VIDEOS)).c_str());
    }
    if(values.HasKey(S_RECENT_PROJECTS)){
        state.recentProjects = splitRecentItems(unbox_value<hstring>(values.Lookup(S_RECENT_PROJECTS)).c_str());
    }

    state.deleteSourceAndProjectAfterExport = values.HasKey(S_DELETE_SOURCE_AND_PROJECT_AFTER_EXPORT)
        && unbox_value<bool>(values.Lookup(S_DELETE_SOURCE_AND_PROJECT_AFTER_EXPORT));
    state.autoReevaluateCutMarkersOnPlacement = values.HasKey(S_AUTO_REEVALUATE_CUT_MARKERS_ON_PLACEMENT)
        && unbox_value<bool>(values.Lookup(S_AUTO_REEVALUATE_CUT_MARKERS_ON_PLACEMENT));
    state.generateExportTimeReport = values.HasKey(S_GENERATE_EXPORT_TIME_REPORT)
        && unbox_value<bool>(values.Lookup(S_GENERATE_EXPORT_TIME_REPORT));
    state.restorePreviewDetachedOnStartup = values.HasKey(S_SEPARATE_PREVIEW_DETACHED)
        && unbox_value<bool>(values.Lookup(S_SEPARATE_PREVIEW_DETACHED));
    state.restorePreviewFullscreenOnStartup = values.HasKey(S_SEPARATE_PREVIEW_FULLSCREEN)
        && unbox_value<bool>(values.Lookup(S_SEPARATE_PREVIEW_FULLSCREEN));

    if(state.recentVideos.size() > state.maxRecentVideos){
        state.recentVideos.resize(state.maxRecentVideos);
    }
    if(state.recentProjects.size() > state.maxRecentProjects){
        state.recentProjects.resize(state.maxRecentProjects);
    }

    if(values.HasKey(S_SEPARATE_PREVIEW_L) && values.HasKey(S_SEPARATE_PREVIEW_T) && values.HasKey(S_SEPARATE_PREVIEW_W) && values.HasKey(S_SEPARATE_PREVIEW_H)){
        WindowPlacementState placement{};
        placement.left = unbox_value<int32_t>(values.Lookup(S_SEPARATE_PREVIEW_L));
        placement.top = unbox_value<int32_t>(values.Lookup(S_SEPARATE_PREVIEW_T));
        placement.widthDips = max<int32_t>(320, unbox_value<int32_t>(values.Lookup(S_SEPARATE_PREVIEW_W)));
        placement.heightDips = max<int32_t>(200, unbox_value<int32_t>(values.Lookup(S_SEPARATE_PREVIEW_H)));
        placement.dpi = values.HasKey(S_SEPARATE_PREVIEW_DPI) ? max<int32_t>(96, unbox_value<int32_t>(values.Lookup(S_SEPARATE_PREVIEW_DPI))) : 96;
        state.separatePreviewPlacement = placement;
    }

    return state;
}

void saveAppSettings(const AppSettingsState& state){
    const auto values{ApplicationData::Current().LocalSettings().Values()};
    values.Insert(S_MAX_RECENT_VIDEOS, box_value(static_cast<int32_t>(state.maxRecentVideos)));
    values.Insert(S_MAX_RECENT_PROJECTS, box_value(static_cast<int32_t>(state.maxRecentProjects)));
    values.Insert(S_DELETE_SOURCE_AND_PROJECT_AFTER_EXPORT, box_value(state.deleteSourceAndProjectAfterExport));
    values.Insert(S_AUTO_REEVALUATE_CUT_MARKERS_ON_PLACEMENT, box_value(state.autoReevaluateCutMarkersOnPlacement));
    values.Insert(S_GENERATE_EXPORT_TIME_REPORT, box_value(state.generateExportTimeReport));
    values.Insert(S_RECENT_VIDEOS, box_value(hstring(joinRecentItems(state.recentVideos))));
    values.Insert(S_RECENT_PROJECTS, box_value(hstring(joinRecentItems(state.recentProjects))));
    values.Insert(S_SEPARATE_PREVIEW_DETACHED, box_value(state.restorePreviewDetachedOnStartup));
    values.Insert(S_SEPARATE_PREVIEW_FULLSCREEN, box_value(state.restorePreviewFullscreenOnStartup));

    if(state.separatePreviewPlacement){
        values.Insert(S_SEPARATE_PREVIEW_L, box_value(state.separatePreviewPlacement->left));
        values.Insert(S_SEPARATE_PREVIEW_T, box_value(state.separatePreviewPlacement->top));
        values.Insert(S_SEPARATE_PREVIEW_W, box_value(state.separatePreviewPlacement->widthDips));
        values.Insert(S_SEPARATE_PREVIEW_H, box_value(state.separatePreviewPlacement->heightDips));
        values.Insert(S_SEPARATE_PREVIEW_DPI, box_value(state.separatePreviewPlacement->dpi));
    }else{
        values.Remove(S_SEPARATE_PREVIEW_L);
        values.Remove(S_SEPARATE_PREVIEW_T);
        values.Remove(S_SEPARATE_PREVIEW_W);
        values.Remove(S_SEPARATE_PREVIEW_H);
        values.Remove(S_SEPARATE_PREVIEW_DPI);
    }
}

}
