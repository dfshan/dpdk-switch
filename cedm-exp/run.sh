#!/bin/bash
set -u


./run-ecn-k-tso.sh ecn off off 2
./run-ecn-k-tso.sh ecn on off 2
./run-ecn-k-tso.sh ecn on on 2
./run-ecn-k-tso.sh ecn off off 3
./run-ecn-k-tso.sh ecn on off 3
./run-ecn-k-tso.sh ecn on on 3
