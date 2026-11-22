OpenNR 0.6.10 - ANR Custom DLSSG Provider integration (V3)
==========================================================

This package is based on the uploaded OpenNR-ANR-Source.zip snapshot and the
ANR FG V.B20 reference source.

What is included
----------------
1. OpenNR-0.6.10-ANR-core.patch
   State enum/state fields, INI load/save, Nvngx_FG custom-provider branch,
   and project registration from V2.
2. OpenNR-0.6.10-ANR-menu.patch
   Adds dynamic custom DLSSG provider entries to the existing FG dropdowns.
   Existing Nukem, Enabler, FFX and Combo entries are preserved.
3. files/misc/CustomProviders.h
4. files/framegen/nvngx/Nvngx_Custom.h and Nvngx_Custom.cpp
5. modified/menu/menu_common.cpp
   Complete modified menu file for visual diff/review.

Apply order
-----------
Run from the OpenNR source root (the folder containing State.h and
OptiScaler.vcxproj). Make a backup or use a clean git branch first.

    git apply --check OpenNR-0.6.10-ANR-core.patch
    git apply OpenNR-0.6.10-ANR-core.patch
    git apply --check OpenNR-0.6.10-ANR-menu.patch
    git apply OpenNR-0.6.10-ANR-menu.patch

The patches were tested for clean application against the uploaded source
snapshot in that order. This is NOT a claim of successful MSVC compilation
or runtime testing. The source archive may differ from the user's current
working tree.

Runtime layout
--------------
OptiScaler\custom\Apex Neural Rendering FG\dlssg\nvngx_dlssg.dll

The INI stores the folder name as FrameGen/FGNvngxReplacement. The dropdown
scans custom subfolders and displays each folder that contains a dlssg DLL.

Important
---------
- Provider DLL is not linked during compilation; it is loaded at runtime.
- This V3 adds the dynamic menu selection that was missing in V2.
- No actual Windows/MSVC build or game test has been run here.
- If your local tree has later edits, inspect `git diff` and resolve conflicts
  instead of forcing the patch.
