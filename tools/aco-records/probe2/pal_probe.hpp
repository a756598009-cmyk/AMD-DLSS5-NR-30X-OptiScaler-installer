// What a driver's pipeline binary looks like, and a lenient form of nr::pal::splice for container tests.
// Probe-only (nr_aco_probe2): the product keeps the strict adapter of nr_pal_binary.hpp.
#pragma once
#include "nr_pal_binary.hpp"
#include <cmath>
#include <cstdarg>
#include <functional>
#include <map>
#include <sstream>

namespace nr::probe {
using nr::pal::Bytes; using nr::pal::Value; using nr::pal::read; using nr::pal::require;

inline std::string hex(const uint8_t* p, size_t n) {
    std::string s; char b[4];
    for (size_t i = 0; i < n; ++i) { std::snprintf(b, sizeof b, "%02x", p[i]); s += b; if (i % 4 == 3 && i + 1 < n) s += ' '; }
    return s;
}
inline std::string fmt(const char* f, ...) {
    char b[1024]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); return b;
}

// MessagePack as indented text. Long number arrays stay on one line.
inline void dump(std::ostringstream& o, const Value& v, int depth) {
    const std::string pad(size_t(depth) * 2, ' ');
    switch (v.kind) {
    case Value::UInt: o << v.u; if (v.u > 9) o << fmt(" (0x%llx)", (unsigned long long)v.u); break;
    case Value::String: o << '"' << v.s << '"'; break;
    case Value::Raw:
        if (v.raw.size() == 1 && v.raw[0] == 0xc2) o << "false";
        else if (v.raw.size() == 1 && v.raw[0] == 0xc3) o << "true";
        else if (v.raw.size() == 1 && v.raw[0] == 0xc0) o << "nil";
        else if (v.raw.size() == 1 && v.raw[0] >= 0xe0) o << int(int8_t(v.raw[0]));
        else o << "raw:" << hex(v.raw.data(), std::min<size_t>(v.raw.size(), 64)) << (v.raw.size() > 64 ? "..." : "");
        break;
    case Value::Array: {
        bool flat = std::all_of(v.items.begin(), v.items.end(), [](const Value& x) { return x.kind == Value::UInt || x.kind == Value::Raw; });
        if (flat) { o << '['; for (size_t i = 0; i < v.items.size(); ++i) { if (i) o << ", "; o << fmt("0x%llx", (unsigned long long)v.items[i].u); if (v.items[i].kind == Value::Raw) o << "?"; } o << ']'; break; }
        o << "[\n"; for (const auto& x : v.items) { o << pad << "  - "; dump(o, x, depth + 2); o << '\n'; } o << pad << ']'; break;
    }
    case Value::Map:
        o << "{\n"; for (const auto& [k, x] : v.fields) { o << pad << "  " << k << ": "; dump(o, x, depth + 1); o << '\n'; } o << pad << '}'; break;
    }
}

