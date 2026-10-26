// NETCASTER — 音效：主循环驱动的双轨调度（SFX 轨 + BGM 轨），不阻塞、不丢音
#pragma once
#include <stdint.h>

void audioInit();
void audioUpdate(uint32_t nowMs, bool bgmOn, bool tideOn);
void audioPlay(uint8_t sfxId);
void audioSetCombo(int combo);
void audioSetMute(bool m);
bool audioMuted();
