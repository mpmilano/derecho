#!/bin/bash
INSTALL_PREFIX="/usr/local"
if [[ $# -gt 0 ]]; then
    INSTALL_PREFIX=$1
fi

echo "Using INSTALL_PREFIX=${INSTALL_PREFIX}"

git clone https://github.com/mpmilano/mutils-tasks.git
cd mutils-tasks
git checkout e9584168390eb3fac438a443f3bb93ed692e972a
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} ..
make -j `lscpu | grep "^CPU(" | awk '{print $2}'`
make install
cd ../..
rm -rf mutils-tasks
