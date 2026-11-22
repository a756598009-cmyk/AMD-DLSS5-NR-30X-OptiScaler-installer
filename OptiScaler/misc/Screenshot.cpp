#include "pch.h"
#include "Screenshot.h"

#include <Config.h>
#include <Logger.h>
#include <State.h>
#include <Util.h>
#include <hooks/GL_Hooks.h>
#include <menu/menu_overlay_base.h>

#include <d3d9.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace
{
// How the bytes of a pixel are laid out: RGB10A2 has red in the low bits, BGR10A2 blue.
enum class Layout
{
    RGBA8,
    BGRA8,
    RGB10A2,
    BGR10A2,
    RGBA16F
};

// What the values mean: sRGB (SDR), PQ in Rec.2020 (HDR10) or linear Rec.709 with 1.0 at 80 nits (scRGB).
enum class Encoding
{
    Srgb,
    Pq,
    ScRgb
};

struct Image
{
    uint32_t width = 0;
    uint32_t height = 0;
    Layout layout = Layout::RGBA8;
    Encoding encoding = Encoding::Srgb;
    // Rows without padding.
    std::vector<uint8_t> pixels;
};

uint32_t BytesPerPixel(Layout layout) { return layout == Layout::RGBA16F ? 8 : 4; }

const char* Name(Encoding encoding)
{
    switch (encoding)
    {
    case Encoding::Pq:
        return "HDR10";
    case Encoding::ScRgb:
        return "scRGB";
    default:
        return "SDR";
    }
}

std::atomic<bool> _requested = false;
// Screenshots copied from the GPU and not written yet.
std::atomic<int> _inFlight = 0;
std::mutex _nameMutex;

// One press: the clean image and the copy with the menu share the time and the name, which the first of the two
// to be written picks (under _nameMutex).
struct Shot
{
    SYSTEMTIME time {};
    std::shared_ptr<std::wstring> base = std::make_shared<std::wstring>();
    bool menu = false;
    // " (menu)", or " (overlay)" when only the FPS overlay is on screen.
    const wchar_t* tag = L" (menu)";
};

// The copy with the menu of the frame just taken, for the ...Menu point of the same present.
std::optional<Shot> _menuShot;

std::optional<Shot> Take()
{
    if (!_requested.exchange(false))
        return std::nullopt;

    Shot shot;
    GetLocalTime(&shot.time);
    _menuShot.reset();
    const bool menu = MenuOverlayBase::IsVisible();
    if ((menu || Config::Instance()->ShowFps.value_or_default()) &&
        Config::Instance()->ScreenshotWithMenu.value_or_default())
    {
        _menuShot = shot;
        _menuShot->menu = true;
        if (!menu)
            _menuShot->tag = L" (overlay)";
    }
    return shot;
}

std::optional<Shot> TakeMenu() { return std::exchange(_menuShot, std::nullopt); }

// A copy with the menu still waiting at the next frame's capture: this path has no point after the overlay.
void DropMenu(const char* api)
{
    if (!_menuShot)
        return;

    _menuShot.reset();
    static bool logged = false;
    if (!std::exchange(logged, true))
        LOG_INFO("Screenshot: no copy with the menu on this {} path, only the frame without it is saved", api);
}

// ---- conversion ---------------------------------------------------------------------------------------------

float PqToNits(float value)
{
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(std::max(value, 0.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

// Linear 0..1 to 8-bit sRGB, by table.
uint8_t ToSrgb8(float linear)
{
    constexpr int size = 16384;
    static const auto table = []
    {
        std::vector<uint8_t> t(size + 1);
        for (int i = 0; i <= size; ++i)
        {
            const float l = float(i) / size;
            const float s = l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
            t[i] = static_cast<uint8_t>(std::clamp(s * 255.0f + 0.5f, 0.0f, 255.0f));
        }
        return t;
    }();
    return table[static_cast<int>(std::clamp(linear, 0.0f, 1.0f) * size + 0.5f)];
}

uint8_t Ten(uint32_t value) { return static_cast<uint8_t>((value * 255 + 511) / 1023); }

// The HDR frame as linear Rec.709 in nits, one RGB triple per pixel.
std::vector<float> ToNits(const Image& image)
{
    const size_t count = size_t(image.width) * image.height;
    std::vector<float> nits(count * 3);

    if (image.layout == Layout::RGBA16F)
    {
        auto source = reinterpret_cast<const uint16_t*>(image.pixels.data());
        for (size_t i = 0; i < count; ++i)
            for (int c = 0; c < 3; ++c)
                nits[i * 3 + c] = DirectX::PackedVector::XMConvertHalfToFloat(source[i * 4 + c]) * 80.0f;
        return nits;
    }

    static const auto pq = []
    {
        std::vector<float> t(1024);
        for (int i = 0; i < 1024; ++i)
            t[i] = PqToNits(i / 1023.0f);
        return t;
    }();

    const bool blueLow = image.layout == Layout::BGR10A2;
    auto source = reinterpret_cast<const uint32_t*>(image.pixels.data());
    for (size_t i = 0; i < count; ++i)
    {
        const uint32_t v = source[i];
        float r = pq[v & 0x3FF], g = pq[(v >> 10) & 0x3FF], b = pq[(v >> 20) & 0x3FF];
        if (blueLow)
            std::swap(r, b);

        // Rec.2020 to Rec.709 primaries.
        nits[i * 3 + 0] = 1.660491f * r - 0.587641f * g - 0.072850f * b;
        nits[i * 3 + 1] = -0.124550f * r + 1.132900f * g - 0.008349f * b;
        nits[i * 3 + 2] = -0.018151f * r - 0.100579f * g + 1.118730f * b;
    }
    return nits;
}

// 24-bit BGR rows for the PNG. HDR: 203 nits (reference white) is 1.0, highlights roll off softly above 0.8
// by the brightest channel so hues hold, then sRGB.
std::vector<uint8_t> ToSdr(const Image& image, const std::vector<float>* nits)
{
    const size_t count = size_t(image.width) * image.height;
    std::vector<uint8_t> out(count * 3);

    if (nits != nullptr)
    {
        constexpr float knee = 0.8f;
        for (size_t i = 0; i < count; ++i)
        {
            float r = std::max((*nits)[i * 3 + 0] / 203.0f, 0.0f);
            float g = std::max((*nits)[i * 3 + 1] / 203.0f, 0.0f);
            float b = std::max((*nits)[i * 3 + 2] / 203.0f, 0.0f);
            const float peak = std::max({ r, g, b });
            if (peak > knee)
            {
                const float mapped = knee + (1.0f - knee) * (1.0f - std::exp(-(peak - knee) / (1.0f - knee)));
                const float scale = mapped / peak;
                r *= scale;
                g *= scale;
                b *= scale;
            }
            out[i * 3 + 0] = ToSrgb8(b);
            out[i * 3 + 1] = ToSrgb8(g);
            out[i * 3 + 2] = ToSrgb8(r);
        }
        return out;
    }

    const uint8_t* p = image.pixels.data();
    for (size_t i = 0; i < count; ++i, p += 4)
    {
        uint8_t r, g, b;
        switch (image.layout)
        {
        case Layout::BGRA8:
            b = p[0], g = p[1], r = p[2];
            break;
        case Layout::RGB10A2:
        case Layout::BGR10A2:
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            r = Ten(v & 0x3FF), g = Ten((v >> 10) & 0x3FF), b = Ten((v >> 20) & 0x3FF);
            if (image.layout == Layout::BGR10A2)
                std::swap(r, b);
            break;
        }
        default:
            r = p[0], g = p[1], b = p[2];
            break;
        }
        out[i * 3 + 0] = b;
        out[i * 3 + 1] = g;
        out[i * 3 + 2] = r;
    }
    return out;
}

// scRGB as half floats, alpha 1, for the JPEG XR file.
std::vector<uint16_t> ToScRgbHalf(const std::vector<float>& nits, size_t count)
{
    std::vector<uint16_t> out(count * 4);
    const uint16_t one = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
    for (size_t i = 0; i < count; ++i)
    {
        for (int c = 0; c < 3; ++c)
            out[i * 4 + c] = DirectX::PackedVector::XMConvertFloatToHalf(nits[i * 3 + c] / 80.0f);
        out[i * 4 + 3] = one;
    }
    return out;
}

// ---- files ---------------------------------------------------------------------------------------------------

#define SHOT_TRY(call)                                                                                                 \
    if (HRESULT hr_ = (call); FAILED(hr_))                                                                             \
    {                                                                                                                  \
        why = std::format("{} failed: {:#x}", #call, (unsigned) hr_);                                                  \
        return false;                                                                                                  \
    }

bool Write(const std::filesystem::path& path, REFGUID container, REFGUID format, const Image& image, UINT stride,
           const void* data, std::string& why)
{
    ComPtr<IWICImagingFactory> factory;
    SHOT_TRY(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;
    SHOT_TRY(factory->CreateStream(&stream));
    SHOT_TRY(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    SHOT_TRY(factory->CreateEncoder(container, nullptr, &encoder));
    SHOT_TRY(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    SHOT_TRY(encoder->CreateNewFrame(&frame, &properties));
    SHOT_TRY(frame->Initialize(properties.Get()));
    SHOT_TRY(frame->SetSize(image.width, image.height));
    WICPixelFormatGUID pixelFormat = format;
    SHOT_TRY(frame->SetPixelFormat(&pixelFormat));
    if (pixelFormat != format)
    {
        why = "the encoder does not take this pixel format";
        return false;
    }
    SHOT_TRY(
        frame->WritePixels(image.height, stride, stride * image.height, static_cast<BYTE*>(const_cast<void*>(data))));
    SHOT_TRY(frame->Commit());
    SHOT_TRY(encoder->Commit());
    return true;
}

// "<exe> 2026-10-17 21-05-33", with -1, -2 ... when a file of that name is there.
std::filesystem::path Unused(const std::filesystem::path& folder, const SYSTEMTIME& time)
{
    const auto base = std::format(L"{} {:04}-{:02}-{:02} {:02}-{:02}-{:02}", Util::ExePath().stem().wstring(),
                                  time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    for (int i = 0;; ++i)
    {
        const auto path = (folder / (i == 0 ? base : std::format(L"{}-{}", base, i))).wstring();
        std::error_code error;
        bool used = false;
        for (const wchar_t* name : { L".png", L".jxr", L" (menu).png", L" (menu).jxr", L" (overlay).png", L" (overlay).jxr" })
            used = used || std::filesystem::exists(path + name, error);
        if (!used)
            return path;
    }
}

bool Save(const Image& image, const Shot& shot, std::string& saved, std::string& why)
{
    const auto folder = Screenshot::Folder();
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error)
    {
        why = std::format("cannot create {}: {}", wstring_to_string(folder.wstring()), error.message());
        return false;
    }

    // Names are taken and files made under one lock, so two shots in the same second get two names.
    std::lock_guard lock(_nameMutex);
    if (shot.base->empty())
        *shot.base = Unused(folder, shot.time).wstring();
    const std::filesystem::path base = *shot.base + (shot.menu ? shot.tag : L"");
    const std::filesystem::path png = base.wstring() + L".png";
    const bool hdr = image.encoding != Encoding::Srgb;

    std::vector<float> nits;
    if (hdr)
    {
        nits = ToNits(image);
        const std::filesystem::path jxr = base.wstring() + L".jxr";
        const auto half = ToScRgbHalf(nits, size_t(image.width) * image.height);
        if (!Write(jxr, GUID_ContainerFormatWmp, GUID_WICPixelFormat64bppRGBAHalf, image, image.width * 8, half.data(),
                   why))
        {
            std::filesystem::remove(jxr, error);
            why = "JPEG XR: " + why;
            return false;
        }
        saved = wstring_to_string(jxr.wstring()) + " and ";
    }

    const auto sdr = ToSdr(image, hdr ? &nits : nullptr);
    if (!Write(png, GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, image, image.width * 3, sdr.data(), why))
    {
        std::filesystem::remove(png, error);
        why = "PNG: " + why;
        return false;
    }
    saved += wstring_to_string(png.wstring());
    return true;
}

// fetch runs on the writer thread: it waits for the GPU copy if it has to and fills the image.
void Queue(std::function<bool(Image&, std::string&)> fetch, Shot shot)
{
    _inFlight++;

    std::thread(
        [fetch = std::move(fetch), shot = std::move(shot)]
        {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Image image;
            std::string why, saved;
            const auto start = Util::MillisecondsNow();

            if (!fetch(image, why))
                LOG_WARN("Screenshot failed: {}", why);
            else if (!Save(image, shot, saved, why))
                LOG_WARN("Screenshot failed: {}", why);
            else
                LOG_INFO("Screenshot saved ({} {}x{}, {:.0f} ms): {}", Name(image.encoding), image.width, image.height,
                         Util::MillisecondsNow() - start, saved);

            if (SUCCEEDED(com))
                CoUninitialize();
            _inFlight--;
        })
        .detach();
}

void Fail(const std::string& why) { LOG_WARN("Screenshot failed: {}", why); }

// ---- DXGI ----------------------------------------------------------------------------------------------------

bool FromDxgiFormat(DXGI_FORMAT format, Layout& layout, Encoding& encoding)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        layout = Layout::RGBA8;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        layout = Layout::BGRA8;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        layout = Layout::RGB10A2;
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        layout = Layout::RGBA16F;
        break;
    default:
        return false;
    }

    // A half float swapchain is scRGB whatever colour space is set; a 10-bit one is HDR10 once the game set PQ.
    if (layout == Layout::RGBA16F)
        encoding = Encoding::ScRgb;
    else if (layout == Layout::RGB10A2 && State::Instance().outputColorSpace.transfer == ColorTransfer::PQ)
        encoding = Encoding::Pq;
    else
        encoding = Encoding::Srgb;

    return true;
}

void CopyRows(Image& image, const uint8_t* source, size_t pitch)
{
    const size_t row = size_t(image.width) * BytesPerPixel(image.layout);
    image.pixels.resize(row * image.height);
    for (uint32_t y = 0; y < image.height; ++y)
        std::memcpy(image.pixels.data() + y * row, source + y * pitch, row);
}

// A D3D11 copy is read back from the immediate context, so from the present thread: Map without waiting on each
// present until the copy is done.
struct PendingDx11
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture2D> staging;
    Image image;
    Shot shot;
    double since = 0;
};
// The clean copy and the one with the menu.
std::vector<std::unique_ptr<PendingDx11>> _pendingDx11;

// Hands an image read on this thread to the writer.
void QueueImage(Image image, Shot shot)
{
    Queue(
        [image = std::move(image)](Image& out, std::string&) mutable
        {
            out = std::move(image);
            return true;
        },
        std::move(shot));
}

void PumpDx11()
{
    for (size_t i = 0; i < _pendingDx11.size();)
    {
        auto& pending = *_pendingDx11[i];
        ComPtr<ID3D11DeviceContext> context;
        pending.device->GetImmediateContext(&context);

        // After two seconds it waits, once.
        const bool wait = Util::MillisecondsNow() - pending.since > 2000;
        D3D11_MAPPED_SUBRESOURCE mapped {};
        const HRESULT hr =
            context->Map(pending.staging.Get(), 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            ++i;
            continue;
        }

        auto owned = std::move(_pendingDx11[i]);
        _pendingDx11.erase(_pendingDx11.begin() + i);
        if (FAILED(hr))
        {
            Fail(std::format("D3D11 Map: {:#x}", (unsigned) hr));
            continue;
        }

        CopyRows(owned->image, static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch);
        context->Unmap(owned->staging.Get(), 0);
        QueueImage(std::move(owned->image), std::move(owned->shot));
    }
}

void CaptureDx11(ID3D11Device* device, ID3D11Texture2D* texture, Shot shot)
{
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);

    Image image;
    image.width = desc.Width;
    image.height = desc.Height;
    if (!FromDxgiFormat(desc.Format, image.layout, image.encoding))
        return Fail(std::format("D3D11 back buffer format {} is not supported", (int) desc.Format));

    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);

    ComPtr<ID3D11Texture2D> source = texture;
    if (desc.SampleDesc.Count > 1)
    {
        auto resolvedDesc = desc;
        resolvedDesc.SampleDesc = { 1, 0 };
        resolvedDesc.MipLevels = 1;
        resolvedDesc.ArraySize = 1;
        resolvedDesc.BindFlags = 0;
        resolvedDesc.MiscFlags = 0;
        resolvedDesc.CPUAccessFlags = 0;
        resolvedDesc.Usage = D3D11_USAGE_DEFAULT;
        ComPtr<ID3D11Texture2D> resolved;
        if (FAILED(device->CreateTexture2D(&resolvedDesc, nullptr, &resolved)))
            return Fail("D3D11 resolve texture could not be created");
        context->ResolveSubresource(resolved.Get(), 0, texture, 0, desc.Format);
        source = resolved;
    }

    D3D11_TEXTURE2D_DESC stagingDesc {};
    stagingDesc.Width = desc.Width;
    stagingDesc.Height = desc.Height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = desc.Format;
    stagingDesc.SampleDesc = { 1, 0 };
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    auto pending = std::make_unique<PendingDx11>();
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &pending->staging)))
        return Fail("D3D11 staging texture could not be created");

    context->CopySubresourceRegion(pending->staging.Get(), 0, 0, 0, 0, source.Get(), 0, nullptr);
    context->Flush();

    pending->device = device;
    pending->image = std::move(image);
    pending->shot = std::move(shot);
    pending->since = Util::MillisecondsNow();
    _pendingDx11.push_back(std::move(pending));
}

