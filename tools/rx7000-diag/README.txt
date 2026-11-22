OpenNR RX 7000 diagnostic kit

This finds out why the mochizuki NR picture flickers on an RX 7000 card at some resolutions.
It runs the OpenNR 0.5.8 mochizuki network on a test picture at 12 sizes (960x540 up to 2560x1440),
with both models, three times each, and compares every result with a picture made on an RX 9070 XT.
It also records each step inside the network, so we can see which step goes wrong, and tries four
experimental settings. It does not install anything and does not touch your games or drivers.

1. Unzip this folder anywhere (for example your Desktop). Keep about 1 GB free there.
2. Close games and other heavy programs.
3. Double-click run-test.bat. If Windows says it protected your PC, click "More info", then "Run anyway".
4. Wait. It takes about 45 to 90 minutes. The first time it downloads the model files (about 250 MB)
   from the same place the OpenNR installer uses. The GPU fans may spin up; that is normal.
   The window shows what it is doing; please leave it open.
5. When it says "Done", send us the one file it names:
   OpenNR-RX7000-diag-result-<date>.zip (next to run-test.bat; the folder opens by itself).

If the window closes or the PC freezes, restart, then zip the "result" folder, send it and tell us
what happened.

You can delete the whole folder afterwards.
