#!/bin/bash

set -x

if [-d "./build" ]; then
    rm `pwd`/build/* -rf
else
    mkdir build
fi

cd `pwd`/build &&
    cmake .. &&
    make