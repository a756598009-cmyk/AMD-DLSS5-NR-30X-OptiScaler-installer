// Person protection's CPU side (OptiScaler/dlssnr/person/): the YOLO and YuNet decoders, the settings and mask
// freshness, the diagnostics window, then with a person-model folder each of the three models on still images,
// in this process and through person-worker.exe, which must give the same mask. Adapted from
// TheAutomatic/dlss-5-amd-project's tests/host/person_inference.cpp (GPL-3.0).
//   person_inference.exe [person-model dir] [mask output dir] [image.ppm ...]
#define NOMINMAX
#include "../OptiScaler/dlssnr/person/PersonInference.h"
#include "../OptiScaler/dlssnr/person/PersonSettings.h"
#include "../OptiScaler/dlssnr/person/PersonDiagnostics.h"
#include <cstdio>
#include <fstream>
#include <limits>

namespace Person = DlssNr::Person;
namespace
{
int failures = 0;
void Expect(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}
bool AllZero(const std::vector<float>& v)
{
    return std::all_of(v.begin(), v.end(), [](float x) { return x == 0; });
}

void FaceDecode()
{
    Expect(Person::ModelFileName(0) == L"pphumanseg.onnx" && Person::ModelFileName(1) == L"yolo11n-seg.onnx" &&
               Person::ModelFileName(2) == L"yunet.onnx" && Person::ModelFileName(7) == L"pphumanseg.onnx",
           "model index picks its file, anything else PP-HumanSeg");
    std::array<std::vector<float>, 3> cls, obj, box;
    std::array<Person::Face::Head, 3> heads;
    for (unsigned l = 0; l < 3; ++l)
    {
        const unsigned side = Person::Face::InputSize / (8u << l), n = side * side;
        cls[l].resize(n);
        obj[l].resize(n);
        box[l].resize(n * 4);
        heads[l] = { cls[l], obj[l], box[l] };
    }
    auto face = [&](unsigned index, float score)
    {
        cls[0][index] = obj[0][index] = score;
        box[0][4 * index + 2] = std::log(10.f);
        box[0][4 * index + 3] = std::log(12.f);
    };
    constexpr unsigned center = 20 * 40 + 20;
    face(center, .9f);
    const auto mask = Person::Face::Decode(heads);
    Expect(mask[80 * 160 + 80] >= .99f && mask[120 * 160 + 80] == 0 && mask[80 * 160 + 110] == 0,
           "YuNet: one face is a soft oval inside its box");
    face(center + 1, .8f);
    box[0][4 * (center + 1)] = -1;
    Expect(Person::Face::Decode(heads) == mask, "YuNet: an overlapping duplicate is suppressed");
    cls[0][center + 1] = 0;
    cls[0][center] = std::numeric_limits<float>::quiet_NaN();
    Expect(AllZero(Person::Face::Decode(heads)), "YuNet: a NaN score is no face");
    cls[0][center] = .9f;
    box[0][4 * center + 2] = 10000;
    Expect(AllZero(Person::Face::Decode(heads)), "YuNet: an unbounded box is no face");
    heads[0].box = heads[0].box.first(1);
    bool rejected = false;
    try
    {
        Person::Face::Decode(heads);
    }
    catch (const std::runtime_error&)
    {
        rejected = true;
    }
    Expect(rejected, "YuNet: a wrongly sized output is refused");
    std::vector<float> rgb(3 * 640 * 640), input(3 * 320 * 320);
    std::fill_n(rgb.data(), 640 * 640, 1.f);
    std::fill_n(rgb.data() + 640 * 640, 640 * 640, .5f);
    Person::Face::Prepare(rgb.data(), input);
    Expect(input[0] == 0 && input[320 * 320] == 127.5f && input[2 * 320 * 320] == 255, "YuNet: BGR 0..255 input");
    bool sizes = true;
    for (unsigned size : { 320u, 384u, 416u, 448u })
    {
        std::array<std::vector<float>, 3> cs, os, bs;
        std::array<Person::Face::Head, 3> hs;
        for (unsigned l = 0; l < 3; ++l)
        {
            const unsigned side = size / (8u << l), n = side * side;
            cs[l].resize(n);
            os[l].resize(n);
            bs[l].resize(n * 4);
            hs[l] = { cs[l], os[l], bs[l] };
        }
        const unsigned side = size / 8, idx = (side / 2) * side + side / 2;
        cs[0][idx] = os[0][idx] = .9f;
        bs[0][4 * idx + 2] = std::log(float(size) / 32);
        bs[0][4 * idx + 3] = std::log(float(size) / 32);
        const auto m = Person::Face::Decode(hs, size);
        std::vector<float> prep(3 * size * size);
        Person::Face::Prepare(rgb.data(), prep, size);
        sizes &= m[80 * 160 + 80] >= .99f && m[0] == 0 && prep[0] == 0 && prep[size * size] == 127.5f &&
                 prep[2 * size * size] == 255;
    }
    Expect(sizes, "YuNet: 320/384/416/448 inputs map to the same mask grid");
    Expect(Person::Face::BoundedSize(400) == 320 && Person::Face::BoundedSize(UINT_MAX) == 320,
           "YuNet: another input size is 320");
}

void Contracts()
{
    Expect(!Person::Settings {}.ChangesSinglePass() && Person::Settings { .5f, 1.f }.ChangesSinglePass(),
           "single pass is untouched at strength 1, detail 1");
    const auto bounded = Person::Settings { std::numeric_limits<float>::quiet_NaN(), -1 }.Bounded();
    Expect(bounded.strength == 1 && bounded.detail == 0, "NaN strength is 1, detail clamps to 0");
    Expect(Person::MaskFreshness(200) == 1 && Person::MaskFreshness(225) == .5f && Person::MaskFreshness(250) == 0 &&
               Person::MaskFreshness(UINT64_MAX) == 0,
           "a mask fades over its last 50 ms");
    Expect(Person::WorkerThreads(16) == 4 && Person::WorkerThreads(32) == 4 && Person::WorkerThreads(8) == 2,
           "worker threads: a quarter of the CPU, 2 to 4");
    Person::PersonDiagnostics diagnostic;
    diagnostic.Enable(true, 1000);
    diagnostic.Mask(Person::MaskDecision::Expired);
    diagnostic.maskAge.Add(270);
    const bool window = diagnostic.Sample(1000) && !diagnostic.Sample(1050) && diagnostic.Sample(1100) &&
                        diagnostic.Report(2999).empty();
    const auto report = diagnostic.Report(3000);
    Expect(window && report.find("=0/0/0/0/1/0/0") != std::string::npos &&
               report.find("mask_age_ms[n/min/mean/max]=1/270/270/270") != std::string::npos &&
               diagnostic.Report(3001).empty(),
           "diagnostics: a two-second window, then cleared");
    std::vector<float> d(116 * 8400), p(32 * 160 * 160, 1.f);
    d[0] = d[8400] = 320;
    d[2 * 8400] = d[3 * 8400] = 160;
    d[4 * 8400] = .9f;
    d[84 * 8400] = 5;
    auto mask = Person::Decode(d.data(), p.data());
    Expect(mask[80 * 160 + 80] >= .9f && mask[0] == 0, "YOLO: a person box fills its mask");
    d[4 * 8400] = std::numeric_limits<float>::quiet_NaN();
    mask = Person::Decode(d.data(), p.data());
    Expect(mask[80 * 160 + 80] == 0, "YOLO: a NaN score is no person");
}

// The capture's letterbox: the frame centred in a 640 square, mid grey around it, nearest sample (capture_main).
std::vector<float> Letterbox(const std::vector<uint8_t>& rgb, unsigned w, unsigned h)
{
    std::vector<float> out(3 * 640 * 640);
    const float sx = float(w) / std::max(w, h), sy = float(h) / std::max(w, h);
    for (unsigned y = 0; y < 640; ++y)
        for (unsigned x = 0; x < 640; ++x)
        {
            const float u = ((x + .5f) / 640 - (1 - sx) * .5f) / sx, v = ((y + .5f) / 640 - (1 - sy) * .5f) / sy;
            for (unsigned c = 0; c < 3; ++c)
            {
                float value = .447f;
                if (u >= 0 && u < 1 && v >= 0 && v < 1)
                    value =
                        rgb[(std::min(unsigned(v * h), h - 1) * w + std::min(unsigned(u * w), w - 1)) * 3 + c] / 255.f;
                out[c * 640 * 640 + y * 640 + x] = value;
            }
        }
    return out;
}

bool ReadPpm(const std::wstring& path, std::vector<uint8_t>& rgb, unsigned& w, unsigned& h)
{
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    unsigned maxValue = 0;
    if (!(in >> magic >> w >> h >> maxValue) || magic != "P6" || maxValue != 255)
        return false;
    in.get();
    rgb.resize(size_t(w) * h * 3);
    return bool(in.read(reinterpret_cast<char*>(rgb.data()), rgb.size()));
}

void WriteMask(const std::filesystem::path& path, const std::vector<float>& mask)
{
    std::ofstream out(path, std::ios::binary);
    out << "P5\n160 160\n255\n";
    for (float v : mask)
        out.put(char(uint8_t(std::clamp(v, 0.f, 1.f) * 255.f + .5f)));
}

double Coverage(const std::vector<float>& mask)
{
    return double(std::count_if(mask.begin(), mask.end(), [](float v) { return v > .5f; })) / mask.size();
}

void Models(const std::filesystem::path& dir, const std::filesystem::path& out, int count, wchar_t** images)
{
    struct Still
    {
        std::wstring name;
        unsigned w, h;
        std::vector<float> rgb;
    };
    std::vector<Still> stills;
    for (int i = 0; i < count; ++i)
    {
        std::vector<uint8_t> rgb;
        unsigned w = 0, h = 0;
        if (!ReadPpm(images[i], rgb, w, h))
        {
            std::printf("FAIL  cannot read %ls\n", images[i]);
            ++failures;
            continue;
        }
        stills.push_back({ std::filesystem::path(images[i]).stem().wstring(), w, h, Letterbox(rgb, w, h) });
    }
    // A flat grey frame with nobody in it.
    stills.push_back({ L"empty", 1920, 1080, std::vector<float>(3 * 640 * 640, .5f) });
    for (int model = 0; model < 3; ++model)
    {
        const auto file = Person::ModelFileName(model);
        Person::Inference inference;
        try
        {
            inference.Open(dir, Person::WorkerThreads(std::thread::hardware_concurrency()), file, 320);
        }
        catch (const std::exception& e)
        {
            std::printf("FAIL  %ls: %s\n", file.c_str(), e.what());
            ++failures;
            continue;
        }
        Person::Provider provider;
        provider.Configure(true, dir, file, 320);
        for (const auto start = GetTickCount64(); !provider.Ready() && GetTickCount64() - start < 20000;)
            Sleep(1);
        Expect(provider.Ready(), ("worker ready: " + provider.Status()).c_str());
        uint64_t frame = 0;
        for (const auto& still : stills)
        {
            Person::Image image { 1, ++frame, GetTickCount64(), still.w, still.h, still.rgb };
            std::vector<double> times;
            Person::Mask mask;
            for (int run = 0; run < 6; ++run)
            {
                mask = inference.Run(image);
                if (run)
                    times.push_back(mask.milliseconds);
            }
            std::sort(times.begin(), times.end());
            const double coverage = Coverage(mask.values);
            const bool finite =
                mask.values.size() == 160 * 160 && std::all_of(mask.values.begin(), mask.values.end(), [](float v)
                                                               { return std::isfinite(v) && v >= 0 && v <= 1; });
            std::printf("      %-12s %-9ls %6.1f ms median (%5.1f-%5.1f), coverage %5.1f%%\n", inference.ModelName(),
                        still.name.c_str(), times[times.size() / 2], times.front(), times.back(), coverage * 100);
            Expect(finite, "mask is 160x160 in 0..1");
            if (still.name == L"empty")
                Expect(coverage < .01, "nobody in a flat frame");
            else
                // A face is small in a full frame: two of the bus's are 0.2% of the grid.
                Expect(coverage > (model == 2 ? .001 : .005), "the still's person or face is found");
            if (!out.empty())
                WriteMask(out / (std::to_wstring(model) + L"-" + still.name + L".pgm"), mask.values);
            const bool sent = provider.Submit(std::make_shared<Person::Image>(image));
            std::shared_ptr<const Person::Mask> remote;
            for (const auto start = GetTickCount64(); sent && GetTickCount64() - start < 10000; Sleep(1))
                if ((remote = provider.Latest()) && remote->frame == image.frame)
                    break;
            Expect(sent && remote && remote->frame == image.frame && remote->epoch == image.epoch &&
                       remote->values == mask.values,
                   "person-worker.exe gives the same mask as in process");
        }
        provider.Configure(false, dir);
    }
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        FaceDecode();
        Contracts();
        if (argc > 1)
            Models(argv[1], argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path(), std::max(0, argc - 3),
                   argv + 3);
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL  %s\n", e.what());
        return 1;
    }
    std::printf(failures ? "person inference: %d failed\n" : "person inference: PASS\n", failures);
    return failures ? 1 : 0;
}
