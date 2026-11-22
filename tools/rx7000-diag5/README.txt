OpenNR RX 7000 diagnostic kit 5

Thanks for running kit 4. It showed that the first piece of the mochizuki network that goes wrong on
your card is the attention step at the 512-channel level, even when it runs alone. This kit runs that
step alone in 16 versions (the released one, one rewritten without the matrix instructions, and the
released one with one thing changed each), then the whole network with the rewritten attention against
the released 0.5.8 one on a test picture (4 sizes, 3 runs each).
It does not install anything and does not touch your games or drivers.

1. Unzip this folder anywhere (for example your Desktop). Keep about 1 GB free there.
2. Optional, saves a download: copy dlssnr-small.bin from kit 1's, 2's or 4's files\dlssnr-amd
   folder into this kit's files\dlssnr-amd folder. Without it the kit downloads it (about 110 MB)
   from the same place the OpenNR installer uses.
3. Close games and other heavy programs.
4. Double-click run-test.bat. If Windows says it protected your PC, click "More info", then
   "Run anyway".
5. Wait. It takes about 10 minutes.
6. When it says "Done", send us the one file it names:
   OpenNR-RX7000-diag5-result-<date>.zip (next to run-test.bat; the folder opens by itself).

If the window closes or the PC freezes, restart, then zip the "result" folder, send it and tell us
what happened.

You can delete the whole folder afterwards.
