// Japanese release: turns a folder unpacked from YDQJ into the one the
// YDQJ release recipe rebuilds the Europe build's code from, with the
// Europe release's data files and the Japanese text.
//
// The work is described by the data plan scripts/ja-convert.py makes
// (it documents the format), filled with the Europe ROM's bytes at build
// time and embedded here deflated (ja_plan.generated.inc). Every file the
// plan names is made from its ops: bytes of a YDQJ file or archive member
// (Level-5 GPC2 or NARC, decompressed), the plan's own bytes, or the
// Europe bytes the plan carries. Each result is checked against the
// SHA-1 the plan gives it, so the folder is what the Python reference
// (ja-convert.py apply) writes. The release recipe then runs as for any
// other release (ndsrecomp release_recipe.h).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "miniz.h"
#include "release_recipe.h"
#include "sha1.h"

#include "ja_plan.generated.inc"

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;

bool Fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

uint16_t U16(const uint8_t* d) { return static_cast<uint16_t>(d[0] | d[1] << 8); }
uint32_t U32(const uint8_t* d) {
    return static_cast<uint32_t>(d[0]) | static_cast<uint32_t>(d[1]) << 8 |
           static_cast<uint32_t>(d[2]) << 16 | static_cast<uint32_t>(d[3]) << 24;
}

bool ReadFile(const fs::path& path, Bytes* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0);
    out->resize(static_cast<size_t>(size));
    return size == 0 || static_cast<bool>(
        f.read(reinterpret_cast<char*>(out->data()), size));
}

