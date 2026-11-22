// compare <out.bin> <w> <h> <ref.jxr> [diff.png] [view.jpg]
//   out.bin: the harness's RGBA16F rows. ref.jxr: the reference's RGB half floats, lossless JPEG XR (alpha dropped),
//   the whole frame or, when smaller, its bottom-right corner (where the network's tiles are padded). Prints the
//   output's NaN/Inf count, then over the reference's area PSNR (peak 1.0), mean and max abs difference,
//   correlation, identical values and channel means. diff.png: the largest channel's |out - ref| x 16 in 8-bit
//   grey over that area; view.jpg: the whole output as an 8-bit picture.
// compare --pack <out.bin> <w> <h> <ref.jxr> [cw ch]: makes a reference from a harness dump, cropped to its
//   bottom-right cw x ch.
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

int main(int argc, char** argv)
{
    if (argc >= 6 && !std::strcmp(argv[1], "--pack"))
    {
        const UINT w = UINT(std::atoi(argv[3])), h = UINT(std::atoi(argv[4]));
        const UINT cw = argc > 7 ? UINT(std::atoi(argv[6])) : w, ch = argc > 7 ? UINT(std::atoi(argv[7])) : h;
        const auto rgb = ReadBin(argv[2], size_t(w) * h);
        std::vector<uint16_t> crop(size_t(cw) * ch * 3);
        if (!rgb.empty() && cw <= w && ch <= h)
            for (UINT y = 0; y < ch; ++y)
                std::memcpy(&crop[size_t(y) * cw * 3], &rgb[(size_t(h - ch + y) * w + (w - cw)) * 3], size_t(cw) * 6);
        if (rgb.empty() || cw > w || ch > h ||
            !Save(argv[5], GUID_ContainerFormatWmp, GUID_WICPixelFormat48bppRGBHalf, cw, ch, cw * 6, crop.data(), UINT(crop.size() * 2)))
        {
            std::printf("pack failed\n");
            return 1;
        }
        UINT rw = 0, rh = 0;
        if (ReadRef(argv[5], rw, rh) != crop)
        {
            std::printf("pack: the reference does not read back exactly\n");
            return 1;
        }
        return 0;
    }
    if (argc < 5)
    {
        std::printf("usage: compare <out.bin> <w> <h> <ref.jxr> [diff.png] [view.jpg]\n");
        return 2;
    }
    const UINT w = UINT(std::atoi(argv[2])), h = UINT(std::atoi(argv[3]));
    const size_t px = size_t(w) * h;
    const auto out = ReadBin(argv[1], px);
    if (out.empty())
    {
        std::printf("COMPARE FAIL no output (%s missing or short)\n", argv[1]);
        return 1;
    }
    long long bad = 0;
    std::vector<uint8_t> view(px * 3);
    for (size_t i = 0; i < px * 3; ++i)
    {
        const float o = H2F(out[i]);
        bad += !std::isfinite(o);
        view[i - i % 3 + 2 - i % 3] = uint8_t(std::lround(std::clamp(std::isfinite(o) ? o : 0.f, 0.f, 1.f) * 255.f)); // BGR
    }
    std::printf("output %ux%u: %lld NaN/Inf values\n", w, h, bad);
    if (argc > 6)
        Save(argv[6], GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, w, h, w * 3, view.data(), UINT(view.size()));

    UINT rw = 0, rh = 0;
    const auto ref = ReadRef(argv[4], rw, rh);
    if (ref.empty() || rw > w || rh > h)
    {
        std::printf("COMPARE SKIPPED no usable reference %s\n", argv[4]);
        return 3;
    }
    long long badHere = 0, badRef = 0, same = 0;
    double se = 0, sad = 0, maxd = 0, so = 0, sr = 0, soo = 0, srr = 0, sor = 0;
    double mo[3] {}, mr[3] {};
    std::vector<uint8_t> diff(size_t(rw) * rh);
    for (UINT y = 0; y < rh; ++y)
        for (UINT x = 0; x < rw; ++x)
            for (int c = 0; c < 3; ++c)
            {
                const uint16_t ob = out[(size_t(h - rh + y) * w + (w - rw + x)) * 3 + c], rb = ref[(size_t(y) * rw + x) * 3 + c];
                const float o = H2F(ob), r = H2F(rb);
                badHere += !std::isfinite(o);
                badRef += !std::isfinite(r);
                if (!std::isfinite(o) || !std::isfinite(r))
                    continue;
                same += ob == rb;
                const double d = double(o) - r;
                se += d * d;
                sad += std::fabs(d);
                maxd = std::max(maxd, std::fabs(d));
                so += o; sr += r; soo += double(o) * o; srr += double(r) * r; sor += double(o) * r;
                mo[c] += o;
                mr[c] += r;
                uint8_t& p = diff[size_t(y) * rw + x];
                p = std::max(p, uint8_t(std::min(255.0, std::fabs(d) * 16 * 255 + 0.5)));
            }
    const double n = double(size_t(rw) * rh * 3) - double(badHere) - double(badRef), n3 = n / 3;
    const double mse = se / n, psnr = mse > 0 ? 10 * std::log10(1.0 / mse) : INFINITY;
    const double corr = (sor - so * sr / n) / std::sqrt((soo - so * so / n) * (srr - sr * sr / n));
    std::printf("reference %ux%u (%s): %lld NaN/Inf values\n", rw, rh, rw == w && rh == h ? "whole frame" : "bottom-right corner", badRef);
    std::printf("channel means out %.4f %.4f %.4f, ref %.4f %.4f %.4f\n", mo[0] / n3, mo[1] / n3, mo[2] / n3, mr[0] / n3,
                mr[1] / n3, mr[2] / n3);
    std::printf("COMPARE psnr %.2f dB, mean|d| %.6f, max|d| %.4f, corr %.6f, identical %.2f%%, nan/inf %lld, area %ux%u\n", psnr,
                sad / n, maxd, corr, 100.0 * double(same) / double(size_t(rw) * rh * 3), bad, rw, rh);
    if (argc > 5)
        Save(argv[5], GUID_ContainerFormatPng, GUID_WICPixelFormat8bppGray, rw, rh, rw, diff.data(), UINT(diff.size()));
    return 0;
}
