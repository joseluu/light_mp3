#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

struct CoverArt {
    std::vector<uint8_t> data;
    std::string mimeType;  // "image/jpeg" or "image/png"
};

struct ID3Tags {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring year;
    std::wstring genre;
    std::wstring trackNumber;
    std::wstring bpm;
    std::map<std::wstring, std::wstring> customTags;    // TXXX: description -> value
    std::map<std::wstring, std::wstring> allTextFrames; // all T*** frame ID -> value
    CoverArt coverArt;
    bool hasCoverArt = false;
};

// Parse ID3v2.2, v2.3 and v2.4 tags from the given MP3 file.
ID3Tags ParseID3v2(const std::wstring& filePath);