bool WriteFile(const fs::path& path, const uint8_t* data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

// ---- Level-5 compression -------------------------------------------------
// A block: u32 size << 3 | method; 0 raw, 1 LZ10, 2 Huffman over nibbles,
// 3 Huffman over bytes, 4 RLE (the GBA BIOS formats without their own
// headers). scripts/l5_archive.py is the reference.

bool Lz10(const Bytes& d, size_t pos, size_t size, Bytes* out) {
    out->clear();
    out->reserve(size);
    while (out->size() < size) {
        if (pos >= d.size()) return false;
        const uint8_t flags = d[pos++];
        for (int bit = 0; bit < 8 && out->size() < size; ++bit) {
            if (flags & (0x80 >> bit)) {
                if (pos + 2 > d.size()) return false;
                const uint8_t b0 = d[pos], b1 = d[pos + 1];
                pos += 2;
                const size_t dist = ((b0 & 0x0Fu) << 8 | b1) + 1;
                if (dist > out->size()) return false;
                for (int i = 0; i < (b0 >> 4) + 3; ++i)
                    out->push_back((*out)[out->size() - dist]);
            } else {
                if (pos >= d.size()) return false;
                out->push_back(d[pos++]);
            }
        }
    }
    out->resize(size);
    return true;
}

bool Huffman(const Bytes& d, size_t pos, size_t size, int bits, Bytes* out) {
    out->clear();
    out->reserve(size);
    if (pos >= d.size()) return false;
    const size_t tree = pos;
    pos = tree + (static_cast<size_t>(d[tree]) + 1) * 2;
    const size_t root = tree + 1;
    size_t node = root;
    int low = -1;
    while (out->size() < size) {
        if (pos + 4 > d.size()) return false;
        const uint32_t word = U32(&d[pos]);
        pos += 4;
        for (int i = 0; i < 32 && out->size() < size; ++i) {
            if (node >= d.size()) return false;
            const uint8_t value = d[node];
            const size_t child = (node & ~size_t{1}) + (value & 0x3Fu) * 2 + 2;
            bool leaf;
            if ((word >> (31 - i)) & 1) {
                leaf = value & 0x40;
                node = child + 1;
            } else {
                leaf = value & 0x80;
                node = child;
            }
            if (!leaf) continue;
            if (node >= d.size()) return false;
            const uint8_t symbol = d[node];
            node = root;
            if (bits == 8) {
                out->push_back(symbol);
            } else if (low < 0) {
                low = symbol & 0x0F;
            } else {
                out->push_back(static_cast<uint8_t>(low | (symbol & 0x0F) << 4));
                low = -1;
            }
        }
    }
    out->resize(size);
    return true;
}

bool Rle(const Bytes& d, size_t pos, size_t size, Bytes* out) {
    out->clear();
    out->reserve(size);
    while (out->size() < size) {
        if (pos >= d.size()) return false;
        const uint8_t flag = d[pos++];
        if (flag & 0x80) {
            if (pos >= d.size()) return false;
            out->insert(out->end(), (flag & 0x7Fu) + 3, d[pos++]);
        } else {
            const size_t n = flag + 1u;
            if (pos + n > d.size()) return false;
            out->insert(out->end(), d.begin() + pos, d.begin() + pos + n);
            pos += n;
        }
    }
    out->resize(size);
    return true;
}

// The block at `pos`; *end is the position after it (raw blocks only:
// the compressed ones are not walked past by callers that need it).
bool Decompress(const Bytes& d, size_t pos, Bytes* out, size_t* end) {
    if (pos + 4 > d.size()) return false;
    const uint32_t head = U32(&d[pos]);
    const size_t size = head >> 3;
    pos += 4;
    switch (head & 7) {
    case 0:
        if (pos + size > d.size()) return false;
        out->assign(d.begin() + pos, d.begin() + pos + size);
        if (end) *end = pos + size;
        return true;
    case 1: return Lz10(d, pos, size, out);
    case 2: return Huffman(d, pos, size, 4, out);
    case 3: return Huffman(d, pos, size, 8, out);
    case 4: return Rle(d, pos, size, out);
    default: return false;
    }
}

// ---- Archives --------------------------------------------------------------

struct Member {
    size_t offset = 0;
    size_t size = 0;     // the bytes stored at `offset`
    bool block = false;  // they are a compressed block
};

struct Archive {
    Bytes data;
    std::unordered_map<std::string, Member> members;
};

// GPC2: see scripts/l5_archive.py. The table and the names are blocks; the
// positions after them come from the header (u16 @8 names / 4, @10 data / 4).
bool ReadGpc2(Archive* a) {
    const Bytes& d = a->data;
    if (d.size() < 0x18) return false;
    Bytes table, names;
    if (!Decompress(d, 0x14, &table, nullptr)) return false;
    const size_t names_at = size_t{U16(&d[8])} * 4;
    const size_t base = size_t{U16(&d[10])} * 4;
    if (!Decompress(d, names_at, &names, nullptr)) return false;
    const bool raw = U32(&d[0x10]) & 0x10000000u;
    for (size_t i = 0; i + 12 <= table.size(); i += 12) {
        const uint32_t w2 = U32(&table[i + 4]), w3 = U32(&table[i + 8]);
        const size_t name_off = (w3 >> 24) << 8 | (w2 >> 24);
        if (name_off >= names.size()) return false;
        const char* name = reinterpret_cast<const char*>(&names[name_off]);
        const size_t len = strnlen(name, names.size() - name_off);
        Member m;
        m.offset = base + 4 * size_t{w2 & 0xFFFFFFu};
        m.size = w3 & 0xFFFFFFu;
        m.block = !raw;
        if (m.offset + m.size > d.size()) return false;
        a->members.emplace(std::string(name, len), m);
    }
    return true;
}

// The Japanese release's menu layouts (bin/menu/bgmake_*.pac): one string
// table, named NUL-terminated in the first 0x20 bytes; u32 @0x40 is where
// it starts and u32 @0x48 where it ends (scripts/l5_archive.py).
bool ReadNamedTablePac(Archive* a) {
    const Bytes& d = a->data;
    if (d.size() < 0x50) return false;
    size_t len = 0;
    while (len < 0x20 && d[len]) {
        if (d[len] <= 0x20 || d[len] >= 0x7F) return false;
        ++len;
    }
    const std::string name(reinterpret_cast<const char*>(d.data()), len);
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".bin")) return false;
    const size_t start = U32(&d[0x40]), end = U32(&d[0x48]);
    if (start < 0x50 || end <= start || end > d.size()) return false;
    Member m;
    m.offset = start;
    m.size = end - start;
    a->members.emplace(name, m);
    return true;
}