void CaptureDx12(ID3D12CommandQueue* gameQueue, IDXGISwapChain* swapChain, Shot shot)
{
    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))))
        return Fail("D3D12 swapchain has no IDXGISwapChain3");

    ComPtr<ID3D12Resource> backBuffer;
    if (FAILED(swapChain3->GetBuffer(swapChain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer))))
        return Fail("D3D12 back buffer unavailable");

    // Streamline's proxy queue stands in front of the real one; the overlay submits to the real one too.
    ID3D12CommandQueue* queue = gameQueue;
    if (!Util::CheckForRealObject(__FUNCTION__, gameQueue, (IUnknown**) &queue))
        queue = gameQueue;

    const auto desc = backBuffer->GetDesc();
    Image image;
    image.width = static_cast<uint32_t>(desc.Width);
    image.height = desc.Height;
    if (!FromDxgiFormat(desc.Format, image.layout, image.encoding))
        return Fail(std::format("D3D12 back buffer format {} is not supported", (int) desc.Format));

    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return Fail("D3D12 queue has no device");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES heap { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = total;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc = { 1, 0 };
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    const auto type = queue->GetDesc().Type;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return Fail("D3D12 readback objects could not be created");

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer.Get();
    barrier.Transition.Subresource = 0;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION target { readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    target.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION from { backBuffer.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    from.SubresourceIndex = 0;
    list->CopyTextureRegion(&target, 0, 0, 0, &from, nullptr);

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);

    if (FAILED(list->Close()))
        return Fail("D3D12 copy list could not be closed");

    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.Get(), 1)))
        return Fail("D3D12 fence signal failed");

    Queue(
        [image = std::move(image), readback, allocator, list, fence, footprint](Image& out, std::string& why) mutable
        {
            HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            const bool finished = done != nullptr && SUCCEEDED(fence->SetEventOnCompletion(1, done)) &&
                                  WaitForSingleObject(done, 5000) == WAIT_OBJECT_0;
            if (done != nullptr)
                CloseHandle(done);
            if (!finished)
            {
                why = "the D3D12 copy did not finish in 5 s";
                return false;
            }

            void* data = nullptr;
            D3D12_RANGE range { 0, static_cast<SIZE_T>(footprint.Footprint.RowPitch) * image.height };
            if (FAILED(readback->Map(0, &range, &data)))
            {
                why = "D3D12 readback Map failed";
                return false;
            }
            CopyRows(image, static_cast<const uint8_t*>(data) + footprint.Offset, footprint.Footprint.RowPitch);
            D3D12_RANGE none { 0, 0 };
            readback->Unmap(0, &none);
            out = std::move(image);
            return true;
        },
        std::move(shot));
}

