# l0destar v1.0 prototype changelog

## 30/05/2026

- created initial version of the schematics and v1.0 prototype design

## 07/06/2026

- fixed the voltage divider that generates the ignition sense signal so that
  it's reliably 3.3v when high and 0v when off. this is because the nRF9151 docs
  say that it's not a good idea to leave signal pins floating around in the
  inbetween section where the pin isn't ambiguously high or low
- added a 4.2v buck converter with an ideal diode to power the MakerDiary
  ConnectKit board. This is so that we can have the device hardwired to power in
  the vehicle but still connect the USB cable for programming. Powering it
  through the 5v input rail (VBUS) doesn't allow this, the docs explicitly say
  not to connect both the 5v supply rail and USB at the same time. The LM66100
  avoids any current flowing back from the charging circuit into the buck but
  the BQ25180 charger can be turned off anyway.