// NARC: names as ndspy gives them ("dir/name"), or the file index as
// "%04d" when the archive has no names.
bool ReadNarc(Archive* a) {
    const Bytes& d = a->data;
    if (d.size() < 0x10) return false;
    size_t pos = U16(&d[0x0C]);
    size_t fat = 0, fnt = 0, fnt_size = 0, img = 0;
    for (int chunk = 0; chunk < 3; ++chunk) {
        if (pos + 8 > d.size()) return false;
        const uint32_t size = U32(&d[pos + 4]);
        if (!std::memcmp(&d[pos], "BTAF", 4)) fat = pos + 8;
        else if (!std::memcmp(&d[pos], "BTNF", 4)) { fnt = pos + 8; fnt_size = size - 8; }
        else if (!std::memcmp(&d[pos], "GMIF", 4)) img = pos + 8;
        else return false;
        pos += size;
    }
    if (!fat || !fnt || !img || fat + 4 > d.size()) return false;
    const size_t count = U16(&d[fat]);
    if (fat + 4 + count * 8 > d.size()) return false;
    auto file = [&](size_t id, Member* m) {
        if (id >= count) return false;
        const size_t start = U32(&d[fat + 4 + id * 8]), end = U32(&d[fat + 8 + id * 8]);
        if (end < start || img + end > d.size()) return false;
        m->offset = img + start;
        m->size = end - start;
        return true;
    };
    bool named = false;
    // Walk the name table from the root directory (0xF000).
    struct Dir { size_t index; std::string prefix; };
    std::vector<Dir> todo = {{0, ""}};
    while (!todo.empty()) {
        const Dir dir = todo.back();
        todo.pop_back();
        const size_t entry = fnt + dir.index * 8;
        if (entry + 8 > fnt + fnt_size || entry + 8 > d.size()) return false;
        size_t p = fnt + U32(&d[entry]);
        size_t id = U16(&d[entry + 4]);
        while (p < d.size()) {
            const uint8_t len = d[p++];
            if (!len) break;
            const size_t n = len & 0x7F;
            if (p + n > d.size()) return false;
            std::string name(reinterpret_cast<const char*>(&d[p]), n);
            p += n;
            named = true;
            if (len & 0x80) {
                if (p + 2 > d.size()) return false;
                todo.push_back({U16(&d[p]) & 0x0FFFu, dir.prefix + name + "/"});
                p += 2;
            } else {
                Member m;
                if (!file(id++, &m)) return false;
                a->members.emplace(dir.prefix + name, m);
            }
        }
    }
    if (!named) {
        for (size_t id = 0; id < count; ++id) {
            Member m;
            if (!file(id, &m)) return false;
            char name[16];
            std::snprintf(name, sizeof name, "%04zu", id);
            a->members.emplace(name, m);
        }
    }
    return true;
}

// ---- Sources ---------------------------------------------------------------

class Sources {
public:
    explicit Sources(fs::path root) : root_(std::move(root)) {}

    // "path" or "path::member", decoded; nullptr and *error if it cannot be.
    const Bytes* Get(const std::string& ref, std::string* error) {
        if (ref == last_ref_) return last_;
        // "path#member": a GPC2 member as stored (its compressed block).
        const size_t hash = ref.find('#');
        const bool stored = hash != std::string::npos;
        const size_t sep = stored ? hash : ref.find("::");
        const size_t skip = stored ? 1 : 2;
        const Bytes* out = nullptr;
        if (sep == std::string::npos) {
            out = File(ref, error);
        } else {
            Archive* a = Open(ref.substr(0, sep), error);
            if (!a) return nullptr;
            const std::string name = ref.substr(sep + skip);
            const auto m = a->members.find(name);
            if (m == a->members.end()) {
                Fail(error, ref + ": no such member");
                return nullptr;
            }
            member_.clear();
            if (stored) {
                member_.assign(a->data.begin() + m->second.offset,
                               a->data.begin() + m->second.offset + m->second.size);
            } else if (m->second.block) {
                if (!Decompress(a->data, m->second.offset, &member_, nullptr)) {
                    Fail(error, ref + ": cannot decompress it");
                    return nullptr;
                }
            } else {
                member_.assign(a->data.begin() + m->second.offset,
                               a->data.begin() + m->second.offset + m->second.size);
            }
            out = &member_;
        }
        last_ref_ = ref;
        last_ = out;
        return out;
    }

private:
    const Bytes* File(const std::string& path, std::string* error) {
        if (path == file_path_) return &file_;
        file_path_.clear();
        if (!ReadFile(root_ / fs::path(path), &file_)) {
            Fail(error, "cannot read " + (root_ / fs::path(path)).string());
            return nullptr;
        }
        file_path_ = path;
        return &file_;
    }

