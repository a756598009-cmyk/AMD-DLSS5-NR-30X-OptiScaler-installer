OpenNR RX 7000 diagnostic kit 6

Thanks for running kit 5. It showed that the attention step at the 512-channel level goes right on your
card once its rows in on-chip memory are 16-byte aligned, and that the next step that changes from run
to run is the ViT attention. This kit has a fix for both: the aligned rows by default, and the ViT
attention reading one of its inputs in a different way. It runs two tiny tests of one matrix load each,
both attention steps alone (the released version, the fixed one, one without matrix instructions and a
few single changes), then the whole network with the fix against the released 0.5.8 one on a test
picture (4 sizes, 3 runs each).
It does not install anything and does not touch your games or drivers.

1. Unzip this folder anywhere (for example your Desktop). Keep about 1 GB free there.
2. Optional, saves a download: copy dlssnr-small.bin from an earlier kit's files\dlssnr-amd folder
   into this kit's files\dlssnr-amd folder. Without it the kit downloads it (about 110 MB) from the
   same place the OpenNR installer uses.
3. Close games and other heavy programs.
4. Double-click run-test.bat. If Windows says it protected your PC, click "More info", then
   "Run anyway".
5. Wait. It takes about 10 minutes.
6. When it says "Done", send us the one file it names:
   OpenNR-RX7000-diag6-result-<date>.zip (next to run-test.bat; the folder opens by itself).

If the window closes or the PC freezes, restart, then zip the "result" folder, send it and tell us
what happened.

You can delete the whole folder afterwards.
