OpenNR RX 7000 diagnostic kit 7 (ACO)

Thanks for running kit 6. Its fix is in the next OpenNR update. This kit tests something different: the
network as machine code made in advance by Mesa's ACO compiler (the one Linux uses on RX 7000 cards),
loaded through the AMD driver instead of being compiled by it. On the RX 9070 XT that is about 25%
faster, and it may also let the faster attention kernels of 0.5.8 run correctly on your card.

It runs two tiny ACO test shaders, the two attention steps alone (driver compiler and ACO), then the
whole network on a test picture three ways (driver compiler as in the update, ACO with the update's
kernels, ACO with 0.5.8's kernels): 4 sizes, 3 runs each, with the time the network takes.
It does not install anything and does not touch your games or drivers.

1. Unzip this folder anywhere (for example your Desktop). Keep about 1 GB free there.
2. Optional, saves a download: copy dlssnr-small.bin from an earlier kit's files\dlssnr-amd folder
   into this kit's files\dlssnr-amd folder. Without it the kit downloads it (about 110 MB) from the
   same place the OpenNR installer uses.
3. Close games and other heavy programs.
4. Double-click run-test.bat. If Windows says it protected your PC, click "More info", then
   "Run anyway".
5. Wait. It takes about 15 minutes (the first ACO run prepares its pipelines, so it is slower).
6. When it says "Done", send us the one file it names:
   OpenNR-RX7000-diag7-result-<date>.zip (next to run-test.bat; the folder opens by itself).

If the window closes or the PC freezes, restart, then zip the "result" folder, send it and tell us
what happened.

You can delete the whole folder afterwards.
