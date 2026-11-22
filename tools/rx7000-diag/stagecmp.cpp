// stagecmp: compares stage dumps (MochizukiNrRuntime with MZ_STAGE_DUMP=<dir>: stages.txt + stages.bin).
//
// stagecmp <ref dir> <test dir> [table.txt]
//   Each activation value of the test run against the same value (same plan key) of the reference run. A value is
//   read in every view the dump has (arena as e4m3, f16, f32; RDNA3 also its FP16 twin); the pairing that matches
//   best is taken (e4m3 against the twin first), since the dump does not know a slot's element type. Per value:
//   IDENTICAL (every chunk hash equal), SAME (chunk sums within 1%), CLOSE (within 5%) or DIFFERENT. Prints the first
//   DIFFERENT value in plan order, the dispatch and kernel that made it on both sides, and where in the tensor (which
//   of the chunks) it is worst. table.txt gets one line per value.
// stagecmp --runs <dir> <dir> [<dir>...]
//   The same network run several times: the first value whose bytes (chunk hashes, any view) change between runs.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct Chunk { float s0, s1, s2, mx; uint32_t bad, zero, hash, n; };
struct Val {
    int idx, key, disp, first, last;
    size_t off, bytes;
    std::string type;
    int block, layer, ph, pw, c, tokens;
};
struct Disp { std::string kern, steps; };
struct Dump {
    std::string dir, network, frame;
    uint32_t chunks = 0;
    std::vector<std::string> views;
    std::vector<Disp> disp;
    std::vector<Val> vals;
    std::map<int, size_t> by_key;
    std::vector<std::pair<uint32_t, uint32_t>> persist;   // epoch, error
    std::vector<Chunk> data;
    const Chunk* at(size_t val, int view) const { return &data[(val * views.size() + size_t(view)) * chunks]; }
    int view(const char* n) const {
        for (size_t i = 0; i < views.size(); ++i) if (views[i] == n) return int(i);
        return -1;
    }
};

static bool Load(const std::string& dir, Dump& d)
{
    d.dir = dir;
    std::ifstream f(dir + "/stages.txt");
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream s(line);
        std::string tag;
        s >> tag;
        if (tag == "network") s >> d.network;
        else if (tag == "frame") s >> d.frame;
        else if (tag == "chunks") s >> d.chunks;
        else if (tag == "views") { std::string v; while (s >> v) d.views.push_back(v); }
        else if (tag == "D") {
            size_t i; Disp x; std::string gx, gy, gz, kw, lo, hi;
            s >> i >> x.kern >> gx >> gy >> gz >> kw >> lo >> hi;
            std::getline(s, x.steps);
            if (d.disp.size() <= i) d.disp.resize(i + 1);
            d.disp[i] = x;
        } else if (tag == "V") {
            Val v{};
            s >> v.idx >> v.key >> v.disp >> v.first >> v.last >> v.off >> v.bytes >> v.type >> v.block >> v.layer >> v.ph >>
                v.pw >> v.c >> v.tokens;
            d.by_key[v.key] = d.vals.size();
            d.vals.push_back(v);
        } else if (tag == "P") {
            size_t i; std::string e, r; uint32_t ep = 0, er = 0;
            s >> i >> e >> ep >> r >> er;
            d.persist.push_back({ep, er});
        }
    }
    if (!d.chunks || d.views.empty()) return false;
    const size_t n = d.vals.size() * d.views.size() * d.chunks;
    d.data.resize(n);
    FILE* b = std::fopen((dir + "/stages.bin").c_str(), "rb");
    if (!b) return false;
    const size_t got = std::fread(d.data.data(), sizeof(Chunk), n, b);
    std::fclose(b);
    return got == n;
}

struct Match { double dev = INFINITY; int worst = -1; bool exact = false; std::string how; std::vector<double> per; };