void CaptureDxgi(IDXGISwapChain* swapChain, IUnknown* device, ID3D11Texture2D* frame, Shot shot)
{
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D12CommandQueue> queue;

    if (frame != nullptr && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device11))))
        return CaptureDx11(device11.Get(), frame, std::move(shot));

    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))))
        return CaptureDx12(queue.Get(), swapChain, std::move(shot));

    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device11))))
    {
        ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
            return Fail("D3D11 back buffer unavailable");
        return CaptureDx11(device11.Get(), backBuffer.Get(), std::move(shot));
    }

    Fail("the swapchain's device is neither D3D11 nor D3D12");
}

// ---- OpenGL --------------------------------------------------------------------------------------------------

using GLuint = unsigned int;
using GLenum = unsigned int;
using GLint = int;
using GLsizei = int;
using GLsizeiptr = ptrdiff_t;
using GLintptr = ptrdiff_t;
using GLbitfield = unsigned int;
using GLsync = struct __GLsync*;

constexpr GLenum GL_RGBA_ = 0x1908, GL_UNSIGNED_BYTE_ = 0x1401, GL_BACK_ = 0x0405, GL_READ_BUFFER_ = 0x0C02,
                 GL_PACK_ALIGNMENT_ = 0x0D05, GL_PACK_ROW_LENGTH_ = 0x0D02, GL_READ_FRAMEBUFFER_ = 0x8CA8,
                 GL_READ_FRAMEBUFFER_BINDING_ = 0x8CAA, GL_PIXEL_PACK_BUFFER_ = 0x88EB,
                 GL_PIXEL_PACK_BUFFER_BINDING_ = 0x88ED, GL_STREAM_READ_ = 0x88E1, GL_MAP_READ_BIT_ = 0x0001,
                 GL_SYNC_GPU_COMMANDS_COMPLETE_ = 0x9117, GL_ALREADY_SIGNALED_ = 0x911A,
                 GL_CONDITION_SATISFIED_ = 0x911C, GL_WAIT_FAILED_ = 0x911D, GL_SYNC_FLUSH_COMMANDS_BIT_ = 0x0001;

