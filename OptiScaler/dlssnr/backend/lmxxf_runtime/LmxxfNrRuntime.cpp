#include "LmxxfNrApi.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include "LmxxfProductionOptions.h"
#include "native_device_identity.h"
#include "native_game_codec.h"
#include "native_lab_paths.h"
#include "native_game_rgb_input.h"
#include "native_network_geometry.h"
#include "native_rgb_texture.h"
#include "native_temporal_feed.h"
#include "native_temporal_sample.h"
#include "hip_d3d12_bridge.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace
{
thread_local char g_lastError[256] = {};

void SetError(const char* text)
{
    if (!text)
        text = "";
    std::strncpy(g_lastError, text, sizeof(g_lastError) - 1);
    g_lastError[sizeof(g_lastError) - 1] = 0;
}

int32_t Fail(int32_t status, const char* text)
{
    SetError(text);
    return status;
}

std::string Utf8(const std::wstring& s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    if (n)
        WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}

bool IsDirectory(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring JoinPath(const std::wstring& dir, const wchar_t* name)
{
    std::wstring out = dir;
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/')
        out += L'\\';
    out += name;
    return out;
}

std::wstring DllDirectory()
{
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LmxxfNrGetApi), &mod);
    wchar_t path[MAX_PATH] {};
    if (!mod || !GetModuleFileNameW(mod, path, MAX_PATH))
        return {};
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        dir.resize(slash);
    return dir;
}

bool TryReadFitLargeFromFlagsFile(const std::wstring& path, bool* outValue)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f)
        return false;
    char line[256];
    unsigned v = 0;
    bool found = false;
    while (fgets(line, sizeof line, f))
    {
        unsigned x = 0;
        if (sscanf(line, "DLSS5_FIT_LARGE=%u", &x) == 1)
        {
            v = x;
            found = true;
        }
    }
    fclose(f);
    if (!found)
        return false;
    *outValue = (v == 1);
    return true;
}

// Upstream enables 1080p+ via DLSS5_FIT_LARGE=1 (env or native-game-flags.txt).
// Codec Supported() uses NativeFitLargeInput(); QueryCapabilities must match.
void EnsureFitLargeApplied()
{
    // Latch only after env or flags resolve. If neither is present yet, retry on later
    // Create/PrepareFrame so a late-written native-game-flags.txt still applies.
    static bool resolved = false;
    if (resolved)
        return;

    if (const char* e = std::getenv("DLSS5_FIT_LARGE"))
    {
        if (e[0] == '1' && !e[1])
            NativeFitLargeInputOverride() = true;
        else
            NativeFitLargeInputOverride() = false;
        resolved = true;
        return;
    }

    std::wstring candidates[8];
    size_t n = 0;
    auto push = [&](const std::wstring& dir)
    {
        if (dir.empty() || n + 2 > 8)
            return;
        candidates[n++] = JoinPath(dir, L"native-game-flags.txt");
        candidates[n++] = JoinPath(JoinPath(dir, L"DLSS5-AMD"), L"native-game-flags.txt");
    };
    push(DllDirectory());
    wchar_t exePath[MAX_PATH] {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH))
    {
        std::wstring dir(exePath);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
        {
            dir.resize(slash);
            push(dir);
        }
    }

    for (size_t i = 0; i < n; ++i)
    {
        bool on = false;
        if (TryReadFitLargeFromFlagsFile(candidates[i], &on))
        {
            if (on)
            {
                NativeFitLargeInputOverride() = true;
                _putenv("DLSS5_FIT_LARGE=1");
            }
            else
            {
                NativeFitLargeInputOverride() = false;
                _putenv("DLSS5_FIT_LARGE=0");
            }
            resolved = true;
            return;
        }
    }
}

std::wstring FindShaderDir(const std::wstring& assets = {})
{
    const std::wstring dll = DllDirectory();
    const std::wstring candidates[] = {
        JoinPath(assets, L"shaders"), JoinPath(dll, L"shaders"), L"shaders",
        L"third_party\\lmxxf\\shaders", // dev fallback
    };
    for (const auto& c : candidates)
    {
        if (c.empty())
            continue;
        wchar_t full[MAX_PATH] {};
        GetFullPathNameW(c.c_str(), MAX_PATH, full, nullptr);
        if (FileExists(JoinPath(full, L"native_codec_encode.hlsl")))
            return full;
    }
    return {};
}

std::wstring FindWeightsDir(const std::wstring& assets)
{
    if (FileExists(JoinPath(assets, L"block0-ffn.f16")) || FileExists(JoinPath(assets, L"block0-ffn.f32")))
        return assets;
    const std::wstring sub = JoinPath(assets, L"weights");
    if (FileExists(JoinPath(sub, L"block0-ffn.f16")) || FileExists(JoinPath(sub, L"block0-ffn.f32")))
        return sub;

    // Prioritize local folders next to DLL / game executable
    const std::wstring dll = DllDirectory();
    if (!dll.empty())
    {
        const std::wstring localTiled = JoinPath(dll, L"native-game-tiled-assets");
        if (FileExists(JoinPath(localTiled, L"block0-ffn.f16")) || FileExists(JoinPath(localTiled, L"block0-ffn.f32")))
            return localTiled;
        const std::wstring localWeights = JoinPath(dll, L"lmxxf-weights");
        if (FileExists(JoinPath(localWeights, L"block0-ffn.f16")) ||
            FileExists(JoinPath(localWeights, L"block0-ffn.f32")))
            return localWeights;
    }

    // Secondary fallback: check LMXXF_WEIGHTS_DIR environment variable (e.g. for development)
    wchar_t env[MAX_PATH] {};
    if (GetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", env, MAX_PATH) && env[0])
    {
        wchar_t full[MAX_PATH] {};
        GetFullPathNameW(env, MAX_PATH, full, nullptr);
        if (FileExists(JoinPath(full, L"block0-ffn.f16")) || FileExists(JoinPath(full, L"block0-ffn.f32")))
            return full;
    }
    return {};
}

bool ResolveModulesDir(const std::wstring& assets, std::wstring* modulesDir)
{
    if (FileExists(JoinPath(assets, L"SHA256SUMS")))
    {
        *modulesDir = assets;
        return true;
    }
    const std::wstring hip = JoinPath(assets, L"HIP");
    if (FileExists(JoinPath(hip, L"SHA256SUMS")))
    {
        *modulesDir = hip;
        return true;
    }
    const std::wstring modules = JoinPath(assets, L"modules");
    if (FileExists(JoinPath(modules, L"SHA256SUMS")))
    {
        *modulesDir = modules;
        return true;
    }
    const std::wstring nested = JoinPath(JoinPath(assets, L"native-game-tiled-assets"), L"HIP");
    if (FileExists(JoinPath(nested, L"SHA256SUMS")))
    {
        *modulesDir = nested;
        return true;
    }
    return false;
}

int32_t ValidateModuleSet(const std::wstring& modulesDir, uint32_t* outCount)
{
    *outCount = 0;
    const std::wstring sumsPath = JoinPath(modulesDir, L"SHA256SUMS");
    HANDLE file = CreateFileW(sumsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return Fail(LMXXF_NR_UNAVAILABLE, "Create: SHA256SUMS missing in modules directory");
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1 << 20)
    {
        CloseHandle(file);
        return Fail(LMXXF_NR_UNAVAILABLE, "Create: SHA256SUMS unreadable");
    }
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr))
    {
        CloseHandle(file);
        return Fail(LMXXF_NR_UNAVAILABLE, "Create: SHA256SUMS read failed");
    }
    CloseHandle(file);
    text.resize(read);

    uint32_t found = 0;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos)
            continue;
        size_t nameStart = line.find_first_not_of(" \t", sp);
        if (nameStart == std::string::npos)
            continue;
        std::string name = line.substr(nameStart);
        if (name.size() < 7 || name.rfind(".hsaco") != name.size() - 6)
            continue;
        std::wstring wname(name.begin(), name.end());
        const std::wstring full = JoinPath(modulesDir, wname.c_str());
        if (!IsDirectory(full) && GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES)
            ++found;
        else
            return Fail(LMXXF_NR_UNAVAILABLE, "Create: hsaco listed in SHA256SUMS is missing");
    }
    if (found == 0)
        return Fail(LMXXF_NR_UNAVAILABLE, "Create: no .hsaco entries in SHA256SUMS");
    if (found < 24)
        return Fail(LMXXF_NR_UNAVAILABLE, "Create: fewer than 24 hsaco modules; host/module set incomplete");
    *outCount = found;
    return static_cast<int32_t>(LMXXF_NR_OK);
}

