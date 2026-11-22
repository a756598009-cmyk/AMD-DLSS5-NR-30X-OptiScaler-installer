#pragma once
#include <upscalers/IFeature_Dx12.h>
#include "opennr_rr_api.h"

#include <string>
#include <vector>

// OpenNrRrRuntime.dll, loaded once from OptiScaler's folder.
namespace OpenNrRr
{
// Loads the runtime and asks it once whether this adapter and nvngx_dlssd.dll can run the network.
bool Usable(LUID adapter);
// Whether Usable has run, and why OpenRR is not offered (empty while it is).
bool Checked();
std::string Reason();
// A failure this session: OpenRR stays off until the game restarts, so the fallback never returns to it.
void Block(const std::string& why);
// The running context's state, for the menu. False without one.
bool Info(OnrRrInfo& info);
// Whether the game's lists are OptiScaler's split proxies, set up at launch when an NR runtime that splits them or
// OpenRR was enabled.
bool SplitReady();
// The split is armed and turns on with the swapchain: enough to offer Ray Reconstruction before that.
bool SplitComing();
// NrOnDenoised: what mochizuki on the denoised image is doing, or why it is not running. Empty while it is off.
std::string DenoisedNrStatus();
// The runtime's ABI 5 (the Vulkan host backend), loading the runtime; null when it does not offer it.
const OnrRrApi5* Api5();
// The nvngx_dlssd.dll Usable settled on, and the runtime's last error on this thread.
std::wstring DlssdPath();
std::string LastError();
// Whether an nvngx_dlssd.dll to build the weights from is there (the opennr-rr folder, OptiScaler's lookup, the game's).
bool DlssdFound();
// The context the menu's Info reads.
void SetInfoContext(void* context);
void ClearInfoContext(void* context);
// [OpenNR-RR] Mode and Fp8 sent to the runtime, every frame (a runtime without them just does not have them).
void ApplyFastMode(void* context);
// The running context's fast mode: -1 not offered by this runtime, 0 off, 1 on, 2 building, 3 not available,
// 4 on at the experimental level.
int FastModeState();
// The running context's FP8: -1 not offered by this runtime, 0 off, 1 on, 2 building, 3 not available,
// 4 asked but the mode does not use it (Legacy).
int Fp8State();
// The DLSS-D texture and scalar keys the inputs inventory in the log reads.
std::vector<const char*> NgxTextureKeys();
std::vector<const char*> NgxScalarKeys();
// The loaded runtime's release, from its file version. Zero before it loads or without a version resource.
feature_version RuntimeVersion();
} // namespace OpenNrRr

class OpenNrRrFeatureDx12 : public IFeature_Dx12
{
  public:
    OpenNrRrFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~OpenNrRrFeatureDx12();

    // OpenRR's release, as the overlay and the menu show it (not the runtime's ABI)
    feature_version Version() override;
    Upscaler GetUpscalerType() const override { return Upscaler::OpenNrRr; }
    bool IsWithDx12() final { return false; }
    // without the NR on the denoised image, which gets a line of its own
    std::optional<double> ReadUpscalerTime(void* commandQueue) override;
    void ReadDetailedGpuTimes(void* commandQueue, std::vector<DetailedGpuTime>& detailedGpuTimes) override;

  protected:
    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

  private:
    void* _context = nullptr;
    // The job armed on a list's between slot, until that list runs.
    void* _job = nullptr;
    bool _jobIsToken = false; // NrImage=output: no job of the runtime, only the between slot's mark
    ID3D12CommandList* _jobList = nullptr;
    uint32_t _maxRenderWidth = 0;
    uint32_t _maxRenderHeight = 0;
    bool _ngxHWDepth = false;
    bool _hasNGXDepthType = false;
    int _roughnessPacked = -1; // -1 until the create params or the first frame settle it
    int _loggedHWDepth = -1;
    int _loggedSwitches = -1;   // the diagnostic switches last logged
    float _loggedMaskScale = 0.0f;
    bool _fellBack = false;

    void DropStaleJob();
    bool FillFrame(const NVSDK_NGX_Parameter& params, OnrRrFrame& frame);
    void FallBack(const std::string& why);
    // NrOnDenoised: mochizuki's lists for this job, run by the between slot from EnqueueNetwork to EnqueuePost (D3D12:
    // between the halves of the game's list); image: the one Record gave (D3D12), null to ask PrepareDenoised. The
    // DXGI format the lists write back into the image, 0 when none were armed.
    uint32_t RecordDenoisedNr(const NVSDK_NGX_Parameter& params, const OnrRrFrame& frame, void* job, const OnrRrDenoisedImage* image,
                              bool onOutput = false);
    // the hop's GPU timeline of a timed frame of this slot into the log (render thread)
    void LogHopTiming(int slotIndex);
    // the NR span of a slot whose lists are done (render thread); the last one, when recent
    void ReadNrTime(int slotIndex);
    std::optional<float> NrTime() const;
    static int32_t Between(void* feature, void* job, void* queue);
    // D3D12 backend: the frame recorded into the game's list (Record / RecordPost)
    bool EvaluateD3d12(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);
    static int32_t BetweenD3d12(void* feature, void* job, void* queue);
};
