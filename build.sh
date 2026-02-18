#!/bin/bash
cd build
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=1
make
cp ../src/shader.vert .
cp ../src/shader.frag .
cp ../src/bg.frag .
cp ../src/bg.vert .

cp ../src/fonts/JetBrainsMono-Bold.ttf .
cp ../src/fonts/JetBrainsMono-Italic.ttf .
cp ../src/fonts/JetBrainsMono-BoldItalic.ttf .
cp ../src/fonts/JetBrainsMono-Medium.ttf .