# IKEA OBEGRÄNSAD on the nRF9151 DK

A clock on the 16x16 LED panel of the IKEA OBEGRÄNSAD, driven by an nRF9151 DK
that gets the time and time zone from the LTE network. Based on [ph1p/ikea-led-obegraensad](https://github.com/ph1p/ikea-led-obegraensad).

> Use these instructions at your own risk. Wrong wiring can damage the panel or
> the DK.

## Wiring

With the original microcontroller removed, the panel's driver board has six
solder pads: `GND`, `VCC`, `EN`, `IN`, `CLK`, `CLA`. Solder a wire to each and
connect them as follows:

| Panel | nRF9151 DK              | Function                              |
| ----- | ----------------------- | ------------------------------------- |
| GND   | GND (Arduino header)    | Common ground (required)              |
| VCC   | **external 5 V supply** | LED power, see [Power](#power)        |
| EN    | D6 (P0.06)              | Output enable, active low, PWM dimmed |
| IN    | D11 (P0.11)             | Serial data (SPI MOSI)                |
| CLK   | D13 (P0.13)             | Shift clock (SPI SCK)                 |
| CLA   | D10 (P0.10)             | Latch (SPI chip select)               |

```
 OBEGRÄNSAD panel                    nRF9151 DK
 ┌───────────┐                       ┌────────────────────┐
 │       GND ├───────────┬───────────┤ GND                │
 │       VCC ├──┐        │           │                    │
 │        EN ├──┼────────┼───────────┤ D6  (P0.06)        │
 │        IN ├──┼────────┼───────────┤ D11 (P0.11)        │
 │       CLK ├──┼────────┼───────────┤ D13 (P0.13)        │
 │       CLA ├──┼────────┼───────────┤ D10 (P0.10)        │
 └───────────┘  │        │           └────────────────────┘
                │        │
              ┌─┴────────┴─┐
              │ 5 V   GND  │  USB power supply
              └────────────┘
```

The panel is a chain of four SCT2024 16-bit LED drivers. IN and CLK are driven
by the SPI peripheral. CLA is driven by the SPI chip select: it is low while a
frame is shifted in and goes high afterwards, which latches the new frame. EN
is driven by PWM to set the brightness.

### Power

Power the panel from its own 5 V USB supply (for example the lamp's original
cable) instead of the DK. With all 256 LEDs on it draws more current than the
DK's 5 V pin should supply.

Connect the GND of the supply, the panel and the DK together, otherwise the
data signals have no reference.

Alternatively, power the panel and the DK from USB ports of the same PC. The
USB cables then provide the common ground. A PC USB port typically supplies
only 500–900 mA, which is enough at the default 25 % brightness but may not be
with higher brightness and many LEDs on.

If the patterns show up with stray pixels, the ground connection is missing or
poor: the panel then misreads some clock edges or data bits.

### Logic level: set VDD_IO to 3.3 V

The nRF9151 DK's GPIOs run at **1.8 V by default**. That is too low for the
panel's SCT2024 drivers. Do this once, before you connect the panel:

1. Connect the DK over USB and turn it on.
2. Start [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop).
3. Install and open the **Board Configurator** app.
4. Click **Select device** and pick the nRF9151 DK.
5. Set the nRF91 VDD (VDD_IO) to **3.3 V**.
6. Click **Write config**.

The board controller saves the setting, so it stays after power cycles and
reflashing. To check it, measure about 3.3 V between the DK's **VDD** pin on
the Arduino power header and GND.

3.3 V is also what the ESP32 builds use. If the panel flickers or stays dark,
add a 5 V level shifter (such as a 74AHCT125 powered from 5 V) on EN, IN, CLK
and CLA.

## Build and flash

Requires the nRF Connect SDK v3.4.1 (other v3.x versions should work too).
Install it with nrfutil:

```sh
nrfutil install sdk-manager
nrfutil sdk-manager install v3.4.1
```

`west` is only available inside the toolchain shell, so open it first, in
every new terminal:

```sh
nrfutil sdk-manager toolchain launch --ncs-version v3.4.1 --shell
```

Then build and flash from the project folder. The application is in `app/`,
because sysbuild names the app image after its folder and does not accept the
"Ä" in the project folder's name.

```sh
west build -p -b nrf9151dk/nrf9151/ns -d build app
west flash -d build --erase
```

`--erase` is only needed the first time, or after other firmware was on the
board. The build uses TF-M with a flash layout without a bootloader, like the
NCS cellular samples.

The UART log (VCOM0, `/dev/ttyACM0`, 115200 baud) shows the LTE connection and
when the time was received.

## The clock

The DK needs a SIM card and LTE-M or NB-IoT coverage.

- While it connects and waits for the time, a dot runs around the border.
- Then it shows the hours in the top half and the minutes in the bottom half.
  The bottom row fills up as the seconds of the current minute pass.

The time zone comes from the network as well. If the network sends the time
without a time zone, the clock shows UTC.

Brightness is limited to 25 % by `BRIGHTNESS_PERCENT` in `app/src/main.c`.
The panel driver is in `app/src/panel.c`.

## Troubleshooting

| Symptom                                       | Check                                                          |
| --------------------------------------------- | -------------------------------------------------------------- |
| Panel stays dark                              | VDD_IO set to 3.3 V, GND shared, panel has 5 V, EN on D6       |
| Only one pixel lit, nothing changes           | A signal pad shorted to GND (solder bridge), wrong GND pin     |
| Stray pixels, patterns mostly right           | Common ground between panel supply and DK, wire length         |
| Random pixels or flicker                      | CLK/IN/CLA wiring, wire length, level shifter                  |
| Digits are mirrored or rotated                | Panel is mounted rotated relative to `positions[]` in `app/src/panel.c` |
| Dot keeps running around the border           | SIM card, LTE coverage, UART log                               |
| Clock is off by whole hours                   | Network sends no time zone, clock shows UTC                    |
| `zsh: command not found: west`                | Open the toolchain shell first (see above)                     |
| Old firmware keeps running after `west flash` | Flash with `--erase`                                           |