bool LooksLikeObject(void* p)
{
    if (!p)
        return false;
    MEMORY_BASIC_INFORMATION info {};
    if (!VirtualQuery(p, &info, sizeof info))
        return false;
    if (!(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        return false;
    return info.State == MEM_COMMIT;
}

// The HIP architecture of the device behind this D3D12 device (gfx1201: RX 9070 series, gfx1200: RX 9060 series), or
// empty when HIP cannot say. The kernels are built per architecture.
std::string HipArchitecture(ID3D12Device* device)
{
    try
    {
        hip_probe::Api probe(7);
        int count = 0;
        if (probe.hipInit(0) || probe.hipGetDeviceCount(&count))
            return {};
        const LUID wanted = device->GetAdapterLuid();
        for (int i = 0; i < count; ++i)
        {
            const auto props = probe.Properties(i);
            if (std::memcmp(props.luid, &wanted, sizeof wanted))
                continue;
            std::string arch(props.gcnArchName, strnlen(props.gcnArchName, sizeof props.gcnArchName));
            return arch.substr(0, arch.find(':'));
        }
    }
    catch (...)
    {
    }
    return {};
}

// Production options, with each newer kernel group switched off when its module is not in the folder (an older
// install), so the network keeps running on the kernels before it instead of failing to load.
hip_reference::Options RuntimeOptions(unsigned w, unsigned h, const std::wstring& modulesDir,
                                      const std::wstring& weightsDir, uint32_t style)
{
    // The network reads DLSS5_STYLE as it is built and writes it into the kernels it loads (Style/128 in the
    // preprocess; 1 keeps their built-in value).
    _putenv_s("DLSS5_STYLE", style == 0 ? "0" : style == 2 ? "2" : "1");
    // Upstream 0.41's default: the lossy fast-numeric twins of the window, ViT and deep kernels (about 53 dB from the exact
    // ones, closer to NVIDIA with all blocks). A missing *-fast module falls back to the normal one.
    if (!std::getenv("DLSS5_FAST_NUMERIC"))
        _putenv("DLSS5_FAST_NUMERIC=1");
    auto opt = LmxxfProductionOptions(w, h, Utf8(modulesDir), Utf8(weightsDir));
    auto has = [&](const wchar_t* name) { return FileExists(JoinPath(modulesDir, name)); };
    if (!(has(L"c32-wave1.hsaco") && has(L"c64-wave2.hsaco")))
        opt.wave_owned = false;
    if (!(has(L"c512-m32-mh.hsaco") && has(L"c512-m32-deep.hsaco")))
        opt.c512_m32 = false;
    if (!has(L"vit-wide-deep.hsaco"))
        opt.vit_proj_n64 = false;
    if (!has(L"vit-stream.hsaco"))
        opt.vit_stream = 0;
    if (!has(L"swin-persistent.hsaco"))
        opt.swin_run = false;
    char note[192];
    std::snprintf(note, sizeof note,
                  "lmxxf: kernels wave_owned=%u c512_m32=%u vit_proj_n64=%u vit_stream=%u swin_run=%u pdl=%u\n",
                  unsigned(hip_reference::WaveOwnedCompatible(opt)), unsigned(hip_reference::C512M32Compatible(opt)),
                  unsigned(hip_reference::VitProjN64Compatible(opt)),
                  hip_reference::VitStreamCompatible(opt) ? opt.vit_stream : 0u,
                  unsigned(hip_reference::SwinRunCompatible(opt)), unsigned(opt.pdl));
    OutputDebugStringA(note);
    opt.small_geometry = NativeCurrentNetworkGeometry().small_model;
    return opt;
}

// Upstream 0.44's Small model: its weights in <weights>\small (model-manifest.json and the 93 files, checked by size)
// and the kernels it runs on, which an older module set lacks.
bool SmallModelUsable(const std::wstring& weightsDir, const std::wstring& modulesDir)
{
    auto has = [&](const wchar_t* name) { return FileExists(JoinPath(modulesDir, name)); };
    return !weightsDir.empty() && has(L"c32-wave1.hsaco") && has(L"c64-wave2.hsaco") && has(L"c512-m32-mh.hsaco") &&
           has(L"c512-m32-deep.hsaco") && has(L"vit-wide-deep.hsaco") &&
           hip_reference::SmallAssetsAvailable(Utf8(JoinPath(weightsDir, L"small")));
}

struct Job
{
    uint32_t state = LMXXF_NR_JOB_NONE;
    ID3D12Resource* color = nullptr;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    UINT width = 0, height = 0;
    uint32_t seed = 1;
    float transfer_strength = 1.0f;
    float color_strength = 1.0f;
    uint32_t debug_view = 0;
    uint32_t passes = 1;
    bool passEstimate = false, skinProtect = false; // LMXXF_NR_FRAME_FLAG_PASS3_ESTIMATE, _SKIN_PROTECT
    bool codec_passthrough = false;
    bool autoWhite = false; // LMXXF_NR_FRAME_FLAG_AUTO_WHITE
    bool firstPass = false; // LMXXF_NR_FRAME_FLAG_FIRST_PASS with two or more passes
    bool temporal = false;
    UINT histories = 0; // bit k: pass k+1 was given a history this frame
    float smoothThreshold = 0, smoothStrength = 0;
    bool smoothResidual = false, residualHistory = false;
    ID3D12Resource* motion = nullptr;
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    UINT motionWidth = 0, motionHeight = 0;
    float motionScaleX = 0, motionScaleY = 0;
};

// Upstream's NativeOutputSmooth (native_game_frame.h, DLSS5_OUTPUT_SMOOTH), with buffers and parameters given per
// frame, and the same blend on the network's change to its input (residual_main).
class OutputSmooth
{
    ID3D12RootSignature *root = nullptr, *residualRoot = nullptr;
    ID3D12PipelineState *pso = nullptr, *residualPso = nullptr;

  public:
    OutputSmooth() = default;
    OutputSmooth(const OutputSmooth&) = delete;
    ~OutputSmooth()
    {
        for (auto* r : { root, residualRoot })
            if (r)
                r->Release();
        for (auto* p : { pso, residualPso })
            if (p)
                p->Release();
    }
    // validWidth: the colour's row width, when the network's (processingWidth) is wider (the Small model's geometry).
    void Create(ID3D12Device* d, const std::wstring& dir, bool residual = false, UINT processingWidth = 0,
                UINT validWidth = 0)
    {
        ID3D12RootSignature*& root = residual ? residualRoot : this->root;
        ID3D12PipelineState*& pso = residual ? residualPso : this->pso;
        D3D12_ROOT_PARAMETER p[5] {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[2].Constants = { 0, 0, 4 };
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[3].Descriptor.ShaderRegister = 1;
        p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[4].Descriptor.ShaderRegister = 1;
        D3D12_ROOT_SIGNATURE_DESC rd {};
        rd.NumParameters = residual ? 5 : 3;
        rd.pParameters = p;
        ID3DBlob *blob = nullptr, *error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (error)
            error->Release();
        if (SUCCEEDED(hr))
            hr = d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
        if (blob)
            blob->Release();
        if (FAILED(hr))
            throw std::runtime_error("output smooth root");
        blob = error = nullptr;
        const std::string pw = std::to_string(processingWidth), vw = std::to_string(validWidth);
        const D3D_SHADER_MACRO macros[] = { { "SMOOTH_RESIDUAL", "1" },
                                            { processingWidth != validWidth ? "NATIVE_RGB_PROCESSING_WIDTH" : nullptr,
                                              pw.c_str() },
                                            { "NATIVE_VALID_WIDTH", vw.c_str() },
                                            { nullptr, nullptr } };
        hr = CompileNativeShader(dir + L"\\native_output_smooth.hlsl", residual ? macros : nullptr,
                                 residual ? "residual_main" : "main", &blob, &error);
        if (FAILED(hr))
        {
            const std::string m =
                error ? std::string(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize())
                      : "output smooth compilation";
            if (error)
                error->Release();
            throw std::runtime_error(m);
        }
        if (error)
            error->Release();
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root;
        pd.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        hr = NativeCreateComputePipelineState(d, &pd, IID_PPV_ARGS(&pso));
        blob->Release();
        if (FAILED(hr))
            throw std::runtime_error("output smooth pipeline");
    }
    // In place on rgb, the network's RGB output, which is readable before and after; warped must be readable.
    void Record(ID3D12GraphicsCommandList* c, ID3D12Resource* rgb, ID3D12Resource* warped, float threshold,
                float strength, UINT pixels)
    {
        UINT words[4] { 0, 0, pixels, 0 };
        std::memcpy(words, &threshold, 4);
        std::memcpy(words + 1, &strength, 4);
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { rgb, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
        c->ResourceBarrier(1, &b);
        c->SetComputeRootSignature(root);
        c->SetPipelineState(pso);
        c->SetComputeRootShaderResourceView(0, warped->GetGPUVirtualAddress());
        c->SetComputeRootUnorderedAccessView(1, rgb->GetGPUVirtualAddress());
        c->SetComputeRoot32BitConstants(2, 4, words, 0);
        c->Dispatch((pixels + 63) / 64, 1, 1);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        c->ResourceBarrier(1, &b);
    }
    // The same blend on rgb minus base (the network's input), toward warped (the previous frame's difference, read
    // only with history). This frame's difference goes to residual, which is readable between frames once written.
    void RecordResidual(ID3D12GraphicsCommandList* c, ID3D12Resource* rgb, ID3D12Resource* warped, ID3D12Resource* base,
                        ID3D12Resource* residual, bool written, float threshold, float strength, UINT pixels,
                        bool history)
    {
        UINT words[4] { 0, 0, pixels, history ? 1u : 0u };
        std::memcpy(words, &threshold, 4);
        std::memcpy(words + 1, &strength, 4);
        D3D12_RESOURCE_BARRIER b[2] {};
        for (UINT i = 0; i < 2; ++i)
        {
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[i].Transition = { i ? residual : rgb, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
        }
        c->ResourceBarrier(written ? 2 : 1, b);
        c->SetComputeRootSignature(residualRoot);
        c->SetPipelineState(residualPso);
        c->SetComputeRootShaderResourceView(0, (history ? warped : base)->GetGPUVirtualAddress());
        c->SetComputeRootUnorderedAccessView(1, rgb->GetGPUVirtualAddress());
        c->SetComputeRoot32BitConstants(2, 4, words, 0);
        c->SetComputeRootShaderResourceView(3, base->GetGPUVirtualAddress());
        c->SetComputeRootUnorderedAccessView(4, residual->GetGPUVirtualAddress());
        c->Dispatch((pixels + 63) / 64, 1, 1);
        for (auto& t : b)
            std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
        c->ResourceBarrier(2, b);
    }
};

// Per-pass history, as upstream's native_game_frame.h builds it for one pass: the game's motion vectors become
// coordinates, and each pass's output from the previous frame is resampled at them before that pass reads it.
struct TemporalChain
{
    NativeTemporalFeed feeds[3]; // feeds[0] also converts the motion vectors; the others only hold history
    NativeTemporalCoordinates coordinates;
    NativeTemporalSample samplers[3];
    OutputSmooth smooth; // on the last pass's output, toward its warped history
    // With SMOOTH_RESIDUAL: the last output minus the network's input, kept for the next frame and warped like the
    // histories. Created on the first frame that asks for it.
    ID3D12Resource* residual = nullptr;
    NativeTemporalSample residualSampler;
    bool residualWritten = false, residualValid = false;
    UINT passesReady = 0;
    bool valid[3] {}; // pass k's history holds its output from the previous frame
    UINT motionTextureWidth = 0, motionTextureHeight = 0, motionWidth = 0, motionHeight = 0;
    float scaleX = 0, scaleY = 0;
    ~TemporalChain()
    {
        // NativeCreateCommittedResource keeps the big buffers in its residency list with a reference of its own, which
        // the vendored temporal classes never give back: every chain made again (a new colour size, a model switch)
        // left its samplers' 35 MB behind.
        for (auto& f : feeds)
        {
            NativeUntrackResource(f.Motion());
            NativeUntrackResource(f.History());
        }
        NativeUntrackResource(coordinates.Output());
        for (auto& s : samplers)
            NativeUntrackResource(s.Output());
        NativeUntrackResource(residualSampler.Output());
        NativeUntrackResource(residual);
        if (residual)
            residual->Release();
    }
    void Invalidate()
    {
        for (auto& v : valid)
            v = false;
        residualValid = false;
    }
};

// LMXXF_NR_FRAME_FLAG_AUTO_WHITE: the frame's brightness, measured on the GPU into a 1x1 R32_FLOAT that lmxxf's codec
// reads as the game's exposure (NATIVE_CODEC_EXPOSURE), so the codec itself is unchanged. Its own estimate (25 points,
// every frame, toward mid-grey both ways) made a dark scene clean but pulsed with the camera and dimmed bright scenes
// (KCD2). This is a log average of 16x16 points of the colour, so one light barely moves it, eased over about half a
// second, and only ever a gain. A log average sits well below the plain mean, so a lit outdoor scene measures a stop
// or two under mid-grey; the gain in stops is the distance below lmxxf's mid-grey through a soft knee: 1x (as with the
// option off) down to 1 stop below, the full distance from 3.5 stops below, at most 3 stops (8x).
static const char kAutoExposureHlsl[] = R"(
Texture2D<float4> Colour : register(t0);
RWTexture2D<float> Exposure : register(u0);
cbuffer Constants : register(b0) { uint2 Size; float Blend; uint Reset; };
groupshared float logs[256];
[numthreads(16, 16, 1)]
void main(uint3 t : SV_GroupThreadID)
{
    const uint i = t.y * 16 + t.x;
    const uint2 p = min(uint2((float2(t.xy) + 0.5) * float2(Size) / 16.0), Size - 1);
    const float3 c = max(Colour.Load(int3(p, 0)).rgb, 0.0);
    logs[i] = log2(max(dot(c, float3(0.2126, 0.7152, 0.0722)), 1e-5));
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 128; step > 0; step >>= 1)
    {
        if (i < step)
            logs[i] += logs[i + step];
        GroupMemoryBarrierWithGroupSync();
    }
    if (i == 0)
    {
        // lmxxf's white point for a mean is mean / 0.2086 (WhitePointForMean: mid-grey 0.45 encoded); as a gain on
        // the colour, 0.2086 / mean, here in stops.
        const float below = log2(0.2086) - logs[0] / 256.0;
        const float target = exp2(clamp(below * smoothstep(1.0, 3.5, below), 0.0, 3.0));
        const float previous = Exposure[uint2(0, 0)];
        Exposure[uint2(0, 0)] = Reset != 0 || !(previous >= 1.0 && previous <= 8.0)
            ? target : exp2(lerp(log2(previous), log2(target), Blend));
    }
}
)";

struct AutoExposure
{
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* value = nullptr; // the exposure, kept in UNORDERED_ACCESS between frames
    UINT increment = 0;
    UINT slot = 0;           // descriptors rotate over kSlots pairs, so a frame still in flight keeps its own
    LARGE_INTEGER last {};   // the last measurement, for the easing
    static constexpr UINT kSlots = 8;

    void Create(ID3D12Device* d)
    {
        auto check = [](HRESULT hr, const char* what)
        {
            if (FAILED(hr))
                throw std::runtime_error(std::string("auto exposure: ") + what);
        };
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT compiled = D3DCompile(kAutoExposureHlsl, sizeof kAutoExposureHlsl - 1, "auto_exposure", nullptr,
                                            nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                                            &errors);
        if (errors)
            errors->Release();
        check(compiled, "shader compile");
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 };
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 4 };
        D3D12_ROOT_SIGNATURE_DESC rd { 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob* sig = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, nullptr);
        if (SUCCEEDED(hr))
            hr = d->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root));
        if (sig)
            sig->Release();
        if (FAILED(hr))
            code->Release();
        check(hr, "root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root;
        pd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        hr = d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        code->Release();
        check(hr, "pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * 2,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "descriptor heap");
        increment = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = td.Height = 1;
        td.DepthOrArraySize = td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                         IID_PPV_ARGS(&value)),
              "texture");
    }

    // Measures the colour's top-left w x h, in `state`, into value (left in UNORDERED_ACCESS).
    void Record(ID3D12GraphicsCommandList* c, ID3D12Device* d, ID3D12Resource* colour, D3D12_RESOURCE_STATES state,
                UINT w, UINT h)
    {
        LARGE_INTEGER now {}, frequency {};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const double dt = last.QuadPart ? double(now.QuadPart - last.QuadPart) / double(frequency.QuadPart) : 1e9;
        last = now;
        // A gap of more than a second (a load, the menu) takes the new value at once.
        const bool reset = dt > 1.0;
        const float blend = reset ? 1.0f : float(1.0 - std::exp(-std::max(dt, 0.0) / 0.5));

        const DXGI_FORMAT f = colour->GetDesc().Format;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv {};
        sv.Format = NativeIsGameColor(f) || f == DXGI_FORMAT_R9G9B9E5_SHAREDEXP || f == DXGI_FORMAT_R16G16B16A16_FLOAT
                        ? NativeViewFormat(f)
                    : NativeFallbackColorView(f) != DXGI_FORMAT_UNKNOWN ? NativeFallbackColorView(f)
                                                                        : NativeViewFormat(f);
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv {};
        uv.Format = DXGI_FORMAT_R32_FLOAT;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * 2 * increment;
        gpu.ptr += UINT64(slot) * 2 * increment;
        slot = (slot + 1) % kSlots;
        d->CreateShaderResourceView(colour, &sv, cpu);
        cpu.ptr += increment;
        d->CreateUnorderedAccessView(value, nullptr, &uv, cpu);

        const bool readable = (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { colour, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        if (!readable)
            c->ResourceBarrier(1, &b);
        c->SetDescriptorHeaps(1, &heap);
        c->SetComputeRootSignature(root);
        c->SetPipelineState(pso);
        c->SetComputeRootDescriptorTable(0, gpu);
        const UINT constants[4] = { std::max(w, 1u), std::max(h, 1u), 0, reset ? 1u : 0u };
        c->SetComputeRoot32BitConstants(1, 4, constants, 0);
        c->SetComputeRoot32BitConstants(1, 1, &blend, 2);
        c->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER after[2] {};
        after[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        after[0].UAV.pResource = value;
        UINT count = 1;
        if (!readable)
        {
            after[1] = b;
            std::swap(after[1].Transition.StateBefore, after[1].Transition.StateAfter);
            count = 2;
        }
        c->ResourceBarrier(count, after);
    }

    ~AutoExposure()
    {
        if (value)
            value->Release();
        if (heap)
            heap->Release();
        if (pso)
            pso->Release();
        if (root)
            root->Release();
    }
};

struct Session
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandQueue* fallbackConsumerQueue = nullptr;
    std::wstring assetsDir;
    std::wstring modulesDir;
    std::wstring weightsDir;
    std::wstring shaderDir;
    uint32_t hsacoCount = 0;
    bool modulesValidated = false;
    bool hipPrepared = false;
    bool queueBound = false;
    // The Style the network was built with (LMXXF_NR_FRAME_FLAG_STYLE); another rebuilds it.
    uint32_t builtStyle = 1;
    // The network size it was built for: the 1080 tier's rows can change it without the colour changing.
    unsigned builtProcW = 0, builtProcH = 0;
    // Upstream 0.44's Small model: whether this install has it (checked once), and whether the network runs it.
    bool smallChecked = false, smallReady = false, builtSmall = false;
    bool zeroOutputFallback = false;
    bool failed = false; /* Fail-closed poisoning */
    hip_reference::D3D12Bridge* bridge = nullptr;
    NativeGameCodec* encode = nullptr;
    // The codec was made reading autoExposure's value (LMXXF_NR_FRAME_FLAG_AUTO_WHITE); switching it makes it again.
    bool codecAutoWhite = false;
    AutoExposure* autoExposure = nullptr;
    NativeGameRgbInput* rgbInput = nullptr;
    NativeRgbTexture* rgbTex = nullptr;
    NativeGameCodec* decode = nullptr;
    ID3D12Resource* decodeDisplay = nullptr;
    // LMXXF_NR_FRAME_FLAG_FIRST_PASS: pass 1's output (the bridge's PassOutput(0), firstSource) through a codec of
    // its own, made on the first frame that keeps it and gone with the codec.
    NativeRgbTexture* firstTex = nullptr;
    NativeGameCodec* firstDecode = nullptr;
    ID3D12Resource* firstDisplay = nullptr;
    ID3D12Resource* firstSource = nullptr; // compared only
    bool firstFailed = false;              // its codec could not be made: no first pass in this session
    TemporalChain* temporal = nullptr;
    std::string temporalError; // why the chain could not be built; frames then run without history
    uint64_t lastFrameId = 0;
    ULONGLONG lastFrameTick = 0;
    Job job {};
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
    // The largest colour seen at one format and aspect, which sizes the network (see PrepareFrame).
    UINT drsWidth = 0, drsHeight = 0;
    DXGI_FORMAT drsFormat = DXGI_FORMAT_UNKNOWN;
    // A colour smaller than drsWidth x drsHeight came: free resolution is off for this geometry.
    bool drsStepped = false;

    void TeardownCodecChain()
    {
        TeardownCodec();
        delete bridge;
        bridge = nullptr;
        hipPrepared = false;
    }

    // The codec, its history and the bridge's in-flight work, keeping the network: what a new colour size needs when
    // the network's geometry stays (a game's dynamic resolution).
    void TeardownCodec()
    {
        if (bridge)
        {
            try
            {
                bridge->CancelUnsubmitted();
                bridge->NotifyOutputSubmittedIfRecorded(queue);
            }
            catch (...)
            {
                AbandonSessionResources();
                throw std::runtime_error("TeardownCodecChain: bridge acknowledgement failed");
            }
            if (!bridge->WaitForSubmittedWork())
            {
                AbandonSessionResources();
                throw std::runtime_error("TeardownCodecChain: bridge work did not complete");
            }
        }
        delete temporal;
        temporal = nullptr;
        TeardownFirstPass();
        if (decodeDisplay)
        {
            decodeDisplay->Release();
            decodeDisplay = nullptr;
        }
        delete decode;
        decode = nullptr;
        delete rgbTex;
        rgbTex = nullptr;
        delete rgbInput;
        rgbInput = nullptr;
        delete encode;
        encode = nullptr;
        colorFormat = DXGI_FORMAT_UNKNOWN;
    }

    // The first pass's codec; only once the GPU is done with it, as the rest of the codec.
    void TeardownFirstPass()
    {
        if (firstDisplay)
            firstDisplay->Release();
        firstDisplay = nullptr;
        delete firstDecode;
        firstDecode = nullptr;
        delete firstTex;
        firstTex = nullptr;
        firstSource = nullptr;
    }

    // Wait until queue work that may touch encode/rgb/bridge shared resources is done.
    // Returns S_OK only when completion is confirmed; callers must retain resources on failure.
    HRESULT DrainQueue(ID3D12CommandQueue* target)
    {
        if (!device || !target)
            return S_OK;
        if (FAILED(device->GetDeviceRemovedReason()))
            return DXGI_ERROR_DEVICE_REMOVED;
        ID3D12Fence* fence = nullptr;
        HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr) || !fence)
            return FAILED(hr) ? hr : E_FAIL;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ev)
        {
            const DWORD err = GetLastError();
            fence->Release();
            return HRESULT_FROM_WIN32(err ? err : ERROR_OUTOFMEMORY);
        }
        const UINT64 v = 1;
        hr = target->Signal(fence, v);
        if (FAILED(hr))
        {
            CloseHandle(ev);
            fence->Release();
            return hr;
        }
        hr = fence->SetEventOnCompletion(v, ev);
        if (FAILED(hr))
        {
            CloseHandle(ev);
            fence->Release();
            return hr;
        }
        const DWORD wr = WaitForSingleObject(ev, 30000);
        const UINT64 completed = fence->GetCompletedValue();
        // A timeout can leave SetEventOnCompletion armed. Retain the event and
        // fence until process exit instead of closing a future signal target.
        if (wr == WAIT_OBJECT_0 && completed >= v)
        {
            CloseHandle(ev);
            fence->Release();
        }
        if (wr != WAIT_OBJECT_0 || completed < v)
            return wr == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : E_FAIL;
        return S_OK;
    }

    HRESULT DrainGpu()
    {
        const HRESULT sessionHr = DrainQueue(queue);
        if (FAILED(sessionHr))
            return sessionHr;
        if (!fallbackConsumerQueue)
            return S_OK;
        const HRESULT consumerHr = DrainQueue(fallbackConsumerQueue);
        if (SUCCEEDED(consumerHr))
        {
            fallbackConsumerQueue->Release();
            fallbackConsumerQueue = nullptr;
        }
        return consumerHr;
    }

    void AbandonSessionResources()
    {
        // Fail-closed intentional leak: GPU may still reference the whole chain.
        bridge = nullptr;
        temporal = nullptr;
        decodeDisplay = nullptr;
        decode = nullptr;
        rgbTex = nullptr;
        firstDisplay = nullptr;
        firstDecode = nullptr;
        firstTex = nullptr;
        rgbInput = nullptr;
        encode = nullptr;
        autoExposure = nullptr;
        // The queue may still own GPU work. Keep our reference on fail-closed teardown.
        fallbackConsumerQueue = nullptr;
        if (queue)
        {
            queue->Release();
            queue = nullptr;
        }
        if (device)
        {
            device->Release();
            device = nullptr;
        }
    }

    ~Session()
    {
        // Fail-closed safe teardown:
        // If already poisoned or GPU drain fails or device lost, intentionally leak rather
        // than freeing memory still touched by GPU (prevents hard crash/BSOD).
        if (failed || FAILED(DrainGpu()))
        {
            AbandonSessionResources();
            return;
        }
        if (bridge)
        {
            try
            {
                bridge->CancelUnsubmitted();
                bridge->NotifyOutputSubmittedIfRecorded(queue);
            }
            catch (...)
            {
                AbandonSessionResources();
                return;
            }
            if (!bridge->WaitForSubmittedWork())
            {
                AbandonSessionResources();
                return;
            }
        }
        // Bridge dtor also synchronizes HIP / pending fence, then frees shared buffers.
        delete bridge;
        bridge = nullptr;
        delete temporal;
        temporal = nullptr;
        TeardownFirstPass();
        if (decodeDisplay)
        {
            decodeDisplay->Release();
            decodeDisplay = nullptr;
        }
        delete decode;
        decode = nullptr;
        delete rgbTex;
        rgbTex = nullptr;
        delete rgbInput;
        rgbInput = nullptr;
        delete encode;
        encode = nullptr;
        delete autoExposure;
        autoExposure = nullptr;
        if (queue)
            queue->Release();
        queue = nullptr;
        if (device)
            device->Release();
        device = nullptr;
    }
};

