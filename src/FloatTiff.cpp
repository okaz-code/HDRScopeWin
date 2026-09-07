#include "FloatTiff.h"
#include "IccProfile.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <cstring>

namespace {

struct Entry {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    std::vector<uint8_t> data;
};

void AppendU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xFF)); v.push_back((uint8_t)(x >> 8));
}
void AppendU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xFF)); v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF)); v.push_back((uint8_t)(x >> 24));
}
uint16_t LoadU16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint32_t)p[1] << 8)); }
uint32_t LoadU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

std::wstring TempSibling(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"." : path.substr(0, slash);
    GUID id{};
    CoCreateGuid(&id);
    wchar_t name[64];
    swprintf_s(name, L"\\.HDRScope-%08lX%04X%04X.tmp", id.Data1, id.Data2, id.Data3);
    return dir + name;
}

void WriteWholeFile(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw ScopeError("ファイルを作成できません：" + Narrow(path) + " · " + HresultMessage((long)GetLastError()));
    size_t written = 0;
    while (written < data.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - written, 1u << 24);
        DWORD done = 0;
        if (!WriteFile(file, data.data() + written, chunk, &done, nullptr) || done == 0) {
            DWORD err = GetLastError();
            CloseHandle(file);
            DeleteFileW(path.c_str());
            throw ScopeError("ファイルを書き込めません：" + HresultMessage((long)err));
        }
        written += done;
    }
    if (!FlushFileBuffers(file)) {
        DWORD err = GetLastError();
        CloseHandle(file);
        DeleteFileW(path.c_str());
        throw ScopeError("ファイルを確定できません：" + HresultMessage((long)err));
    }
    CloseHandle(file);
}

std::vector<uint8_t> ReadWholeFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw ScopeError("ファイルを開けません：" + Narrow(path) + " · " + HresultMessage((long)GetLastError()));
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::vector<uint8_t> data((size_t)size.QuadPart);
    size_t read = 0;
    while (read < data.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - read, 1u << 24);
        DWORD done = 0;
        if (!ReadFile(file, data.data() + read, chunk, &done, nullptr) || done == 0) break;
        read += done;
    }
    CloseHandle(file);
    data.resize(read);
    return data;
}

}  // namespace

namespace FloatTiff {

std::vector<uint8_t> Encode(const PixelImage& image) {
    std::vector<uint8_t> profile = BuildLinearSRGBProfile();
    size_t byteCount = image.Pixels().size() * 4;
    if (byteCount + profile.size() + 4096 >= (size_t)0xFFFFFFFFu)
        throw ScopeError("TIFFの4GB制限を超えています。");

    const char* descText =
        "HDRScope; extended linear sRGB; 1.0 = SDR reference white; straight alpha";
    std::vector<uint8_t> description(descText, descText + strlen(descText) + 1);

    std::vector<Entry> entries;
    auto addShort = [&](uint16_t tag, std::initializer_list<uint16_t> values) {
        Entry e{tag, 3, (uint32_t)values.size(), {}};
        for (uint16_t v : values) AppendU16(e.data, v);
        entries.push_back(std::move(e));
    };
    auto addLong = [&](uint16_t tag, uint32_t value) {
        Entry e{tag, 4, 1, {}};
        AppendU32(e.data, value);
        entries.push_back(std::move(e));
    };

    addLong(256, (uint32_t)image.Width());          // ImageWidth
    addLong(257, (uint32_t)image.Height());         // ImageLength
    addShort(258, {32, 32, 32, 32});                // BitsPerSample
    addShort(259, {1});                             // Compression: none
    addShort(262, {2});                             // PhotometricInterpretation: RGB
    entries.push_back({270, 2, (uint32_t)description.size(), description});  // ImageDescription
    addLong(273, 0);                                // StripOffsets, patched below
    addShort(274, {1});                             // Orientation: top-left
    addShort(277, {4});                             // SamplesPerPixel
    addLong(278, (uint32_t)image.Height());         // RowsPerStrip: one strip
    addLong(279, (uint32_t)byteCount);              // StripByteCounts
    addShort(284, {1});                             // PlanarConfiguration: chunky
    addShort(338, {2});                             // ExtraSamples: unassociated alpha
    addShort(339, {3, 3, 3, 3});                    // SampleFormat: IEEE float
    entries.push_back({34675, 7, (uint32_t)profile.size(), profile});        // ICC profile

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });

    size_t base = 8 + 2 + entries.size() * 12 + 4;
    std::vector<uint8_t> extra, fields;
    size_t stripField = 0;
    for (const Entry& e : entries) {
        AppendU16(fields, e.tag);
        AppendU16(fields, e.type);
        AppendU32(fields, e.count);
        if (e.tag == 273) stripField = fields.size();
        if (e.data.size() <= 4) {
            fields.insert(fields.end(), e.data.begin(), e.data.end());
            fields.insert(fields.end(), 4 - e.data.size(), 0);
        } else {
            AppendU32(fields, (uint32_t)(base + extra.size()));
            extra.insert(extra.end(), e.data.begin(), e.data.end());
            if (extra.size() % 4) extra.insert(extra.end(), 4 - extra.size() % 4, 0);
        }
    }
    // The pixel data sits after every out-of-line tag value, so its offset is only
    // known once the rest of the directory is laid out.
    std::vector<uint8_t> stripOffset;
    AppendU32(stripOffset, (uint32_t)(base + extra.size()));
    std::copy(stripOffset.begin(), stripOffset.end(), fields.begin() + stripField);

    std::vector<uint8_t> data;
    data.reserve(base + extra.size() + byteCount);
    data.push_back(0x49); data.push_back(0x49); data.push_back(42); data.push_back(0);
    AppendU32(data, 8);
    AppendU16(data, (uint16_t)entries.size());
    data.insert(data.end(), fields.begin(), fields.end());
    AppendU32(data, 0);   // no next IFD
    data.insert(data.end(), extra.begin(), extra.end());
    const uint8_t* raw = (const uint8_t*)image.Pixels().data();
    data.insert(data.end(), raw, raw + byteCount);
    return data;
}