struct GlFunctions
{
    void(WINAPI* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void(WINAPI* ReadBuffer)(GLenum) = nullptr;
    void(WINAPI* PixelStorei)(GLenum, GLint) = nullptr;
    void(WINAPI* GetIntegerv)(GLenum, GLint*) = nullptr;
    void(WINAPI* Flush)() = nullptr;
    void(WINAPI* BindFramebuffer)(GLenum, GLuint) = nullptr;
    void(WINAPI* GenBuffers)(GLsizei, GLuint*) = nullptr;
    void(WINAPI* DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void(WINAPI* BindBuffer)(GLenum, GLuint) = nullptr;
    void(WINAPI* BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void*(WINAPI* MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = nullptr;
    unsigned char(WINAPI* UnmapBuffer)(GLenum) = nullptr;
    GLsync(WINAPI* FenceSync)(GLenum, GLbitfield) = nullptr;
    GLenum(WINAPI* ClientWaitSync)(GLsync, GLbitfield, uint64_t) = nullptr;
    void(WINAPI* DeleteSync)(GLsync) = nullptr;

    bool Load()
    {
        auto get = [](auto& f, const char* name)
        { f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(GLHooks::GetProc(name)); };
        get(ReadPixels, "glReadPixels");
        get(ReadBuffer, "glReadBuffer");
        get(PixelStorei, "glPixelStorei");
        get(GetIntegerv, "glGetIntegerv");
        get(Flush, "glFlush");
        get(BindFramebuffer, "glBindFramebuffer");
        get(GenBuffers, "glGenBuffers");
        get(DeleteBuffers, "glDeleteBuffers");
        get(BindBuffer, "glBindBuffer");
        get(BufferData, "glBufferData");
        get(MapBufferRange, "glMapBufferRange");
        get(UnmapBuffer, "glUnmapBuffer");
        get(FenceSync, "glFenceSync");
        get(ClientWaitSync, "glClientWaitSync");
        get(DeleteSync, "glDeleteSync");
        return ReadPixels && ReadBuffer && PixelStorei && GetIntegerv && Flush;
    }

    bool Async() const
    {
        return BindFramebuffer && GenBuffers && DeleteBuffers && BindBuffer && BufferData && MapBufferRange &&
               UnmapBuffer && FenceSync && ClientWaitSync && DeleteSync;
    }
};

struct PendingGl
{
    GLuint buffer = 0;
    GLsync sync = nullptr;
    uint32_t generation = 0;
    double since = 0;
    Image image;
    Shot shot;
};
std::vector<std::unique_ptr<PendingGl>> _pendingGl;

// Null without the functions the copy needs.
GlFunctions* GlApi()
{
    static GlFunctions gl;
    static const bool loaded = gl.Load();
    return loaded ? &gl : nullptr;
}

// OpenGL rows run bottom to top.
void QueueFlipped(Image image, const uint8_t* source, Shot shot)
{
    const size_t row = size_t(image.width) * 4;
    image.pixels.resize(row * image.height);
    for (uint32_t y = 0; y < image.height; ++y)
        std::memcpy(image.pixels.data() + y * row, source + (image.height - 1 - y) * row, row);

    QueueImage(std::move(image), std::move(shot));
}

// ---- Vulkan --------------------------------------------------------------------------------------------------

bool FromVkFormat(VkFormat format, VkColorSpaceKHR colorSpace, Layout& layout, Encoding& encoding)
{
    switch (format)
    {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        layout = Layout::RGBA8;
        break;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        layout = Layout::BGRA8;
        break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        layout = Layout::RGB10A2;
        break;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        layout = Layout::BGR10A2;
        break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        layout = Layout::RGBA16F;
        break;
    default:
        return false;
    }

    if (layout == Layout::RGBA16F)
        encoding = Encoding::ScRgb;
    else if ((layout == Layout::RGB10A2 || layout == Layout::BGR10A2) && colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
        encoding = Encoding::Pq;
    else
        encoding = Encoding::Srgb;

    return true;
}
// The semaphores the last captured present waited for, one per copy (clean, with the menu); each goes with the
// next capture of its kind, long after that present.
VkDevice _semaphoreDevice[2] {};
VkSemaphore _semaphore[2] {};

void CaptureVulkan(const Screenshot::VulkanTarget& t, VkPresentInfoKHR& present, Shot shot)
{
    Image image;
    image.width = t.extent.width;
    image.height = t.extent.height;
    if (!FromVkFormat(t.format, t.colorSpace, image.layout, image.encoding))
        return Fail(std::format("Vulkan swapchain format {} is not supported", (int) t.format));
    if (!t.transferSource)
        return Fail("the Vulkan swapchain was made without transfer source usage");

    const VkDeviceSize size = VkDeviceSize(image.width) * image.height * BytesPerPixel(image.layout);
    VkBufferCreateInfo bufferInfo { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    struct Objects
    {
        VkDevice device = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;

        void Destroy()
        {
            if (fence)
                vkDestroyFence(device, fence, nullptr);
            if (pool)
                vkDestroyCommandPool(device, pool, nullptr);
            if (buffer)
                vkDestroyBuffer(device, buffer, nullptr);
            if (memory)
                vkFreeMemory(device, memory, nullptr);
        }
    } o { t.device };

    auto failed = [&](const char* what)
    {
        o.Destroy();
        Fail(std::format("Vulkan: {}", what));
    };

    if (vkCreateBuffer(t.device, &bufferInfo, nullptr, &o.buffer) != VK_SUCCESS)
        return failed("buffer");

    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(t.device, o.buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory {};
    vkGetPhysicalDeviceMemoryProperties(t.physicalDevice, &memory);
    uint32_t typeIndex = UINT32_MAX;
    for (VkMemoryPropertyFlags wanted :
         { VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
           VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) })
    {
        for (uint32_t i = 0; i < memory.memoryTypeCount && typeIndex == UINT32_MAX; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
                typeIndex = i;
        if (typeIndex != UINT32_MAX)
            break;
    }
    if (typeIndex == UINT32_MAX)
        return failed("no host visible memory");

    VkMemoryAllocateInfo allocateInfo { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = typeIndex;
    if (vkAllocateMemory(t.device, &allocateInfo, nullptr, &o.memory) != VK_SUCCESS ||
        vkBindBufferMemory(t.device, o.buffer, o.memory, 0) != VK_SUCCESS)
        return failed("memory");

    VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = t.family;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmdInfo { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateCommandPool(t.device, &poolInfo, nullptr, &o.pool) != VK_SUCCESS ||
        (cmdInfo.commandPool = o.pool, vkAllocateCommandBuffers(t.device, &cmdInfo, &cmd)) != VK_SUCCESS ||
        vkCreateFence(t.device, &fenceInfo, nullptr, &o.fence) != VK_SUCCESS)
        return failed("command objects");

    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkSemaphoreCreateInfo semaphoreInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateSemaphore(t.device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS)
        return failed("semaphore");

    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = t.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    VkBufferImageCopy region {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { image.width, image.height, 1 };
    vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.buffer, 1, &region);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    std::swap(barrier.oldLayout, barrier.newLayout);
    VkBufferMemoryBarrier hostRead { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostRead.buffer = o.buffer;
    hostRead.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead,
                         1, &barrier);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
    {
        vkDestroySemaphore(t.device, semaphore, nullptr);
        return failed("command buffer");
    }

    std::vector<VkPipelineStageFlags> stages(present.waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = present.waitSemaphoreCount;
    submit.pWaitSemaphores = present.pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &semaphore;
    if (vkQueueSubmit(t.queue, 1, &submit, o.fence) != VK_SUCCESS)
    {
        vkDestroySemaphore(t.device, semaphore, nullptr);
        return failed("submit");
    }

    const int slot = shot.menu ? 1 : 0;
    if (_semaphore[slot] != VK_NULL_HANDLE && _semaphoreDevice[slot] == t.device)
        vkDestroySemaphore(_semaphoreDevice[slot], _semaphore[slot], nullptr);
    _semaphore[slot] = semaphore;
    _semaphoreDevice[slot] = t.device;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &_semaphore[slot];

    Queue(
        [image = std::move(image), o, size](Image& out, std::string& why) mutable
        {
            void* data = nullptr;
            const bool ok = vkWaitForFences(o.device, 1, &o.fence, VK_TRUE, 5'000'000'000ull) == VK_SUCCESS &&
                            vkMapMemory(o.device, o.memory, 0, size, 0, &data) == VK_SUCCESS;
            if (ok)
            {
                CopyRows(image, static_cast<const uint8_t*>(data), size_t(image.width) * BytesPerPixel(image.layout));
                vkUnmapMemory(o.device, o.memory);
            }
            else
                why = "the Vulkan copy did not finish in 5 s";

            o.Destroy();
            out = std::move(image);
            return ok;
        },
        std::move(shot));
}

// OptiScaler's context current: the back buffer as it is now, read back on a later present.
void CaptureGl(GlFunctions& gl, uint32_t width, uint32_t height, uint32_t generation, Shot shot)
{
    Image image;
    image.width = width;
    image.height = height;
    image.layout = Layout::RGBA8;

    // OptiScaler's context: its own bindings, put back as they were.
    GLint readFramebuffer = 0, packBuffer = 0, alignment = 4, rowLength = 0, readBuffer = GL_BACK_;
    const bool async = gl.Async();
    if (async)
    {
        gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING_, &readFramebuffer);
        gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING_, &packBuffer);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    }
    gl.GetIntegerv(GL_PACK_ALIGNMENT_, &alignment);
    gl.GetIntegerv(GL_PACK_ROW_LENGTH_, &rowLength);
    gl.GetIntegerv(GL_READ_BUFFER_, &readBuffer);
    gl.ReadBuffer(GL_BACK_);
    gl.PixelStorei(GL_PACK_ALIGNMENT_, 4);
    gl.PixelStorei(GL_PACK_ROW_LENGTH_, 0);

    const auto size = GLsizeiptr(width) * height * 4;
    if (async)
    {
        auto pending = std::make_unique<PendingGl>();
        gl.GenBuffers(1, &pending->buffer);
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER_, pending->buffer);
        gl.BufferData(GL_PIXEL_PACK_BUFFER_, size, nullptr, GL_STREAM_READ_);
        gl.ReadPixels(0, 0, width, height, GL_RGBA_, GL_UNSIGNED_BYTE_, nullptr);
        pending->sync = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE_, 0);
        gl.Flush();
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER_, packBuffer);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER_, readFramebuffer);
        pending->generation = generation;
        pending->since = Util::MillisecondsNow();
        pending->image = std::move(image);
        pending->shot = std::move(shot);
        if (pending->sync != nullptr)
            _pendingGl.push_back(std::move(pending));
        else
        {
            gl.DeleteBuffers(1, &pending->buffer);
            Fail("OpenGL fence could not be made");
        }
    }
    else
    {
        // Without buffer objects (OpenGL before 3.2): read at once, the frame waits for the GPU.
        std::vector<uint8_t> pixels(size);
        gl.ReadPixels(0, 0, width, height, GL_RGBA_, GL_UNSIGNED_BYTE_, pixels.data());
        QueueFlipped(std::move(image), pixels.data(), std::move(shot));
    }

    gl.PixelStorei(GL_PACK_ALIGNMENT_, alignment);
    gl.PixelStorei(GL_PACK_ROW_LENGTH_, rowLength);
    gl.ReadBuffer(readBuffer);
}

// The copies of earlier presents whose fence has passed, or that waited long enough.
void PumpGl(GlFunctions& gl, uint32_t generation)
{
    for (size_t i = 0; i < _pendingGl.size();)
    {
        auto& pending = *_pendingGl[i];

        // The buffer belongs to an older OptiScaler context, which can no longer be made current.
        if (pending.generation != generation)
        {
            _pendingGl.erase(_pendingGl.begin() + i);
            Fail("the OpenGL context changed before the copy was read");
            continue;
        }

        const bool wait = Util::MillisecondsNow() - pending.since > 2000;
        const GLenum status =
            gl.ClientWaitSync(pending.sync, wait ? GL_SYNC_FLUSH_COMMANDS_BIT_ : 0, wait ? 5'000'000'000ull : 0);
        if (status != GL_ALREADY_SIGNALED_ && status != GL_CONDITION_SATISFIED_ && !wait)
        {
            ++i;
            continue;
        }

        auto owned = std::move(_pendingGl[i]);
        _pendingGl.erase(_pendingGl.begin() + i);
        gl.DeleteSync(owned->sync);
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER_, owned->buffer);
        const auto size = GLsizeiptr(owned->image.width) * owned->image.height * 4;
        auto data =
            status == GL_WAIT_FAILED_ ? nullptr : gl.MapBufferRange(GL_PIXEL_PACK_BUFFER_, 0, size, GL_MAP_READ_BIT_);
        if (data != nullptr)
        {
            QueueFlipped(std::move(owned->image), static_cast<const uint8_t*>(data), std::move(owned->shot));
            gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER_);
        }
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER_, 0);
        gl.DeleteBuffers(1, &owned->buffer);
        if (data == nullptr)
            Fail("the OpenGL copy could not be read");
    }
}

void CaptureDx9(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer, Shot shot)
{
    D3DSURFACE_DESC desc {};
    backBuffer->GetDesc(&desc);

    Image image;
    image.width = desc.Width;
    image.height = desc.Height;
    switch (desc.Format)
    {
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8R8G8B8:
        image.layout = Layout::BGRA8;
        break;
    case D3DFMT_A2R10G10B10:
        image.layout = Layout::BGR10A2;
        break;
    case D3DFMT_A2B10G10R10:
        image.layout = Layout::RGB10A2;
        break;
    default:
        return Fail(std::format("D3D9 back buffer format {} is not supported", (int) desc.Format));
    }

    // D3D9 has no copy to read later: GetRenderTargetData waits for the frame here, once.
    ComPtr<IDirect3DSurface9> source = backBuffer;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE)
    {
        ComPtr<IDirect3DSurface9> resolved;
        if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                              &resolved, nullptr)) ||
            FAILED(device->StretchRect(backBuffer, nullptr, resolved.Get(), nullptr, D3DTEXF_NONE)))
            return Fail("D3D9 multisampled back buffer could not be resolved");
        source = resolved;
    }

    ComPtr<IDirect3DSurface9> system;
    if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &system,
                                                   nullptr)) ||
        FAILED(device->GetRenderTargetData(source.Get(), system.Get())))
        return Fail("D3D9 back buffer could not be read");

    D3DLOCKED_RECT locked {};
    if (FAILED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
        return Fail("D3D9 copy could not be locked");
    CopyRows(image, static_cast<const uint8_t*>(locked.pBits), locked.Pitch);
    system->UnlockRect();

    QueueImage(std::move(image), std::move(shot));
}