struct ElfView {
    bool ok = false; std::string why;
    size_t at = 0, end = 0;                    // where the ELF lies in the blob
    uint8_t osabi = 0, abiver = 0; uint16_t type = 0, machine = 0; uint32_t flags = 0;
    std::vector<nr::pal::Section> sections; std::vector<std::string> names;
    bool has_md = false; Value md; uint64_t md_major = 0, md_minor = 0;
};
inline ElfView view(const Bytes& b, size_t at) {
    ElfView e; e.at = at;
    try {
        Bytes x(b.begin() + at, b.end());
        nr::pal::range(x, 0, 64);
        require(x[4] == 2 && x[5] == 1, "not a little-endian ELF64");
        e.osabi = x[7]; e.abiver = x[8]; e.type = read<uint16_t>(x, 16); e.machine = read<uint16_t>(x, 18); e.flags = read<uint32_t>(x, 48);
        auto count = read<uint16_t>(x, 60), str = read<uint16_t>(x, 62); auto sh = read<uint64_t>(x, 40);
        require(read<uint16_t>(x, 58) == 64 && count > 0 && count < 1024 && str < count, "ELF section header table");
        nr::pal::range(x, sh, size_t(count) * 64); e.end = at + sh + size_t(count) * 64;
        for (unsigned i = 0; i < count; ++i) {
            auto s = read<nr::pal::Section>(x, sh + i * 64); e.sections.push_back(s);
            if (s.type != 8) { nr::pal::range(x, s.offset, s.size); e.end = std::max<size_t>(e.end, at + s.offset + s.size); }
        }
        const auto& ss = e.sections[str];
        for (const auto& s : e.sections) {
            std::string n;
            for (size_t p = ss.offset + s.name; p < ss.offset + ss.size && x[p]; ++p) n += char(x[p]);
            e.names.push_back(n);
        }
        for (size_t i = 0; i < e.sections.size(); ++i) {
            if (e.sections[i].type != 7) continue;   // SHT_NOTE
            const auto& s = e.sections[i];
            for (size_t p = s.offset; p + 12 <= s.offset + s.size;) {
                auto ns = read<uint32_t>(x, p), ds = read<uint32_t>(x, p + 4), ty = read<uint32_t>(x, p + 8);
                size_t name = p + 12, desc = name + ((ns + 3) & ~3u); p = desc + ((ds + 3) & ~3u);
                if (ty == 32 && !e.has_md) {
                    nr::pal::range(x, desc, ds);
                    e.md = nr::pal::unpack(Bytes(x.begin() + desc, x.begin() + desc + ds)); e.has_md = true;
                    try { auto& v = e.md.at("amdpal.version"); e.md_major = v.items.at(0).u; e.md_minor = v.items.at(1).u; } catch (...) {}
                }
            }
        }
        e.ok = true;
    } catch (const std::exception& ex) { e.why = ex.what(); }
    return e;
}
inline std::vector<size_t> elf_offsets(const Bytes& b) {
    std::vector<size_t> r;
    for (size_t i = 0; i + 4 <= b.size() && r.size() < 16; ++i) if (!std::memcmp(b.data() + i, "\x7f" "ELF", 4)) r.push_back(i);
    return r;
}
inline const char* compression(const Bytes& b) {
    if (b.size() < 4) return nullptr;
    if (!std::memcmp(b.data(), "\x28\xb5\x2f\xfd", 4)) return "zstd frame";
    if (!std::memcmp(b.data(), "\x04\x22\x4d\x18", 4)) return "LZ4 frame";
    if (b[0] == 0x1f && b[1] == 0x8b) return "gzip";
    if (b[0] == 0x78 && (b[1] == 0x01 || b[1] == 0x5e || b[1] == 0x9c || b[1] == 0xda)) return "zlib";
    return nullptr;
}
inline double entropy(const Bytes& b) {
    size_t n[256]{}; for (auto c : b) ++n[c]; double h = 0;
    for (auto c : n) if (c) { double p = double(c) / double(b.size()); h -= p * std::log2(p); }
    return h;
}