// One pairing of views: the largest chunk deviation of the sums against the chunk's own size.
static Match Pair(const Dump& r, size_t rv, int rview, const Dump& t, size_t tv, int tview)
{
    Match m;
    if (rview < 0 || tview < 0 || r.chunks != t.chunks) return m;
    const Chunk* a = r.at(rv, rview);
    const Chunk* b = t.at(tv, tview);
    double total = 0;
    bool exact = true;
    for (uint32_t c = 0; c < r.chunks; ++c) {
        if (a[c].n != b[c].n) return m;   // another element count: not the same tensor in this view
        total += a[c].s1;
        exact = exact && a[c].hash == b[c].hash && a[c].bad == b[c].bad;
    }
    const double floor_ = 0.05 * total / r.chunks + 1e-20;
    m.dev = 0;
    m.per.resize(r.chunks);
    for (uint32_t c = 0; c < r.chunks; ++c) {
        const double s = std::fabs(double(b[c].s1) - a[c].s1) + std::fabs(double(b[c].s0) - a[c].s0);
        double d = s / (a[c].s1 + floor_);
        if (!std::isfinite(a[c].s1) || !std::isfinite(b[c].s1) || !std::isfinite(a[c].s0) || !std::isfinite(b[c].s0))
            d = (std::isfinite(a[c].s1) == std::isfinite(b[c].s1) && a[c].hash == b[c].hash) ? 0 : INFINITY;
        if (a[c].bad != b[c].bad) d = std::max(d, 1.0);
        m.per[c] = d;
        if (d > m.dev || m.worst < 0) { m.dev = std::max(m.dev, d); m.worst = int(c); }
    }
    m.exact = exact;
    if (exact) m.dev = 0;
    m.how = r.views[size_t(rview)] + "~" + t.views[size_t(tview)];
    return m;
}

static Match Best(const Dump& r, size_t rv, const Dump& t, size_t tv)
{
    const std::pair<const char*, const char*> order[] = {
        {"arena-e4m3", "twin-f16"}, {"arena-e4m3", "arena-e4m3"}, {"arena-f16", "arena-f16"}, {"arena-f32", "arena-f32"}};
    Match best;
    for (const auto& p : order) {
        const Match m = Pair(r, rv, r.view(p.first), t, tv, t.view(p.second));
        if (m.exact) return m;
        if (m.dev < best.dev) best = m;
    }
    return best;
}

static const char* Verdict(const Match& m)
{
    return m.exact ? "IDENTICAL" : m.dev <= 0.01 ? "SAME" : m.dev <= 0.05 ? "CLOSE" : "DIFFERENT";
}

static std::string Where(const Val& v, const Dump& d)
{
    char b[256];
    std::snprintf(b, sizeof b, "key %d (block %d layer %d, %s, plan h %d w %d C %d) made by dispatch %d %s", v.key, v.block,
                  v.layer, v.type.c_str(), v.ph, v.pw, v.c, v.disp,
                  v.disp >= 0 && size_t(v.disp) < d.disp.size() ? d.disp[size_t(v.disp)].kern.c_str() : "?");
    return b;
}

static void Persist(const Dump& d, const char* who)
{
    size_t bad = 0;
    for (const auto& p : d.persist) bad += p.second != 0;
    std::printf("PERSIST %s: %zu persistent runs, %zu with a wait that ran out (error word set)", who, d.persist.size(), bad);
    for (size_t i = 0; i < d.persist.size(); ++i)
        if (d.persist[i].second) std::printf(" [run %zu epoch %u error %u]", i, d.persist[i].first, d.persist[i].second);
    std::printf("\n");
}

