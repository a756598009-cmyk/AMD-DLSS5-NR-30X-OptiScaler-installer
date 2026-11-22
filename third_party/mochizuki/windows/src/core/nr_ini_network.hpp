#pragma once
// [Network] in dlssnr-amd.ini: Model (which model the network runs) and Motion Without Depth (where the pre block
// reads the motion vectors), read by the runtime when it builds the network. ini_network: -1 for no file, no
// section or no readable value (the runtime's default, the small model).
#include <cctype>
#include <cstdio>
#include <string>
#include "nr_path_u8.hpp"

namespace nr {
// The networks a user picks, by the DLL's own codenames (dlssnr-amd.ini [Network] Model, NR Control, the log).
// CC_SMALL_FP8_EXPERIMENTAL and CC_SILVER_AARDWOLD_OPTIMIZED are ours, not the DLL's: the small model and the 310.8
// model made faster (ReLU, fixed attention on some decoder layers, ...), their weights fitted from the model's at
// install. CC_SILVER_AARDWOLD_OPTIMIZED runs at one of four levels of its optimized layers ([Network] OptimizedLayers:
// Quality, Balanced, Performance, Ultra Performance; RX 9000): four networks here (each built, kept and switched to as a
// network), one in the lists. Quality: none of its layers in int4 (kSilverAardwoldOptimizedFp8); Balanced: some of them (kSilverAardwoldOptimized, int4/); Performance: Balanced with fixed attention on the first full-resolution
// layers (kSilverAardwoldOptimizedPerformance, int4-performance/); Ultra Performance, the default: Performance with more
// int4 layers (kSilverAardwoldOptimizedUltra, int4-ultra/). One FP8 pack for the four (CC_SILVER_AARDWOLD_OPTIMIZED.bin; Performance and
// Ultra Performance read its "performance/" entries in place of some), one int4 pack for the three int4 levels
// (CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin).
enum NetworkChoice : int {
    kSmallFp8 = 0, kSilverAardwold = 1, kSilverAardwoldOptimized = 2, kSmallFp8Experimental = 3,
    kSilverAardwoldOptimizedFp8 = 4, kSilverAardwoldOptimizedPerformance = 5, kSilverAardwoldOptimizedUltra = 6,
    kNetworkChoices = 7
};
// [Network] OptimizedLayers: 0 Quality, 1 Balanced, 2 Performance, 3 Ultra Performance (the default).
inline constexpr int kLayersQuality = 0, kLayersBalanced = 1, kLayersPerformance = 2, kLayersUltra = 3,
                     kLayersDefault = kLayersUltra, kLayersCount = 4;
inline bool network_optimized(int n) {
    return n == kSilverAardwoldOptimized || n == kSilverAardwoldOptimizedFp8 || n == kSilverAardwoldOptimizedPerformance ||
           n == kSilverAardwoldOptimizedUltra;
}
// CC_SILVER_AARDWOLD_OPTIMIZED at a level of its optimized layers (other networks as they are), and the other way.
inline int network_with_layers(int n, int level) {
    return !network_optimized(n) ? n : level == kLayersQuality ? kSilverAardwoldOptimizedFp8
         : level == kLayersPerformance ? kSilverAardwoldOptimizedPerformance
         : level == kLayersUltra ? kSilverAardwoldOptimizedUltra : kSilverAardwoldOptimized;
}
inline int network_layers(int n) {
    return n == kSilverAardwoldOptimizedFp8 ? kLayersQuality : n == kSilverAardwoldOptimizedPerformance ? kLayersPerformance
         : n == kSilverAardwoldOptimizedUltra ? kLayersUltra : kLayersBalanced;
}
// A level's word ([Network] OptimizedLayers also takes these; the logs and the OSD show them).
inline const char* layers_word(int level) {
    return level == kLayersQuality ? "quality" : level == kLayersPerformance ? "performance"
         : level == kLayersUltra ? "ultra performance" : "balanced";
}
// The order the networks are listed in (the two small ones together, then the 310.8 model's; the optimized network
// once, as OptimizedLayers picks it) and the hotkey cycles them in. kNetworkOrder: every network, for the tables that
// show each one apart (video memory).
inline constexpr int kNetworkListed = 4;
inline constexpr int kNetworkList[kNetworkListed] = {kSmallFp8, kSmallFp8Experimental, kSilverAardwold,
                                                     kSilverAardwoldOptimized};
inline constexpr int kNetworkOrder[kNetworkChoices] = {kSmallFp8, kSmallFp8Experimental, kSilverAardwold,
    kSilverAardwoldOptimizedFp8, kSilverAardwoldOptimized, kSilverAardwoldOptimizedPerformance,
    kSilverAardwoldOptimizedUltra};
// Where a network is in the list (the optimized ones at the optimized network's place).
inline int network_rank(int n) {
    if (network_optimized(n)) n = kSilverAardwoldOptimized;
    for (int i = 0; i < kNetworkListed; ++i) if (kNetworkList[i] == n) return i;
    return 0;
}
// The codename (the optimized ones: CC_SILVER_AARDWOLD_OPTIMIZED; the ini keeps the level apart).
inline const char* network_codename(int n) {
    return n == kSmallFp8 ? "CC_SMALL_FP8" : n == kSilverAardwold ? "CC_SILVER_AARDWOLD"
         : network_optimized(n) ? "CC_SILVER_AARDWOLD_OPTIMIZED"
         : n == kSmallFp8Experimental ? "CC_SMALL_FP8_EXPERIMENTAL" : "?";
}
// The name shown where the optimized ones must be told apart (logs, the OSD, NR Control's tables).
inline const char* network_label(int n) {
    return n == kSilverAardwoldOptimizedFp8 ? "CC_SILVER_AARDWOLD_OPTIMIZED (layers: quality)"
         : n == kSilverAardwoldOptimized ? "CC_SILVER_AARDWOLD_OPTIMIZED (layers: balanced)"
         : n == kSilverAardwoldOptimizedPerformance ? "CC_SILVER_AARDWOLD_OPTIMIZED (layers: performance)"
         : n == kSilverAardwoldOptimizedUltra ? "CC_SILVER_AARDWOLD_OPTIMIZED (layers: ultra performance)"
         : network_codename(n);
}
// A codename, or the old words (small, original); -1 for anything else (case-insensitive). CC_SILVER_AARDWOLD_OPTIMIZED:
// kSilverAardwoldOptimized (OptimizedLayers picks the level). CC_SILVER_AARDWOLD_INT4_MIXED, the network it
// replaced (released ini files name it), reads as CC_SILVER_AARDWOLD_OPTIMIZED.
inline int network_choice(std::string v) {
    for (auto& c : v) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (v == "CC_SMALL_FP8" || v == "SMALL") return kSmallFp8;
    if (v == "CC_SILVER_AARDWOLD" || v == "ORIGINAL") return kSilverAardwold;
    if (v == "CC_SILVER_AARDWOLD_OPTIMIZED" || v == "CC_SILVER_AARDWOLD_INT4_MIXED") return kSilverAardwoldOptimized;
    if (v == "CC_SMALL_FP8_EXPERIMENTAL") return kSmallFp8Experimental;
    return -1;
}
// RuntimeConfig::network's numbering: 1 the small model (CC_SMALL_FP8), 2 CC_SMALL_FP8_EXPERIMENTAL, 3
// CC_SILVER_AARDWOLD_OPTIMIZED, 0 the 310.8 model (CC_SILVER_AARDWOLD), -1 anything else.
inline int network_value(std::string v) {
    const int n = network_choice(v);
    return n == kSmallFp8 ? 1 : n == kSmallFp8Experimental ? 2 : network_optimized(n) ? 3 : n < 0 ? -1 : 0;
}
inline int ini_network(const std::string& path) {
    FILE* f = nr::u8::fopen(path.c_str(), "r");
    if (!f) return -1;
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    };
    auto lower = [](std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    int r = -1;
    std::string section; char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string t = line;
        const auto c = t.find_first_of(";#");
        if (c != std::string::npos) t.resize(c);
        t = trim(t);
        if (t.empty()) continue;
        if (t.front() == '[') { const auto e = t.find(']'); section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1))); continue; }
        const auto eq = t.find('=');
        if (eq == std::string::npos || section != "network") continue;
        if (lower(trim(t.substr(0, eq))) == "model") r = network_value(trim(t.substr(eq + 1)));
    }
    std::fclose(f);
    return r;
}
// Another [Network] key's value, trimmed ("" when there is none): `key` lower case, as the file's keys are compared.
inline std::string ini_network_key(const std::string& path, const std::string& key) {
    FILE* f = nr::u8::fopen(path.c_str(), "r");
    if (!f) return {};
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    };
    auto lower = [](std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    std::string r, section; char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string t = line;
        const auto c = t.find_first_of(";#");
        if (c != std::string::npos) t.resize(c);
        t = trim(t);
        if (t.empty()) continue;
        if (t.front() == '[') { const auto e = t.find(']'); section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1))); continue; }
        const auto eq = t.find('=');
        if (eq == std::string::npos || section != "network") continue;
        if (lower(trim(t.substr(0, eq))) == key) r = trim(t.substr(eq + 1));
    }
    std::fclose(f);
    return r;
}
}  // namespace nr
