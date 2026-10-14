# PoulpyGun

Wemos D1 mini (ESP8266) prop: 130 NeoPixels and three buttons. It speaks the PoulpyTube ESP-NOW slot (10 channels) and sends button edges to whichever hub is the radio (PoulpyNode or, later, PoulpyLink).

Pins, already wired: data D1, button A D3, button B D4, trigger D5, grounds D6 D7 D8. Buttons are active low.

Firmware name in GET_INFO is `PoulpyGun`. Button command is `0xB0` (index, down, sequence).

```
pio run -e d1_mini
pio run -e d1_mini -t upload
```
