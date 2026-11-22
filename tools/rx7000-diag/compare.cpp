// compare: the harness's output (RGBA16F rows, alpha ignored) against a reference, or against another output.
//
// compare --pack <out.bin> <w> <h> <prefix>
//   Makes a reference from an output: <prefix>_q.jxr the whole frame averaged over 4x4 blocks, <prefix>_b.jxr the
//   bottom 64 rows and <prefix>_r.jxr the right 64 columns at full resolution (where the network's windows are
//   partial), RGB half floats in lossless JPEG XR; each is read back and checked.
// compare <out.bin> <w> <h> <prefix> [diff.png] [view.jpg]
//   The output reduced the same way against the reference: per area PSNR (peak 1.0), mean and max |d|,
//   correlation, identical values; the quarter area's PSNR in 8 horizontal bands, top to bottom; NaN/Inf count.
//   diff.png: |out - ref| x 16 over the quarter area; view.jpg: the whole output as 8-bit.
// compare --bins <a.bin> <b.bin> <w> <h>
//   Two outputs at full resolution (run to run): PSNR, identical values, the box of pixels that differ, and PSNR in
//   8 horizontal bands.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

static float H2F(uint16_t x)
{
    const uint32_t e = (x >> 10) & 31, m = x & 1023;
    float v = e == 31 ? (m ? NAN : INFINITY) : e ? std::ldexp(float(m | 1024), int(e) - 25) : std::ldexp(float(m), -24);
    return (x & 0x8000) ? -v : v;
}

static IWICImagingFactory* Wic()
{
    static IWICImagingFactory* f = [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* w = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&w));
        return w;
    }();
    return f;
}

static std::wstring W(const char* s)
{
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring r(n ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, r.data(), n);
    return r;
}

static bool Save(const char* path, const GUID& container, WICPixelFormatGUID format, UINT w, UINT h, UINT stride,
                 const void* data, UINT bytes)
{
    IWICStream* s = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    IPropertyBag2* bag = nullptr;
    bool ok = SUCCEEDED(Wic()->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(W(path).c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(Wic()->CreateEncoder(container, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, &bag));
    if (ok && (container == GUID_ContainerFormatJpeg || container == GUID_ContainerFormatWmp))
    {
        PROPBAG2 opt {};
        VARIANT v {};
        if (container == GUID_ContainerFormatJpeg)
        {
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            v.vt = VT_R4;
            v.fltVal = 0.92f;
        }
        else
        {
            opt.pstrName = const_cast<LPOLESTR>(L"Lossless");
            v.vt = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
        }
        ok = SUCCEEDED(bag->Write(1, &opt, &v));
    }
    WICPixelFormatGUID want = format;
    ok = ok && SUCCEEDED(fr->Initialize(bag)) && SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&want)) &&
         want == format && SUCCEEDED(fr->WritePixels(h, stride, bytes, static_cast<BYTE*>(const_cast<void*>(data)))) &&
         SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    for (IUnknown* u : std::initializer_list<IUnknown*> { bag, fr, enc, s })
        if (u)
            u->Release();
    return ok;
}

static std::vector<uint16_t> ReadBin(const char* path, size_t px) // RGBA16F -> RGB half bits
{
    std::vector<uint16_t> rgba(px * 4), rgb;
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return rgb;
    const size_t got = std::fread(rgba.data(), 8, px, f);
    std::fclose(f);
    if (got != px)
        return rgb;
    rgb.resize(px * 3);
    for (size_t i = 0; i < px; ++i)
        std::memcpy(&rgb[i * 3], &rgba[i * 4], 6);
    return rgb;
}

static std::vector<uint16_t> ReadRef(const char* path, UINT& rw, UINT& rh)
{
    std::vector<uint16_t> rgb;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    WICPixelFormatGUID pf {};
    if (SUCCEEDED(Wic()->CreateDecoderFromFilename(W(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&rw, &rh)) && SUCCEEDED(fr->GetPixelFormat(&pf)) &&
        pf == GUID_WICPixelFormat48bppRGBHalf)
    {
        rgb.resize(size_t(rw) * rh * 3);
        if (FAILED(fr->CopyPixels(nullptr, rw * 6, UINT(rgb.size() * 2), reinterpret_cast<BYTE*>(rgb.data()))))
            rgb.clear();
    }
    if (fr)
        fr->Release();
    if (dec)
        dec->Release();
    return rgb;
}


struct Img { UINT w = 0, h = 0; std::vector<float> v; };   // RGB

static Img FromHalf(const std::vector<uint16_t>& rgb, UINT w, UINT h)
{
    Img o;
    o.w = w;
    o.h = h;
    o.v.resize(rgb.size());
    for (size_t i = 0; i < rgb.size(); ++i)
        o.v[i] = H2F(rgb[i]);
    return o;
}

// Round to nearest even, as the GPU stores a half.
static uint16_t F2H(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    const uint32_t s = (b >> 16) & 0x8000u, be = (b >> 23) & 255;
    uint32_t m = b & 0x7fffffu;
    if (be == 255)
        return uint16_t(s | 0x7c00u | (m ? 0x200u : 0));
    const int e = int(be) - 127 + 15;
    if (e >= 31)
        return uint16_t(s | 0x7c00u);
    if (e <= 0)
    {
        if (e < -10)
            return uint16_t(s);
        m |= 0x800000u;
        const int sh = 14 - e;
        uint32_t r = m >> sh;
        const uint32_t rem = m & ((1u << sh) - 1), half = 1u << (sh - 1);
        if (rem > half || (rem == half && (r & 1)))
            ++r;
        return uint16_t(s | r);
    }
    uint32_t r = (uint32_t(e) << 10) | (m >> 13);
    const uint32_t rem = m & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (r & 1)))
        ++r;
    return uint16_t(s | r);
}

