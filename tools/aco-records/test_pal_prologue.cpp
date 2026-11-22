// CPU tests of the PAL adapter (nr_pal_binary.hpp), no GPU:
//  1. every record in the given directories parses;
//  2. gfx12: the prologue of every record is byte-identical to the adapter before the gfx11 work (orig/);
//  3. gfx11 and gfx10.3: random argument layouts run through a small SGPR interpreter of the prologue's
//     instructions, starting from PAL's layout, end in exactly the registers ACO's code expects;
//  4. every record's prologue as dd989104c's adapter (base/) makes it; with --probe2 <tools/aco-records/probe2>,
//     the RX 6600's containers and the synthesized gfx10.3 container (probe2_test).
//
//   cl /std:c++17 /EHsc /O2 /I<core> /I<orig> test_pal_prologue.cpp && test_pal_prologue [--probe2 <dir>] <records dir>...
#include "nr_pal_binary.hpp"
#include "nr_pal_binary_orig.hpp"
#include "nr_pal_binary_base.hpp"
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>

using nr::pal::Bytes;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

struct Machine {
    bool g10;
    uint32_t s[128] = {};
    std::map<uint64_t, uint32_t> mem;
    uint64_t pc = 0x00007fff12340000ull;
    unsigned m0() const { return g10 ? 124 : 125; }
    // Runs until the branch over the padding; false on an instruction it does not know.
    bool run(const Bytes& code) {
        const uint32_t op_mov = g10 ? 0x03 : 0x00, op_getpc = g10 ? 0x1f : 0x47, null_reg = g10 ? 125 : 124;
        for (size_t p = 0; p + 4 <= code.size();) {
            uint32_t w = nr::pal::read<uint32_t>(code, p);
            if ((w >> 23) == 0x17d) {   // SOP1
                unsigned dst = (w >> 16) & 127, op = (w >> 8) & 255, src = w & 255;
                if (op == op_mov) s[dst] = s[src];
                else if (op == op_getpc) { s[dst] = uint32_t(pc); s[dst + 1] = uint32_t(pc >> 32); }
                else return false;
                p += 4;
            } else if ((w >> 30) == 2 && ((w >> 23) & 127) == 0) {   // SOP2 s_add_u32
                unsigned dst = (w >> 16) & 127, s1 = (w >> 8) & 255, s0 = w & 255;
                uint32_t a = s0 >= 128 && s0 <= 192 ? s0 - 128 : s[s0], b = s1 >= 128 && s1 <= 192 ? s1 - 128 : s[s1];
                s[dst] = a + b;
                p += 4;
            } else if ((w >> 26) == 0x3d) {   // SMEM s_load_b32
                uint32_t w2 = nr::pal::read<uint32_t>(code, p + 4);
                if (((w >> 18) & 255) != 0 || (w2 >> 25) != null_reg) return false;
                unsigned dst = (w >> 6) & 127, base = (w & 63) * 2;
                uint64_t addr = (uint64_t(s[base + 1]) << 32 | s[base]) + (w2 & 0x1fffff);
                if (!mem.count(addr)) return false;
                s[dst] = mem[addr];
                p += 8;
            } else if ((w >> 23) == 0x17f) {   // SOPP
                unsigned op = (w >> 16) & 127;
                const unsigned nop = 0, branch = g10 ? 2 : 0x20, wait = g10 ? 0x0c : 0x09;
                if (op == branch) return true;
                if (op != nop && op != wait) return false;
                p += 4;
            } else return false;
        }
        return false;
    }
};