void RequireSession(Session* s)
{
    if (!s)
        throw std::runtime_error("session is null");
    if (s->failed)
        throw std::runtime_error("session is poisoned due to previous fatal error");
}

void ListContract(Session* s, ID3D12GraphicsCommandList* c)
{
    if (!c)
        throw std::runtime_error("command list is null");
    if (s->queue && c->GetType() != s->queue->GetDesc().Type)
        throw std::runtime_error("command list type mismatch with session queue");
    ID3D12Device* owner = nullptr;
    HRESULT hr = c->GetDevice(IID_PPV_ARGS(&owner));
    if (FAILED(hr) || !owner)
        throw std::runtime_error("failed to query command list device");
    bool same = NativeSameDevice(owner, s->device);
    owner->Release();
    if (!same)
        throw std::runtime_error("command list device mismatch with session device");
}

void QueueContract(Session* s, ID3D12CommandQueue* q)
{
    if (!q)
        throw std::runtime_error("command queue is null");
    if (s->device)
    {
        ID3D12Device* owner = nullptr;
        HRESULT hr = q->GetDevice(IID_PPV_ARGS(&owner));
        if (FAILED(hr) || !owner)
            throw std::runtime_error("failed to query command queue device");
        bool same = NativeSameDevice(owner, s->device);
        owner->Release();
        if (!same)
            throw std::runtime_error("command queue device mismatch with session device");
    }
    if (s->queue && !NativeSameDevice(q, s->queue))
        throw std::runtime_error("command queue does not match session queue");
}