std::atomic<bool> _picking = false;
std::mutex _pickedMutex;
std::optional<std::wstring> _picked;
} // namespace

void Screenshot::Request()
{
    if (_inFlight >= 3)
    {
        LOG_WARN("Screenshot skipped: {} are still being saved", _inFlight.load());
        return;
    }

    _requested = true;
}

std::filesystem::path Screenshot::DefaultFolder()
{
    // Known folders follow OneDrive and a moved Pictures folder.
    static const std::filesystem::path pictures = []
    {
        std::filesystem::path path;
        PWSTR known = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &known)))
            path = known;
        CoTaskMemFree(known);
        return path;
    }();

    if (pictures.empty())
        return Util::ExePath().parent_path() / L"screenshots";

    return pictures / L"OptiScaler" / Util::ExePath().stem();
}

std::filesystem::path Screenshot::Folder()
{
    std::filesystem::path folder = Config::Instance()->ScreenshotPath.value_or(L"");
    if (folder.empty())
        return DefaultFolder();

    return folder.is_relative() ? Util::ExePath().parent_path() / folder : folder;
}

void Screenshot::PickFolder()
{
    if (_picking.exchange(true))
        return;

    std::thread(
        [start = Folder()]
        {
            // The dialog is a COM object of its own apartment; the game's threads never wait for it.
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            ComPtr<IFileOpenDialog> dialog;
            if (SUCCEEDED(com) &&
                SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            {
                FILEOPENDIALOGOPTIONS options = 0;
                dialog->GetOptions(&options);
                dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                dialog->SetTitle(L"Screenshot folder");

                std::error_code error;
                ComPtr<IShellItem> current;
                if (std::filesystem::is_directory(start, error) &&
                    SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&current))))
                    dialog->SetFolder(current.Get());

                ComPtr<IShellItem> item;
                PWSTR path = nullptr;
                if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item)) &&
                    SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    std::lock_guard lock(_pickedMutex);
                    _picked = path;
                }
                CoTaskMemFree(path);
            }
            else
                LOG_WARN("Screenshot folder picker could not be opened");

            dialog.Reset();
            if (SUCCEEDED(com))
                CoUninitialize();
            _picking = false;
        })
        .detach();
}

