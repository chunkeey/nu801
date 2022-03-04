# NU801 Userspace controller program

## Introduction

This project aims to provide a userspace controller program that can
program the NUMEN Tech. NU801 Chip without the need of any kernel
module. For this purpose it registers the defined LEDs with the help
of the Userspace LED feature and controls them via the established
GPIO V2 Interface.

## Requirements

This program needs a 5.10+ linux kernel and the following config symbols:

 * `CONFIG_GPIO_CDEV`
 * `CONFIG_LEDS_USER`

## Build

 cmake .

 make

 ./nu801 "Board Name" 
 i.e. `./nu801 "cisco-mx100-hw"`
 
## Supported Hardware

Currently, only the Cisco Meraki MX100 is getting supported. It works on Debian
and OpenWrt. Cisco Meraki MR18 works too, but has no upstream device.

## Options
By default, the project will be built as "Release". To build a STATIC version of
this program select the `BUILD_STATIC_PROGRAM` cmake option.