// Builds or rebuilds the chain for this frame's motion vectors and creates the passes it lacks. Geometry and transform
// follow upstream's native_game_frame.h, with the motion extent in the place of its render size.
void EnsureTemporal(Session* s, const Job& j)
{
    const D3D12_RESOURCE_DESC md = j.motion->GetDesc();
    if (md.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        throw std::runtime_error("motion vectors are not a 2D texture");
    const UINT gridW = j.motionWidth ? j.motionWidth : j.width, gridH = j.motionHeight ? j.motionHeight : j.height;
    TemporalChain* t = s->temporal;
    if (t &&
        (t->motionTextureWidth != UINT(md.Width) || t->motionTextureHeight != md.Height || t->motionWidth != gridW ||
         t->motionHeight != gridH || t->scaleX != j.motionScaleX || t->scaleY != j.motionScaleY))
    {
        if (FAILED(s->DrainGpu()))
            throw std::runtime_error("temporal rebuild: GPU drain failed");
        delete t;
        s->temporal = t = nullptr;
    }
    if (!_wgetenv(L"DLSS5_FAST_TEMPORAL"))
        _wputenv(L"DLSS5_FAST_TEMPORAL=1");
    const auto fit = s->encode->Geometry();
    const auto ng = NativeCurrentNetworkGeometry();
    const bool fresh = !t;
    if (fresh)
        t = new TemporalChain();
    try
    {
        if (fresh)
        {
            // Raster value * scale = pixels of the motion extent; * fit / extent = pixels of the network surface.
            const float sx = j.motionScaleX * float(fit.fit_width) / float(gridW);
            const float sy = j.motionScaleY * float(fit.fit_height) / float(gridH);
            t->feeds[0].Create(s->device, UINT(md.Width), md.Height, sx, sy, s->shaderDir);
            const float rx = float(gridW) / float(fit.fit_width), ry = float(gridH) / float(fit.fit_height);
            const float transform[6] = { -float(fit.x) * rx,
                                         -float(fit.y) * ry,
                                         fit.Adapted() ? float(ng.valid_width) * rx : float(gridW),
                                         fit.Adapted() ? float(ng.valid_height) * ry : float(gridH),
                                         1.f / float(ng.valid_width),
                                         1.f / float(ng.valid_height) };
            const float viewport[4] = { float(fit.x), float(fit.y), float(fit.fit_width), float(fit.fit_height) };
            t->coordinates.Create(s->device, t->feeds[0].Motion(), ng.valid_width, ng.valid_height, ng.processing_width,
                                  ng.processing_height, UINT(md.Width), md.Height, transform, s->shaderDir, true,
                                  fit.Adapted() ? viewport : nullptr);
            t->smooth.Create(s->device, s->shaderDir);
            t->motionTextureWidth = UINT(md.Width);
            t->motionTextureHeight = md.Height;
            t->motionWidth = gridW;
            t->motionHeight = gridH;
            t->scaleX = j.motionScaleX;
            t->scaleY = j.motionScaleY;
        }
        for (; t->passesReady < j.passes; ++t->passesReady)
        {
            const UINT k = t->passesReady;
            if (k)
                t->feeds[k].Create(s->device, 1, 1, 0.f, 0.f, s->shaderDir); // history only, motion never recorded
            t->samplers[k].Create(s->device, t->feeds[k].History(), t->coordinates.Output(), ng.valid_width,
                                  ng.valid_height, ng.processing_width * ng.processing_height, s->shaderDir, true);
        }
        if (j.smoothResidual && !t->residual)
        {
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = UINT64(ng.valid_width) * ng.valid_height * 16;
            rd.Height = 1;
            rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(NativeCreateCommittedResource(s->device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                     IID_PPV_ARGS(&t->residual))))
                throw std::runtime_error("residual buffer");
            t->residualSampler.Create(s->device, t->residual, t->coordinates.Output(), ng.valid_width, ng.valid_height,
                                      ng.processing_width * ng.processing_height, s->shaderDir, true);
            t->smooth.Create(s->device, s->shaderDir, true, ng.processing_width, ng.valid_width);
        }
    }
    catch (...)
    {
        // A fresh chain never reached the GPU. A live one may still be read by it, so it is leaked, as the
        // session's fail-closed teardown does.
        if (fresh)
            delete t;
        s->temporal = nullptr;
        throw;
    }
    s->temporal = t;
}

template <class Fn> int32_t Guard(Fn&& fn)
{
    try
    {
        return fn();
    }
    catch (const std::exception& ex)
    {
        return Fail(LMXXF_NR_FAILED, ex.what());
    }
    catch (...)
    {
        return Fail(LMXXF_NR_FAILED, "unhandled exception");
    }
}

template <class Fn> int32_t GuardSession(Session* s, Fn&& fn)
{
    if (s && s->failed)
        return Fail(LMXXF_NR_UNAVAILABLE, "session is poisoned due to previous fatal error");
    try
    {
        return fn();
    }
    catch (const std::exception& ex)
    {
        if (s)
            s->failed = true;
        return Fail(LMXXF_NR_FAILED, ex.what());
    }
    catch (...)
    {
        if (s)
            s->failed = true;
        return Fail(LMXXF_NR_FAILED, "unhandled exception; session poisoned");
    }
}

int32_t QueryCapabilities(LmxxfNrCapabilities* out)
{
    return Guard(
        [&]
        {
            if (!out)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "QueryCapabilities: null out");
            if (out->struct_size != sizeof(LmxxfNrCapabilities))
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "QueryCapabilities: struct_size mismatch");
            out->abi_version = LMXXF_NR_ABI_VERSION;
            EnsureFitLargeApplied();
            // Without FIT_LARGE: native 1080p admit only. With it: NativeInputGeometry::Supported(..., large) ceiling.
            if (NativeFitLargeInput())
            {
                out->max_input_width = 16384;
                out->max_input_height = 16384;
            }
            else
            {
                out->max_input_width = 1920;
                out->max_input_height = 1080;
            }
            out->history_supported = 0;
            out->overlap_supported = 0;
            out->graph_supported = 0;
            out->hip_ready = 0;
            out->gfx1201_target = 1;
            SetError("");
            return static_cast<int32_t>(LMXXF_NR_OK);
        });
}

int32_t Create(const LmxxfNrCreateInfo* info, void** context)
{
    return Guard(
        [&]
        {
            if (!info || !context)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: null info or context");
            *context = nullptr;
            if (info->struct_size != sizeof(LmxxfNrCreateInfo))
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: struct_size mismatch");
            if (info->flags & ~LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: unknown flags");
            if (!info->device || !info->queue)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: device and queue required");
            if (!info->assets_directory || !info->assets_directory[0])
                return Fail(LMXXF_NR_INVALID_ARGUMENT,
                            "Create: assets_directory required (modules dir from 68dc099 build)");

            EnsureFitLargeApplied();
            std::wstring assets = info->assets_directory;
            if (!IsDirectory(assets))
                return Fail(LMXXF_NR_UNAVAILABLE, "Create: assets_directory is not a directory");
            std::wstring modulesDir;
            if (!ResolveModulesDir(assets, &modulesDir))
                return Fail(LMXXF_NR_UNAVAILABLE,
                            "Create: modules directory needs SHA256SUMS + hsaco (or HIP/ under assets)");
            // The RX 9060 series' kernels sit beside the RX 9070 ones, in <modules>-gfx1200 (installers only write
            // one folder level).
            if (LooksLikeObject(info->device))
            {
                const std::string arch = HipArchitecture(static_cast<ID3D12Device*>(info->device));
                if (arch == "gfx1200")
                {
                    const std::wstring own = modulesDir + L"-gfx1200";
                    if (!FileExists(JoinPath(own, L"SHA256SUMS")))
                        return Fail(LMXXF_NR_UNAVAILABLE,
                                    "Create: this GPU (gfx1200, RX 9060 series) needs the lmxxf-modules-gfx1200 kernels");
                    modulesDir = own;
                }
                else if (!arch.empty() && arch != "gfx1201")
                {
                    static char why[160];
                    std::snprintf(why, sizeof why,
                                  "Create: lmxxf runs on RX 9070 and 9060 series GPUs (gfx1201, gfx1200), not %s",
                                  arch.c_str());
                    return Fail(LMXXF_NR_UNAVAILABLE, why);
                }
            }
            uint32_t count = 0;
            const int32_t st = ValidateModuleSet(modulesDir, &count);
            if (st != LMXXF_NR_OK)
                return st;

            auto* session = new Session;
            session->zeroOutputFallback = (info->flags & LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK) != 0;
            session->assetsDir = assets;
            session->modulesDir = modulesDir;
            session->weightsDir = FindWeightsDir(assets);
            session->shaderDir = FindShaderDir(assets);
            session->hsacoCount = count;
            session->modulesValidated = true;
            if (LooksLikeObject(info->device) && LooksLikeObject(info->queue))
            {
                auto* dev = static_cast<ID3D12Device*>(info->device);
                auto* q = static_cast<ID3D12CommandQueue*>(info->queue);
                ID3D12Device* qiDev = nullptr;
                ID3D12CommandQueue* qiQ = nullptr;
                if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&qiDev))) &&
                    SUCCEEDED(q->QueryInterface(IID_PPV_ARGS(&qiQ))))
                {
                    session->device = qiDev;
                    session->queue = qiQ;
                }
                else
                {
                    if (qiDev)
                        qiDev->Release();
                    if (qiQ)
                        qiQ->Release();
                }
            }
            *context = session;
            SetError("");
            return static_cast<int32_t>(LMXXF_NR_OK);
        });
}

