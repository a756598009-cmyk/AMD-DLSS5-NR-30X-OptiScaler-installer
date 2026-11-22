OpenNR RX 6000 ACO probe

A small test for the RX 6000 version of the mochizuki network. It checks whether the AMD driver on your
card accepts shader machine code made in advance (by Mesa's ACO compiler, the one Linux uses) instead of
compiling the shaders itself. Two tiny test shaders run both ways and their results are compared number
for number. No model, no network, nothing big: it runs for a few seconds.
It does not install anything and does not touch your games or drivers.

1. Unzip this folder anywhere (for example your Desktop).
2. Double-click run-test.bat. If Windows says it protected your PC, click "More info", then "Run anyway".
3. When it says "Done", send us the file it names:
   OpenNR-RX6000-aco-probe-result.txt (next to run-test.bat).

If the screen goes black for a moment, the driver restarted itself after a test shader went wrong; that is
an answer too. If the window closes or the PC freezes, restart and tell us what happened.

You can delete the whole folder afterwards.
