OpenNR RX 6000 confirmation kit

Thanks for running kit v8. We took what it found: the next OpenNR's INT8 network (the default on RX 6000 cards)
does the attention products of its first layers in 8-bit too, which was a little faster on your card with the
same picture quality. This kit runs the network exactly as that update will ship it, so we can check the picture
and the speed before it goes out. It works on RX 6400 up to RX 6950 XT, and RDNA2 laptops and handhelds.

One run checks:
  - your card and driver;
  - that every shader compiles (the FP16 network, the new INT8 one and 0.6.9's INT8 one);
  - the first start of each network, with the runtime's own shader check, as in a game;
  - that the picture is right (small model at 1280x720 and 1920x1080, large model at 1280x720, compared with the
    same picture made on an RX 9070 XT);
  - how fast each one is (the network's time per frame, two rounds, taking turns) and the time of each shader once.

It does not install anything and does not touch your games, drivers or settings. Everything stays inside this
folder.

What to do:
1. Unzip this folder anywhere (for example your Desktop). Keep about 1 GB free there.
2. Optional, saves a download: copy dlssnr-small.bin and dlssnr.bin from kit v8's files\dlssnr-amd folder into this
   kit's files\dlssnr-amd folder. Without them the kit downloads them (about 250 MB) from the same place the
   OpenNR installer uses.
3. Close games, browsers with videos and other heavy programs. Plug in a laptop.
4. Double-click run-test.bat.
   - If Windows says "Windows protected your PC" (SmartScreen), click "More info", then "Run anyway".
   - If Smart App Control blocks the kit's program, the test goes on as far as it can and says BLOCKED BY WINDOWS
     APP CONTROL. Please do not turn Smart App Control off for this. Just send the result.
5. Wait. It takes about 15 to 20 minutes (less on fast cards), plus the download. Please leave the window open and
   do not use the PC for games or videos meanwhile.
6. When it says "Done", send us the one file it names:
   OpenNR-RX6000-confirm-result-<date>.zip (next to run-test.bat; the folder opens by itself).

If the window closes or the PC freezes, restart, then zip the "result" folder, send it and tell us what happened.

You can delete the whole folder afterwards.