static void layout_test(bool g10, std::mt19937& rng, int iterations) {
    const uint32_t mach = g10 ? 0x38 : 0x41;
    for (int it = 0; it < iterations; ++it) {
        nr::pal::Record r;
        const bool grid = rng() % 2, push = rng() % 4 == 0;
        unsigned n_inline = rng() % (push ? 6 : 14);
        // ACO's order (RADV): set 0, push pointer, inline push constants, grid size (3 values).
        unsigned reg = 0;
        r.set0 = int(reg++);
        if (push) r.push = int(reg++);
        std::vector<int> dws;
        for (int d = 0; d < 32; ++d) dws.push_back(d);
        std::shuffle(dws.begin(), dws.end(), rng);
        dws.resize(n_inline);
        std::sort(dws.begin(), dws.end());
        for (int d : dws) r.inlines.emplace_back(int(reg++), d);
        if (grid) { r.grid = int(reg); reg += 3; }
        r.user = reg;
        r.push_bytes = 128;
        uint32_t rsrc2 = r.user << 1;
        for (unsigned bit : {7u, 8u, 9u, 10u}) if (rng() % 2) rsrc2 |= 1u << bit;
        if (!g10 && rng() % 3 == 0) { rsrc2 |= 1; r.scratch = 256; }
        r.rsrc2 = rsrc2;
        // push_base: the push constants' first user-data entry (1 on a plain device; 2 or 3 with the entries
        // VK_EXT_vertex_input_dynamic_state / transform feedback reserve, mochizuki's GitLab issue 5)
        const unsigned push_base = 1 + rng() % 3, push_bytes = 128, slot = push_bytes / 4 + push_base;
        nr::pal::Arguments a(r, slot, push_base);
        Bytes pro;
        try { pro = nr::pal::prologue(r, slot, mach, push_base); }
        catch (const std::exception& e) { CHECK(false, "prologue threw %s", e.what()); continue; }
        Machine m{g10};
        // PAL's side: user data entries by a.map, the spill table in memory, system SGPRs after a.user.
        const uint64_t spill = (m.pc & 0xffffffff00000000ull) | 0x00100000, gridp = 0x0000123400200000ull;
        auto entry = [&](unsigned e) { return 0xA0000000u + e; };   // user-data entry e's value
        for (unsigned e = 0; e < 64; ++e) m.mem[spill + 4 * e] = entry(e);
        for (unsigned j = 0; j < 3; ++j) m.mem[gridp + 4 * j] = 0xC0000000u + j;
        for (unsigned i = 0; i < 32; ++i) {
            uint64_t v = a.map[i];
            if (v == 0xffffffff) continue;
            if (v == 0x10000000) m.s[i] = 0x11111111;
            else if (v == 0x10000002) m.s[i] = uint32_t(spill);
            else if (v == 0x10000006) { m.s[i] = uint32_t(gridp); m.s[i + 1] = uint32_t(gridp >> 32); }
            else m.s[i] = entry(unsigned(v));
        }
        std::vector<uint32_t> sysv;
        unsigned k = 0;
        for (unsigned bit : {7u, 8u, 9u, 10u, 0u})
            if (rsrc2 & (1u << bit)) { m.s[a.user + k] = 0xD0000000u + bit; sysv.push_back(0xD0000000u + bit); ++k; }
        CHECK(m.run(pro), "%s: the prologue ran into an unknown instruction", g10 ? "gfx10.3" : "gfx11");
        CHECK(pro.size() % 256 == 0, "prologue not 256-byte aligned");
        // ACO's side.
        CHECK(m.s[r.set0] == entry(slot), "set0");
        for (auto [rg, dw] : r.inlines) CHECK(m.s[rg] == entry(dw + push_base), "inline s%d = dw %d (got %08x)", rg, dw, m.s[rg]);
        if (r.push >= 0) CHECK(m.s[r.push] == uint32_t(spill) + 4 * push_base, "push pointer");
        if (r.grid >= 0) for (unsigned j = 0; j < 3; ++j) CHECK(m.s[r.grid + j] == 0xC0000000u + j, "grid %u", j);
        for (unsigned i = 0; i < sysv.size(); ++i)
            CHECK(m.s[r.user + i] == sysv[i], "%s system SGPR %u (record user %u, PAL user %u): %08x",
                  g10 ? "gfx10.3" : "gfx11", i, r.user, a.user, m.s[r.user + i]);
        CHECK(m.s[m.m0()] == uint32_t(m.pc >> 32), "m0 = address32 high");
    }
}