int32_t Destroy(void* context)
{
    return Guard(
        [&]
        {
            delete static_cast<Session*>(context);
            SetError("");
            return static_cast<int32_t>(LMXXF_NR_OK);
        });
}

int32_t PrepareSession(void* context)
{
    auto* session = static_cast<Session*>(context);
    return GuardSession(session,
                        [&]
                        {
                            if (!session)
                                return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareSession: null context");
                            RequireSession(session);
                            if (!session->modulesValidated)
                                return Fail(LMXXF_NR_UNAVAILABLE, "PrepareSession: modules not validated");
                            if (!session->device || !session->queue)
                                return Fail(LMXXF_NR_INVALID_ARGUMENT,
                                            "PrepareSession: device/queue are not live ID3D12 objects");
                            // Upstream 0.21+: network tier follows the first real input size
                            // (DLSS5_NETWORK_HEIGHT=auto). Defer HIP bridge Create until PrepareFrame so we do not bake
                            // 1920x1080 for a 720p Color.
                            session->queueBound = true;
                            SetError("");
                            return static_cast<int32_t>(LMXXF_NR_OK);
                        });
}

int32_t PrepareFrame(void* context, const LmxxfNrFrameInfo* info, LmxxfNrJob* job)
{
    auto* session = static_cast<Session*>(context);
    return GuardSession(
        session,
        [&]
        {
            if (!session || !info || !job)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: null argument");
            RequireSession(session);
            const uint32_t legacySize = 64;
            const uint32_t noPassesSize = offsetof(LmxxfNrFrameInfo, passes);
            const uint32_t noTemporalSize = offsetof(LmxxfNrFrameInfo, motion);
            const uint32_t noSmoothSize = offsetof(LmxxfNrFrameInfo, smooth_threshold);
            const uint32_t noStyleSize = offsetof(LmxxfNrFrameInfo, style);
            const uint32_t noDepthSize = offsetof(LmxxfNrFrameInfo, depth); // the depth fields are mochizuki's
            if ((info->struct_size != sizeof(LmxxfNrFrameInfo) && info->struct_size != noDepthSize &&
                 info->struct_size != noStyleSize && info->struct_size != noSmoothSize &&
                 info->struct_size != noTemporalSize && info->struct_size != noPassesSize &&
                 info->struct_size != legacySize) ||
                (job->struct_size != sizeof(LmxxfNrJob) && job->struct_size != offsetof(LmxxfNrJob, first_pass_output)))
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: struct_size mismatch");
            job->handle = nullptr;
            job->private_output = nullptr;
            const bool fullJob = job->struct_size == sizeof(LmxxfNrJob);
            if (fullJob)
                job->first_pass_output = nullptr;
            if (session->fallbackConsumerQueue && FAILED(session->DrainGpu()))
            {
                session->failed = true;
                return Fail(LMXXF_NR_FAILED, "PrepareFrame: fallback consumer queue did not drain");
            }
            if (!session->queueBound && !session->hipPrepared)
                return Fail(LMXXF_NR_NOT_IMPLEMENTED,
                            "PrepareFrame: call PrepareSession with a live D3D12 queue first");
            if (!info->color || !info->color_width || !info->color_height)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: color resource and size required");
            if (session->bridge && session->bridge->CurrentPhase() != hip_reference::D3D12Bridge::Phase::Ready)
            {
                session->bridge->CancelUnsubmitted();
                if (session->bridge->CurrentPhase() == hip_reference::D3D12Bridge::Phase::OutputRecorded)
                {
                    session->bridge->NotifyOutputSubmitted(session->queue);
                    session->DrainGpu();
                }
                if (session->bridge->CurrentPhase() != hip_reference::D3D12Bridge::Phase::Ready)
                    return Fail(LMXXF_NR_UNAVAILABLE,
                                "PrepareFrame: previous frame consumer not yet submitted (bridge not Ready)");
            }
            const uint32_t allowedFlags =
                LMXXF_NR_FRAME_FLAG_STRENGTH | LMXXF_NR_FRAME_FLAG_DEBUG_VIEW | LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH |
                LMXXF_NR_FRAME_FLAG_TEMPORAL | LMXXF_NR_FRAME_FLAG_SMOOTH_RESIDUAL | LMXXF_NR_FRAME_FLAG_STYLE |
                LMXXF_NR_FRAME_FLAG_VIT_REUSE | LMXXF_NR_FRAME_FLAG_COMPACT_1080 | LMXXF_NR_FRAME_FLAG_PASS3_ESTIMATE |
                LMXXF_NR_FRAME_FLAG_SKIN_PROTECT | LMXXF_NR_FRAME_FLAG_FIRST_PASS | LMXXF_NR_FRAME_FLAG_AUTO_WHITE |
                LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL;
            if ((info->flags & ~allowedFlags) != 0)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: unknown flags");
            if (session->shaderDir.empty())
                session->shaderDir = FindShaderDir(session->assetsDir);
            if (session->shaderDir.empty())
                return Fail(LMXXF_NR_UNAVAILABLE, "PrepareFrame: native_codec_encode.hlsl not found");

            float transfer_strength = 1.0f;
            float color_strength = 1.0f;
            uint32_t debug_view = 0;
            float model_scale = 1.0f;
            uint32_t passes = 1;
            if (info->struct_size >= noTemporalSize)
                passes = std::clamp(info->passes, 1u, 3u);
            const bool temporal = info->struct_size >= noSmoothSize && (info->flags & LMXXF_NR_FRAME_FLAG_TEMPORAL) &&
                                  info->motion && !(info->flags & LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH);
            const uint32_t style = (info->flags & LMXXF_NR_FRAME_FLAG_STYLE) &&
                                           info->struct_size > noStyleSize
                                       ? std::min(info->style, 2u)
                                       : 1u;
            const bool smooth = temporal && info->struct_size >= noStyleSize &&
                                info->smooth_threshold > 0.f && info->smooth_strength > 0.f;
            if (info->struct_size >= noPassesSize)
            {
                if (info->flags & LMXXF_NR_FRAME_FLAG_STRENGTH)
                {
                    transfer_strength = info->transfer_strength;
                    color_strength = info->color_strength;
                }
                if (info->flags & LMXXF_NR_FRAME_FLAG_DEBUG_VIEW)
                {
                    debug_view = info->debug_view;
                }
                if (info->model_scale > 0.1f && info->model_scale <= 2.0f)
                {
                    model_scale = info->model_scale;
                }
            }
            if (!(info->flags & LMXXF_NR_FRAME_FLAG_STRENGTH))
            {
                if (const wchar_t* e = _wgetenv(L"DLSS5_STRENGTH"))
                {
                    float a = 1.f, b = 1.f;
                    if (swscanf(e, L"%f,%f", &a, &b) == 2 && a >= 0.f && b >= 0.f)
                    {
                        transfer_strength = a;
                        color_strength = b;
                    }
                }
            }
            if (debug_view == 0 && _wgetenv(L"DLSS5_DEBUG_TINT") && !wcscmp(_wgetenv(L"DLSS5_DEBUG_TINT"), L"1"))
            {
                debug_view = 4; // Tint
            }
            if (transfer_strength < 0.0f || transfer_strength > 2.0f || color_strength < 0.0f || color_strength > 2.0f)
                return Fail(LMXXF_NR_INVALID_ARGUMENT,
                            "PrepareFrame: transfer_strength and color_strength must be in [0, 2]");

            // Match upstream auto tier: <=1280x720 -> 720, <=1600x900 -> 900, else 1080.
            // Prefer CRT _putenv so MinGW std::getenv sees "auto" (SetEnvironmentVariable alone may not).
            EnsureFitLargeApplied();
            if (!std::getenv("DLSS5_NETWORK_HEIGHT"))
                _putenv("DLSS5_NETWORK_HEIGHT=auto");
            _putenv_s("DLSS5_NETWORK_1080_ROWS", (info->flags & LMXXF_NR_FRAME_FLAG_COMPACT_1080) ? "1088" : "1152");
            // Read by the network on every run. Its reuse decision compares a run with the one before, so a second
            // pass (whose input is the first one's output) would reuse the wrong answer: one pass only.
            _putenv_s("DLSS5_VIT_ADAPTIVE", (info->flags & LMXXF_NR_FRAME_FLAG_VIT_REUSE) && passes == 1 ? "1" : "0");
            // A game's dynamic resolution: the network keeps the geometry of the largest colour seen at this format and
            // aspect, and the codec fits a smaller one into it, so a step rebuilds only the codec. A larger colour, or
            // another aspect, sizes it again.
            {
                const auto format = static_cast<ID3D12Resource*>(info->color)
                                        ? static_cast<ID3D12Resource*>(info->color)->GetDesc().Format
                                        : DXGI_FORMAT_UNKNOWN;
                const UINT w = info->color_width, h = info->color_height;
                const bool sameAspect = session->drsHeight && h &&
                                        std::abs(double(w) / h - double(session->drsWidth) / session->drsHeight) < .02;
                if (!(format == session->drsFormat && sameAspect && w <= session->drsWidth && h <= session->drsHeight))
                {
                    session->drsWidth = w;
                    session->drsHeight = h;
                    session->drsFormat = format;
                    session->drsStepped = false;
                }
                else if (w < session->drsWidth || h < session->drsHeight)
                    session->drsStepped = true;
            }
            // An input up to 1080p runs on its own size with NVIDIA's padding (upstream 0.40's free resolution). The
            // 720 tier (1280x768) has no padding row in the ViT grid, which shifts the colour and leaves Style without
            // effect. 1600x900 and 1920x1080 are the tiers themselves, and above 1080p the tiers stay as they were.
            // Free resolution places the colour 1:1 and mirrors the rest, so once the colour comes smaller than the
            // network (dynamic resolution) the tiers take over and fit it.
            {
                const unsigned cw = session->drsWidth, ch = session->drsHeight;
                const bool freeRes = !session->drsStepped && cw <= 1920 && ch <= 1080 && !(cw == 1920 && ch == 1080) &&
                                     !(cw == 1600 && ch == 900) && NativeNetworkGeometry::FreeFits(cw, ch);
                _putenv_s("DLSS5_NETWORK_FREE_RES", freeRes ? "1" : "0");
            }
            // The Small model of NVIDIA's 310.9.16 DLL (upstream 0.44's 0.5x mode): 31 blocks, the default when its
            // weights are in <weights>\small, at any pass count. ORIGINAL_MODEL runs the 71-block model. A codec
            // passthrough frame keeps the model the frames around it run.
            if (!session->smallChecked)
            {
                session->smallReady = SmallModelUsable(session->weightsDir, session->modulesDir);
                session->smallChecked = true;
                OutputDebugStringA(session->smallReady ? "lmxxf: Small model found\n"
                                                       : "lmxxf: no Small model; the original one runs\n");
            }
            const bool smallModel =
                (info->flags & LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH) && session->hipPrepared
                    ? session->builtSmall
                    : session->smallReady && !(info->flags & LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL);
            // The network reads the mode as it is built (dlss5_mode::Requested); the geometry follows the model.
            dlss5_mode::Request(smallModel ? 0u : 1u);
            NativeSmallGeometryEnabled() = smallModel;
            NativeResolveNetworkGeometry(session->drsWidth, session->drsHeight);
            const auto tier = NativeCurrentNetworkGeometry();
            // A model change builds the network again, even at the 1080 tier where both models have one size:
            // upstream's in-place switch (Network::SetModelMode) keeps the other model's weights, and a model it
            // returns to differs from a fresh one as soon as a history comes in (46 dB on the frame after).
            if (session->hipPrepared &&
                (session->builtStyle != style || session->builtProcW != tier.processing_width ||
                 session->builtProcH != tier.processing_height || session->builtSmall != smallModel))
            {
                if (FAILED(session->DrainGpu()))
                    return Fail(
                        LMXXF_NR_UNAVAILABLE,
                        "PrepareFrame: style, tier or model change; GPU drain failed (retry or rebuild session)");
                session->TeardownCodecChain();
                session->job = {};
            }
            if (!session->hipPrepared)
            {
                auto geo = NativeCurrentNetworkGeometry();
                auto opt = RuntimeOptions(geo.processing_width, geo.processing_height, session->modulesDir, session->weightsDir,
                                          style);
                if (opt.graph)
                    return Fail(LMXXF_NR_FAILED, "PrepareFrame: graph must stay off");
                session->bridge = new hip_reference::D3D12Bridge();
                session->bridge->Create(session->queue, opt, {});
                session->builtStyle = style;
                session->builtProcW = geo.processing_width;
                session->builtProcH = geo.processing_height;
                session->builtSmall = session->bridge->EffectiveModelMode() == 0;
                if (smallModel && !session->builtSmall)
                    session->smallReady = false; // the network turned it down; the next frame builds the original
                session->hipPrepared = true;
                char geoMsg[192] {};
                std::snprintf(geoMsg, sizeof geoMsg,
                              "lmxxf: HIP lazy Create color=%ux%u network=%ux%u (proc %ux%u) model=%s",
                              info->color_width, info->color_height, geo.valid_width, geo.valid_height,
                              geo.processing_width, geo.processing_height, session->builtSmall ? "small" : "original");
                OutputDebugStringA(geoMsg);
                OutputDebugStringA("\n");
                SetError(geoMsg);
            }
            auto* color = static_cast<ID3D12Resource*>(info->color);
            if (!color)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: null color resource");
            const D3D12_RESOURCE_DESC cdesc = color->GetDesc();
            const DXGI_FORMAT cfmt = cdesc.Format;
            const UINT cw = static_cast<UINT>(cdesc.Width);
            const UINT ch = cdesc.Height;
            const bool autoWhite = (info->flags & LMXXF_NR_FRAME_FLAG_AUTO_WHITE) != 0;
            const bool geoChanged =
                session->encode &&
                (session->job.width != info->color_width || session->job.height != info->color_height ||
                 session->colorFormat != cfmt || (session->job.width && cw != session->job.width) ||
                 (session->job.height && ch != session->job.height) || session->codecAutoWhite != autoWhite);
            const bool pointerChanged = session->encode && color != session->job.color;

            if (session->encode && geoChanged)
            {
                if (FAILED(session->DrainGpu()))
                    return Fail(LMXXF_NR_UNAVAILABLE,
                                "PrepareFrame: color geometry change; GPU drain failed (retry or rebuild session)");
                // The network's geometry was settled above; a new size inside it needs a new codec only.
                session->TeardownCodec();
                session->job = {};
            }

            if (!session->encode)
            {
                if (!session->bridge)
                {
                    auto geo = NativeCurrentNetworkGeometry();
                    auto opt = RuntimeOptions(geo.processing_width, geo.processing_height, session->modulesDir, session->weightsDir,
                                              style);
                    if (opt.graph)
                        return Fail(LMXXF_NR_FAILED, "PrepareFrame: graph must stay off");
                    session->bridge = new hip_reference::D3D12Bridge();
                    session->bridge->Create(session->queue, opt, {});
                    session->builtStyle = style;
                    session->builtProcW = geo.processing_width;
                    session->builtProcH = geo.processing_height;
                    session->builtSmall = session->bridge->EffectiveModelMode() == 0;
                    if (smallModel && !session->builtSmall)
                        session->smallReady = false;
                    session->hipPrepared = true;
                    char geoMsg[192] {};
                    std::snprintf(
                        geoMsg, sizeof geoMsg, "lmxxf: HIP lazy Create color=%ux%u network=%ux%u (proc %ux%u) model=%s",
                        info->color_width, info->color_height, geo.valid_width, geo.valid_height, geo.processing_width,
                        geo.processing_height, session->builtSmall ? "small" : "original");
                    OutputDebugStringA(geoMsg);
                    OutputDebugStringA("\n");
                    SetError(geoMsg);
                }
                NativeGameCodec* enc = nullptr;
                NativeGameRgbInput* rgbIn = nullptr;
                NativeRgbTexture* rgbOut = nullptr;
                NativeGameCodec* dec = nullptr;
                ID3D12Resource* disp = nullptr;
                try
                {
                    const bool privateFloatOutput = cfmt == DXGI_FORMAT_R9G9B9E5_SHAREDEXP ||
                                                    NativeFallbackColor(cfmt) != DXGI_FORMAT_UNKNOWN;
                    if (autoWhite && !session->autoExposure)
                    {
                        auto* meter = new AutoExposure();
                        try
                        {
                            meter->Create(session->device);
                        }
                        catch (...)
                        {
                            delete meter;
                            throw;
                        }
                        session->autoExposure = meter;
                    }
                    ID3D12Resource* exposure = autoWhite ? session->autoExposure->value : nullptr;
                    enc = new NativeGameCodec();
                    enc->Create(session->device, { color }, session->shaderDir, privateFloatOutput, exposure);
                    rgbIn = new NativeGameRgbInput();
                    rgbIn->Create(session->device, enc->Output(), session->shaderDir);
                    rgbOut = new NativeRgbTexture();
                    rgbOut->Create(session->device, session->bridge->Output(), session->shaderDir);
                    dec = new NativeGameCodec();
                    dec->Create(session->device, { enc->Output(), rgbOut->Output(), color }, session->shaderDir,
                                privateFloatOutput, exposure);
                    if (dec->BufferOutput())
                    {
                        D3D12_RESOURCE_DESC td = cdesc;
                        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                        D3D12_HEAP_PROPERTIES hp {};
                        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                        const HRESULT chr = session->device->CreateCommittedResource(
                            &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                            IID_PPV_ARGS(&disp));
                        if (FAILED(chr) || !disp)
                            throw std::runtime_error("decode display texture create failed");
                    }
                }
                catch (...)
                {
                    if (disp)
                        disp->Release();
                    delete dec;
                    delete rgbOut;
                    delete rgbIn;
                    delete enc;
                    throw;
                }
                session->encode = enc;
                session->codecAutoWhite = autoWhite;
                session->rgbInput = rgbIn;
                session->rgbTex = rgbOut;
                session->decode = dec;
                session->decodeDisplay = disp;
            }
            else if (pointerChanged)
            {
                const bool needDrain = session->encode->RebindNeedsCompletion(0, color) ||
                                       (session->decode && session->decode->RebindNeedsCompletion(2, color)) ||
                                       (session->firstDecode && session->firstDecode->RebindNeedsCompletion(2, color));
                if (needDrain && FAILED(session->DrainGpu()))
                    return Fail(LMXXF_NR_UNAVAILABLE,
                                "PrepareFrame: color rebind needs GPU completion (drain failed; host rebuild)");
                try
                {
                    session->encode->RebindInputAfterCompletion(0, color);
                    if (session->decode)
                        session->decode->RebindInputAfterCompletion(2, color);
                    if (session->firstDecode)
                        session->firstDecode->RebindInputAfterCompletion(2, color);
                }
                catch (const std::exception& ex)
                {
                    SetError(ex.what());
                    if (FAILED(session->DrainGpu()))
                        return Fail(LMXXF_NR_UNAVAILABLE, "PrepareFrame: rebind threw; GPU drain failed");
                    session->TeardownCodecChain();
                    session->job = {};
                    return Fail(LMXXF_NR_UNAVAILABLE,
                                "PrepareFrame: color rebind failed after drain; chain torn down (retry)");
                }
            }

            session->job = {};
            session->job.color = color;
            session->job.colorState = static_cast<D3D12_RESOURCE_STATES>(info->color_state);
            session->job.width = info->color_width;
            session->job.height = info->color_height;
            session->job.transfer_strength = transfer_strength;
            session->job.color_strength = color_strength;
            session->job.debug_view = debug_view;
            session->job.passes = passes;
            session->job.passEstimate = (info->flags & LMXXF_NR_FRAME_FLAG_PASS3_ESTIMATE) != 0;
            session->job.skinProtect = (info->flags & LMXXF_NR_FRAME_FLAG_SKIN_PROTECT) != 0;
            session->job.codec_passthrough = (info->flags & LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH) != 0;
            session->job.autoWhite = (info->flags & LMXXF_NR_FRAME_FLAG_AUTO_WHITE) != 0;
            session->job.firstPass = fullJob && (info->flags & LMXXF_NR_FRAME_FLAG_FIRST_PASS) && passes > 1 &&
                                     !session->job.codec_passthrough && !session->firstFailed;
            session->colorFormat = cfmt;
            session->job.seed = 1;

            // History is one frame old only when this frame directly follows the last one prepared.
            const ULONGLONG now = GetTickCount64();
            const bool continuous = info->frame_id == session->lastFrameId + 1 && now - session->lastFrameTick < 250;
            session->lastFrameId = info->frame_id;
            session->lastFrameTick = now;
            if (temporal && session->temporalError.empty())
            {
                session->job.temporal = true;
                session->job.motion = static_cast<ID3D12Resource*>(info->motion);
                session->job.motionState = static_cast<D3D12_RESOURCE_STATES>(info->motion_state);
                session->job.motionWidth = info->motion_width;
                session->job.motionHeight = info->motion_height;
                session->job.motionScaleX = info->motion_scale_x;
                session->job.motionScaleY = info->motion_scale_y;
                if (smooth)
                {
                    session->job.smoothThreshold = std::min(info->smooth_threshold, 1.f);
                    session->job.smoothStrength = std::min(info->smooth_strength, 1.f);
                    session->job.smoothResidual = (info->flags & LMXXF_NR_FRAME_FLAG_SMOOTH_RESIDUAL) != 0;
                }
                try
                {
                    EnsureTemporal(session, session->job);
                }
                catch (const std::exception& ex)
                {
                    session->temporalError = ex.what();
                    session->job.temporal = false;
                    OutputDebugStringA(("lmxxf: temporal history off: " + session->temporalError + "\n").c_str());
                }
            }
            else if (session->temporal)
            {
                if (FAILED(session->DrainGpu()))
                    return Fail(LMXXF_NR_UNAVAILABLE, "PrepareFrame: temporal off; GPU drain failed");
                delete session->temporal;
                session->temporal = nullptr;
            }
            if (session->temporal && (info->reset || !continuous))
                session->temporal->Invalidate();
            session->job.state = LMXXF_NR_JOB_PREPARED;
            job->handle = &session->job;
            if (!session->decode)
                return Fail(LMXXF_NR_FAILED, "PrepareFrame: decode missing");
            job->private_output = session->decode->BufferOutput() ? static_cast<void*>(session->decodeDisplay)
                                                                  : static_cast<void*>(session->decode->Output());
            // The first frame that asks for it makes that codec (RecordOutputs) and goes without.
            if (session->job.firstPass && session->firstDecode)
                job->first_pass_output = session->firstDecode->BufferOutput()
                                             ? static_cast<void*>(session->firstDisplay)
                                             : static_cast<void*>(session->firstDecode->Output());
            SetError("");
            return static_cast<int32_t>(LMXXF_NR_OK);
        });
}

