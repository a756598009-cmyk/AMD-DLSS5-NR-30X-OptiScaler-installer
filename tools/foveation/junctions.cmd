@echo off
rem junctions.cmd on|off: the submodules under external\ as junctions to wt-upstream-merge's checkout (same commits),
rem FidelityFX-SDK-v2.4 to ffx-sdk-v2.4.0, for a build in this worktree. Run "off" before any git command.
cd /d "%~dp0..\.."
for %%n in (FidelityFX-SDK FidelityFX-SDK-v2 magic_enum nvapi simpleini spdlog unordered_dense vulkan xess) do (
  if exist "external\%%n" rmdir "external\%%n"
  if /i "%1"=="on" (mklink /J "external\%%n" "E:\Projetos\opti_rr_dlss5\wt-upstream-merge\external\%%n" >nul) else (mkdir "external\%%n")
)
if exist "external\FidelityFX-SDK-v2.4" rmdir "external\FidelityFX-SDK-v2.4"
if /i "%1"=="on" (mklink /J "external\FidelityFX-SDK-v2.4" "E:\Projetos\opti_rr_dlss5\ffx-sdk-v2.4.0" >nul) else (mkdir "external\FidelityFX-SDK-v2.4")
