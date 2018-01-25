# Introduction
This is a l2 learning switch based on DPDK.

# Features
* ECN marking
* Fine-grained queue length monitoring
* Shared Memory

# Requirements
* [DPDK](http://dpdk.org/) == 17.08.1
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

# References

Danfeng Shan, Wanchun Jiang, and Fengyuan Ren, "Analyzing and Enhancing Dynamic Threshold Policy of Data Center Switches", IEEE Transactions on Parallel and Distributed Systems (TPDS), 2017

Bibtex:

    @ARTICLE{7859368,
        author={D. Shan and W. Jiang and F. Ren},
        journal={IEEE Transactions on Parallel and Distributed Systems},
        title={Analyzing and Enhancing Dynamic Threshold Policy of Data Center Switches},
        year={2017},
        volume={28},
        number={9},
        pages={2454-2470},
        doi={10.1109/TPDS.2017.2671429},
        ISSN={1045-9219},
        month={Sept},}