int32_t RecordInputs(void* context, void* job, void* command_list)
{
    auto* session = static_cast<Session*>(context);
    return GuardSession(
        session,
        [&]
        {
            if (!session)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordInputs: null context");
            if (!session->hipPrepared)
                return Fail(LMXXF_NR_NOT_IMPLEMENTED, "RecordInputs is not wired (HIP/codec next)");
            RequireSession(session);
            auto* list = static_cast<ID3D12GraphicsCommandList*>(command_list);
            auto* j = static_cast<Job*>(job ? job : &session->job);
            if (!list || !j)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordInputs: need job and command list");
            if (j->state != LMXXF_NR_JOB_PREPARED)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordInputs: job not in PREPARED state");
            ListContract(session, list);

            if (session->codecAutoWhite)
            {
                // Measured once here; the decode reads the same value, so both sides take one white point.
                session->autoExposure->Record(list, session->device, j->color, j->colorState, j->width, j->height);
                session->encode->Record(list, { j->colorState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS }, 1.f);
            }
            else
                session->encode->Record(list, { j->colorState }, 1.f);
            if (j->codec_passthrough)
            {
                // Bypass HIP: Copy encoder output directly to rgbTex output so decoder receives it as
                // neural input.
                ID3D12Resource* src = session->encode->Output();
                ID3D12Resource* dst = session->rgbTex->Output();
                D3D12_RESOURCE_BARRIER barriers[2] {};
                barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[0].Transition = { src, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE };
                barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[1].Transition = { dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_COPY_DEST };
                list->ResourceBarrier(2, barriers);
                list->CopyResource(dst, src);
                std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
                std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
                list->ResourceBarrier(2, barriers);
                j->state = LMXXF_NR_JOB_PRODUCER_SUBMITTED;
                SetError("");
                return static_cast<int32_t>(LMXXF_NR_OK);
            }
            session->rgbInput->Record(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            j->histories = 0;
            if (auto* t = j->temporal ? session->temporal : nullptr)
            {
                const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                D3D12_RESOURCE_BARRIER motionBarrier {};
                motionBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                motionBarrier.Transition = { j->motion, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, j->motionState,
                                             readable };
                const bool moveMotion = !(j->motionState & readable);
                if (moveMotion)
                    list->ResourceBarrier(1, &motionBarrier);
                t->feeds[0].RecordMotion(list, j->motion);
                if (moveMotion)
                {
                    std::swap(motionBarrier.Transition.StateBefore, motionBarrier.Transition.StateAfter);
                    list->ResourceBarrier(1, &motionBarrier);
                }
                t->coordinates.Record(list);
                if (j->smoothResidual && t->residualValid)
                {
                    t->residualSampler.Record(list);
                    j->residualHistory = true;
                }
                ID3D12Resource* histories[3] {};
                for (UINT k = 0; k < j->passes; ++k)
                {
                    if (!t->valid[k])
                        continue;
                    t->samplers[k].Record(list);
                    histories[k] = t->samplers[k].Output();
                    j->histories |= 1u << k;
                }
                session->bridge->RecordPassInputCopy(list, session->rgbInput->PostBase(), histories, j->passes);
            }
            else if (j->firstPass)
            {
                // Pass 1's output is kept (PassOutput(0)) for its own codec in RecordOutputs.
                ID3D12Resource* none[3] {};
                session->bridge->RecordPassInputCopy(list, session->rgbInput->PostBase(), none, j->passes);
            }
            else
            {
                session->bridge->RecordInputCopy(list, session->rgbInput->PostBase(), nullptr);
            }
            j->state = LMXXF_NR_JOB_PRODUCER_SUBMITTED;
            SetError("");
            return static_cast<int32_t>(LMXXF_NR_OK);
        });
}

int32_t EnqueueHip(void* context, void* job, void* command_queue)
{
    auto* session = static_cast<Session*>(context);
    return GuardSession(
        session,
        [&]
        {
            if (!session)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "EnqueueHip: null context");
            if (!session->hipPrepared)
                return Fail(LMXXF_NR_NOT_IMPLEMENTED, "EnqueueHip is not wired (HIP/codec next)");
            RequireSession(session);
            if (session->weightsDir.empty())
                return Fail(LMXXF_NR_UNAVAILABLE,
                            "EnqueueHip: weights not found (set LMXXF_WEIGHTS_DIR to tiled assets, not 0.24.2 HIP/)");
            auto* j = static_cast<Job*>(job ? job : &session->job);
            if (!j)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "EnqueueHip: null job");
            if (j->state != LMXXF_NR_JOB_PRODUCER_SUBMITTED && j->state != LMXXF_NR_JOB_CONSUMER_COMPLETE)
                return Fail(LMXXF_NR_INVALID_ARGUMENT,
                            "EnqueueHip: job not in PRODUCER_SUBMITTED or CONSUMER_COMPLETE state");
            if (j->codec_passthrough)
            {
                if (j->state == LMXXF_NR_JOB_PRODUCER_SUBMITTED)
                    j->state = LMXXF_NR_JOB_NR_COMPLETE;
                SetError("");
                return static_cast<int32_t>(LMXXF_NR_OK);
            }
            auto* targetQueue = static_cast<ID3D12CommandQueue*>(command_queue ? command_queue : session->queue);
            const bool queueMatch = !session->queue || NativeSameDevice(targetQueue, session->queue);
            if (!queueMatch)
            {
                if (!session->zeroOutputFallback)
                    throw std::runtime_error("EnqueueHip: command queue does not match session queue");
                ID3D12Device* targetDevice = nullptr;
                if (!targetQueue || FAILED(targetQueue->GetDevice(IID_PPV_ARGS(&targetDevice))) || !targetDevice)
                    throw std::runtime_error("EnqueueHip: cannot query fallback queue device");
                const bool sameDevice = NativeSameDevice(targetDevice, session->device);
                targetDevice->Release();
                if (!sameDevice)
                    throw std::runtime_error("EnqueueHip: fallback queue device mismatch");
                // Queue mismatch: cannot synchronize HIP with targetQueue on this session.
                // Complete old readers and the target queue's submitted producer
                // before a HIP zero write. A D3D12 zero copy is also ordered here.
                if (FAILED(session->DrainGpu()) || FAILED(session->DrainQueue(targetQueue)))
                {
                    session->failed = true;
                    return Fail(LMXXF_NR_FAILED,
                                "EnqueueHip: producer or old session queue did not drain before fallback clear");
                }
                // A zero neural output makes the decode shader use original Color.
                if (session->temporal)
                    session->temporal->Invalidate();
                const bool cleared = session->bridge && session->bridge->ClearOutput(targetQueue);
                if (cleared)
                {
                    targetQueue->AddRef();
                    session->fallbackConsumerQueue = targetQueue;
                    if (j->state == LMXXF_NR_JOB_PRODUCER_SUBMITTED)
                        j->state = LMXXF_NR_JOB_NR_COMPLETE;
                    SetError("EnqueueHip: queue mismatch; output zeroed for original Color passthrough");
                    return static_cast<int32_t>(LMXXF_NR_OK);
                }
                else
                {
                    session->failed = true;
                    SetError(
                        "EnqueueHip: queue mismatch and output clear failed; cannot guarantee clean visual fallback");
                    return static_cast<int32_t>(LMXXF_NR_FAILED);
                }
            }
            QueueContract(session, targetQueue);
            try
            {
                session->bridge->SetPassOptions(j->passEstimate, j->skinProtect);
                session->bridge->EnqueueAfterProducer(targetQueue, j->seed, (j->histories & 1u) != 0, j->passes);
                if (j->state == LMXXF_NR_JOB_PRODUCER_SUBMITTED)
                    j->state = LMXXF_NR_JOB_NR_COMPLETE;
                SetError("");
                return static_cast<int32_t>(LMXXF_NR_OK);
            }
            catch (const std::exception& ex)
            {
                if (!session->zeroOutputFallback)
                    throw;
                if (session->temporal)
                    session->temporal->Invalidate();
                const bool cleared = session->bridge && session->bridge->ClearOutput(targetQueue);
                if (cleared)
                {
                    if (j->state == LMXXF_NR_JOB_PRODUCER_SUBMITTED)
                        j->state = LMXXF_NR_JOB_NR_COMPLETE;
                    std::string msg = "EnqueueHip: enqueue failed (";
                    msg += ex.what();
                    msg += "); output zeroed for original Color passthrough";
                    SetError(msg.c_str());
                    return static_cast<int32_t>(LMXXF_NR_OK);
                }
                else
                {
                    session->failed = true;
                    std::string msg = "EnqueueHip: enqueue failed (";
                    msg += ex.what();
                    msg += ") and clear failed; cannot guarantee clean visual fallback";
                    SetError(msg.c_str());
                    return static_cast<int32_t>(LMXXF_NR_FAILED);
                }
            }
        });
}

