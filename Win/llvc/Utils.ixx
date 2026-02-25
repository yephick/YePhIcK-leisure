export module Utils;

import std;

export namespace llvc{

using namespace std;

wstring trim(wstring value);
wstring serializeIndexList(const vector<uint32_t>& values);
wstring serializeIndexPairs(const vector<pair<uint32_t, uint32_t>>& values);
vector<uint32_t> parseIndexList(const wstring& text);
vector<pair<uint32_t, uint32_t>> parseIndexPairs(const wstring& text);

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

}