    Archive* Open(const std::string& path, std::string* error) {
        for (auto it = archives_.begin(); it != archives_.end(); ++it) {
            if (it->first == path) {
                archives_.splice(archives_.begin(), archives_, it);
                return &archives_.front().second;
            }
        }
        Archive a;
        if (!ReadFile(root_ / fs::path(path), &a.data)) {
            Fail(error, "cannot read " + (root_ / fs::path(path)).string());
            return nullptr;
        }
        bool ok = false;
        if (a.data.size() >= 4 && !std::memcmp(a.data.data(), "GPC2", 4)) ok = ReadGpc2(&a);
        else if (a.data.size() >= 4 && !std::memcmp(a.data.data(), "NARC", 4)) ok = ReadNarc(&a);
        else ok = ReadNamedTablePac(&a);
        if (!ok) {
            Fail(error, path + ": not an archive this plan can read");
            return nullptr;
        }
        archives_.emplace_front(path, std::move(a));
        if (archives_.size() > 8) archives_.pop_back();
        last_ref_.clear();
        return &archives_.front().second;
    }

    fs::path root_;
    std::string file_path_;
    Bytes file_;
    std::list<std::pair<std::string, Archive>> archives_;
    Bytes member_;
    std::string last_ref_;
    const Bytes* last_ = nullptr;
};

// ---- The plan --------------------------------------------------------------

enum Op : uint8_t { kJ, kP, kR, kZ };

struct PlanFile {
    std::string path;
    uint32_t size = 0;
    const uint8_t* digest = nullptr;
    const uint8_t* ops = nullptr;
    size_t ops_size = 0;
};

struct Plan {
    Bytes blob;
    std::string jp_sha1;
    std::vector<std::string> sources;
    std::vector<PlanFile> files;
    const uint8_t* literals = nullptr;
    size_t literals_size = 0;
};

std::string Hex(const uint8_t* d, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        out += digits[d[i] >> 4];
        out += digits[d[i] & 15];
    }
    return out;
}

bool ReadLeb(const uint8_t* d, size_t size, size_t* p, uint64_t* out) {
    uint64_t n = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (*p >= size) return false;
        const uint8_t b = d[(*p)++];
        n |= uint64_t{b & 0x7Fu} << shift;
        if (!(b & 0x80)) {
            *out = n;
            return true;
        }
    }
    return false;
}

bool LoadPlan(Plan* plan, std::string* error) {
    plan->blob.resize(DQ9_JA_PLAN_SIZE);
    mz_ulong size = DQ9_JA_PLAN_SIZE;
    if (mz_uncompress(plan->blob.data(), &size, kJaPlan, sizeof kJaPlan) != MZ_OK ||
        size != DQ9_JA_PLAN_SIZE)
        return Fail(error, "the built-in Japanese data plan is damaged");
    const Bytes& b = plan->blob;
    size_t p = 0;
    auto need = [&](size_t n) { return p + n <= b.size(); };
    if (!need(48) || std::memcmp(b.data(), "DQ9J", 4) || U32(&b[4]) != 1)
        return Fail(error, "the built-in Japanese data plan has an unknown format");
    plan->jp_sha1 = Hex(&b[8], 20);
    p = 48;
    auto string = [&](std::string* s) {
        if (!need(2)) return false;
        const size_t n = U16(&b[p]);
        if (!need(2 + n)) return false;
        s->assign(reinterpret_cast<const char*>(&b[p + 2]), n);
        p += 2 + n;
        return true;
    };
    for (int table = 0; table < 2; ++table) {
        if (!need(4)) return Fail(error, "truncated plan");
        const uint32_t n = U32(&b[p]);
        p += 4;
        for (uint32_t i = 0; i < n; ++i) {
            std::string s;
            if (!string(&s)) return Fail(error, "truncated plan");
            if (table == 0) plan->sources.push_back(std::move(s));
        }
    }
    if (!need(4)) return Fail(error, "truncated plan");
    const uint32_t files = U32(&b[p]);
    p += 4;
    for (uint32_t i = 0; i < files; ++i) {
        PlanFile f;
        if (!string(&f.path) || !need(28)) return Fail(error, "truncated plan");
        f.size = U32(&b[p]);
        f.digest = &b[p + 4];
        f.ops_size = U32(&b[p + 24]);
        p += 28;
        if (!need(f.ops_size)) return Fail(error, "truncated plan");
        f.ops = &b[p];
        p += f.ops_size;
        plan->files.push_back(std::move(f));
    }
    if (!need(4)) return Fail(error, "truncated plan");
    plan->literals_size = U32(&b[p]);
    p += 4;
    if (!need(plan->literals_size)) return Fail(error, "truncated plan");
    plan->literals = &b[p];
    return true;
}

