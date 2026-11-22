OpenNR 0.6.10 ANR 编译错误修复 V4

针对 V3 编译日志中的错误：
1. 删除 CustomProviders.h 中本版本 Config 不存在的 CustomUpscaler / CustomUpscalerVk 逻辑。
2. 保留 DLSSG Provider 的目录扫描、DLL 定位和动态加载接口，兼容 menu_common.cpp 中 CustomProviders::Scan().dlssg 的调用。
3. 在 Nvngx_Custom.cpp 中直接解析 NGX DLSSG 导出函数，替代目标版本不存在的 Nvngx_DllProxy::LoadNgxExports()。

应用方式：
- 先备份当前 OptiScaler/misc/CustomProviders.h
- 先备份当前 OptiScaler/framegen/nvngx/Nvngx_Custom.cpp
- 将 ZIP 中 OptiScaler 文件夹内的两个文件复制覆盖到源码对应位置。
- 从源码根目录 C:\OpenNR-OptiScaler-0.6.10-opennr 执行原来的构建脚本。

本包只针对日志中已确认的 C2039/C3536/C3861 错误进行修复。
尚未在用户的 Windows/VS2022 环境中编译验证。若出现新错误，请提供完整日志。