// Each requirement of nr::pal::Elf / nr::pal::splice checked on its own: the list of what differs from what the
// product's importer takes. Empty = the strict importer would take this container.
inline std::vector<std::string> strict_issues(const Bytes& b, unsigned push_bytes) {
    std::vector<std::string> out;
    auto e = view(b, 0);
    if (b.size() < 64 || std::memcmp(b.data(), "\x7f" "ELF", 4)) { out.push_back("not an ELF at offset 0"); return out; }
    auto chk = [&](bool ok, const std::string& what) { if (!ok) out.push_back(what); };
    chk(b[4] == 2, fmt("EI_CLASS %u (want 2, ELF64)", b[4])); chk(b[5] == 1, fmt("EI_DATA %u (want 1)", b[5]));
    chk(b[7] == 65, fmt("EI_OSABI %u (want 65, AMDGPU_PAL)", b[7]));
    chk(read<uint16_t>(b, 16) == 1, fmt("e_type %u (want 1, REL)", read<uint16_t>(b, 16)));
    chk(read<uint16_t>(b, 18) == 224, fmt("e_machine %u (want 224, AMDGPU)", read<uint16_t>(b, 18)));
    chk(nr::pal::takes_machine(read<uint32_t>(b, 48)), fmt("e_flags 0x%x (not a machine value the importer takes)", read<uint32_t>(b, 48)));
    chk(read<uint64_t>(b, 32) == 0, "e_phoff not 0 (program headers)"); chk(read<uint16_t>(b, 56) == 0, fmt("e_phnum %u (want 0)", read<uint16_t>(b, 56)));
    chk(read<uint16_t>(b, 58) == 64, fmt("e_shentsize %u (want 64)", read<uint16_t>(b, 58)));
    if (!e.ok) { out.push_back("sections: " + e.why); return out; }
    for (size_t i = 0; i < e.sections.size(); ++i) {
        const auto& s = e.sections[i];
        chk(s.type != 4 && s.type != 9 && s.type != 8, fmt("section %s type %u (relocations/NOBITS refused)", e.names[i].c_str(), s.type));
        chk(s.align <= 4096 && (!s.align || !(s.align & (s.align - 1))), "section " + e.names[i] + " alignment");
    }
    auto find = [&](const char* n) { int k = -1, c = 0; for (size_t i = 0; i < e.names.size(); ++i) if (e.names[i] == n) { k = int(i); ++c; } chk(c == 1, fmt("section %s count %d (want 1)", n, c)); return k; };
    int text = find(".text"); find(".note"); find(".symtab");
    if (text >= 0) chk(e.sections[text].align == 256 && e.sections[text].addr == 0, fmt(".text align %llu addr %llu (want 256, 0)",
        (unsigned long long)e.sections[text].align, (unsigned long long)e.sections[text].addr));
    if (!e.has_md) { out.push_back("no AMDGPU PAL metadata note (type 32)"); return out; }
    const unsigned slot = push_bytes / 4 + 1;
    auto key = [&](const std::function<void()>& f, const char* what) { try { f(); } catch (const std::exception& ex) { out.push_back(std::string(what) + ": " + ex.what()); } };
    key([&] { chk(e.md_major == 3 && e.md_minor == 6, fmt("amdpal.version %llu.%llu (want 3.6)", (unsigned long long)e.md_major, (unsigned long long)e.md_minor)); }, "amdpal.version");
    key([&] {
        auto& ps = e.md.at("amdpal.pipelines"); chk(ps.items.size() == 1, fmt("amdpal.pipelines %zu (want 1)", ps.items.size()));
        auto& p = ps.items.at(0);
        key([&] { chk(p.at(".user_data_limit").number() == slot + 1, fmt(".user_data_limit %llu (want %u for %u push bytes)", (unsigned long long)p.at(".user_data_limit").number(), slot + 1, push_bytes)); }, ".user_data_limit");
        key([&] { chk(p.at(".api").s == "Vulkan", ".api " + p.at(".api").s); }, ".api");
        key([&] { chk(p.at(".type").s == "Cs", ".type " + p.at(".type").s); }, ".type");
        key([&] { p.at(".compute_registers"); }, ".compute_registers");
        key([&] {
            auto& cs = p.at(".hardware_stages").at(".cs");
            key([&] { chk(cs.at(".wavefront_size").number() == 32, ".wavefront_size not 32"); }, ".wavefront_size");
            key([&] { chk(cs.at(".entry_point_symbol").s == "_amdgpu_cs_main", ".entry_point_symbol " + cs.at(".entry_point_symbol").s); }, ".entry_point_symbol");
            key([&] { chk(cs.at(".threadgroup_dimensions").items.size() == 3, ".threadgroup_dimensions"); }, ".threadgroup_dimensions");
            key([&] {
                auto& map = cs.at(".user_data_reg_map");
                chk(map.items.size() == 32 && map.items[0].u == 0x10000000, ".user_data_reg_map not 32 entries starting with the global table");
                for (const auto& v : map.items) { auto x = v.u; if (!(x <= slot || x == 0xffffffff || x == 0x10000000 || x == 0x10000002 || x == 0x10000006)) out.push_back(fmt(".user_data_reg_map entry 0x%llx unhandled", (unsigned long long)x)); }
            }, ".user_data_reg_map");
        }, ".hardware_stages.cs");
    }, "amdpal.pipelines");
    return out;
}

