// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <zstd.h>

#include "common/logging.h"
#include "common/oot_4k.h"
#include "core/file_sys/oot_4k.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_layered.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys {
namespace {

constexpr u32 ZstdDictMagic = 0xEC30A437;
constexpr u64 MaxBlob = 64ull << 20;
// 3840x2160 / 2560x1440.
constexpr double PixelRatio = (3840.0 * 2160.0) / (2560.0 * 1440.0);

struct Byml {
    u8* p{};
    size_t n{};
    u32 keys{};
};

bool U32At(const Byml& b, size_t off, u32* v) {
    if (off + 4 > b.n) {
        return false;
    }
    std::memcpy(v, b.p + off, 4);
    return true;
}

const char* KeyName(const Byml& b, u32 k) {
    u32 head{};
    u32 at{};
    if (!U32At(b, b.keys, &head) || (head & 0xff) != 0xc2 || k >= (head >> 8) ||
        !U32At(b, b.keys + 4 + 4 * k, &at) || static_cast<size_t>(b.keys) + at >= b.n ||
        !std::memchr(b.p + static_cast<size_t>(b.keys) + at, 0,
                     b.n - (static_cast<size_t>(b.keys) + at))) {
        return nullptr;
    }
    return reinterpret_cast<const char*>(b.p + static_cast<size_t>(b.keys) + at);
}

size_t Field(const Byml& b, u32 node, const char* name, u8 type) {
    u32 head{};
    if (!U32At(b, node, &head) || (head & 0xff) != 0xc1) {
        return 0;
    }
    for (u32 i = 0; i < (head >> 8); ++i) {
        u32 e{};
        if (!U32At(b, node + 4 + 8 * static_cast<size_t>(i), &e)) {
            return 0;
        }
        const char* k = KeyName(b, e & 0xffffff);
        if (k && std::strcmp(k, name) == 0) {
            return (e >> 24) == type ? node + 8 + 8 * static_cast<size_t>(i) : 0;
        }
    }
    return 0;
}

size_t HeapSizeField(const Byml& b, u32 heaps, const char* heap) {
    const size_t at = Field(b, heaps, heap, 0xc1);
    u32 node{};
    return at && U32At(b, at, &node) ? Field(b, node, "Size", 0xd1) : 0;
}

// Whole-MiB extra: ceil(shipped * (ratio-1) / 1024) * 1024 KiB, as NX2navi does.
s32 ExtraKib(s32 shipped) {
    if (shipped <= 0) {
        return 0;
    }
    const double grow = PixelRatio - 1.0;
    return static_cast<s32>(std::ceil(static_cast<double>(shipped) * grow / 1024.0) * 1024.0);
}

struct SarcMember {
    std::string name;
    std::span<const u8> data;
};

u16 U16le(std::span<const u8> d, size_t at) {
    return static_cast<u16>(d[at] | (d[at + 1] << 8));
}
u32 U32le(std::span<const u8> d, size_t at) {
    return static_cast<u32>(d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) | (d[at + 3] << 24));
}
u16 U16be(std::span<const u8> d, size_t at) {
    return static_cast<u16>((d[at] << 8) | d[at + 1]);
}
u32 U32be(std::span<const u8> d, size_t at) {
    return (static_cast<u32>(d[at]) << 24) | (static_cast<u32>(d[at + 1]) << 16) |
           (static_cast<u32>(d[at + 2]) << 8) | d[at + 3];
}

