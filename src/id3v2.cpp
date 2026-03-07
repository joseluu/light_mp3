#include "id3v2.h"
#define NOMINMAX
#include <windows.h>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <set>

// ---------------------------------------------------------------------------
// Integer helpers
// ---------------------------------------------------------------------------

static uint32_t ReadSynchsafe(const uint8_t* b) {
    return ((b[0] & 0x7F) << 21) | ((b[1] & 0x7F) << 14)
         | ((b[2] & 0x7F) <<  7) |  (b[3] & 0x7F);
}

static uint32_t ReadUint32BE(const uint8_t* b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
         | ((uint32_t)b[2] <<  8) |  (uint32_t)b[3];
}

static uint32_t ReadUint24BE(const uint8_t* b) {
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
}

// ---------------------------------------------------------------------------
// Text decoding
// ---------------------------------------------------------------------------

static std::wstring DecodeText(const uint8_t* data, size_t size, uint8_t enc) {
    if (size == 0) return L"";

    switch (enc) {
    case 0: { // ISO-8859-1
        int len = MultiByteToWideChar(1252, 0, (const char*)data, (int)size, nullptr, 0);
        std::wstring r(len, L'\0');
        MultiByteToWideChar(1252, 0, (const char*)data, (int)size, r.data(), len);
        while (!r.empty() && r.back() == L'\0') r.pop_back();
        return r;
    }
    case 1: { // UTF-16 with BOM
        const uint8_t* p = data;
        size_t n = size;
        bool be = false;
        if (n >= 2) {
            if      (p[0] == 0xFF && p[1] == 0xFE) { p += 2; n -= 2; be = false; }
            else if (p[0] == 0xFE && p[1] == 0xFF) { p += 2; n -= 2; be = true;  }
        }
        size_t wlen = n / 2;
        std::wstring r(wlen, L'\0');
        for (size_t i = 0; i < wlen; i++) {
            uint8_t lo = be ? p[i*2+1] : p[i*2];
            uint8_t hi = be ? p[i*2]   : p[i*2+1];
            r[i] = (wchar_t)((hi << 8) | lo);
        }
        while (!r.empty() && r.back() == L'\0') r.pop_back();
        return r;
    }
    case 2: { // UTF-16BE, no BOM
        size_t wlen = size / 2;
        std::wstring r(wlen, L'\0');
        for (size_t i = 0; i < wlen; i++)
            r[i] = (wchar_t)((data[i*2] << 8) | data[i*2+1]);
        while (!r.empty() && r.back() == L'\0') r.pop_back();
        return r;
    }
    case 3: { // UTF-8
        int len = MultiByteToWideChar(CP_UTF8, 0, (const char*)data, (int)size, nullptr, 0);
        std::wstring r(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, (const char*)data, (int)size, r.data(), len);
        while (!r.empty() && r.back() == L'\0') r.pop_back();
        return r;
    }
    default:
        return L"";
    }
}

// Returns byte offset of the null terminator within [data, data+size).
// For single-byte encodings (0,3): looks for \x00.
// For double-byte encodings (1,2): looks for \x00\x00 on even boundary.
static size_t FindNullTerm(const uint8_t* data, size_t size, uint8_t enc) {
    if (enc == 0 || enc == 3) {
        for (size_t i = 0; i < size; i++)
            if (data[i] == 0) return i;
    } else {
        for (size_t i = 0; i + 1 < size; i += 2)
            if (data[i] == 0 && data[i+1] == 0) return i;
    }
    return size;
}

// ---------------------------------------------------------------------------
// Main parser
// ---------------------------------------------------------------------------

