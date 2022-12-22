# Keychron Q2 `STM32L432` Experimental Support

![Image of Keychron Q2 Keyboard](https://cdn.shopify.com/s/files/1/0059/0630/1017/t/5/assets/keychron-q2blaick2-1635527456578.png?v=1635527462)

### Specifications:

 - `STM32L432KB` (128kB Flash, 64kB RAM)
 - 15 column x 5 row key matrix with a ROW -> COL (`row2col`) layout
 - DIP switch embedded in matrix at `(4,4)`
 - LED Matrix
 - 2x I2C `CKLED2001` LED Drivers
 - EC11 Encoder

### Current Progress:

 - [x] Flashing Over SWD
 - [x] Flashing Over `dfu-util`
 - [x] USB Enumeration
 - [x] Key Matrix Polling
 - [x] Encoder Support
 - [x] DIP Switch for Toggling Layers
 - [x] I2C `CK2001LED` Drivers (thanks to [XiNGRZ](https://github.com/xingrz) for the drivers)
 - [ ] Documentation (in progress)

### Flashing:

 - Download the latest build of the ZMK firmware, or compile it yourself
 - Enter "DFU" mode by holding down the RESET button under the space bar
 - Use a flashing tool, such as STM32CubeProgrammer, [WebDFU](https://devanlai.github.io/webdfu/dfu-util/), or [`dfu-util`](https://dfu-util.sourceforge.net) to flash the firmware onto the board over USB.

### Development:

1. Follow the [ZMK Documentation](https://zmk.dev/docs/development/setup) to set up your local build environment
2. To build the firmware, run:
```sh
west build -p -b keychron_q2
```
3. To build and flash the the firmware, hold down the RESET button while connecting the keyboard over USB. The run:
```sh
west build -p -b keychron_q2 && west flash
```

If you only built and did not flash, follow Step 3 in #Flashing to upload the firmware. The file should be in `/zmk/app/build/zephyr/zmk.bin`.

### Further Information:
 - [Key Matrix Schematic](./Keychron%20Q2.pdf)
 - [LED Schematic](./Q2_US_Leds_.pdf) (Thank you to [@lokher](https://github.com/lokher) for the schematic)
