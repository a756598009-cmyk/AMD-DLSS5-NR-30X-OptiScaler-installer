OpenNR RX 6000 test kit (v6)

This tests the mochizuki NR network for RX 6000 cards (RX 6400 up to RX 6950 XT, and RDNA2
laptops and handhelds) before it goes into OpenNR. This round compares two ways of running it:
the AMD driver's own shader compiler, and ACO's machine code (the compiler Linux uses), which
OpenNR can load on RX 6000 now. It checks on your card:
  - what your driver offers (the card, the driver version, the features the network needs);
  - that AMD's shader compiler builds every shader of the network (each one in its own process,
    so a compiler crash cannot take the test down);
  - how long the first start takes on a driver that has never seen these shaders, the way
    OpenNR does it now and the way it did before, and with ACO;
  - that the picture is right: both models at 1152x648, 1280x720, 1920x1080 and 2560x1440,
    three times each, with the driver and with ACO, every result compared with the same picture
    made on an RX 9070 XT;
  - how fast it is with each: the network's time per frame (three rounds, the two alternating)
    and the time of each shader, with the GPU clock.

It does not install anything, and it does not touch your games, your drivers or their settings.
Everything stays inside this folder.

1. Unzip this folder anywhere (for example your Desktop). Keep about 2 GB free there.
2. Close games and other heavy programs.
3. Double-click run-test.bat.
   - If Windows says "Windows protected your PC" (SmartScreen), click "More info", then "Run anyway".
     The kit's programs are not signed, which is why Windows asks.
   - If Microsoft Defender or another antivirus removes or blocks a file from the "files" folder,
     the test cannot run. Allow the folder (or restore the file) and run it again, or just tell us.
     The test uses Windows' own rundll32.exe to compile the shaders in a separate process; some
     antivirus programs warn about that.
4. Wait. It takes about 25 to 40 minutes. It downloads the two model files (about 250 MB) from
   the same place the OpenNR installer uses, and it compiles the shaders several times. The GPU
   fans may spin up; that is normal. Every step has a time limit, so a step that hangs is stopped
   and the test goes on. Please leave the window open.
   The ACO part comes after the driver part and the window says when it starts. If the PC
   freezes or the screen goes black there, restart, zip the "result" folder and send it: the
   driver results are already in it.
5. When it says "Done", send us the one file it names:
   OpenNR-RX6000-result-<date>.zip (next to run-test.bat; the folder opens by itself).
   It holds text logs, your card and driver names and small pictures of the results. Nothing
   else from your PC.

If the window closes or the PC freezes, restart, then zip the "result" folder, send it and tell us
what happened.

You can delete the whole folder afterwards.
