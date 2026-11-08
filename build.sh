#!/usr/bin/env bash

if [ ! -d "./build/bin" ]; then
    mkdir -p ./build/bin
fi
if [ -e "./build/bin/nob" ]; then
    ./build/bin/nob
else 
    cc -O3 -Wall -Wextra -o ./build/bin/nob nob.c && ./build/bin/nob
fi