// One of the three areas ('q' quarter, 'b' bottom 64 rows, 'r' right 64 columns), rounded to half.
static std::vector<uint16_t> Area(const Img& a, char which, UINT& w, UINT& h)
{
    std::vector<uint16_t> o;
    if (which == 'q')
    {
        w = (a.w + 3) / 4;
        h = (a.h + 3) / 4;
        o.resize(size_t(w) * h * 3);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                {
                    double s = 0;
                    int n = 0;
                    for (UINT j = 4 * y; j < std::min(a.h, 4 * y + 4); ++j)
                        for (UINT i = 4 * x; i < std::min(a.w, 4 * x + 4); ++i)
                        {
                            const float v = a.v[(size_t(j) * a.w + i) * 3 + c];
                            if (std::isfinite(v))
                            {
                                s += v;
                                ++n;
                            }
                        }
                    o[(size_t(y) * w + x) * 3 + c] = F2H(n ? float(s / n) : 0.f);
                }
        return o;
    }
    const UINT x0 = which == 'r' ? a.w - std::min(a.w, 64u) : 0, y0 = which == 'b' ? a.h - std::min(a.h, 64u) : 0;
    w = a.w - x0;
    h = a.h - y0;
    o.resize(size_t(w) * h * 3);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                o[(size_t(y) * w + x) * 3 + c] = F2H(a.v[(size_t(y0 + y) * a.w + x0 + x) * 3 + c]);
    return o;
}

struct Stats
{
    double psnr = 0, mad = 0, maxd = 0, corr = 0, same = 0;
    long long bad = 0;
    std::vector<double> bands;
};

static Stats Diff(const std::vector<uint16_t>& o, const std::vector<uint16_t>& r, UINT w, UINT h, std::vector<uint8_t>* img = nullptr)
{
    Stats s;
    double se = 0, sad = 0, so = 0, sr = 0, soo = 0, srr = 0, sor = 0, n = 0;
    long long same = 0;
    std::vector<double> bse(8, 0), bn(8, 0);
    if (img)
        img->assign(size_t(w) * h, 0);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
            {
                const size_t i = (size_t(y) * w + x) * 3 + c;
                const float a = H2F(o[i]), b = H2F(r[i]);
                if (!std::isfinite(a) || !std::isfinite(b))
                {
                    ++s.bad;
                    continue;
                }
                same += o[i] == r[i];
                const double d = double(a) - b;
                se += d * d;
                sad += std::fabs(d);
                s.maxd = std::max(s.maxd, std::fabs(d));
                so += a;
                sr += b;
                soo += double(a) * a;
                srr += double(b) * b;
                sor += double(a) * b;
                n += 1;
                const size_t band = size_t(y) * 8 / h;
                bse[band] += d * d;
                bn[band] += 1;
                if (img)
                {
                    uint8_t& p = (*img)[size_t(y) * w + x];
                    p = std::max(p, uint8_t(std::min(255.0, std::fabs(d) * 16 * 255 + 0.5)));
                }
            }
    const double mse = n ? se / n : 0;
    s.psnr = mse > 0 ? 10 * std::log10(1.0 / mse) : INFINITY;
    s.mad = n ? sad / n : 0;
    const double den = std::sqrt((soo - so * so / n) * (srr - sr * sr / n));
    s.corr = den > 0 ? (sor - so * sr / n) / den : 1.0;
    s.same = 100.0 * double(same) / double(std::max<size_t>(1, o.size()));
    for (int b = 0; b < 8; ++b)
        s.bands.push_back(bn[b] && bse[b] > 0 ? 10 * std::log10(bn[b] / bse[b]) : INFINITY);
    return s;
}

static void PrintBands(const char* tag, const Stats& s)
{
    std::printf("%s", tag);
    for (double b : s.bands)
        if (std::isfinite(b))
            std::printf(" %.1f", b);
        else
            std::printf(" inf");
    std::printf("\n");
}

