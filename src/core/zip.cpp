#include "core/zip.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"

namespace usb::zip {
namespace {

constexpr std::uint32_t kSignatureLocal = 0x04034b50u;
constexpr std::uint32_t kSignatureCentral = 0x02014b50u;
constexpr std::uint32_t kSignatureEnd = 0x06054b50u;
constexpr std::uint64_t kMaxArchiveBytes = 512ull * 1024ull * 1024ull;
constexpr std::size_t kMaxEntries = 1024;

struct Entry {
    std::wstring name;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t local_offset = 0;
};

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return L"";
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) return L"";
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), length);
    return wide;
}

bool ReadWholeFile(const std::wstring& path, std::vector<std::uint8_t>* data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > kMaxArchiveBytes) {
        CloseHandle(file);
        return false;
    }

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        const DWORD want = static_cast<DWORD>(
            std::min<std::size_t>(buffer.size() - offset, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(file, buffer.data() + offset, want, &read, nullptr) ||
            read == 0) {
            CloseHandle(file);
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    *data = std::move(buffer);
    return true;
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t pos) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(data[pos]) |
        (static_cast<std::uint32_t>(data[pos + 1]) << 8));
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, std::size_t pos) {
    return static_cast<std::uint32_t>(data[pos]) |
           (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
           (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
           (static_cast<std::uint32_t>(data[pos + 3]) << 24);
}

const std::array<std::uint32_t, 256>& CrcTable() {
    static const std::array<std::uint32_t, 256> table = []() {
        std::array<std::uint32_t, 256> values = {};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) != 0 ? (value >> 1) ^ 0xEDB88320u
                                          : (value >> 1);
            }
            values[i] = value;
        }
        return values;
    }();
    return table;
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    const std::array<std::uint32_t, 256>& table = CrcTable();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// DEFLATE 解压
// ---------------------------------------------------------------------------

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    bool Bits(int count, std::uint32_t* value) {
        if (count <= 0) {
            *value = 0;
            return true;
        }
        if (count > 24) return false;
        if (!Refill(count)) return false;
        *value = buffer_ & ((1u << count) - 1u);
        buffer_ >>= count;
        bits_ -= count;
        return true;
    }

    // 丢弃当前字节中剩余的比特，使后续读取按字节对齐。
    void AlignToByte() {
        const int drop = bits_ % 8;
        buffer_ >>= drop;
        bits_ -= drop;
    }

private:
    bool Refill(int count) {
        while (bits_ < count) {
            if (offset_ >= size_) return false;
            buffer_ |= static_cast<std::uint32_t>(data_[offset_++]) << bits_;
            bits_ += 8;
        }
        return true;
    }

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
    std::uint32_t buffer_ = 0;
    int bits_ = 0;
};

struct Huffman {
    std::uint16_t counts[16] = {};
    std::uint16_t symbols[288] = {};

    bool Build(const std::uint8_t* lengths, int count) {
        std::memset(counts, 0, sizeof(counts));
        for (int i = 0; i < count; ++i) {
            if (lengths[i] > 15) return false;
            ++counts[lengths[i]];
        }
        if (counts[0] == count) return true;  // 空树：允许（例如没有距离码）

        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= counts[len];
            if (left < 0) return false;  // 码长超额
        }

        std::uint16_t offsets[16] = {};
        for (int len = 1; len < 15; ++len) {
            offsets[len + 1] =
                static_cast<std::uint16_t>(offsets[len] + counts[len]);
        }
        for (int i = 0; i < count; ++i) {
            if (lengths[i] != 0) {
                symbols[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
            }
        }
        return true;
    }

