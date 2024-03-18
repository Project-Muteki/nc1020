# GGV NC1020 emulator for Besta RTOS (Arm)

"Closed system no game" my a$$. /s

## Details

This is a port of the ["cross-platform" version of NC1020 emulator](https://github.com/Wang-Yue/NC1020), originally developed by Wang-Yue (which is based on some Chinese code of unknown origin). I made it even more cross-platform by introducing a HAL layer that handles page I/O, and wrote a HAL implementation targeting the toolchain I developed for Besta RTOS. When the emulator is built in release mode and on scenes where page swapping is light, it can manage about 50% speed on weak Arm7-based machines like BA110 and more than 100% on powerful machines like BA742.

The project is in proof-of-concept stage and quite a lot of things needs to be finished and/or can be optimized further.

In the future, I may also branch off the NC1020 emulator used here into a standalone library (à la TamaLIB) so it can be easily ported to even more platforms, potentially including bare-metal targets.

## Notes on the ROM format

This port uses a simplified, slightly different ROM format than the typical one used by most emulators. They can be generated with the included script under scripts/gen_simplified.py.
