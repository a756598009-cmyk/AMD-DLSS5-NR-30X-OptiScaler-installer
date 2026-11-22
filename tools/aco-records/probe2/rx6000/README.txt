OpenNR RX 6000 ACO probe 2

Thanks for running the first probe. It showed that the AMD driver on RX 6000 cards packs compiled shaders in a
different format from the one our code expects. This second probe finds out what that format is.

It asks the driver to compile two tiny test shaders and two shaders of the RX 6000 network, then saves the
driver's own compiled copy of each one (your card's own machine code, nothing personal) and loads it back to
check that works. Then it tries our test code in that format and in a few variations, and checks every result
number for number against the driver. It runs for about a minute.
It does not install anything and does not touch your games or drivers.

1. Unzip this folder anywhere (for example your Desktop).
2. Double-click run-test.bat. If Windows says it protected your PC, click "More info", then "Run anyway".
3. When it says "Done", send us the file it names:
   OpenNR-RX6000-aco-probe2-result.zip (next to run-test.bat).

If the screen goes black for a moment in step 2, the driver restarted itself after a test went wrong; that is
an answer too, and the zip is already saved. If the window closes or the PC freezes, restart, then send the zip
if it is there and tell us what happened.

You can delete the whole folder afterwards.