std::optional<std::vector<SarcMember>> ParseSarc(std::span<const u8> data) {
    if (data.size() < 0x14 || std::memcmp(data.data(), "SARC", 4) != 0) {
        return std::nullopt;
    }
    const bool big = U16le(data, 6) == 0xFFFE;
    const auto u16_at = [&](size_t at) { return big ? U16be(data, at) : U16le(data, at); };
    const auto u32_at = [&](size_t at) { return big ? U32be(data, at) : U32le(data, at); };
    const u16 header = u16_at(4);
    const u32 data_offset = u32_at(12);
    if (header < 0x14 || data_offset > data.size()) {
        return std::nullopt;
    }
    const size_t sfat = header;
    if (sfat + 8 > data.size() || std::memcmp(data.data() + sfat, "SFAT", 4) != 0) {
        return std::nullopt;
    }
    const u16 count = u16_at(sfat + 6);
    const size_t nodes = sfat + u16_at(sfat + 4);
    const size_t sfnt = nodes + static_cast<size_t>(count) * 0x10;
    if (sfnt + 8 > data.size() || std::memcmp(data.data() + sfnt, "SFNT", 4) != 0) {
        return std::nullopt;
    }
    const size_t names = sfnt + u16_at(sfnt + 4);
    std::vector<SarcMember> members;
    members.reserve(count);
    for (u16 i = 0; i < count; ++i) {
        const size_t node = nodes + static_cast<size_t>(i) * 0x10;
        if (node + 0x10 > data.size()) {
            return std::nullopt;
        }
        const u32 flags = u32_at(node + 4);
        const u32 start = u32_at(node + 8);
        const u32 end = u32_at(node + 12);
        if (end < start || static_cast<u64>(data_offset) + end > data.size()) {
            return std::nullopt;
        }
        std::string name;
        if ((flags >> 24) != 0) {
            const size_t at = names + static_cast<size_t>(flags & 0xFFFFFF) * 4;
            if (at < data.size()) {
                const size_t limit = std::min<size_t>(data.size(), data_offset);
                const auto* begin = data.data() + at;
                const auto* zero = static_cast<const u8*>(std::memchr(begin, 0, limit - at));
                name.assign(reinterpret_cast<const char*>(begin),
                            zero ? zero - begin : static_cast<std::ptrdiff_t>(limit - at));
            }
        }
        members.push_back({std::move(name), data.subspan(static_cast<size_t>(data_offset) + start,
                                                         end - start)});
    }
    return members;
}

class ZstdDicts {
public:
    ~ZstdDicts() {
        for (auto& [id, dict] : ddicts) {
            ZSTD_freeDDict(dict);
        }
        for (auto& [id, dict] : cdicts) {
            ZSTD_freeCDict(dict);
        }
    }

    void Add(std::span<const u8> dictionary) {
        const unsigned id = ZSTD_getDictID_fromDict(dictionary.data(), dictionary.size());
        if (id == 0 || ddicts.contains(id)) {
            return;
        }
        if (ZSTD_DDict* d = ZSTD_createDDict(dictionary.data(), dictionary.size())) {
            ddicts.emplace(id, d);
        }
        if (ZSTD_CDict* c = ZSTD_createCDict(dictionary.data(), dictionary.size(), 9)) {
            cdicts.emplace(id, c);
        }
    }

    const ZSTD_DDict* DDict(std::span<const u8> frame) const {
        const unsigned id = ZSTD_getDictID_fromFrame(frame.data(), frame.size());
        if (id == 0) {
            return nullptr;
        }
        const auto it = ddicts.find(id);
        return it == ddicts.end() ? nullptr : it->second;
    }

    unsigned DictId(std::span<const u8> frame) const {
        return ZSTD_getDictID_fromFrame(frame.data(), frame.size());
    }

    const ZSTD_CDict* CDict(unsigned id) const {
        const auto it = cdicts.find(id);
        return it == cdicts.end() ? nullptr : it->second;
    }

private:
    std::unordered_map<unsigned, ZSTD_DDict*> ddicts;
    std::unordered_map<unsigned, ZSTD_CDict*> cdicts;
};

void LoadZsDic(const VirtualDir& root, ZstdDicts& dicts) {
    const auto pack = root->GetDirectoryRelative("Pack");
    const auto file = pack ? pack->GetFile("ZsDic.pack.zs") : nullptr;
    if (!file) {
        return;
    }
    const auto packed = file->ReadAllBytes();
    unsigned long long raw = ZSTD_getFrameContentSize(packed.data(), packed.size());
    if (raw == ZSTD_CONTENTSIZE_UNKNOWN || raw == ZSTD_CONTENTSIZE_ERROR || raw > MaxBlob) {
        LOG_WARNING(Loader, "Ocarina 4K: Pack/ZsDic.pack.zs is not a zstd frame with a size");
        return;
    }
    std::vector<u8> sarc(static_cast<size_t>(raw));
    const size_t got = ZSTD_decompress(sarc.data(), sarc.size(), packed.data(), packed.size());
    if (ZSTD_isError(got)) {
        LOG_WARNING(Loader, "Ocarina 4K: failed to decompress Pack/ZsDic.pack.zs: {}",
                    ZSTD_getErrorName(got));
        return;
    }
    sarc.resize(got);
    const auto members = ParseSarc(sarc);
    if (!members) {
        LOG_WARNING(Loader, "Ocarina 4K: Pack/ZsDic.pack.zs is not a SARC");
        return;
    }
    for (const auto& member : *members) {
        if (member.data.size() >= 4 && U32le(member.data, 0) == ZstdDictMagic) {
            dicts.Add(member.data);
        }
    }
}

