/*
 * sim_core_config.h
 *
 * simavr normally generates this header from its Makefile by test-compiling
 * every core in cores/ with avr-gcc.  This project vendors simavr and builds
 * it with CMake, and only ever needs a single core (the ATtiny85 of the
 * TinyJoypad), so the generated result is checked in instead - no avr-gcc
 * toolchain required to build the emulator.
 */
#ifndef __SIM_CORE_CONFIG_H__
#define __SIM_CORE_CONFIG_H__

#define CONFIG_SIMAVR_VERSION "1.7-tinyjoypad"

#define CONFIG_TINY85 1

#endif