// The [rom] sha1 of an unpacked folder's rom.toml.
std::string FolderSha1(const fs::path& dir) {
    Bytes text;
    if (!ReadFile(dir / "rom.toml", &text)) return {};
    const std::string s(text.begin(), text.end());
    const size_t at = s.find("\nsha1 = \"");
    if (at == std::string::npos || at + 9 + 40 > s.size()) return {};
    return s.substr(at + 9, 40);
}

bool LinkOrCopy(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::create_hard_link(from, to, ec);
    if (!ec) return true;
    return fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
}

bool Build(const Plan& plan, const fs::path& source, const fs::path& out,
           std::string* error) {
    Sources sources(source);
    size_t lit_at = 0;
    Bytes data;
    size_t done = 0;
    for (const PlanFile& f : plan.files) {
        const uint8_t* ops = f.ops;
        const size_t n_ops = f.ops_size;
        size_t p = 0;
        data.clear();
        data.reserve(f.size);
        // A whole source file, as it is: linked rather than written.
        const std::string* whole = nullptr;
        while (p < n_ops) {
            const uint8_t kind = ops[p++];
            uint64_t a = 0, off = 0, n = 0;
            bool ok = true;
            if (kind == kJ || kind == kP)
                ok = ReadLeb(ops, n_ops, &p, &a) && ReadLeb(ops, n_ops, &p, &off) &&
                     ReadLeb(ops, n_ops, &p, &n);
            else
                ok = ReadLeb(ops, n_ops, &p, &n);
            if (!ok) return Fail(error, f.path + ": the plan's ops are damaged");
            switch (kind) {
            case kJ: {
                if (a >= plan.sources.size())
                    return Fail(error, f.path + ": the plan names no source " + std::to_string(a));
                const std::string& ref = plan.sources[a];
                const Bytes* src = sources.Get(ref, error);
                if (!src) return false;
                if (off + n > src->size())
                    return Fail(error, f.path + ": " + ref + " is shorter than the plan says");
                if (p == n_ops && data.empty() && off == 0 && n == src->size() &&
                    ref.find_first_of(":#") == std::string::npos)
                    whole = &ref;
                data.insert(data.end(), src->begin() + off, src->begin() + off + n);
                break;
            }
            case kP:
                if (lit_at + n > plan.literals_size)
                    return Fail(error, "the plan's Europe bytes run out (the build "
                                       "did not fill it in)");
                data.insert(data.end(), plan.literals + lit_at, plan.literals + lit_at + n);
                lit_at += n;
                break;
            case kR:
                if (p + n > n_ops) return Fail(error, f.path + ": the plan's ops are damaged");
                data.insert(data.end(), ops + p, ops + p + n);
                p += n;
                break;
            case kZ:
                data.insert(data.end(), n, 0);
                break;
            default:
                return Fail(error, f.path + ": the plan's ops are damaged");
            }
        }
        if (data.size() != f.size)
            return Fail(error, f.path + ": the plan's ops give the wrong size");
        const gba::Sha1Digest digest = gba::sha1(data.data(), data.size());
        if (std::memcmp(digest.bytes.data(), f.digest, 20))
            return Fail(error, f.path + " does not come out as the plan says (is "
                                        "this the Japanese ROM it was made for?)");
        const fs::path dest = out / fs::path(f.path);
        if (whole ? !LinkOrCopy(source / fs::path(*whole), dest)
                  : !WriteFile(dest, data.data(), data.size()))
            return Fail(error, "cannot write " + dest.string());
        if (++done % 1000 == 0)
            std::fprintf(stderr, "[ja] %zu of %zu files\n", done, plan.files.size());
    }
    if (lit_at != plan.literals_size)
        return Fail(error, "the plan's Europe bytes are not all used");
    return true;
}

bool Prepare(const fs::path& source, const fs::path& out, bool* prepared,
             std::string* error) {
    *prepared = false;
    if (DQ9_JA_PLAN_SIZE == 0) return true;
    Plan plan;
    if (!LoadPlan(&plan, error)) return false;
    if (FolderSha1(source) != plan.jp_sha1) return true;
    std::fprintf(stderr, "[ja] converting the Japanese release's files (%zu)\n",
                 plan.files.size());
    if (!Build(plan, source, out, error)) return false;
    *prepared = true;
    return true;
}

const bool kRegistered = (nds_release_set_prepare(Prepare), true);

}  // namespace
