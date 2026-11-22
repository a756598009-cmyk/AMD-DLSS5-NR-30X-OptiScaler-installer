OpenNR RX 7000 speed kit

Thanks for testing the RX 7000 network. The flicker fix you helped with is in the released OpenNR. This kit
looks for speed: it times every step of the network on your card in a few ways, so we can pick the fastest
one that still gives the right picture. It does not install anything and does not touch your games or drivers.

What it runs, on a test picture at 6 sizes (648p to 4K):
 - the network as OpenNR runs it today;
 - the same network with a faster ViT attention step (we need to know at which sizes it is right on your card);
 - the network as ready-made machine code (ACO, as in kit 7), then a mix of both that it builds from your timings.

1. Unzip this folder anywhere (for example your Desktop). Keep about 1 GB free there.
2. Optional, saves a download: copy dlssnr-small.bin from an earlier kit's files\dlssnr-amd folder into this
   kit's files\dlssnr-amd folder. Without it the kit downloads it (about 110 MB) from the same place the
   OpenNR installer uses.
3. Close games and other heavy programs (browsers with video too).
4. Double-click run-test.bat. If Windows says it protected your PC, click "More info", then "Run anyway".
5. Wait. It takes about 30 minutes. The first run prepares the shaders, so it is slower.
6. When it says "Done", send us the one file it names:
   OpenNR-RX7000-speed-result-<date>.zip (next to run-test.bat; the folder opens by itself).
   Please also tell us your card and whether anything looked odd (black screen, driver reset).

If the window closes or the PC freezes, restart, then zip the "result" folder, send it and tell us what
happened.

You can delete the whole folder afterwards.
