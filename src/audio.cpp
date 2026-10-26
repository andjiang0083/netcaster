#include "audio.h"
#include "game.h"
#include <M5Cardputer.h>

// 回到已经验证过的那套：tone() + 双轨 + 主循环驱动（用户确认过"能一直听"）。
// 只把 SFX 音色表换成新的（落空闷噗 / 挣脱三段 / 捕获按鱼种分档）。
// 教训：BGM 不能改成"随机间距生成式"，节奏必须固定；PCM/playRaw 这条路
// 受硬件 48kHz 限制（RAM 放不下），也不适合这块板。
struct Seq { uint16_t f[8]; uint8_t n; uint8_t ms; };

static const Seq TONES[] = {
  { {0},                                             0,  0  },  // NONE
  { {880, 1175, 1568},                               3, 65  },  // COININ 投币
  { {2400},                                          1, 45  },  // AIM
  { {1800, 1400, 1050, 780, 600},                    5, 45  },  // FIRE 下扫
  { {2200, 900, 1900, 800, 1600, 700, 520, 400},     8, 55  },  // ESCAPE 网绷→挣扎→下坠
  { {1320, 1760, 2340, 1050, 1400, 1800, 2200, 2700}, 8, 45 }, // CATCH 小鱼/鳗鱼
  { {1050,1400,1800,2200,2700,3200,3600},            7, 45  },  // COINDROP 备用
  { {300, 200, 260, 190},                            4, 65  },  // EMPTY 金币不足：低沉两下，像机器把卡退回来
  { {1320, 1760},                                    2, 45  },  // BET
  { {280},                                           1, 60  },  // MISSHOT 落空闷噗
  { {1568, 2093, 2637, 1568, 2093, 2637, 3136, 3520}, 8, 45 }, // CATCH_MID 中鱼/金鱼
  { {294, 392, 494, 587, 784, 988, 1175, 1480},      8, 50  },  // CATCH_BIG 怪鱼
  { {300, 420, 560, 700},                            4, 75  },  // TIDE 潮汐来了：涌动上扬
  { {640, 480, 360},                                 3, 80  },  // TIDE_END 潮水退去
  { {900, 1300, 700, 1500, 520},                     5, 60  },  // DIVER 抓到潜水员：上窜下跳
  { {2400, 1900, 1400, 950, 700},                    5, 30  },  // WHOOSH 抛投：一口气往下掉，像东西擦过空气
  { {1760, 1320, 1760},                              3, 38  },  // PAY 扣款：高-低-高，收银机/柜员机的"叮嗒"
};
static const int TONE_COUNT = (int)(sizeof(TONES)/sizeof(TONES[0]));

// BGM：卡农变奏（旋律 + 三音琶音交替，即阿尔贝蒂低音的写法）。
// 和声走 D A Bm F#m G D G A —— 卡农最经典的八小节进行；
// 每小节一个旋律音 + 三个琶音音，三遍变奏只换"重心"和"八度"，
// 和声骨架始终不动，这就是卡农变奏曲的基本形制。
// 音区 220~880Hz，全部落在 1W 小腔体推得动的范围。
// 全曲 96 个音符事件 ≈ 16.7 秒一圈。
struct BgmNote { uint16_t f; uint16_t ms; };
static const BgmNote BGM[] = {
  // 遍 1：旋律 → 琶音（主题陈述）
  {740,280},{587,140},{740,140},{880,140},{659,280},{440,140},{554,140},{659,140},
  {587,280},{494,140},{587,140},{740,140},{554,280},{370,140},{440,140},{554,140},
  {494,280},{392,140},{494,140},{587,140},{440,280},{587,140},{740,140},{880,140},
  {494,280},{392,140},{494,140},{587,140},{554,280},{440,140},{554,140},{659,140},
  // 遍 2：琶音 → 旋律（重心换到伴奏，追逐感）
  {587,140},{740,140},{880,140},{740,280},{440,140},{554,140},{659,140},{659,280},
  {494,140},{587,140},{740,140},{587,280},{370,140},{440,140},{554,140},{554,280},
  {392,140},{494,140},{587,140},{494,280},{587,140},{740,140},{880,140},{440,280},
  {392,140},{494,140},{587,140},{494,280},{440,140},{554,140},{659,140},{554,280},
  // 遍 3：旋律降八度（退潮，收束）
  {370,300},{587,130},{740,130},{880,130},{330,300},{440,130},{554,130},{659,130},
  {294,300},{494,130},{587,130},{740,130},{277,300},{370,130},{440,130},{554,130},
  {247,300},{392,130},{494,130},{587,130},{220,300},{587,130},{740,130},{880,130},
  {247,300},{392,130},{494,130},{587,130},{277,300},{440,130},{554,130},{659,130},
};
static const int BGM_COUNT = (int)(sizeof(BGM)/sizeof(BGM[0]));