// The RX 6600 result of probe 2 (probe2/rx6600-result) and the RX 9070 XT's containers (probe2/ref-gfx12):
//  - the driver's own gfx1032 code objects (e_flags 0x938, registers keyed by number) parse, the metadata round trips;
//  - synthesize() on the navi23 probe records gives the container the RX 6600 ran BIT-EXACT (synth), but for the
//    fields it sets per pipeline (.api_shader_hash, .128_bit_cache_hash) and the ISA name note (its own machine);
//  - a navi21 record builds the same container for gfx1032 (one gfx1030 set for every gfx10.3 GPU);
//  - gfx12 / gfx11 splices are byte-identical to dd989104c's adapter (nr_base) and gfx10.3 records are refused there.
static Bytes file(const std::filesystem::path& p) { return nr::pal::load(p.string()); }
static nr::pal::Value& path(nr::pal::Value& m, std::initializer_list<const char*> keys) {
    nr::pal::Value* v = &m.at("amdpal.pipelines").items.at(0);
    for (auto k : keys) v = &v->at(k);
    return *v;
}
static nr::pal::Value metadata(const nr::pal::Elf& e, std::string* isa = nullptr) {
    const auto& d = e.data[e.find(".note")];
    nr::pal::Value md;
    for (size_t p = 0; p + 12 <= d.size();) {
        auto ns = nr::pal::read<uint32_t>(d, p), ds = nr::pal::read<uint32_t>(d, p + 4), ty = nr::pal::read<uint32_t>(d, p + 8);
        size_t desc = p + 12 + ((ns + 3) & ~3u);
        if (ty == 32) md = nr::pal::unpack(Bytes(d.begin() + desc, d.begin() + desc + ds));
        if (ty == 11 && isa) isa->assign(d.begin() + desc, d.begin() + desc + ds);
        p = desc + ((ds + 3) & ~3u);
    }
    return md;
}
static void probe2_test(const std::filesystem::path& dir) {
    const auto res = dir / "rx6600-result", ref = dir / "ref-gfx12";
    unsigned own = 0;
    for (const char* n : {"probe_a", "probe_b", "g_gemmproj", "g_fswin32"}) {
        const Bytes b = file(res / (std::string(n) + ".0.bin"));
        try {
            nr::pal::Elf e(b);
            CHECK(e.mach == 0x38, "%s: machine 0x%x", n, e.mach);
            auto md = metadata(e);
            Bytes x, y; nr::pal::encode(x, md); nr::pal::encode(y, nr::pal::unpack(x));
            CHECK(x == y, "%s: metadata does not round trip", n);
            unsigned numbered = 0;
            for (auto& f : path(md, {".registers"}).fields) numbered += f.first[0] == '\0';
            CHECK(numbered > 0, "%s: no numbered register keys", n);
            ++own;
        } catch (const std::exception& ex) { CHECK(false, "%s: the RX 6600's own code object: %s", n, ex.what()); }
        bool base_refuses = false;
        try { nr_base::pal::Elf e(b); } catch (...) { base_refuses = true; }
        CHECK(base_refuses, "%s: dd989104c took the RX 6600's container", n);
    }
    struct P { const char* name; uint64_t key; unsigned push; std::array<uint32_t, 3> wg; };
    unsigned synth = 0, generic = 0, same = 0;
    for (const P& p : {P{"probe_a", 0x92166f4c10ec629eull, 12, {64, 1, 1}}, P{"probe_b", 0x231f53957b4de7d8ull, 16, {64, 2, 1}}}) {
        char key[24]; std::snprintf(key, sizeof key, "%016llx.nrp", (unsigned long long)p.key);
        const auto rec = [&](const char* fam) { return nr::pal::Record::parse(file(res / "records" / fam / key)); };
        try {
            const Bytes want = file(res / (std::string(p.name) + ".synth.bin")), got = nr::pal::synthesize(rec("navi23"), 0x38, p.push, p.wg);
            nr::pal::Elf w(want), g(got);
            CHECK(Bytes(want.begin(), want.begin() + 40) == Bytes(got.begin(), got.begin() + 40) &&
                  Bytes(want.begin() + 48, want.begin() + 64) == Bytes(got.begin() + 48, got.begin() + 64), "%s: ELF header", p.name);
            CHECK(w.names == g.names, "%s: section names", p.name);
            for (size_t i = 0; i < w.sections.size() && i < g.sections.size(); ++i) {
                const auto &a = w.sections[i], &c = g.sections[i];
                CHECK(a.name == c.name && a.type == c.type && a.flags == c.flags && a.align == c.align && a.link == c.link &&
                      a.info == c.info && a.entsize == c.entsize && (a.type == 7 || a.offset == c.offset), "%s: section %zu", p.name, i);
                if (w.names[i] != ".note") CHECK(w.data[i] == g.data[i], "%s: %s bytes", p.name, w.names[i].c_str());
            }
            std::string wi, gi;
            auto wm = metadata(w, &wi), gm = metadata(g, &gi);
            CHECK(wi == "amdgcn--amdpal--gfx1201" && gi == "amdgcn--amdpal--gfx1032", "%s: ISA notes %s / %s", p.name, wi.c_str(), gi.c_str());
            for (auto ks : {std::initializer_list<const char*>{".shaders", ".compute", ".api_shader_hash"},
                            std::initializer_list<const char*>{".xgl_cache_info", ".128_bit_cache_hash"}})
                path(wm, ks) = path(gm, ks);
            Bytes x, y; nr::pal::encode(x, wm); nr::pal::encode(y, gm);
            CHECK(x == y, "%s: metadata differs from the RX 6600's synth", p.name);
            ++synth;
            CHECK(nr::pal::synthesize(rec("navi21"), 0x38, p.push, p.wg) == got, "%s: the navi21 record builds another container", p.name);
            ++generic;
        } catch (const std::exception& ex) { CHECK(false, "%s: synthesize: %s", p.name, ex.what()); }
        for (const char* fam : {"navi31", "gfx1201"}) {
            bool refused = false;
            try { nr::pal::synthesize(rec(fam), 0x38, p.push, p.wg); } catch (...) { refused = true; }
            CHECK(refused, "%s: a %s record synthesized for gfx1032", p.name, fam);
        }
        bool refused = false;
        try { nr::pal::synthesize(rec("navi23"), 0x4e, p.push, p.wg); } catch (...) { refused = true; }
        CHECK(refused, "%s: a gfx10.3 record synthesized for gfx1201", p.name);
        // gfx12 and gfx11 splices into the RX 9070 XT's container (the gfx11 one with e_flags 0x41), against dd989104c.
        const Bytes ref12 = file(ref / (std::string(p.name) + ".0.bin"));
        Bytes ref11 = ref12; nr::pal::write(ref11, 48, uint32_t(0x41));
        for (auto [fam, container] : {std::pair<const char*, const Bytes*>{"gfx1201", &ref12}, {"navi31", &ref11}}) {
            try {
                const Bytes rb = file(res / "records" / fam / key);
                CHECK(nr::pal::splice(*container, nr::pal::Record::parse(rb), p.push, p.wg) ==
                      nr_base::pal::splice(*container, nr_base::pal::Record::parse(rb), p.push, p.wg), "%s %s: splice changed", p.name, fam);
                ++same;
            } catch (const std::exception& ex) { CHECK(false, "%s %s: splice: %s", p.name, fam, ex.what()); }
            for (const char* other : {"navi23"}) {
                bool r = false;
                try { nr::pal::splice(*container, rec(other), p.push, p.wg); } catch (...) { r = true; }
                CHECK(r, "%s: a gfx10.3 record spliced into a %s container", p.name, fam);
            }
        }
    }
    std::printf("probe 2: %u RX 6600 code objects read, %u synthesized = RX 6600 synth, %u navi21 = navi23, %u splices = dd989104c\n",
                own, synth, generic, same);
}

