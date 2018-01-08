#!/bin/bash
set -u


ecn_ks="$(seq 5 5 100)"
./run-ecn-k-tso.sh dctcp off off 2 "$ecn_ks"
./run-ecn-k-tso.sh dctcp on off 2 "$ecn_ks"
./run-ecn-k-tso.sh dctcp on on 2 "$ecn_ks"
./run-ecn-k-tso.sh dctcp off off 3 "$ecn_ks"
./run-ecn-k-tso.sh dctcp on off 3 "$ecn_ks"
./run-ecn-k-tso.sh dctcp on on 3 "$ecn_ks"

ecn_ks="$(seq 5 5 300)"
./run-ecn-k-tso.sh ecn off off 2 "$ecn_ks"
./run-ecn-k-tso.sh ecn on off 2 "$ecn_ks"
./run-ecn-k-tso.sh ecn on on 2 "$ecn_ks"
./run-ecn-k-tso.sh ecn off off 3 "$ecn_ks"
./run-ecn-k-tso.sh ecn on off 3 "$ecn_ks"
./run-ecn-k-tso.sh ecn on on 3 "$ecn_ks"