// Everything a report needs to know about one blob, as text.
inline std::string describe(const Bytes& b, unsigned push_bytes) {
    std::ostringstream o;
    o << fmt("size %zu bytes, entropy %.2f bits/byte\n", b.size(), b.empty() ? 0.0 : entropy(b));
    o << "head " << hex(b.data(), std::min<size_t>(b.size(), 64)) << '\n';
    if (auto c = compression(b)) o << "compression magic: " << c << '\n';
    auto at = elf_offsets(b);
    o << "ELF magic at:"; for (auto a : at) o << ' ' << a; if (at.empty()) o << " none"; o << '\n';
    for (auto a : at) {
        auto e = view(b, a);
        o << fmt("ELF @%zu: EI_OSABI %u, EI_ABIVERSION %u, e_type %u, e_machine %u, e_flags 0x%x (mach 0x%02x, features 0x%x)",
                 a, e.osabi, e.abiver, e.type, e.machine, e.flags, e.flags & 0xff, e.flags & ~0xffu);
        if (!e.ok) { o << ", unreadable: " << e.why << '\n'; continue; }
        o << fmt(", ends at %zu of %zu\n", e.end, b.size());
        for (size_t i = 0; i < e.sections.size(); ++i) {
            const auto& s = e.sections[i];
            o << fmt("  [%zu] %-24s type %u flags 0x%llx addr 0x%llx off 0x%llx size %llu align %llu entsize %llu link %u info %u\n", i, e.names[i].c_str(), s.type,
                     (unsigned long long)s.flags, (unsigned long long)s.addr, (unsigned long long)s.offset, (unsigned long long)s.size,
                     (unsigned long long)s.align, (unsigned long long)s.entsize, s.link, s.info);
        }
        Bytes x(b.begin() + a, b.end());
        for (size_t i = 0; i < e.sections.size(); ++i) {
            const auto& s = e.sections[i];
            if (s.type == 7)
                for (size_t p = s.offset; p + 12 <= s.offset + s.size;) {
                    auto ns = read<uint32_t>(x, p), ds = read<uint32_t>(x, p + 4), ty = read<uint32_t>(x, p + 8);
                    std::string owner; for (size_t q = p + 12; q < p + 12 + ns && x[q]; ++q) owner += char(x[q]);
                    o << fmt("  note in %s: owner \"%s\" type %u desc %u bytes", e.names[i].c_str(), owner.c_str(), ty, ds);
                    if (ty != 32 && ds <= 64) o << " " << hex(x.data() + p + 12 + ((ns + 3) & ~3u), ds);
                    o << '\n';
                    p = p + 12 + ((ns + 3) & ~3u) + ((ds + 3) & ~3u);
                }
            if (s.type == 2 && s.entsize == 24 && s.link < e.sections.size()) {
                const auto& st = e.sections[s.link];
                for (size_t p = s.offset; p + 24 <= s.offset + s.size; p += 24) {
                    std::string n; for (size_t q = st.offset + read<uint32_t>(x, p); q < st.offset + st.size && x[q]; ++q) n += char(x[q]);
                    o << fmt("  symbol %-28s info 0x%02x shndx %u value 0x%llx size %llu\n", n.c_str(), x[p + 4], read<uint16_t>(x, p + 6),
                             (unsigned long long)read<uint64_t>(x, p + 8), (unsigned long long)read<uint64_t>(x, p + 16));
                }
            }
            if (e.names[i] == ".text") o << "  .text head " << hex(x.data() + s.offset, std::min<size_t>(s.size, 64)) << '\n';
        }
        if (e.has_md) { o << fmt("  PAL metadata (amdpal.version %llu.%llu):\n  ", (unsigned long long)e.md_major, (unsigned long long)e.md_minor); dump(o, e.md, 1); o << '\n'; }
    }
    auto issues = strict_issues(b, push_bytes);
    o << (issues.empty() ? "strict importer: would take this container\n" : "strict importer refuses:\n");
    for (auto& s : issues) o << "  - " << s << '\n';
    return o.str();
}