std::optional<std::vector<u8>> DecompressZs(std::span<const u8> frame, const ZstdDicts& dicts,
                                            std::string_view name) {
    const unsigned long long raw = ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (raw == ZSTD_CONTENTSIZE_UNKNOWN || raw == ZSTD_CONTENTSIZE_ERROR || raw > MaxBlob) {
        LOG_WARNING(Loader, "Ocarina 4K: {} is not a zstd frame with a size", name);
        return std::nullopt;
    }
    const unsigned id = dicts.DictId(frame);
    const ZSTD_DDict* dict = dicts.DDict(frame);
    if (id != 0 && !dict) {
        LOG_WARNING(Loader,
                    "Ocarina 4K: {} needs zstd dictionary {} and Pack/ZsDic.pack.zs did not "
                    "provide it",
                    name, id);
        return std::nullopt;
    }
    std::vector<u8> out(static_cast<size_t>(raw));
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    if (!ctx) {
        return std::nullopt;
    }
    const size_t got = dict ? ZSTD_decompress_usingDDict(ctx, out.data(), out.size(), frame.data(),
                                                         frame.size(), dict)
                            : ZSTD_decompressDCtx(ctx, out.data(), out.size(), frame.data(),
                                                  frame.size());
    ZSTD_freeDCtx(ctx);
    if (ZSTD_isError(got) || got != out.size()) {
        LOG_WARNING(Loader, "Ocarina 4K: failed to decompress {}: {}", name,
                    ZSTD_isError(got) ? ZSTD_getErrorName(got) : "size mismatch");
        return std::nullopt;
    }
    return out;
}

std::optional<std::vector<u8>> CompressZs(std::span<const u8> raw, const ZstdDicts& dicts,
                                          unsigned dict_id, std::string_view name) {
    const size_t cap = ZSTD_compressBound(raw.size());
    std::vector<u8> out(cap);
    size_t zn = 0;
    if (dict_id != 0) {
        const ZSTD_CDict* cdict = dicts.CDict(dict_id);
        if (!cdict) {
            LOG_WARNING(Loader, "Ocarina 4K: no compressor dictionary {} for {}", dict_id, name);
            return std::nullopt;
        }
        ZSTD_CCtx* ctx = ZSTD_createCCtx();
        if (!ctx) {
            return std::nullopt;
        }
        zn = ZSTD_compress_usingCDict(ctx, out.data(), out.size(), raw.data(), raw.size(), cdict);
        ZSTD_freeCCtx(ctx);
    } else {
        zn = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), 9);
    }
    if (ZSTD_isError(zn)) {
        LOG_WARNING(Loader, "Ocarina 4K: failed to recompress {}: {}", name, ZSTD_getErrorName(zn));
        return std::nullopt;
    }
    out.resize(zn);
    return out;
}

std::optional<std::vector<u8>> GrowHeapDef(std::vector<u8> byml, std::string_view name) {
    if (byml.size() < 16 || std::memcmp(byml.data(), "YB", 2) != 0) {
        LOG_WARNING(Loader, "Ocarina 4K: {} is not a little-endian BYML", name);
        return std::nullopt;
    }
    u16 ver{};
    std::memcpy(&ver, byml.data() + 2, 2);
    Byml b{byml.data(), byml.size(), 0};
    u32 root{};
    u32 heaps = 0;
    if (ver < 2 || ver > 7 || !U32At(b, 4, &b.keys) || !U32At(b, 12, &root)) {
        LOG_WARNING(Loader, "Ocarina 4K: {} has an unsupported BYML header (version {})", name, ver);
        return std::nullopt;
    }
    const size_t heaps_at = Field(b, root, "Heaps", 0xc1);
    if (!heaps_at || !U32At(b, heaps_at, &heaps)) {
        LOG_WARNING(Loader, "Ocarina 4K: {} has no Heaps table", name);
        return std::nullopt;
    }
    const size_t dta_at = HeapSizeField(b, heaps, "DynamicTextureAllocatorMem");
    const size_t gfx_at = HeapSizeField(b, heaps, "Graphics");
    if (!dta_at || !gfx_at) {
        LOG_WARNING(Loader,
                    "Ocarina 4K: {} has no DynamicTextureAllocatorMem / Graphics sizes", name);
        return std::nullopt;
    }
    s32 dta{};
    s32 gfx{};
    std::memcpy(&dta, b.p + dta_at, 4);
    std::memcpy(&gfx, b.p + gfx_at, 4);
    const s32 extra = ExtraKib(dta);
    if (extra <= 0) {
        LOG_WARNING(Loader, "Ocarina 4K: {} DynamicTextureAllocatorMem size {} is not usable",
                    name, dta);
        return std::nullopt;
    }
    const s32 dta2 = dta + extra;
    const s32 gfx2 = gfx + extra;
    std::memcpy(b.p + dta_at, &dta2, 4);
    std::memcpy(b.p + gfx_at, &gfx2, 4);
    LOG_INFO(Loader,
             "Ocarina 4K: HeapDef {} DynamicTextureAllocatorMem {} -> {} KiB, Graphics {} -> "
             "{} KiB (extra {} KiB, {:.0f} MiB; application pool reserved +{} MiB)",
             name, dta, dta2, gfx, gfx2, extra, extra / 1024.0,
             Common::Oot4kAppPoolExtraBytes() >> 20);
    return byml;
}

} // namespace

