# l0destar v1.0 bench test unit - ISO-9141 version

## Overview

NOTE: THIS HAS NOT BEEN TESTED, USE AT YOUR OWN RISK

This is a bench test PCB designed for use with the Nordic nRF9151 Dev Kit. It
has all of the ancillary electronics with pin headers intended for connection to
the dev kit:

 - ~12v power input
 - 3.3V buck powering a 3V relay which gates the 12V supply to the rest of the
   board
 - All power rails (including 12v) gated by the presence of the 3.3v rail from
   the dev kit using a 3V relay (see below)
 - INA228 voltage monitoring
 - Manual switch to simulate the ignition signal
 - Relay power source switching
 - 5V buck converter for the ISO-9141
 - Auxillary power rails controlled by the AUX_SW signal
 - Accelerometer (pin headers to accept the STEVAL-MKI212V1 accelerometer eval
   board
 - Dual L9637D chips on DIP adapters to simulate K-line communications

## Power supply

The 12V input goes via an initial relay stage which gates power to the rest of
the board. The 3.3V rail from the DK must be present in order for everything on
the test board to power on. This is because having voltage on any of the GPIO
signal lines while the DK is powered off is not recomended by Nordic. This
configuration means that as soon as the 3.3V rail from the Nordic board goes off
the power is cut to the entire accessory board. On my oscilloscope and with the
specific level shifters I got from amazon (not linked below as they're no longer
available) repeated tests of the falling edge showed no spikes at all and the
lines on the LV side of the shifter were always below the 3.3V reference rail.
However I would strongly encourage anyone considering using this board to do
their own testing with the specific parts they end up using. The Nordic board
GPIOs are only specced for ~0.3v over their VDDIO voltage, if the board is
unpowered that VDDIO is 0v.

## Parts list

| Part                                                                                   | Quantity |
|----------------------------------------------------------------------------------------|----------|
| [3V relay](https://amzn.to/43xMQR2)                                                    | 1        |
| [3.3v buck converter](https://amzn.to/4fIUC1W)                                         | 1        |
| [RT424F12 12V bistable relay](https://amzn.to/4egbXN5)                                 | 1        |
| [STEVAL-MKI212V1 accelerometer eval board](https://www.st.com/en/evaluation-tools/steval-mki212v1.html) | 1 |
| [L9637D](https://amzn.to/3QFI7Ka)                                                      | 2        |
| [INA228 breakout](https://amzn.to/4efyaed)                                             | 1        |
| [5V buck converter](https://amzn.to/4uwe5Gy)                                           | 1        |
| [5mm switch](https://amzn.to/3S40cCg)                                                  | 1        |
| [Level shifter](https://amzn.to/4apB7I4)                                               | 1        |
| [Pin headers](https://amzn.to/3Qm8qoH)                                                 | Various  |
| [510R resistor](https://amzn.to/4gc8ZvK)                                               | 1        |
| [4.7K resistor](https://amzn.to/4gc8ZvK)                                               | 2        |
| [10K resistor](https://amzn.to/4gc8ZvK)                                                | 9        |
| [56K resistor](https://amzn.to/4gc8ZvK)                                                | 2        |
| [100K resistor](https://amzn.to/4gc8ZvK)                                               | 7        |
| [MOSFET - ZVN4206A](https://amzn.to/3S775mb)                                           | 4        |
| [TVS Diode - P6KE24CA](https://amzn.to/4aLxvAd)                                        | 1        |
| [MOSFET - DMG3415U](https://amzn.to/4uvGYCY)                                           | 2        |
| [SOT-23 adapter](https://amzn.to/4fIxoc5)                                              | 2        |
| [100nF ceramic capacitor](https://amzn.to/4xpB60v)                                     | 5        |
| [10uF ceramic capacitor](https://amzn.to/4xpB60v)                                      | 2        |
| [Diode - 1N4148](https://amzn.to/4vPo4bi)                                              | 2        |
| [Molex 43045-0400 4-pin receptacle](https://uk.farnell.com/molex/43045-0400/conn-r-a-hdr-4pos-2row-3mm/dp/9733019) | 1 |

## Notes

- The TVS diode isn't strictly necessary on a bench
- I mounted the DMG3415Us on SOT-23 adapters but there's no reason you need to
  if you'd rather surface mount them
- The breakouts can either be soldered directly or mounted on PCB pin headers, I
  would suggest the latter for ease of re-use
- Any buck converters with stable output will work. The 3.3V one is necessary
  to drive the relay coils because they draw over 100mA and I wasn't comfortable
  putting that load on the Nordic VDDIO rail comfortable putting that load on
  the Nordic VDDIO rail. I positioned the 2pin connector footprints for the
  modules I bought, if you buy different ones you'll need to adjust them
- I used a 4-pin Molex receptable because it's what I had to hand but only two
  of the pins are needed
- Never plug or unplug anything into the Nordic dev board while anything is
  powered (I killed at least one dev board this way)
- This PCB has not been tested or validated at all yet, I would strongly
  recommend carefully testing it (ideally with an oscilloscope) before
  connecting it to the dev board
