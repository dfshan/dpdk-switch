# Introduction
This is a l2 learning switch based on DPDK.

# Features
* Fine-grained queue length monitoring
* Shared Memory

# Requirements
* [DPDK](http://dpdk.org/) >= 16.07
* [libConfuse](https://github.com/martinh/libconfuse) >= 2.7

# How to run it
1. Install requirements
1. Build and Setup DPDK library, including
    * Correctly set environment variable `RTE_SDK` and `RTE_TARGET`
    * Insert `IGB UIO` module, setup hugpage, bind Ethernet device to `IGB UIO` module

1. Create a configuration file (and change configurations in it if needed)

    ``$ cp switch.conf.example switch.conf``

1. Build and Run the application

    ``make``

    ``sudo ./build/app/main -c 0x7 --log-level=7 -- -p 0xf``