// LMXXF_NR_FRAME_FLAG_FIRST_PASS: pass 1's output, kept by the bridge, through the same codec as the last pass's, into
// the texture PrepareFrame handed out. The codec is made here on the first frame that keeps pass 1 (that frame's
// PrepareFrame had none to hand out); a bridge rebuilt since leaves it unused until the codec goes.
void RecordFirstPass(Session* session, ID3D12GraphicsCommandList* list, const Job& j,
                     const NativeCodecParameters& codecParams)
{
    ID3D12Resource* source = session->bridge->PassOutput(0);
    if (session->firstTex && session->firstSource != source)
        return;
    if (!session->firstTex)
    {
        const DXGI_FORMAT cfmt = session->colorFormat;
        const bool privateFloatOutput =
            cfmt == DXGI_FORMAT_R9G9B9E5_SHAREDEXP || NativeFallbackColor(cfmt) != DXGI_FORMAT_UNKNOWN;
        auto* tex = new NativeRgbTexture();
        auto* dec = new NativeGameCodec();
        ID3D12Resource* disp = nullptr;
        try
        {
            tex->Create(session->device, source, session->shaderDir);
            dec->Create(session->device, { session->encode->Output(), tex->Output(), j.color }, session->shaderDir,
                        privateFloatOutput, session->codecAutoWhite ? session->autoExposure->value : nullptr);
            if (dec->BufferOutput())
            {
                D3D12_RESOURCE_DESC td = j.color->GetDesc();
                td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                D3D12_HEAP_PROPERTIES hp {};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                if (FAILED(session->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                                    nullptr, IID_PPV_ARGS(&disp))) ||
                    !disp)
                    throw std::runtime_error("first pass display texture create failed");
            }
        }
        catch (...)
        {
            if (disp)
                disp->Release();
            delete dec;
            delete tex;
            throw;
        }
        session->firstTex = tex;
        session->firstDecode = dec;
        session->firstDisplay = disp;
        session->firstSource = source;
        OutputDebugStringA("lmxxf: first pass output ready\n");
    }
    session->firstTex->Record(list);
    std::vector<D3D12_RESOURCE_STATES> before { D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, j.colorState };
    if (session->codecAutoWhite)
        before.push_back(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    session->firstDecode->Record(list, before, 1.f, codecParams);
    if (!session->firstDecode->BufferOutput())
        return;
    ID3D12Resource* src = session->firstDecode->Output();
    ID3D12Resource* dst = session->firstDisplay;
    D3D12_RESOURCE_BARRIER barriers[2] {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = { src, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE };
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition = { dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST };
    list->ResourceBarrier(2, barriers);
    D3D12_TEXTURE_COPY_LOCATION dstLoc {};
    dstLoc.pResource = dst;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION srcLoc {};
    srcLoc.pResource = src;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Footprint = session->firstDecode->BufferFootprint();
    list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
    std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
    list->ResourceBarrier(2, barriers);
}

int32_t RecordOutputs(void* context, void* job, void* command_list)
{
    auto* session = static_cast<Session*>(context);
    return GuardSession(
        session,
        [&]
        {
            if (!session)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordOutputs: null context");
            if (!session->hipPrepared)
                return Fail(LMXXF_NR_NOT_IMPLEMENTED, "RecordOutputs is not wired (HIP/codec next)");
            RequireSession(session);
            auto* list = static_cast<ID3D12GraphicsCommandList*>(command_list);
            auto* j = static_cast<Job*>(job ? job : &session->job);
            if (!list || !j)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordOutputs: need job and command list");
            if (j->state != LMXXF_NR_JOB_NR_COMPLETE && j->state != LMXXF_NR_JOB_PRODUCER_SUBMITTED)
                return Fail(LMXXF_NR_INVALID_ARGUMENT,
                            "RecordOutputs: job not in NR_COMPLETE or PRODUCER_SUBMITTED state");
            ListContract(session, list);

            bool residualRecorded = false;
            if (!j->codec_passthrough)
            {
                session->bridge->RecordOutputReadable(list);
                // Before the output is shown or becomes history, so the blend is recursive.
                const UINT last = j->passes - 1;
                if (j->temporal && session->temporal && j->smoothStrength > 0.f && j->smoothResidual)
                {
                    const auto ng = NativeCurrentNetworkGeometry();
                    TemporalChain* t = session->temporal;
                    t->smooth.RecordResidual(list, session->bridge->Output(), t->residualSampler.Output(),
                                             session->rgbInput->PostBase(), t->residual, t->residualWritten,
                                             j->smoothThreshold, j->smoothStrength,
                                             ng.processing_width * ng.valid_height, j->residualHistory);
                    residualRecorded = t->residualWritten = true;
                }
                else if (j->temporal && session->temporal && j->smoothStrength > 0.f && (j->histories >> last & 1u))
                {
                    const auto ng = NativeCurrentNetworkGeometry();
                    session->temporal->smooth.Record(list, session->bridge->Output(),
                                                     session->temporal->samplers[last].Output(), j->smoothThreshold,
                                                     j->smoothStrength, ng.processing_width * ng.valid_height);
                }
                session->rgbTex->Record(list);
            }
            if (auto* t = j->temporal ? session->temporal : nullptr)
            {
                t->residualValid = residualRecorded;
                for (UINT k = 0; k < 3; ++k)
                {
                    t->valid[k] = k < j->passes;
                    if (!t->valid[k])
                        continue;
                    t->feeds[k].BindNetworkOutput(session->bridge->PassOutput(k));
                    t->feeds[k].RecordHistory(list);
                }
            }
            if (!session->decode)
                return Fail(LMXXF_NR_FAILED, "RecordOutputs: decode missing");
            NativeCodecParameters codecParams;
            codecParams.transfer_strength = j->transfer_strength;
            codecParams.color_strength = j->color_strength;
            codecParams.debug_view = static_cast<NativeCodecDebugView>(j->debug_view);
            std::vector<D3D12_RESOURCE_STATES> before { D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, j->colorState };
            if (session->codecAutoWhite)
                before.push_back(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            session->decode->Record(list, before, 1.f, codecParams);
            if (session->decode->BufferOutput())
            {
                if (!session->decodeDisplay)
                    return Fail(LMXXF_NR_FAILED, "RecordOutputs: decode display missing");
                ID3D12Resource* src = session->decode->Output();
                ID3D12Resource* dst = session->decodeDisplay;
                D3D12_RESOURCE_BARRIER barriers[2] {};
                barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[0].Transition = { src, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE };
                barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[1].Transition = { dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_COPY_DEST };
                list->ResourceBarrier(2, barriers);
                D3D12_TEXTURE_COPY_LOCATION dstLoc {};
                dstLoc.pResource = dst;
                dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION srcLoc {};
                srcLoc.pResource = src;
                srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                srcLoc.PlacedFootprint.Footprint = session->decode->BufferFootprint();
                list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
                std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
                std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
                list->ResourceBarrier(2, barriers);
            }
            if (j->firstPass)
                try
                {
                    RecordFirstPass(session, list, *j, codecParams);
                }
                catch (const std::exception& ex)
                {
                    session->firstFailed = true;
                    OutputDebugStringA((std::string("lmxxf: first pass output off: ") + ex.what() + "\n").c_str());
                }
            j->state = LMXXF_NR_JOB_CONSUMER_COMPLETE;
            SetError("");
            return static_cast<int32_t>(LMXXF_NR_OK);
        });
}

int32_t ExecuteAfterProducer(void* context, void* job, void* command_queue)
{
    return EnqueueHip(context, job, command_queue);
}

int32_t CancelUnsubmitted(void* context, void* job)
{
    auto* session = static_cast<Session*>(context);
    return GuardSession(session,
                        [&]
                        {
                            if (!session)
                                return Fail(LMXXF_NR_INVALID_ARGUMENT, "null context");
                            auto* j = static_cast<Job*>(job ? job : &session->job);
                            if (j)
                                j->state = LMXXF_NR_JOB_RETIRED;
                            if (session->bridge)
                                session->bridge->CancelUnsubmitted();
                            if (session->temporal)
                                session->temporal->Invalidate();
                            SetError("");
                            return static_cast<int32_t>(LMXXF_NR_OK);
                        });
}
int32_t Poll(void* context, void* job, uint32_t* state)
{
    return Guard(
        [&]
        {
            auto* session = static_cast<Session*>(context);
            if (!session)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "null context");
            auto* j = static_cast<Job*>(job ? job : &session->job);
            if (state)
                *state = j ? j->state : LMXXF_NR_JOB_NONE;
            SetError("");
            return static_cast<int32_t>(LMXXF_NR_OK);
        });
}
int32_t Retire(void* context, void* job)
{
    auto* session = static_cast<Session*>(context);
    return GuardSession(session,
                        [&]
                        {
                            if (!session)
                                return Fail(LMXXF_NR_INVALID_ARGUMENT, "null context");
                            auto* j = static_cast<Job*>(job ? job : &session->job);
                            if (j)
                                j->state = LMXXF_NR_JOB_RETIRED;
                            if (session->bridge)
                                session->bridge->NotifyOutputSubmittedIfRecorded(session->queue);
                            SetError("");
                            return static_cast<int32_t>(LMXXF_NR_OK);
                        });
}
int32_t ResetHistory(void* context)
{
    auto* session = static_cast<Session*>(context);
    return GuardSession(session,
                        [&]
                        {
                            if (!session)
                                return Fail(LMXXF_NR_INVALID_ARGUMENT, "null context");
                            if (session->temporal)
                                session->temporal->Invalidate();
                            SetError("");
                            return static_cast<int32_t>(LMXXF_NR_OK);
                        });
}
int32_t Drain(void* context)
{
    auto* session = static_cast<Session*>(context);
    return GuardSession(session,
                        [&]
                        {
                            if (!session)
                                return Fail(LMXXF_NR_INVALID_ARGUMENT, "null context");
                            const HRESULT hr = session->DrainGpu();
                            if (FAILED(hr))
                                return Fail(LMXXF_NR_FAILED, "Drain: GPU wait failed or timed out");
                            SetError("");
                            return static_cast<int32_t>(LMXXF_NR_OK);
                        });
}

int32_t GetStatus(void* context, char* buf, uint32_t buf_chars)
{
    return Guard(
        [&]
        {
            if (!buf || buf_chars == 0)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetStatus: empty buffer");
            auto* session = static_cast<Session*>(context);
            char text[256] {};
            if (!session)
                std::snprintf(text, sizeof text, "no session");
            else if (session->failed)
                std::snprintf(text, sizeof text, "lmxxf poisoned (fatal error)");
            else if (!session->modulesValidated)
                std::snprintf(text, sizeof text, "lmxxf runtime stub (no modules path)");
            else if (session->hipPrepared && NativeNetworkGeometryResolved())
            {
                auto geo = NativeCurrentNetworkGeometry();
                std::snprintf(text, sizeof text,
                              "lmxxf modules_ok=%u hip=1 net=%ux%u color_job=%ux%u weights=%u model=%s temporal=%s",
                              static_cast<unsigned>(session->hsacoCount), geo.valid_width, geo.valid_height,
                              session->job.width, session->job.height, session->weightsDir.empty() ? 0u : 1u,
                              session->builtSmall ? "small" : "original",
                              session->temporal                ? "on"
                              : session->temporalError.empty() ? "off"
                                                               : session->temporalError.c_str());
            }
            else
            {
                std::snprintf(text, sizeof text, "lmxxf modules_ok=%u hip=0 prepared=%u queue=%u weights=%u",
                              static_cast<unsigned>(session->hsacoCount), session->hipPrepared ? 1u : 0u,
                              session->queueBound ? 1u : 0u, session->weightsDir.empty() ? 0u : 1u);
            }
            std::strncpy(buf, text, buf_chars - 1);
            buf[buf_chars - 1] = 0;
            SetError("");
            return static_cast<int32_t>(LMXXF_NR_OK);
        });
}

int32_t GetLastError(char* buf, uint32_t buf_chars)
{
    return Guard(
        [&]
        {
            if (!buf || buf_chars == 0)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetLastError: empty buffer");
            std::strncpy(buf, g_lastError, buf_chars - 1);
            buf[buf_chars - 1] = 0;
            return static_cast<int32_t>(LMXXF_NR_OK);
        });
}
} // namespace