int main(int argc, char** argv)
{
    const char areas[3] = { 'q', 'b', 'r' };
    const char* names[3] = { "quarter", "bottom64", "right64" };
    if (argc >= 6 && !std::strcmp(argv[1], "--pack"))
    {
        const UINT w = UINT(std::atoi(argv[3])), h = UINT(std::atoi(argv[4]));
        const auto rgb = ReadBin(argv[2], size_t(w) * h);
        if (rgb.empty())
        {
            std::printf("pack failed: no output\n");
            return 1;
        }
        const Img img = FromHalf(rgb, w, h);
        for (int k = 0; k < 3; ++k)
        {
            UINT aw = 0, ah = 0, rw = 0, rh = 0;
            const auto a = Area(img, areas[k], aw, ah);
            const std::string path = std::string(argv[5]) + "_" + areas[k] + ".jxr";
            if (!Save(path.c_str(), GUID_ContainerFormatWmp, GUID_WICPixelFormat48bppRGBHalf, aw, ah, aw * 6, a.data(), UINT(a.size() * 2)) ||
                ReadRef(path.c_str(), rw, rh) != a)
            {
                std::printf("pack failed: %s\n", path.c_str());
                return 1;
            }
        }
        return 0;
    }
    if (argc >= 6 && !std::strcmp(argv[1], "--bins"))
    {
        const UINT w = UINT(std::atoi(argv[4])), h = UINT(std::atoi(argv[5]));
        const auto a = ReadBin(argv[2], size_t(w) * h), b = ReadBin(argv[3], size_t(w) * h);
        if (a.empty() || b.empty())
        {
            std::printf("RUNS FAIL missing output\n");
            return 1;
        }
        UINT x0 = w, x1 = 0, y0 = h, y1 = 0;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                if (std::memcmp(&a[(size_t(y) * w + x) * 3], &b[(size_t(y) * w + x) * 3], 6))
                {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
        if (x0 > x1)
        {
            std::printf("RUNS identical\n");
            return 0;
        }
        const Stats s = Diff(a, b, w, h);
        std::printf("RUNS psnr %.2f dB, mean|d| %.6f, max|d| %.4f, identical %.2f%%, differing pixels in x %u-%u y %u-%u\n", s.psnr,
                    s.mad, s.maxd, s.same, x0, x1, y0, y1);
        PrintBands("RUNS bands (top to bottom, dB):", s);
        return 0;
    }
    if (argc < 5)
    {
        std::printf("usage: see the top of compare.cpp\n");
        return 2;
    }
    const UINT w = UINT(std::atoi(argv[2])), h = UINT(std::atoi(argv[3]));
    const auto rgb = ReadBin(argv[1], size_t(w) * h);
    if (rgb.empty())
    {
        std::printf("COMPARE FAIL no output (%s missing or short)\n", argv[1]);
        return 1;
    }
    const Img img = FromHalf(rgb, w, h);
    long long bad = 0;
    std::vector<uint8_t> view(size_t(w) * h * 3);
    for (size_t i = 0; i < img.v.size(); ++i)
    {
        const float o = img.v[i];
        bad += !std::isfinite(o);
        view[i - i % 3 + 2 - i % 3] = uint8_t(std::lround(std::clamp(std::isfinite(o) ? o : 0.f, 0.f, 1.f) * 255.f)); // BGR
    }
    if (argc > 6)
        Save(argv[6], GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, w, h, w * 3, view.data(), UINT(view.size()));
    double worst = INFINITY;
    int worst_k = 0;
    for (int k = 0; k < 3; ++k)
    {
        UINT aw = 0, ah = 0, rw = 0, rh = 0;
        const auto a = Area(img, areas[k], aw, ah);
        const auto ref = ReadRef((std::string(argv[4]) + "_" + areas[k] + ".jxr").c_str(), rw, rh);
        if (ref.empty() || rw != aw || rh != ah)
        {
            std::printf("COMPARE SKIPPED no usable reference %s_%c.jxr\n", argv[4], areas[k]);
            return 3;
        }
        std::vector<uint8_t> dimg;
        const Stats s = Diff(a, ref, aw, ah, k == 0 && argc > 5 ? &dimg : nullptr);
        std::printf("AREA %s %ux%u: psnr %.2f dB, mean|d| %.6f, max|d| %.4f, corr %.6f, identical %.2f%%\n", names[k], aw, ah, s.psnr,
                    s.mad, s.maxd, s.corr, s.same);
        if (k == 0)
            PrintBands("BANDS quarter (top to bottom, dB):", s);
        if (k == 0 && argc > 5)
            Save(argv[5], GUID_ContainerFormatPng, GUID_WICPixelFormat8bppGray, aw, ah, aw, dimg.data(), UINT(dimg.size()));
        if (s.psnr < worst)
        {
            worst = s.psnr;
            worst_k = k;
        }
    }
    std::printf("COMPARE psnr %.2f dB (worst area %s), nan/inf %lld\n", worst, names[worst_k], bad);
    return 0;
}