ID3Tags ParseID3v2(const std::wstring& filePath) {
    ID3Tags tags;

    std::ifstream f(filePath, std::ios::binary);
    if (!f) return tags;

    uint8_t hdr[10];
    if (!f.read((char*)hdr, 10)) return tags;

    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') return tags;

    const uint8_t version  = hdr[3]; // 2, 3 or 4
    const uint8_t flags    = hdr[5];
    const bool hasExtHdr   = (flags & 0x40) != 0;
    const uint32_t tagSize = ReadSynchsafe(hdr + 6);

    std::vector<uint8_t> td(tagSize);
    if (!f.read((char*)td.data(), tagSize)) return tags;

    const bool isV22       = (version == 2);
    const size_t idBytes   = isV22 ? 3 : 4;
    const size_t szBytes   = isV22 ? 3 : 4;

    size_t pos = 0;

    // Skip extended header (v2.3/v2.4 only)
    if (hasExtHdr && version >= 3 && pos + 4 <= tagSize) {
        uint32_t extSz = (version == 4) ? ReadSynchsafe(td.data() + pos)
                                        : ReadUint32BE(td.data() + pos);
        pos += extSz;
    }

    while (pos + idBytes + szBytes <= tagSize) {
        if (td[pos] == 0) break; // padding

        std::string fid(td.begin() + pos, td.begin() + pos + idBytes);
        pos += idBytes;

        uint32_t fsz;
        if (isV22)          fsz = ReadUint24BE(td.data() + pos);
        else if (version==4) fsz = ReadSynchsafe(td.data() + pos);
        else                 fsz = ReadUint32BE(td.data() + pos);
        pos += szBytes;

        if (!isV22) pos += 2; // skip frame flags

        if (fsz == 0 || pos + fsz > tagSize) break;

        const uint8_t* fd = td.data() + pos;

        // Map ID3v2.2 three-char IDs to v2.3 equivalents
        if (isV22) {
            static const std::pair<const char*,const char*> map22[] = {
                {"TT2","TIT2"},{"TP1","TPE1"},{"TAL","TALB"},
                {"TYE","TDRC"},{"TCO","TCON"},{"TRK","TRCK"},
                {"TBP","TBPM"},{"TXX","TXXX"},{"PIC","APIC"},
            };
            for (auto& [a,b] : map22)
                if (fid == a) { fid = b; break; }
        }

        if (!fid.empty() && fid[0] == 'T' && fid != "TXXX") {
            // Standard text frame
            if (fsz >= 1) {
                uint8_t enc = fd[0];
                std::wstring text = DecodeText(fd + 1, fsz - 1, enc);
                std::wstring wid(fid.begin(), fid.end());
                tags.allTextFrames[wid] = text;

                if      (fid == "TIT2") tags.title       = text;
                else if (fid == "TPE1") tags.artist      = text;
                else if (fid == "TALB") tags.album       = text;
                else if (fid == "TDRC" || fid == "TYER") tags.year  = text;
                else if (fid == "TCON") tags.genre       = text;
                else if (fid == "TRCK") tags.trackNumber = text;
                else if (fid == "TBPM") tags.bpm         = text;
            }

        } else if (fid == "TXXX") {
            // Custom text frame: encoding + description\0 + value
            if (fsz >= 2) {
                uint8_t enc = fd[0];
                size_t nullOff = FindNullTerm(fd + 1, fsz - 1, enc);
                size_t termSz  = (enc == 0 || enc == 3) ? 1 : 2;
                std::wstring desc = DecodeText(fd + 1, nullOff, enc);
                size_t valStart = 1 + nullOff + termSz;
                if (valStart <= fsz) {
                    std::wstring val = DecodeText(fd + valStart, fsz - valStart, enc);
                    tags.customTags[desc] = val;
                    tags.allTextFrames[L"TXXX:" + desc] = val;
                }
            }

        } else if (fid == "APIC") {
            // Attached picture — only grab the first (front cover preferred)
            if (!tags.hasCoverArt && fsz >= 4) {
                uint8_t enc = fd[0];
                size_t p = 1;
                std::string mime;
                while (p < fsz && fd[p] != 0) mime += (char)fd[p++];
                p++; // skip null
                if (p < fsz) {
                    /*uint8_t picType =*/ fd[p++];
                    size_t descNull = FindNullTerm(fd + p, fsz - p, enc);
                    size_t termSz   = (enc == 0 || enc == 3) ? 1 : 2;
                    p += descNull + termSz;
                    if (p < fsz) {
                        tags.coverArt.data.assign(fd + p, fd + fsz);
                        tags.coverArt.mimeType = mime.empty() ? "image/jpeg" : mime;
                        tags.hasCoverArt = true;
                    }
                }
            }
        }

        pos += fsz;
    }

    // Normalise genre: strip numeric reference "(N)" prefix used by ID3v2.3
    if (!tags.genre.empty() && tags.genre[0] == L'(') {
        size_t cp = tags.genre.find(L')');
        if (cp != std::wstring::npos && cp + 1 < tags.genre.size())
            tags.genre = tags.genre.substr(cp + 1);
    }

    return tags;
}