extern "C" int32_t LmxxfNrGetApi(uint32_t abi_version, LmxxfNrApi* out)
{
    return Guard(
        [&]
        {
            if (!out)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetApi: null out");
            if (out->struct_size != sizeof(LmxxfNrApi))
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetApi: struct_size mismatch");
            if (abi_version != LMXXF_NR_ABI_VERSION)
                return Fail(LMXXF_NR_UNSUPPORTED_ABI, "GetApi: unsupported abi_version");
            std::memset(out, 0, sizeof(*out));
            out->struct_size = sizeof(LmxxfNrApi);
            out->abi_version = LMXXF_NR_ABI_VERSION;
            out->QueryCapabilities = QueryCapabilities;
            out->Create = Create;
            out->Destroy = Destroy;
            out->PrepareSession = PrepareSession;
            out->PrepareFrame = PrepareFrame;
            out->RecordInputs = RecordInputs;
            out->EnqueueHip = EnqueueHip;
            out->RecordOutputs = RecordOutputs;
            out->ExecuteAfterProducer = ExecuteAfterProducer;
            out->CancelUnsubmitted = CancelUnsubmitted;
            out->Poll = Poll;
            out->Retire = Retire;
            out->ResetHistory = ResetHistory;
            out->Drain = Drain;
            out->GetStatus = GetStatus;
            out->GetLastError = GetLastError;
            SetError("");
            return static_cast<int32_t>(LMXXF_NR_OK);
        });
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
