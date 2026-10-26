// NETCASTER — 渲染（不许 include M5 任何头）
#pragma once
#include <stdint.h>
#include "game.h"

void renderInit();
void renderFrame(uint16_t* fb, const Game& g, float timeSec);
