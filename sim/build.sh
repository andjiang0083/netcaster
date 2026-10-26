#!/bin/bash
# 编译 PC 模拟器并导出关键帧
set -e
cd "$(dirname "$0")"
g++ -O2 -std=c++11 -Wall -Wextra -Wno-unused-parameter \
    -I ../src sim.cpp ../src/game.cpp ../src/render.cpp -o netcaster_sim
./netcaster_sim frame
python3 - <<'PY'
from PIL import Image
import glob, os
for p in sorted(glob.glob("frame_*.ppm")):
    im = Image.open(p)
    im.save(p.replace(".ppm", ".png"))
    im.resize((im.width*4, im.height*4), Image.NEAREST).save(p.replace(".ppm", "_4x.png"))
    print(p, im.size)
PY