// nr::pal::splice without the header and version checks, with optional header/metadata overrides. The blob may
// hold the ELF inside a wrapper (prefix/suffix kept, size fields of the old ELF size in the prefix rewritten).
struct Loose {
    int eflags = -1, osabi = -1, abiver = -1, md_major = -1, md_minor = -1;
    bool keep_limit = false;      // leave .user_data_limit as the driver wrote it
};
inline Bytes splice_loose(const Bytes& blob, const nr::pal::Record& r, unsigned push_bytes, std::array<uint32_t, 3> workgroup,
                          const Loose& o, std::string& notes) {
    using namespace nr::pal;
    auto at = elf_offsets(blob); require(!at.empty(), "no ELF in the container");
    auto v = view(blob, at[0]); require(v.ok, "ELF unreadable");
    Bytes b(blob.begin() + at[0], blob.begin() + v.end);
    if (o.eflags >= 0) write(b, 48, uint32_t(o.eflags));
    if (o.osabi >= 0) b[7] = uint8_t(o.osabi);
    if (o.abiver >= 0) b[8] = uint8_t(o.abiver);
    const uint32_t mach = read<uint32_t>(b, 48) & 0xff;
    require(same_generation(r.mach ? r.mach : 0x4e, mach), "record and GPU are different generations");
    const unsigned slot = push_bytes / 4 + 1;
    auto count = read<uint16_t>(b, 60), str = read<uint16_t>(b, 62); auto sh = read<uint64_t>(b, 40);
    Bytes header(b.begin(), b.begin() + 64); std::vector<Section> sections; std::vector<Bytes> data;
    for (unsigned i = 0; i < count; ++i) {
        auto s = read<Section>(b, sh + i * 64); require(s.type != 4 && s.type != 9 && s.type != 8, "relocations/NOBITS in the container");
        sections.push_back(s); data.emplace_back(b.begin() + s.offset, b.begin() + s.offset + s.size);
    }
    auto find = [&](const std::string& n) { for (size_t i = 0; i < v.names.size(); ++i) if (v.names[i] == n) return i; throw std::runtime_error("missing ELF section " + n); };
    (void)str;
    auto text = find(".text"), note = find(".note"), sym = find(".symtab");
    require(sections[text].addr == 0, ".text address");
    if (sections[text].align < 256) { notes += fmt(" .text align %llu->256", (unsigned long long)sections[text].align); sections[text].align = 256; }
    const auto& sy = sections[sym]; unsigned found = 0;
    auto pre = prologue(r, slot, mach);
    for (size_t p = 0; p + 24 <= data[sym].size(); p += 24) {
        if (read<uint16_t>(data[sym], p + 6) != text) continue;
        auto off = read<uint32_t>(data[sym], p); std::string n;
        for (size_t q = off; q < data[sy.link].size() && data[sy.link][q]; ++q) n += char(data[sy.link][q]);
        if (n != "_amdgpu_cs_main") { notes += " symbol " + n + " in .text left as it is"; continue; }
        write(data[sym], p + 8, uint64_t(0)); write(data[sym], p + 16, uint64_t(pre.size() + r.exec)); ++found;
    }
    require(found == 1, "PAL compute entry count");
    data[text] = pre; data[text].insert(data[text].end(), r.code.begin(), r.code.end());
    Bytes out_notes; size_t pos = 0; unsigned metadata = 0;
    while (pos + 12 <= data[note].size()) {
        auto& src = data[note]; size_t start = pos;
        auto ns = read<uint32_t>(src, pos), ds = read<uint32_t>(src, pos + 4), type = read<uint32_t>(src, pos + 8);
        pos += 12; Bytes name(src.begin() + pos, src.begin() + pos + ns); pos += (ns + 3) & ~3u; size_t desc = pos; pos += (ds + 3) & ~3u;
        if (type != 32) { out_notes.insert(out_notes.end(), src.begin() + start, src.begin() + pos); continue; }
        auto m = unpack(Bytes(src.begin() + desc, src.begin() + desc + ds));
        if (o.md_major >= 0) m.set("amdpal.version", array({uint64_t(o.md_major), uint64_t(o.md_minor)}));
        auto& p = m.at("amdpal.pipelines").items.at(0);
        if (!o.keep_limit && p.at(".user_data_limit").number() != slot + 1) {
            notes += fmt(" .user_data_limit %llu->%u", (unsigned long long)p.at(".user_data_limit").number(), slot + 1);
            p.set(".user_data_limit", Value(slot + 1));
        }
        auto& cs = p.at(".hardware_stages").at(".cs");
        if (workgroup[0]) cs.set(".threadgroup_dimensions", array({workgroup[0], workgroup[1], workgroup[2]}));
        auto& map = cs.at(".user_data_reg_map");
        Arguments a(r, slot); map.items.clear(); for (auto x : a.map) map.items.emplace_back(x);
        p.set(".spill_threshold", Value(a.spill >= 0 ? 1 : 65535));
        cs.set(".shader_spill_threshold", Value(a.spill >= 0 ? 1 : 65535));
        cs.set(".user_sgprs", Value(a.user)); cs.set(".sgpr_count", Value(103)); cs.set(".vgpr_count", Value(((r.rsrc1 & 63) + 1) * 8));
        cs.set(".lds_size", Value(((r.rsrc2 >> 15) & 511) * 512)); cs.set(".float_mode", Value((r.rsrc1 >> 12) & 255));
        cs.set(".wgp_mode", Value::boolean((r.rsrc1 >> 29) & 1)); cs.set(".mem_ordered", Value::boolean((r.rsrc1 >> 30) & 1));
        cs.set(".forward_progress", Value::boolean((r.rsrc1 >> 31) & 1)); cs.set(".fp16_overflow", Value::boolean((r.rsrc1 >> 26) & 1));
        cs.set(".scratch_en", Value::boolean(r.scratch != 0)); cs.set(".scratch_memory_size", Value(r.scratch));
        auto& cr = p.at(".compute_registers");
        for (unsigned i = 0; i < 3; ++i) cr.set(i == 0 ? ".tgid_x_en" : i == 1 ? ".tgid_y_en" : ".tgid_z_en", Value::boolean((r.rsrc2 >> (7 + i)) & 1));
        cr.set(".tg_size_en", Value::boolean((r.rsrc2 >> 10) & 1)); cr.set(".tidig_comp_cnt", Value((r.rsrc2 >> 11) & 3));
        cr.set(".x_interleave", Value(0)); cr.set(".y_interleave", Value(0));
        auto h = hash(data[text].data(), data[text].size());
        cs.set(".checksum_value", Value(uint32_t(h))); p.set(".internal_pipeline_hash", array({h, r.spv_hash}));
        Bytes md; encode(md, m); word(out_notes, ns); word(out_notes, uint32_t(md.size())); word(out_notes, type);
        out_notes.insert(out_notes.end(), name.begin(), name.end()); while (out_notes.size() % 4) out_notes.push_back(0);
        out_notes.insert(out_notes.end(), md.begin(), md.end()); while (out_notes.size() % 4) out_notes.push_back(0); ++metadata;
    }
    require(metadata == 1, "PAL metadata note count"); data[note] = std::move(out_notes);
    Bytes elf = header;
    for (size_t i = 1; i < sections.size(); ++i) {
        auto& s = sections[i]; size_t align = std::max<uint64_t>(s.align, 1);
        elf.resize((elf.size() + align - 1) & ~(align - 1)); s.offset = elf.size(); s.size = data[i].size();
        elf.insert(elf.end(), data[i].begin(), data[i].end());
    }
    elf.resize((elf.size() + 7) & ~size_t(7)); write(elf, 40, uint64_t(elf.size()));
    for (const auto& s : sections) { size_t p = elf.size(); elf.resize(p + 64); write(elf, p, s); }
    if (at[0] == 0 && v.end == blob.size()) return elf;
    // A wrapper: rewrite any 4-aligned u32/u64 of the old ELF size or the old total size in the prefix.
    Bytes out(blob.begin(), blob.begin() + at[0]);
    const uint64_t old_elf = v.end - at[0], old_total = blob.size(), new_total = at[0] + elf.size() + (blob.size() - v.end);
    for (size_t p = 0; p + 4 <= out.size(); p += 4) {
        auto x = read<uint32_t>(out, p);
        if (x == old_elf) { write(out, p, uint32_t(elf.size())); notes += fmt(" prefix u32@%zu elf size", p); }
        else if (x == old_total) { write(out, p, uint32_t(new_total)); notes += fmt(" prefix u32@%zu total size", p); }
    }
    notes += fmt(" wrapper prefix %zu suffix %zu", at[0], blob.size() - v.end);
    out.insert(out.end(), elf.begin(), elf.end()); out.insert(out.end(), blob.begin() + v.end, blob.end());
    return out;
}
} // namespace nr::probe
