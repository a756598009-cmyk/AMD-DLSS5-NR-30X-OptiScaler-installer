# ONNX Runtime C API headers

`onnxruntime_c_api.h` and `onnxruntime_ep_c_api.h` from Microsoft ONNX Runtime, tag `v1.23.2`, MIT licence
(`LICENSE` beside this file). Source:
https://github.com/microsoft/onnxruntime/tree/v1.23.2/include/onnxruntime/core/session (byte for byte the
headers of `onnxruntime-win-x64-1.23.2.zip`).

Only the headers are here. `person-worker.exe` (person protection, `NrPersonPartition`) loads a CPU x64 ONNX Runtime
with C API 23 or newer at run time. Ported from TheAutomatic/dlss-5-amd-project (GPL-3.0), `dlssnr/person/`.

## The person-model folder

Person protection reads everything from `person-model\` beside the folder of the OptiScaler DLL. Nothing here is
downloaded by OptiScaler or kept in git: the installer ships the folder. `person-worker.exe` is built from this tree
(`tools\build-person-worker.cmd`); the rest is pinned below.

| File | Size (bytes) | SHA-256 | Source | Licence |
|---|---|---|---|---|
| `person-worker.exe` | per build | per build | `OptiScaler\dlssnr\person\worker\person_worker.cpp`, `tools\build-person-worker.cmd` | GPL-3.0 |
| `onnxruntime.dll` | 14186016 | `dec964ab1ee36cc9b0ae247d13b376627992fc57dec0454354017ab8fd84f1ea` | `lib\onnxruntime.dll` of `onnxruntime-win-x64-1.23.2.zip` (GitHub release v1.23.2, zip SHA-256 `0b38df9af21834e41e73d602d90db5cb06dbd1ca618948b8f1d66d607ac9f3cd`) | MIT, `LICENSE` |
| `pphumanseg.onnx` | 6163938 | `552d8a984054e59b5d773d24b9b12022b22046ceb2bbc4c9aaeaceb36a9ddf24` | OpenCV Zoo `2027dd2f`, `models/human_segmentation_pphumanseg/human_segmentation_pphumanseg_2023mar.onnx` | Apache-2.0, `PPHumanSeg_Apache-2.0.txt` |
| `yolo11n-seg.onnx` | 11719609 | `4debe9ba74e54c2f9ed9098c3bac6ed5706f80547d378c103e5435587d3083a7` | Ultralytics `yolo11n-seg.pt` (assets v8.3.0) exported with ultralytics 8.3.215, onnx 1.19.1, opset 17, imgsz 640, FP32 | AGPL-3.0, `YOLO11_AGPL-3.0.txt` |
| `yunet.onnx` | 229738 | `ebafce4e3c118d6554634be5c27ab333b4c047a9a8c3faf1d7cf93101c22f0f0` | OpenCV Zoo `47534e27`, `models/face_detection_yunet/face_detection_yunet_2026may.onnx` | MIT, `YuNet_MIT.txt` |
| `msvcp140.dll` | 643512 | `7c26614e1d733892c2deac7e245ce115504b1d80592dd0a01b08e3e5a55f89ca` | Visual C++ redistributable 14.51.36231, `x64\Microsoft.VC145.CRT` (file version 14.51.36247.0) | Microsoft redistributable |
| `vcruntime140.dll` | 178616 | `d1f4225df2cd877dbf130d5668a021dce3f94118455ff5ec952061c30afc9ce7` | same | same |
| `vcruntime140_1.dll` | 50112 | `a7146c08f89fe5b04541ab507cdb59ff7b44534d4ba3c668a426c6450a03434e` | same | same |

The CRT DLLs are for `onnxruntime.dll`, loaded from the same folder; the worker itself links the CRT statically, and
the game's own CRT is never replaced. Model 0 (`NrPersonModel=0`, the default) needs `pphumanseg.onnx`, 1
`yolo11n-seg.onnx`, 2 `yunet.onnx`; only the chosen one is loaded. The licence texts ship with the models.
