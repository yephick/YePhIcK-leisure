module;

#include <Windows.h>

export module llvc.Utils;

import std;

export namespace llvc{

using namespace std;

struct WindowPlacementState final{
    int32_t left{};
    int32_t top{};
    int32_t widthDips{};
    int32_t heightDips{};
    int32_t dpi{96};
    bool maximized{false};
};

wstring trim(wstring value);
wstring serializeIndexList(const vector<uint32_t>& values);
wstring serializeIndexPairs(const vector<pair<uint32_t, uint32_t>>& values);
vector<uint32_t> parseIndexList(const wstring& text);
vector<pair<uint32_t, uint32_t>> parseIndexPairs(const wstring& text);
int32_t pixelsToDips(int32_t pixelValue, uint32_t dpi);
int32_t dipsToPixels(int32_t dipValue, uint32_t dpi);
bool isRectVisibleOnAnyMonitor(const RECT& rect);
optional<WindowPlacementState> captureWindowPlacement(HWND hwnd);
bool applyWindowPlacement(HWND hwnd, const WindowPlacementState& state, HWND fallbackWindow = nullptr, bool restoreMaximized = false);

}

namespace llvc{

using namespace std;

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

optional<WindowPlacementState> captureWindowPlacement(HWND hwnd){
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

}
