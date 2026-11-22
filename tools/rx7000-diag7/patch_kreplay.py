"""Kit 7's kreplay = kit 6's plus ACO: with KREPLAY_ACO=<records dir> a variant whose SPIR-V has a record there is
imported (nr_pal_binary.hpp splice into the driver's compile of KREPLAY_ACO_TEMPLATE, the shipped kernel);
otherwise, or if the import fails, the driver compiles it and the run says so on stderr."""
from pathlib import Path

p = Path(__file__).with_name('kreplay.cpp')
s = p.read_text(encoding='utf-8')


def rep(old, new):
    global s
    assert s.count(old) == 1, old[:80]
    s = s.replace(old, new)


rep('#include "child.hpp"\n', '#include "child.hpp"\n#include "nr_pipeline_binary.hpp"\n')
rep('''        std::vector<const char*> ext = { VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME };
        if (robust) ext.push_back(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);''',
'''        std::vector<const char*> ext = { VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME };
        if (robust) ext.push_back(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
        // KREPLAY_ACO: pipeline binaries, as the runtime's device has them for ACO (nr::binary::Features).
        VkPhysicalDevicePipelineBinaryFeaturesKHR pbf { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR };
        VkPhysicalDeviceMaintenance5FeaturesKHR m5f { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &pbf };
        if (const char* a = std::getenv("KREPLAY_ACO"); a && *a && HasExt(phys, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) &&
            HasExt(phys, VK_KHR_MAINTENANCE_5_EXTENSION_NAME))
        {
            ext.push_back(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME);
            ext.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
            pbf.pipelineBinaries = VK_TRUE; m5f.maintenance5 = VK_TRUE;
            pbf.pNext = f2.pNext; f2.pNext = &m5f;
            aco = true;
        }''')
rep('    bool robust = false;\n', '    bool robust = false;\n    bool aco = false;\n')
rep('''        const VkResult r = vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe);''',
'''        VkResult r = VK_ERROR_UNKNOWN;
        bool imported = false;
        if (g.aco)
        {
            const fs::path rec = fs::path(std::getenv("KREPLAY_ACO")) /
                                 nr::binary::keyname(nr::pal::hash(code.data(), code.size()));
            const char* tpl = std::getenv("KREPLAY_ACO_TEMPLATE");
            if (fs::exists(rec) && tpl && *tpl)
            {
                VkShaderModule tm {};
                try
                {
                    const std::vector<uint8_t> t = ReadFile(tpl);
                    VkShaderModuleCreateInfo tmi { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                    tmi.codeSize = t.size(); tmi.pCode = reinterpret_cast<const uint32_t*>(t.data());
                    nr::binary::check(vkCreateShaderModule(g.dev, &tmi, nullptr, &tm), "template module");
                    std::array<uint32_t, 3> wg {};
                    const uint32_t* w = reinterpret_cast<const uint32_t*>(code.data());
                    for (size_t q = 5; q < code.size() / 4;)
                    {
                        const uint32_t len = w[q] >> 16, op = w[q] & 0xffff;
                        if (!len) break;
                        if (op == 16 && len == 6 && w[q + 2] == 17) wg = { w[q + 3], w[q + 4], w[q + 5] };
                        q += len;
                    }
                    auto tci = cpi; tci.stage.module = tm;
                    const auto record = nr::pal::Record::parse(nr::pal::load(rec.string()));
                    pipe = nr::binary::create(g.dev, tci, record, c.pushRange, wg);
                    r = VK_SUCCESS; imported = true;
                    std::fprintf(stderr, "ACO imported %s (record %s)\\n", spv.filename().string().c_str(), rec.filename().string().c_str());
                }
                catch (const std::exception& e)
                {
                    std::fprintf(stderr, "ACO not imported %s: %s; the driver compiles it\\n", spv.filename().string().c_str(), e.what());
                }
                if (tm) vkDestroyShaderModule(g.dev, tm, nullptr);
            }
            else std::fprintf(stderr, "ACO no record for %s; the driver compiles it\\n", spv.filename().string().c_str());
        }
        if (!imported) r = vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe);''')
rep(r'"  " + c.id + ":\n      released 0.5.8 (" + c.vars[0].name + "): "', r'"  " + c.id + ":\n      first variant (" + c.vars[0].name + "): "')
rep(r'"\n      kit 6 fix (" + c.vars[1].name + "): "', r'"\n      second variant (" + c.vars[1].name + "): "')
rep('diagnostic kit 6, one dispatch alone', 'diagnostic kit 7, one dispatch alone')
p.write_text(s, encoding='utf-8')
print('patched')
