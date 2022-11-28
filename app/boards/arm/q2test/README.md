# Keychron Q2 `STM32L432` Experimental Support

Specifications:
 - `STM32L432KB` (128kB Flash, 64kB RAM)
 - 15 column x 5 row key matrix with a ROW -> COL (`row2col`) layout
 - DIP switch embedded in matrix at `(4,4)`
 - LED Matrix
 - 2x I2C `CKLED2001` LED Drivers

Current Progress:
 - [x] Flashing Over SWD
 - [x] Flashing Over `dfu-util`
 - [x] USB Enumeration
 - [x] Key Matrix Polling
 - [x] Encoder Support
 - [x] DIP Switch for Toggling Layers
 - [ ] I2C `CK2001LED` Drivers
 - [ ] Documentation
