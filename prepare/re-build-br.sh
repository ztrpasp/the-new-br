#!/bin/bash
cd ./the-new-br
rm -rf build
meson setup build
meson compile -C build
cd ..