int main(int argc, char** argv) {
    unsigned parsed = 0, gfx12 = 0, base = 0;
    std::filesystem::path probe2;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--probe2" && i + 1 < argc) { probe2 = argv[++i]; continue; }
        for (const auto& f : std::filesystem::directory_iterator(argv[i])) {
            if (f.path().extension() != ".nrp") continue;
            const Bytes b = nr::pal::load(f.path().string());
            try {
                auto r = nr::pal::Record::parse(b);
                ++parsed;
                if (!r.mach) {
                    auto o = nr_orig::pal::Record::parse(b);
                    CHECK(nr::pal::prologue(r, 33) == nr_orig::pal::prologue(o, 33) &&
                          nr::pal::prologue(r, 33, 0x48) == nr_orig::pal::prologue(o, 33),
                          "gfx12 prologue changed: %s", f.path().string().c_str());
                    ++gfx12;
                }
                // Every record's prologue for its own machine (gfx12: 0x4e), as dd989104c makes it.
                auto o = nr_base::pal::Record::parse(b);
                const uint32_t m = r.mach ? r.mach : 0x4e;
                for (unsigned slot : {5u, 23u, 33u})
                    CHECK(nr::pal::prologue(r, slot, m) == nr_base::pal::prologue(o, slot, m), "prologue changed: %s", f.path().string().c_str());
                ++base;
            } catch (const std::exception& e) {
                CHECK(false, "%s: %s", f.path().string().c_str(), e.what());
            }
        }
    }
    std::mt19937 rng(1234);
    layout_test(false, rng, 20000);
    layout_test(true, rng, 20000);
    if (!probe2.empty()) probe2_test(probe2);
    std::printf("%u records parsed (%u gfx12 with an identical prologue, %u = dd989104c), 40000 layouts, %d failures\n",
                parsed, gfx12, base, failures);
    return failures ? 1 : 0;
}