bool Screenshot::Picking() { return _picking; }

std::optional<std::wstring> Screenshot::Picked()
{
    std::lock_guard lock(_pickedMutex);
    return std::exchange(_picked, std::nullopt);
}

void Screenshot::Dxgi(IDXGISwapChain* swapChain, IUnknown* device, ID3D11Texture2D* frame)
{
    DropMenu("D3D11/D3D12");

    if (!_pendingDx11.empty())
    {
        PumpDx11();
        if (!_pendingDx11.empty())
            return;
    }

    if (!_requested || swapChain == nullptr || device == nullptr)
        return;

    if (auto shot = Take())
        CaptureDxgi(swapChain, device, frame, std::move(*shot));
}

void Screenshot::DxgiMenu(IDXGISwapChain* swapChain, IUnknown* device, ID3D11Texture2D* frame)
{
    if (!_menuShot || swapChain == nullptr || device == nullptr)
        return;

    if (auto shot = TakeMenu())
        CaptureDxgi(swapChain, device, frame, std::move(*shot));
}

void Screenshot::Vulkan(const VulkanTarget& target, VkPresentInfoKHR& present)
{
    DropMenu("Vulkan");

    if (!_requested)
        return;

    if (auto shot = Take())
        CaptureVulkan(target, present, std::move(*shot));
}