    bool Decode(BitReader& reader, int* symbol) const {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= 15; ++len) {
            std::uint32_t bit = 0;
            if (!reader.Bits(1, &bit)) return false;
            code |= static_cast<int>(bit);
            const int count = counts[len];
            if (code - first < count) {
                *symbol = symbols[index + (code - first)];
                return true;
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return false;
    }
};

constexpr std::uint16_t kLengthBase[29] = {
    3,   4,   5,   6,   7,   8,   9,   10,  11,  13,  15,  17,  19,  23,  27,
    31,  35,  43,  51,  59,  67,  83,  99,  115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                           1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                           4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {
    1,    2,    3,    4,    5,    7,    9,    13,   17,   25,   33,   49,
    65,   97,   129,  193,  257,  385,  513,  769,  1025, 1537, 2049, 3073,
    4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,
                                         4, 4, 5, 5, 6, 6, 7, 7,  8,  8,
                                         9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Inflate(const std::uint8_t* input, std::size_t input_size,
             std::size_t expected_size, std::vector<std::uint8_t>* output) {
    output->clear();
    output->reserve(expected_size);

    BitReader reader(input, input_size);
    bool last = false;
    while (!last) {
        std::uint32_t final_block = 0;
        std::uint32_t type = 0;
        if (!reader.Bits(1, &final_block) || !reader.Bits(2, &type)) {
            return false;
        }
        last = final_block != 0;

        if (type == 0) {
            reader.AlignToByte();
            std::uint32_t length = 0;
            std::uint32_t complement = 0;
            if (!reader.Bits(16, &length) || !reader.Bits(16, &complement)) {
                return false;
            }
            if ((length ^ 0xFFFFu) != complement) return false;
            if (length > expected_size - output->size()) return false;
            for (std::uint32_t i = 0; i < length; ++i) {
                std::uint32_t byte = 0;
                if (!reader.Bits(8, &byte)) return false;
                output->push_back(static_cast<std::uint8_t>(byte));
            }
            continue;
        }
        if (type == 3) return false;

        Huffman literals;
        Huffman distances;
        if (type == 1) {
            std::uint8_t lengths[288] = {};
            for (int i = 0; i < 144; ++i) lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lengths[i] = 8;
            std::uint8_t distance_lengths[30] = {};
            for (int i = 0; i < 30; ++i) distance_lengths[i] = 5;
            if (!literals.Build(lengths, 288) ||
                !distances.Build(distance_lengths, 30)) {
                return false;
            }
        } else {
            std::uint32_t hlit = 0;
            std::uint32_t hdist = 0;
            std::uint32_t hclen = 0;
            if (!reader.Bits(5, &hlit) || !reader.Bits(5, &hdist) ||
                !reader.Bits(4, &hclen)) {
                return false;
            }
            const int literal_count = static_cast<int>(hlit) + 257;
            const int distance_count = static_cast<int>(hdist) + 1;
            const int code_count = static_cast<int>(hclen) + 4;
            if (literal_count > 286 || distance_count > 30) return false;

            static constexpr int kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                               11, 4,  12, 3, 13, 2, 14, 1, 15};
            std::uint8_t code_lengths[19] = {};
            for (int i = 0; i < code_count; ++i) {
                std::uint32_t value = 0;
                if (!reader.Bits(3, &value)) return false;
                code_lengths[kOrder[i]] = static_cast<std::uint8_t>(value);
            }
            Huffman codes;
            if (!codes.Build(code_lengths, 19)) return false;

            std::uint8_t lengths[286 + 30] = {};
            const int total = literal_count + distance_count;
            int index = 0;
            while (index < total) {
                int symbol = 0;
                if (!codes.Decode(reader, &symbol)) return false;
                if (symbol < 16) {
                    lengths[index++] = static_cast<std::uint8_t>(symbol);
                    continue;
                }
                std::uint32_t repeat = 0;
                if (symbol == 16) {
                    if (index == 0 || !reader.Bits(2, &repeat)) return false;
                    repeat += 3;
                    const std::uint8_t value = lengths[index - 1];
                    while (repeat > 0) {
                        if (index >= total) return false;
                        lengths[index++] = value;
                        --repeat;
                    }
                } else if (symbol == 17) {
                    if (!reader.Bits(3, &repeat)) return false;
                    repeat += 3;
                    while (repeat > 0) {
                        if (index >= total) return false;
                        lengths[index++] = 0;
                        --repeat;
                    }
                } else if (symbol == 18) {
                    if (!reader.Bits(7, &repeat)) return false;
                    repeat += 11;
                    while (repeat > 0) {
                        if (index >= total) return false;
                        lengths[index++] = 0;
                        --repeat;
                    }
                } else {
                    return false;
                }
            }
            if (!literals.Build(lengths, literal_count) ||
                !distances.Build(lengths + literal_count, distance_count)) {
                return false;
            }
        }

        for (;;) {
            int symbol = 0;
            if (!literals.Decode(reader, &symbol)) return false;
            if (symbol < 256) {
                if (output->size() >= expected_size) return false;
                output->push_back(static_cast<std::uint8_t>(symbol));
                continue;
            }
            if (symbol == 256) break;

            const int length_index = symbol - 257;
            if (length_index < 0 || length_index >= 29) return false;
            std::uint32_t extra = 0;
            if (!reader.Bits(kLengthExtra[length_index], &extra)) return false;
            const std::size_t length = kLengthBase[length_index] + extra;

            int distance_symbol = 0;
            if (!distances.Decode(reader, &distance_symbol) ||
                distance_symbol >= 30) {
                return false;
            }
            if (!reader.Bits(kDistExtra[distance_symbol], &extra)) return false;
            const std::size_t distance = kDistBase[distance_symbol] + extra;
            if (distance == 0 || distance > output->size()) return false;
            if (length > expected_size - output->size()) return false;
            for (std::size_t i = 0; i < length; ++i) {
                output->push_back((*output)[output->size() - distance]);
            }
        }
    }
    return output->size() == expected_size;
}

// ---------------------------------------------------------------------------
// 文件系统
// ---------------------------------------------------------------------------

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return true;

    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) {
        if (!EnsureDirectory(path.substr(0, slash))) return false;
        if (CreateDirectoryW(path.c_str(), nullptr)) return true;
        return GetLastError() == ERROR_ALREADY_EXISTS;
    }
    return false;
}