static int Runs(int argc, char** argv)
{
    std::vector<Dump> d(size_t(argc - 2));
    for (int i = 2; i < argc; ++i)
        if (!Load(argv[i], d[size_t(i - 2)])) { std::printf("RUNS FAIL cannot read the dump in %s\n", argv[i]); return 1; }
    const Dump& a = d[0];
    std::vector<size_t> order(a.vals.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return a.vals[x].first < a.vals[y].first; });
    size_t varying = 0;
    const Val* first = nullptr;
    std::string what;   // the first varying value's views and chunks
    for (size_t i : order) {
        const Val& v = a.vals[i];
        std::string changed;
        for (size_t k = 1; k < d.size(); ++k) {
            const auto it = d[k].by_key.find(v.key);
            if (it == d[k].by_key.end() || d[k].views != a.views || d[k].chunks != a.chunks) { changed = " (missing)"; break; }
            for (size_t w = 0; w < a.views.size(); ++w) {
                std::string cs;
                for (uint32_t c = 0; c < a.chunks; ++c)
                    if (a.at(i, int(w))[c].hash != d[k].at(it->second, int(w))[c].hash) cs += (cs.empty() ? "" : ",") + std::to_string(c);
                if (!cs.empty() && changed.find(a.views[w]) == std::string::npos) changed += " " + a.views[w] + " chunks " + cs;
            }
        }
        if (!changed.empty()) {
            ++varying;
            if (!first) { first = &v; what = changed; }
        }
    }
    if (!first) std::printf("RUNS all %zu values identical in all %zu runs\n", a.vals.size(), d.size());
    else std::printf("RUNS %zu of %zu values change between runs; the first: %s; in%s\n", varying, a.vals.size(),
                     Where(*first, a).c_str(), what.c_str());
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 4 && !std::strcmp(argv[1], "--runs")) return Runs(argc, argv);
    if (argc < 3) { std::printf("usage: stagecmp <ref dir> <test dir> [table.txt] | stagecmp --runs <dir> <dir>...\n"); return 2; }
    Dump r, t;
    if (!Load(argv[1], r)) { std::printf("STAGE FAIL cannot read the reference dump in %s\n", argv[1]); return 1; }
    if (!Load(argv[2], t)) { std::printf("STAGE FAIL cannot read the dump in %s\n", argv[2]); return 1; }
    if (r.frame != t.frame) { std::printf("STAGE FAIL frame %s against a reference at %s\n", t.frame.c_str(), r.frame.c_str()); return 1; }
    FILE* table = argc > 3 ? std::fopen(argv[3], "w") : nullptr;
    if (table)
        std::fprintf(table, "# reference %s (%s), test %s (%s), %s; plan order\n# key block layer type | ref kernel | test kernel | "
                     "verdict deviation views worst-chunk/%u | per-chunk deviation\n",
                     argv[1], r.network.c_str(), argv[2], t.network.c_str(), r.frame.c_str(), r.chunks);
    std::vector<size_t> order(r.vals.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        return r.vals[x].first != r.vals[y].first ? r.vals[x].first < r.vals[y].first : r.vals[x].key < r.vals[y].key;
    });
    size_t counts[4] = {}, missing = 0, compared = 0;
    const Val* first = nullptr;
    const Val* first_t = nullptr;
    Match first_m;
    for (size_t i : order) {
        const Val& rv = r.vals[i];
        const auto it = t.by_key.find(rv.key);
        if (it == t.by_key.end()) { ++missing; continue; }
        const Val& tv = t.vals[it->second];
        const Match m = Best(r, i, t, it->second);
        ++compared;
        const char* v = Verdict(m);
        counts[m.exact ? 0 : m.dev <= 0.01 ? 1 : m.dev <= 0.05 ? 2 : 3]++;
        if (!first && !m.exact && m.dev > 0.05) { first = &rv; first_t = &tv; first_m = m; }
        if (table) {
            std::fprintf(table, "%4d b%d l%d %-32s | %-22s | %-22s | %-9s %9.2e %-22s %2d |", rv.key, rv.block, rv.layer,
                         rv.type.c_str(), size_t(rv.disp) < r.disp.size() ? r.disp[size_t(rv.disp)].kern.c_str() : "?",
                         size_t(tv.disp) < t.disp.size() ? t.disp[size_t(tv.disp)].kern.c_str() : "?", v, m.dev,
                         m.how.empty() ? "-" : m.how.c_str(), m.worst);
            for (double d : m.per) std::fprintf(table, " %.1e", d);
            std::fprintf(table, "\n");
        }
    }
    if (table) std::fclose(table);
    std::printf("STAGE %s vs %s at %s: %zu values compared (%zu identical, %zu same, %zu close, %zu different), %zu not in the test\n",
                t.network.c_str(), r.network.c_str(), r.frame.c_str(), compared, counts[0], counts[1], counts[2], counts[3],
                missing);
    if (!first) std::printf("STAGE first divergence: none\n");
    else {
        const std::string tk = size_t(first_t->disp) < t.disp.size() ? t.disp[size_t(first_t->disp)].kern : "?";
        const bool c256 = tk.rfind("fswinp256", 0) == 0;
        std::printf("STAGE first divergence: %s; test dispatch %d %s; deviation %.3g (%s), worst in chunk %d of %u%s; "
                    "C=256 persistent run (fswinp256): %s\n",
                    Where(*first, r).c_str(), first_t->disp, tk.c_str(), first_m.dev, first_m.how.c_str(), first_m.worst,
                    r.chunks, first_m.worst == int(r.chunks) - 1 ? " (the tensor's end)" : "", c256 ? "yes" : "no");
    }
    Persist(t, "test");
    return 0;
}