std::optional<Decoded> Decode(const uint8_t* data, size_t size) {
    if (size < 8 || data[0] != 0x49 || data[1] != 0x49 || LoadU16(data + 2) != 42) return std::nullopt;
    uint32_t ifd = LoadU32(data + 4);
    if ((size_t)ifd + 2 > size) return std::nullopt;
    uint16_t count = LoadU16(data + ifd);
    if ((size_t)ifd + 2 + (size_t)count * 12 > size) return std::nullopt;

    uint32_t width = 0, height = 0, stripOffset = 0, stripBytes = 0, rowsPerStrip = 0;
    uint16_t compression = 0, photometric = 0, samples = 0, planar = 0, orientation = 1;
    uint16_t bits[4] = {0, 0, 0, 0}, format[4] = {0, 0, 0, 0};
    Decoded out;

    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* e = data + ifd + 2 + (size_t)i * 12;
        uint16_t tag = LoadU16(e), type = LoadU16(e + 2);
        uint32_t n = LoadU32(e + 4);
        size_t elementSize = type == 3 ? 2 : (type == 4 ? 4 : 1);
        size_t total = elementSize * n;
        const uint8_t* value = e + 8;
        if (total > 4) {
            uint32_t off = LoadU32(e + 8);
            if ((size_t)off + total > size) return std::nullopt;
            value = data + off;
        }
        auto scalar = [&]() -> uint32_t { return type == 3 ? LoadU16(value) : LoadU32(value); };
        switch (tag) {
        case 256: width = scalar(); break;
        case 257: height = scalar(); break;
        case 258: for (uint32_t k = 0; k < n && k < 4; ++k) bits[k] = LoadU16(value + k * 2); break;
        case 259: compression = (uint16_t)scalar(); break;
        case 262: photometric = (uint16_t)scalar(); break;
        case 270: out.description.assign((const char*)value, n ? n - 1 : 0); break;
        case 273: if (n != 1) return std::nullopt; stripOffset = scalar(); break;
        case 274: orientation = (uint16_t)scalar(); break;
        case 277: samples = (uint16_t)scalar(); break;
        case 278: rowsPerStrip = scalar(); break;
        case 279: if (n != 1) return std::nullopt; stripBytes = scalar(); break;
        case 284: planar = (uint16_t)scalar(); break;
        case 339: for (uint32_t k = 0; k < n && k < 4; ++k) format[k] = LoadU16(value + k * 2); break;
        case 34675: out.hasIccProfile = n > 0; break;
        default: break;
        }
    }

    if (width == 0 || height == 0 || samples != 4 || compression != 1 || photometric != 2
        || planar != 1 || orientation != 1 || rowsPerStrip < height)
        return std::nullopt;
    for (int k = 0; k < 4; ++k)
        if (bits[k] != 32 || format[k] != 3) return std::nullopt;

    size_t expected = (size_t)width * height * 16;
    if (stripBytes != expected || (size_t)stripOffset + expected > size) return std::nullopt;
    out.width = (int)width;
    out.height = (int)height;
    out.pixels.resize((size_t)width * height * 4);
    memcpy(out.pixels.data(), data + stripOffset, expected);
    return out;
}

void Save(const PixelImage& image, const std::wstring& path) {
    std::vector<uint8_t> data = Encode(image);
    // Validate the bytes before anything is put where a good file may already be.
    auto check = Decode(data.data(), data.size());
    if (!check || check->width != image.Width() || check->height != image.Height()
        || !check->hasIccProfile || check->pixels != image.Pixels())
        throw ScopeError("保存前のfloat TIFF検証に失敗しました。");

    std::wstring staging = TempSibling(path);
    WriteWholeFile(staging, data);
    if (!MoveFileExW(staging.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        DeleteFileW(staging.c_str());
        throw ScopeError("保存先へ差し替えられません：" + HresultMessage((long)err));
    }
    if (ReadWholeFile(path) != data)
        throw ScopeError("保存したTIFFの読み戻し検証に失敗しました。");
}

}  // namespace FloatTiff