void Screenshot::VulkanMenu(const VulkanTarget& target, VkPresentInfoKHR& present)
{
    if (auto shot = TakeMenu())
        CaptureVulkan(target, present, std::move(*shot));
}

bool Screenshot::GlWanted() { return _requested || !_pendingGl.empty() || _menuShot.has_value(); }

void Screenshot::Gl(uint32_t width, uint32_t height, uint32_t generation)
{
    DropMenu("OpenGL");

    auto gl = GlApi();
    if (gl == nullptr)
    {
        if (Take())
            Fail("OpenGL functions unavailable");
        _menuShot.reset();
        return;
    }

    PumpGl(*gl, generation);
    if (!_pendingGl.empty())
        return;

    if (auto shot = Take())
        CaptureGl(*gl, width, height, generation, std::move(*shot));
}

bool Screenshot::GlMenuWanted() { return _menuShot.has_value(); }

void Screenshot::GlMenu(uint32_t width, uint32_t height, uint32_t generation)
{
    auto gl = GlApi();
    if (auto shot = TakeMenu(); shot && gl != nullptr)
        CaptureGl(*gl, width, height, generation, std::move(*shot));
}

void Screenshot::Dx9(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer)
{
    DropMenu("D3D9");

    if (!_requested || device == nullptr || backBuffer == nullptr)
        return;

    if (auto shot = Take())
        CaptureDx9(device, backBuffer, std::move(*shot));
}

void Screenshot::Dx9Menu(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer)
{
    if (!_menuShot || device == nullptr || backBuffer == nullptr)
        return;

    if (auto shot = TakeMenu())
        CaptureDx9(device, backBuffer, std::move(*shot));
}
