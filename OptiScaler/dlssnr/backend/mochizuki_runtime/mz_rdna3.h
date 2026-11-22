// The checks the RDNA3 network (RX 7000, FP16 matrices) passes before it runs on a device (MochizukiNrRuntime.cpp).
// The RDNA2 network (RX 6000, matrices emulated in ALU) takes the pipeline check only, kept in rdna2-check.txt.
//
// Layout: its kernels assume the gfx11 fragment layout mauri870's fork measured on RADV (an accumulator component c
// of lane l is element (2c + l/16, l%16), an A or B fragment holds 16 components a lane, row m of a product comes from
// lane half m%2 alone, and swapping every k pair in the upper half changes no product). AMD's Windows compiler may
// order the components otherwise; the network then runs and draws a wrong picture. LayoutCheck runs the probes of the
// RX 7000 kit (rdna3_probe/, embedded) on the network's own device, at the session's start.
//
// Pipelines: AMD's compiler (LLPC) has crashed on some of the network's shaders. PipelineCheck compiles every one of
// them in a child process (rundll32 on this DLL's MochizukiNrPipelineCheck entry), before the first network build on a
// driver, on the builder thread, so a crash or a hang ends only the child.
//
// Both verdicts are kept in dlssnr-amd\rdna3-check.txt by device id and driver version, the pipelines' also by
// the shader files (Cached, Keep), so each runs once a driver.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <string>
#include <vector>

namespace mzr3
{
// The key a verdict is kept under: the device as AMD's driver reports it and its driver version.
std::string Key(uint32_t deviceId, uint32_t driverVersion);

// A verdict for `key` and `stage` ("layout", "pipelines") kept in `file`: true when there is one, `why` empty when it
// passed, else what failed.
bool Cached(const std::wstring& file, const std::string& key, const char* stage, std::string& why);
void Keep(const std::wstring& file, const std::string& key, const char* stage, const std::string& why);

// The fragment layout probes on `device` (wave32, the FP16 matrices): empty when every one matches what the RDNA3
// network's kernels assume, else each failure, what was observed against what is expected. Waits for the queue.
std::string LayoutCheck(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family);

// Every file compiled in a child process on the adapter `luid`, as the runtime creates its pipelines (wave32, robust
// access off, a layout read from the module). Empty when all compiled, else the pipeline and what happened (a crash
// of the compiler, no answer in `timeoutMs`, a VkResult). `ran` false: the check could not be made (no child
// process, no device in it, `stop` set) and the text says why; the network then builds unprotected and nothing is
// kept. `note`: how many compiled, in how long, the slowest. `work` holds the child's list and log.
// The RDNA2 network's emulated kernels take minutes each (the slowest about 7 on the RX 9070 XT with 8 at once), so
// it waits kPipelineTimeoutRdna2Ms. With `data` (dlssnr-amd) the child compiles the files the prewarm manifest of
// data\shaders<variant> names with the core's create infos and keeps their machine code in work\binaries
// (mzi::ManifestPipelines), which the caller hands to mzi::AdoptBinaries.
constexpr unsigned kPipelineTimeoutMs = 300000, kPipelineTimeoutRdna2Ms = 1800000;
std::string PipelineCheck(const LUID& luid, const std::vector<std::wstring>& files, const std::wstring& work,
                          const std::atomic<bool>* stop, bool& ran, std::string& note,
                          unsigned timeoutMs = kPipelineTimeoutMs, const std::wstring& data = {},
                          const std::wstring& variant = {});
} // namespace mzr3