void ApplyOot4kHeapDef(VirtualFile& romfs) {
    if (!Common::Oot4kRequested() || !romfs) {
        return;
    }
    auto extracted = ExtractRomFS(romfs);
    if (!extracted) {
        LOG_WARNING(Loader, "Ocarina 4K: could not parse the program RomFS to grow HeapDef");
        return;
    }
    ZstdDicts dicts;
    LoadZsDic(extracted, dicts);

    auto heap_dir = extracted->GetDirectoryRelative("System/HeapSize");
    if (!heap_dir) {
        heap_dir = extracted->GetDirectoryRelative("system/heapsize");
    }
    if (!heap_dir) {
        LOG_WARNING(Loader,
                    "Ocarina 4K: no System/HeapSize directory in RomFS; texture heaps stay at "
                    "the shipped size (a 4K picture may run out of render-target memory)");
        return;
    }

    std::vector<VirtualFile> patched_files;
    for (const auto& file : heap_dir->GetFiles()) {
        const auto name = file->GetName();
        if (!name.ends_with("HeapDef.byml.zs") && !name.ends_with("HeapDef.byml.zstd")) {
            continue;
        }
        const auto packed = file->ReadAllBytes();
        if (packed.empty()) {
            LOG_WARNING(Loader, "Ocarina 4K: {} is empty", name);
            continue;
        }
        const unsigned dict_id = dicts.DictId(packed);
        auto raw = DecompressZs(packed, dicts, name);
        if (!raw) {
            continue;
        }
        auto grown = GrowHeapDef(std::move(*raw), name);
        if (!grown) {
            continue;
        }
        auto recompressed = CompressZs(*grown, dicts, dict_id, name);
        if (!recompressed) {
            continue;
        }
        patched_files.push_back(
            std::make_shared<VectorVfsFile>(std::move(*recompressed), name));
    }
    if (patched_files.empty()) {
        LOG_WARNING(Loader,
                    "Ocarina 4K: no HeapDef.byml.zs in System/HeapSize could be grown; texture "
                    "heaps stay at the shipped size (a 4K picture may run out of render-target "
                    "memory)");
        return;
    }

    auto size_dir =
        std::make_shared<VectorVfsDirectory>(std::move(patched_files), std::vector<VirtualDir>{},
                                             heap_dir->GetName());
    auto sys_dir = std::make_shared<VectorVfsDirectory>(
        std::vector<VirtualFile>{}, std::vector<VirtualDir>{size_dir}, "System");
    // Match the extracted directory's own casing if it was "system".
    if (heap_dir->GetParentDirectory() &&
        heap_dir->GetParentDirectory()->GetName() == "system") {
        sys_dir = std::make_shared<VectorVfsDirectory>(
            std::vector<VirtualFile>{}, std::vector<VirtualDir>{size_dir}, "system");
    }
    auto overlay = std::make_shared<VectorVfsDirectory>(
        std::vector<VirtualFile>{}, std::vector<VirtualDir>{sys_dir}, extracted->GetName());
    std::vector<VirtualDir> layers{std::move(overlay), std::move(extracted)};
    auto layered = LayeredVfsDirectory::MakeLayeredDirectory(std::move(layers));
    auto packed = CreateRomFS(std::move(layered));
    if (!packed) {
        LOG_WARNING(Loader, "Ocarina 4K: failed to rebuild RomFS with the grown HeapDef");
        return;
    }
    LOG_INFO(Loader, "Ocarina 4K: packed the grown HeapDef into the program RomFS");
    romfs = std::move(packed);
}

} // namespace FileSys