bool WriteBytes(const std::wstring& path, const std::vector<std::uint8_t>& data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    std::size_t offset = 0;
    bool ok = true;
    while (offset < data.size()) {
        const DWORD want = static_cast<DWORD>(
            std::min<std::size_t>(data.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + offset, want, &written, nullptr) ||
            written != want) {
            ok = false;
            break;
        }
        offset += written;
    }
    CloseHandle(file);
    return ok;
}

bool IsSafeEntryName(const std::wstring& name) {
    if (name.empty()) return false;
    if (name.front() == L'/' || name.front() == L'\\') return false;
    if (name.find(L':') != std::wstring::npos) return false;
    if (name.back() == L' ' || name.back() == L'.') return false;

    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = name.find_first_of(L"\\/", start);
        const std::wstring part =
            name.substr(start, slash == std::wstring::npos
                                   ? std::wstring::npos
                                   : slash - start);
        if (part == L"..") return false;
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    return true;
}

}  // namespace

bool ExtractAll(const std::wstring& zip_path, const std::wstring& dest_dir,
                std::wstring* error, const ProgressFn& progress) {
    const auto fail = [error](const wchar_t* message) {
        if (error) *error = message;
        return false;
    };

    std::vector<std::uint8_t> data;
    if (!ReadWholeFile(zip_path, &data)) return fail(L"无法读取更新包");

    if (data.size() < 22) return fail(L"更新包格式无效");

    // 从尾部向前找 EOCD 记录。
    std::size_t end_record = std::string::npos;
    const std::size_t lowest =
        data.size() > 22 + 65535 ? data.size() - 22 - 65535 : 0;
    for (std::size_t pos = data.size() - 22;; --pos) {
        if (ReadU32(data, pos) == kSignatureEnd &&
            pos + 22 + ReadU16(data, pos + 20) <= data.size()) {
            end_record = pos;
            break;
        }
        if (pos == lowest) break;
    }
    if (end_record == std::string::npos) return fail(L"更新包格式无效");

    const std::uint16_t count = ReadU16(data, end_record + 10);
    const std::uint32_t central_size = ReadU32(data, end_record + 12);
    const std::uint32_t central_offset = ReadU32(data, end_record + 16);

    if (count == 0) return fail(L"更新包内容为空");
    if (count > kMaxEntries) return fail(L"更新包条目过多");
    if (central_offset == 0xFFFFFFFFu || central_size == 0xFFFFFFFFu) {
        return fail(L"不支持的更新包格式");
    }
    if (central_offset > data.size() ||
        central_size > data.size() - central_offset) {
        return fail(L"更新包格式无效");
    }

    std::vector<Entry> entries;
    entries.reserve(count);
    std::size_t pos = central_offset;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (pos + 46 > data.size() || ReadU32(data, pos) != kSignatureCentral) {
            return fail(L"更新包目录损坏");
        }
        Entry entry;
        entry.flags = ReadU16(data, pos + 8);
        entry.method = ReadU16(data, pos + 10);
        entry.crc = ReadU32(data, pos + 16);
        entry.compressed_size = ReadU32(data, pos + 20);
        entry.uncompressed_size = ReadU32(data, pos + 24);
        const std::uint16_t name_length = ReadU16(data, pos + 28);
        const std::uint16_t extra_length = ReadU16(data, pos + 30);
        const std::uint16_t comment_length = ReadU16(data, pos + 32);
        entry.local_offset = ReadU32(data, pos + 42);

        if (pos + 46 + name_length > data.size()) {
            return fail(L"更新包目录损坏");
        }
        if ((entry.flags & 0x1u) != 0) return fail(L"不支持的加密更新包");
        if (entry.method != 0 && entry.method != 8) {
            return fail(L"不支持的压缩方式");
        }
        if (entry.compressed_size == 0xFFFFFFFFu ||
            entry.uncompressed_size == 0xFFFFFFFFu ||
            entry.local_offset == 0xFFFFFFFFu) {
            return fail(L"不支持的更新包格式");
        }

        const std::string name_bytes(
            reinterpret_cast<const char*>(data.data() + pos + 46),
            name_length);
        entry.name = Utf8ToWide(name_bytes);
        entries.push_back(std::move(entry));
        pos += 46 + static_cast<std::size_t>(name_length) + extra_length +
               comment_length;
    }

    if (!EnsureDirectory(dest_dir)) return fail(L"无法创建解压目录");

    // 进度按解压后的字节数统计：目录条目通常为 0，不影响总量。
    std::uint64_t total_bytes = 0;
    for (const Entry& entry : entries) total_bytes += entry.uncompressed_size;
    std::uint64_t done_bytes = 0;
    if (progress) progress(0, total_bytes);

    for (const Entry& entry : entries) {
        if (entry.name.empty()) continue;
        if (entry.name.back() == L'/' || entry.name.back() == L'\\') {
            continue;  // 目录条目：写入文件时会自动建目录
        }
        if (!IsSafeEntryName(entry.name)) return fail(L"更新包路径非法");

        const std::size_t local = entry.local_offset;
        if (local + 30 > data.size() || ReadU32(data, local) != kSignatureLocal) {
            return fail(L"更新包数据损坏");
        }
        const std::size_t data_start =
            local + 30 + ReadU16(data, local + 26) + ReadU16(data, local + 28);
        if (data_start > data.size() ||
            entry.compressed_size > data.size() - data_start) {
            return fail(L"更新包数据损坏");
        }

        const std::uint8_t* source = data.data() + data_start;
        std::vector<std::uint8_t> content;
        if (entry.method == 0) {
            content.assign(source, source + entry.compressed_size);
        } else if (!Inflate(source, entry.compressed_size,
                            entry.uncompressed_size, &content)) {
            return fail(L"更新包解压失败");
        }
        if (content.size() != entry.uncompressed_size) {
            return fail(L"更新包解压失败");
        }
        if (Crc32(content.data(), content.size()) != entry.crc) {
            return fail(L"更新包校验失败");
        }

        std::wstring target = dest_dir;
        if (target.back() != L'\\' && target.back() != L'/') target += L'\\';
        for (wchar_t ch : entry.name) {
            target.push_back(ch == L'/' ? L'\\' : ch);
        }
        const std::size_t slash = target.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash > 2 &&
            !EnsureDirectory(target.substr(0, slash))) {
            return fail(L"无法创建解压目录");
        }
        if (!WriteBytes(target, content)) return fail(L"无法写入解压文件");
        done_bytes += entry.uncompressed_size;
        if (progress) progress(done_bytes, total_bytes);
        USB_LOG_INFO(L"zip", L"解压：%s（%llu 字节）", target.c_str(),
                     static_cast<unsigned long long>(content.size()));
    }
    return true;
}

}  // namespace usb::zip