// 潮汐 BGM：还是 D 做根音，但翻成小调五声（D F G A C）。
// 同根音切换不会跑调，只是整个场子一下子"暗"下来 —— 玩家不用看屏幕就知道潮来了。
// 结构是一波潮水：低音爬升 → 涌 → 浪峰盘旋 → 退 → 暗流 → 回到起点。
// 8 小节 × 8 音 × 130ms ≈ 8.3 秒（卡农 16.7 秒的一半，重复更紧、更有压迫感）。
// 每小节首音压到低八度，像潮水从底下顶上来。
static const BgmNote TIDE_BGM[] = {
  // 1 起：低音爬升
  {147,180},{349,130},{392,130},{440,130},{392,130},{349,130},{294,130},{262,130},
  // 2 涌
  {294,150},{392,130},{440,130},{523,130},{440,130},{392,130},{349,130},{294,130},
  // 3 浪峰
  {262,180},{523,130},{587,130},{698,130},{587,130},{523,130},{440,130},{392,130},
  // 4 浪峰盘旋：高音被顶住
  {523,150},{587,130},{698,130},{784,130},{880,130},{784,130},{698,130},{587,130},
  // 5 开始退
  {440,170},{523,130},{440,130},{392,130},{349,130},{392,130},{440,130},{523,130},
  // 6 退
  {392,150},{440,130},{392,130},{349,130},{294,130},{349,130},{392,130},{440,130},
  // 7 暗流：最低的一层
  {165,200},{262,130},{294,130},{330,130},{349,130},{330,130},{294,130},{262,130},
  // 8 回到起点
  {196,180},{294,130},{349,130},{392,130},{440,130},{392,130},{349,130},{294,130},
};
static const int TIDE_BGM_COUNT = (int)(sizeof(TIDE_BGM)/sizeof(TIDE_BGM[0]));

// 连击音阶：每多抓一条，捕获音整体升半音（最多 8 个半音）
static const uint16_t SEMI[9] = {1000,1059,1122,1189,1260,1335,1414,1498,1587};
static int      sCombo = 1;
void audioSetCombo(int c) { sCombo = (c < 1) ? 1 : (c > 8 ? 8 : c); }

static bool     sMute = false;
static uint8_t  sCur = 0;        // 当前 SFX
static uint8_t  sIdx = 0;
static uint32_t sNext = 0;
static uint8_t  sBgmIdx = 0;
static uint32_t sBgmNext = 0;
static bool     sBgmWasOn = false;

static bool isHighPrio(uint8_t id) {
  return id == SFX_FIRE || id == SFX_CATCH || id == SFX_CATCH_MID ||
         id == SFX_CATCH_BIG || id == SFX_COINDROP || id == SFX_ESCAPE;
}

void audioInit() {
  M5.Speaker.begin();
  M5.Speaker.setVolume(80);      // Cardputer 小腔体基线（StickS3 同档约 46）
}

void audioPlay(uint8_t id) {
  if (sMute || id == 0 || id >= TONE_COUNT) return;
  // 高优先级立刻抢占；低优先级只在空闲时播
  if (isHighPrio(id) || sCur == 0) {
    sCur = id;
    sIdx = 0;
    sNext = 0;
  }
}

static bool sBgmTide = false;
void audioUpdate(uint32_t nowMs, bool bgmOn, bool tideOn) {
  if (sMute) return;

  // SFX 轨（ch0）
  if (sCur) {
    if (nowMs >= sNext) {
      const Seq& s = TONES[sCur];
      if (sIdx < s.n) {
        uint16_t f = s.f[sIdx];
        if (sCur == SFX_CATCH || sCur == SFX_CATCH_MID || sCur == SFX_CATCH_BIG)
          f = (uint16_t)((uint32_t)f * SEMI[sCombo-1] / 1000u);
        M5.Speaker.stop(0);
        M5.Speaker.tone(f, s.ms, 0);
        sNext = nowMs + s.ms + 5;
        sIdx++;
      } else sCur = 0;
    }
  }

  // BGM 轨（ch1）：固定 110ms 琶音，只在 RADAR/FLY 出声
  if (!bgmOn) { sBgmWasOn = false; return; }
  if (!sBgmWasOn) { sBgmWasOn = true; sBgmNext = nowMs; }
  // 潮汐来了就换曲：同根音小调，不会跑调，只是突然"暗"下来
  if (tideOn != sBgmTide) {
    sBgmTide = tideOn;
    sBgmIdx  = 0;
    sBgmNext = nowMs + 120;      // 留一点缝，别把上一首的尾音咬断
  }
  if (nowMs >= sBgmNext) {
    const BgmNote* cur = sBgmTide ? TIDE_BGM : BGM;
    int curN = sBgmTide ? TIDE_BGM_COUNT : BGM_COUNT;
    const BgmNote& n = cur[sBgmIdx];
    if (n.f) {
      M5.Speaker.stop(1);
      M5.Speaker.tone(n.f, n.ms, 1);
    }
    sBgmNext = nowMs + n.ms + 5;
    sBgmIdx = (uint8_t)((sBgmIdx + 1) % curN);
  }
}

void audioSetMute(bool m) {
  sMute = m;
  if (m) M5.Speaker.stop();
}

bool audioMuted() { return sMute; }
