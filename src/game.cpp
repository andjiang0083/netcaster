#include "game.h"
#include <math.h>

// ---- v0.2 玩法参数 ----
// 经济重做：分值整体下调一个数量级；大鱼改成多段血，每次命中给伤害分，
// 打完最后一格才捕获。旧版"小鱼 10 分 × 0.92 捕获率 = 6 倍成本"是钱爆炸的根因。
static const int     KW[FK_COUNT]     = {22, 28, 54, 44, 60, 92, 26, 110, 18};
static const int     KH[FK_COUNT]     = {11, 14, 10, 22, 30, 40, 42, 46, 30};
// 捕获收益（金币）：小鱼是陷阱，贵鱼才赚
static const int     KSCORE[FK_COUNT] = {16, 43, 28, 85, 190, 140, 50, 150, 10};   // 小鱼只够温饱，暴富靠大鱼
// 捕获率：面积越大越好打，但越难抓（鱼越大越有力气挣网）
// 捕鱼达人式判定（不许血条）：nCatchPower = (rand%100+1) × 炮弹权值 × 系统权值，
// 大于"鱼权值"就捕获。权值越大越难抓。
static const int     KWGT[FK_COUNT]   = { 13, 48, 37, 60, 65, 64, 52, 68, 0 };

// 炮台等级：成本与炮弹权值同步放大（原版 1~7 级 0.1~0.7，这里压成 3 级）
const int            CANNON_COST[3]   = { 4, 10, 18 };
static const float   CANNON_POWER[3]  = { 0.19f, 0.80f, 1.70f };   // 低炮只够糊口，高炮才吃得到大鱼

// ===== 道具（4/5/6）：都不掷骰子，方差由玩家的时机/位置/节奏决定 =====
#define ITEM_NET      0     // 渔网：撒出去，范围内全部必中
#define ITEM_DECOY    0     // 打窝：唯一的道具（网和静电已删）
#define ITEM_STUN     2     // 静电：全场鱼按慢，让你从容瞄
const int          ITEM_COST[3] = { 25, 0, 0 };       // 打窝 25：约一条半小鱼，敢打但白打了会心疼
static const float ITEM_CD[3]   = { 6.f, 0.f, 0.f };   // 冷却 6 秒：窝是"等你来"，不是连点
#define DECOY_REACH   150.f
#define DECOY_R       150.f   // 吸引半径
#define DECOY_LIFE      9.0f  // 一个窝能撑 9 秒：抛 0.4 + 聚 4 + 停留 4.6
#define DECOY_FLY       0.40f // 饵团在空中的时间
#define DECOY_REACH   150.f
static const float   KSPEED[FK_COUNT] = {26.f, 20.f, 42.f, 14.f, 9.f, 11.f, 46.f, 32.f, 22.f};

#define BULLET_SPEED 420.f
#define BULLET_MAXD  340.f
#define TRACK_RATE   0.f            // 无辅助瞄准：打偏就是打偏
#define TRACK_CONE   0.f

#define CATCH_HOLD   0.32f
#define CATCH_TOTAL  1.30f
#define MISS_TOTAL   0.90f

int fishWorldW(uint8_t k)    { return KW[k]; }
int fishWorldH(uint8_t k)    { return KH[k]; }
int fishBaseScore(uint8_t k) { return KSCORE[k]; }

static inline uint32_t rnd(Game& g) {
  g.rng ^= g.rng << 13; g.rng ^= g.rng >> 17; g.rng ^= g.rng << 5; return g.rng;
}
static inline float rndf(Game& g) { return (rnd(g) & 0xFFFF) * (1.0f/65535.0f); }

static void pushSfx(Game& g, uint8_t id) {
  if (g.sfxCount < SFX_QUEUE) g.sfx[g.sfxCount++] = id;
}

bool gamePopSfx(Game& g, uint8_t& out) {
  if (g.sfxCount == 0) return false;
  out = g.sfx[0];
  for (int i = 1; i < g.sfxCount; i++) g.sfx[i-1] = g.sfx[i];
  g.sfxCount--;
  return true;
}

static float norm180(float d) {
  while (d >  180.f) d -= 360.f;
  while (d < -180.f) d += 360.f;
  return d;
}

// 潮汐：整群鱼从同一侧密密地涌过去。期间没有大鱼，但每条价值翻倍
// （"一网收多条"的隐喻），否则一堆 60 分的小鱼只会倒扣钱。
static void spawnTideFish(Game& g, int i) {
  Fish& f = g.fish[i];
  float r = rndf(g);
  uint8_t k = (r < 0.55f) ? FK_SMALL : (r < 0.85f) ? FK_EEL : FK_MID;
  int side = g.tideDir ? 1 : -1;
  f.kind = k;
  f.x = CANNON_X - side * (150.f + rndf(g) * 190.f);
  f.y = 20.f + rndf(g) * 300.f;
  f.dir = side;
  float sp = KSPEED[k] * (1.15f + rndf(g) * 0.35f);
  f.vx = side * sp;
  f.vy = (rndf(g) - 0.5f) * 6.f;
  f.alive = 1;
  f.scared = 0.f;
  f.stun = 0.f;
  f.bob = rndf(g) * 6.283f;
  f.wobble = rndf(g) * 6.283f;
}

// 大鲨鱼：独立事件，和潮汐错开。体型巨大、极难抓、极贵。
// 它一出现全场小鱼都会惊慌逃散，气势比"多几条鱼"强得多。
static void spawnShark(Game& g) {
  int slot = -1;
  for (int i = 0; i < MAX_FISH; i++) if (!g.fish[i].alive) { slot = i; break; }
  if (slot < 0) {
    float far = -1.f;
    for (int i = 0; i < MAX_FISH; i++) {
      float dx = g.fish[i].x - CANNON_X, dy = g.fish[i].y - CANNON_Y;
      float d = dx*dx + dy*dy;
      if (d > far) { far = d; slot = i; }
    }
  }
  if (slot < 0) return;
  Fish& f = g.fish[slot];
  f.kind  = FK_SHARK;
  f.dir   = (rndf(g) < 0.5f) ? 1 : -1;
  f.x     = CANNON_X - f.dir * 300.f;
  f.y     = 55.f + rndf(g) * 190.f;
  f.vx    = f.dir * KSPEED[FK_SHARK];
  f.vy    = (rndf(g) - 0.5f) * 8.f;
  f.alive = 1;
  f.scared = 0.f; f.stun = 0.f;
  f.bob = 0.f; f.wobble = rndf(g) * 6.283f;
  g.sharkOn = 1;
  g.sharkNext = 100.f + rndf(g) * 70.f;
  for (int i = 0; i < MAX_FISH; i++)
    if (g.fish[i].alive && g.fish[i].kind != FK_SHARK) g.fish[i].scared = 2.6f;
  pushSfx(g, SFX_TIDE);        // 低频涌动，复用潮汐音
}

// 潜水员：恶搞彩蛋。必抓、只值 100（刚好回本），但抓到会弹一句 OOPS。
// 每局只有 10% 概率出现一次，藏得浅一点（用户明确要求别太深）。
static void spawnDiver(Game& g) {
  int slot = -1;
  for (int i = 0; i < MAX_FISH; i++) if (!g.fish[i].alive) { slot = i; break; }
  if (slot < 0) return;
  Fish& f = g.fish[slot];
  f.kind = FK_DIVER;
  f.dir  = (rndf(g) < 0.5f) ? 1 : -1;
  f.x    = CANNON_X - f.dir * 240.f;
  f.y    = 80.f + rndf(g) * 140.f;
  f.vx   = f.dir * KSPEED[FK_DIVER];
  f.vy   = (rndf(g) - 0.5f) * 10.f;
  f.alive = 1;
  f.scared = 0.f; f.stun = 0.f;
  f.bob = 0.f; f.wobble = rndf(g) * 6.283f;
  g.diverOn = 1;
  g.diverNext = 9999.f;          // 每局只来一次
}

static void spawnFish(Game& g, int i) {
  if (g.tideOn) { spawnTideFish(g, i); return; }
  Fish& f = g.fish[i];
  float r = rndf(g) * 10.0f;
  // 新手头 90 秒：小鱼从 34% 提到 52%。否则开局满海都是打不动的大鱼，
  // 新玩家只会觉得"这游戏打不中东西"。
  float sw = (g.playSec < 90.f) ? 5.2f : 3.4f;
  uint8_t k = (r < sw)          ? FK_SMALL
            : (r < sw + 2.6f)   ? FK_MID
            : (r < sw + 4.1f)   ? FK_EEL
            : (r < sw + 5.4f)   ? FK_GOLD
            : (r < sw + 6.0f)   ? FK_SQUID
            : (r < sw + 6.4f)   ? FK_MANTA : FK_MON;
  f.kind = k;
  // 小鱼成群：60% 概率挂到已有的鱼群里，贴着同伴出生、同向游
  f.group = 255;
  if (k == FK_SMALL && rndf(g) < 0.60f) {
    for (int j = 0; j < MAX_FISH; j++) {
      if (j == i || !g.fish[j].alive || g.fish[j].kind != FK_SMALL) continue;
      f.group = g.fish[j].group;
      if (f.group == 255) f.group = g.fish[j].group = (uint8_t)(1 + (j % 7));
      f.x = g.fish[j].x + (rndf(g) - 0.5f) * 54.f;
      f.y = g.fish[j].y + (rndf(g) - 0.5f) * 32.f;
      f.vx = g.fish[j].vx; f.vy = g.fish[j].vy; f.dir = g.fish[j].dir;
      f.alive = 1; f.scared = 0.f; f.stun = 0.f;
      f.bob = rndf(g) * 6.28f; f.wobble = 0.f;
      return;
    }
    f.group = (uint8_t)(1 + (i % 7));
  }

  float a = (-168.0f + rndf(g) * 156.0f) * (3.14159265f/180.0f);
  float R = 110.0f + rndf(g) * 220.0f;
  f.x = CANNON_X + cosf(a) * R;
  f.y = CANNON_Y + sinf(a) * R;
  f.dir = (i & 1) ? -1 : 1;          // 保证场上左右都有
  float sp = KSPEED[k] * (0.8f + rndf(g) * 0.5f);
  f.vx = f.dir * sp;
  f.vy = (rndf(g) - 0.5f) * sp * 0.5f;
  f.alive = 1;
  f.scared = 0.f;
  f.stun = 0.f;
  f.bob = rndf(g) * 6.283f;
  f.wobble = rndf(g) * 6.283f;
}

// 鱼获本的静态备份：gameInit 会 memset 整个 Game，但鱼获是跨局累计的，
// 所以单独存一份，在 gameInit 末尾灌回去。
static uint16_t sLogC[FK_COUNT];
static uint32_t sLogV[FK_COUNT];
static uint32_t sLogTF = 0, sLogTV = 0;
static uint32_t sPlaySec = 0;     // 总游戏时长（累计秒）
static int sPeakWallet = 0;       // 历史最高金币余额

void gameInit(Game& g, int bestScore) {
  for (int i = 0; i < (int)sizeof(Game); i++) ((uint8_t*)&g)[i] = 0;
  g.rng = 0x2F6B96u ^ 0x9E3779B9u;
  if (!g.rng) g.rng = 12345u;
  g.best = bestScore;
  g.wallet = START_COINS;
  g.rechargeT = 0.f;
  g.cannonAng = 55.f;
  g.phase = PH_RADAR;
  g.slow = 1.f;
  g.bullet.target = -1;
  g.scoreAnim = 0.f;
  for (int i = 0; i < MAX_FISH; i++) { spawnFish(g, i); g.fish[i].alive = 1; }
  pushSfx(g, SFX_COININ);            // 开局投币（之前定义了却没人播）
}

// 潮汐用"潮流"实现，不做瞬间重置：给整片水域一股定向流，鱼被推着游过去，
// 游出边界就不再补（或慢速补），所以进场和退场都是渐变的。
static void updateFish(Game& g, float dt) {
  const float push = g.tideOn ? (g.tideDir ? 85.f : -85.f) : 0.f;
  for (int i = 0; i < MAX_FISH; i++) {
    Fish& f = g.fish[i];
    if (!f.alive) {
      if (g.tideOn) {
        // 潮汐中：慢速补充（0.38s 一条），鱼群看起来是一波波涌进来的
        if (g.spawnTimer <= 0.f) { spawnFish(g, i); g.spawnTimer = 0.20f; }
      } else if (i < 14) {
        spawnFish(g, i);          // 平时：保持 14 条
      }
      continue;
    }
    // 挣脱后先瘫住（0.25 倍速）→ 玩家能补枪；瘫完才加速逃
    float sp = (f.stun > 0.f) ? 0.25f : (1.f + (f.scared > 0.f ? 1.9f : 0.f));
    // 静电：全场按住。不掷骰子，只是把时间还给你

    // ---- 性格：每条鱼偶尔做点自己的事（不加钱，只加"活物感"）----
    // 怪鱼盯着你：慢吞吞转身，一直正面对着炮口。丑，但你会觉得它在看你
    if (f.kind == FK_MON) {
      int8_t want = (f.x < CANNON_X) ? 1 : -1;
      f.dir = want;
      f.vx  = want * fabsf(f.vx) * 0.45f;
    }
    // 小鱼装死：被炮口指着时偶尔不跑，僵在原地（比逃跑更可笑）
    if (f.frozen > 0) { f.frozen--; sp = 0.04f; }

    // 鲨鱼吃小鱼：它游过就吞，你会心疼 —— 这是生态，不是数值
    if (f.kind == FK_SHARK) {
      for (int j = 0; j < MAX_FISH; j++) {
        Fish& o = g.fish[j];
        if (j == i || !o.alive || o.kind != FK_SMALL) continue;
        float odx = o.x - f.x, ody = o.y - f.y;
        if (odx*odx + ody*ody < 2500.f) o.alive = 0;   // 50px 内一律进肚子
      }
    }
    // 鱿鱼喷墨：炮口盯着它 0.5 秒就喷你一脸墨。
    // 原来是 1.5 秒 + 12° 容差，实战里几乎不可能做到 —— 彩蛋等于不存在。
    // 放宽到 0.5 秒 + 18°，会心一笑的东西不该藏在概率里。
    if (f.kind == FK_SQUID) {
      float sdx = f.x - CANNON_X, sdy = f.y - CANNON_Y;
      float sdd = sqrtf(sdx*sdx + sdy*sdy);
      float sang = atan2f(sdy, sdx) * 57.29578f;
      if (sdd < 380.f && fabsf(norm180(g.cannonAng - sang)) < 18.f) {
        g.squidAimT += dt;
        if (g.squidAimT >= 0.5f && g.inkT <= 0.f) {
          g.inkT = 1.2f; g.squidX = f.x; g.squidY = f.y; g.squidAimT = 0.f;
          pushSfx(g, SFX_MISSHOT);
        }
      } else if (g.squidAimT > 0.f) {
        g.squidAimT -= dt * 2.f;
        if (g.squidAimT < 0.f) g.squidAimT = 0.f;
      }
    }

    if (f.stun <= 0.f && f.scared <= 0.f && rndf(g) < 0.12f * dt) {
      f.dir = (int8_t)-f.dir;
      f.vx = f.dir * fabsf(f.vx);
      f.vy = -f.vy * 0.5f;
    }
    // 打窝：只给一个"朝窝的位移"，不覆盖鱼自己的速度。
    // 之前是直接改 f.vx，窝散了鱼还保持着被吸引时的低速，
    // 看起来就是不活了，直到潮汐把它们推醒 —— 这是个 bug。
    float attX = 0.f, attY = 0.f;
    if (g.decoyT > 0.f && g.decoyFly <= 0.f && g.decoyFeed > 0.f) {
      float ddx = g.decoyX - f.x, ddy = g.decoyY - f.y;
      float dd2 = sqrtf(ddx*ddx + ddy*ddy);
      float pull = DECOY_R * (0.55f + 0.45f * g.decoyFeed);
      if (dd2 < pull && dd2 > 14.f) {
        float near = 1.f - dd2 / pull;                       // 近 1 → 远 0
        float k = (16.f + 44.f * near * near) * g.decoyFeed; // 额度随距离和窝料衰减
        attX = (ddx / dd2) * k;
        attY = (ddy / dd2) * k;
        if (f.frozen > 0) f.frozen = 0;                      // 有吃的就不装死了
      }
    }
    f.x += (f.vx * sp + push + attX) * dt;
    f.y += (f.vy * sp + attY) * dt + sinf(f.bob) * 9.0f * dt;
    f.bob += dt * (1.5f + sp * 1.2f);
    if (f.stun > 0.f) f.stun -= dt;
    // 鱼群同向：跟着同群的第一条鱼转
    if (f.group != 255 && f.stun <= 0.f) {
      for (int j = 0; j < MAX_FISH; j++) {
        const Fish& o = g.fish[j];
        if (j == i || !o.alive || o.group != f.group) continue;
        f.dir = o.dir;
        break;
      }
    }
    // 见炮就逃：炮口指过来，小鱼（连同整群）散开 —— 瞄准变成预判而不是对准
    if (f.kind == FK_SMALL && f.scared <= 0.f && f.stun <= 0.f) {
      float dx = f.x - CANNON_X, dy = CANNON_Y - f.y;
      float dd = sqrtf(dx*dx + dy*dy);
      if (dd > 40.f && dd < 300.f) {
        float ang = atan2f(dy, dx) * 57.29578f;
        if (fabsf(norm180(g.cannonAng - ang)) < 13.f) {
          bool playDead = (rndf(g) < 0.12f);      // 12%：不跑了，全体僵住
          for (int j = 0; j < MAX_FISH; j++) {
            if (!g.fish[j].alive) continue;
            if (j == i || (f.group != 255 && g.fish[j].group == f.group)) {
              if (playDead) g.fish[j].frozen = 22;
              else          g.fish[j].scared = 0.9f;
            }
          }
        }
      }
    }
    if (f.scared > 0.f) f.scared -= dt;

    bool out = (f.x < -110.f || f.x > WORLD_W + 110.f || f.y < -70.f || f.y > 352.f);
    if (out) {
      if (f.kind == FK_SHARK) g.sharkOn = 0;
      if (f.kind == FK_DIVER) g.diverOn = 0;
      f.alive = 0;
      if (!g.tideOn && i < 14) spawnFish(g, i);
    }
  }
}

static bool fire(Game& g) {
  const int cost = CANNON_COST[g.cannonLv];
  if (g.wallet < cost) { pushSfx(g, SFX_EMPTY); g.noMoneyT = 0.75f; return false; }
  g.wallet -= cost;
  // 只记真实花掉的钱。免单/白送那套会和"系统权值"互相放大：
  // 免费的一炮却算进投入 → 系统以为玩家亏了很多 → 放宽捕获 → 白赚。
  g.invested += (float)cost;
  g.bullet.active = 1;
  g.bullet.dist = 0.f;
  g.bullet.ang = g.cannonAng;
  g.bullet.x = CANNON_X + cosf(g.cannonAng * (3.14159265f/180.f)) * 16.f;
  g.bullet.y = CANNON_Y - sinf(g.cannonAng * (3.14159265f/180.f)) * 16.f;
  g.bullet.px = g.bullet.x; g.bullet.py = g.bullet.y;
  g.bullet.target = -1;
  g.bullet.pierce = (g.cannonLv == 2) ? 1 : 0;   // LV3 能再穿一条
  g.bullet.killed = 0;
  g.bullet.gainSum = 0;
  g.bullet.bestKind = FK_SMALL;
  g.phase = PH_FLY;
  g.phaseT = 0.f;
  g.muzzle = 0.14f;
  g.recoil = 3.f;
  pushSfx(g, SFX_FIRE);
  return true;
}

static void addScore(Game& g, int gain) {
  if (gain < 0) gain = 0;
  g.score += gain;                   // 最高分用
  g.wallet += gain;                  // 真金白银
  g.lastGain = gain;
}

// 打空最后一格血：捕获特写 + 连击
// 纯记账：分数、鱼获本、连击、赔付。穿透多杀时每条都走这里，
// 只有最后结算才进特写镜头。
static int settleCatch(Game& g, int idx) {
  Fish& f = g.fish[idx];
  int gain = (KSCORE[f.kind] * (g.tideOn ? 3 : 2)) / 2;   // 潮汐价值 ×1.5
  g.returned += (float)gain;
  g.logCount[f.kind]++;
  g.logValue[f.kind] += (uint32_t)gain;
  g.logTotalFish++;
  g.logTotalValue += (uint32_t)gain;
  g.logDirty = 1;
  for (int i = 0; i < FK_COUNT; i++) { sLogC[i] = g.logCount[i]; sLogV[i] = g.logValue[i]; }
  sLogTF = g.logTotalFish;
  sLogTV = g.logTotalValue;
  addScore(g, gain);
  g.combo++;
  g.comboT = 3.2f;
  g.dry = 0;
  f.alive = 0;
  return gain;
}

static void enterCatch(Game& g, int idx) {
  Fish& f = g.fish[idx];
  // 顿帧：按鱼的大小给不同重量 —— 小鱼一记清脆，大鱼砸下来
  g.hitStop = (KW[f.kind] >= 60) ? 0.095f : ((KW[f.kind] >= 30) ? 0.075f : 0.055f);
  settleCatch(g, idx);
  g.catchKind = f.kind;
  g.catchFlip = (f.dir < 0) ? 1 : 0;
  g.catchX = f.x;
  g.catchY = f.y;
  g.catchShake = 0.30f;
  g.phase = PH_CATCH;
  g.phaseT = 0.f;
  g.slow = 0.25f;
  g.missFlag = 0;
  g.bullet.active = 0;
  g.flash = 1;
  if (f.kind == FK_DIVER) pushSfx(g, SFX_DIVER);
  else
  pushSfx(g, (f.kind == FK_MON) ? SFX_CATCH_BIG
            : (f.kind == FK_GOLD || f.kind == FK_MID) ? SFX_CATCH_MID
                                                      : SFX_CATCH);
  for (int i = 0; i < MAX_COINS; i++) {
    CoinFx& c = g.coins[i];
    float a = (-172.f + rndf(g) * 164.f) * (3.14159265f/180.f);
    float sp = 280.f + rndf(g) * 260.f;
    c.x = 120.f; c.y = 69.f;
    c.vx = cosf(a) * sp * 1.15f;
    c.vy = sinf(a) * sp * 0.80f;
    c.life = 2.f;
    c.big = (i < 7) ? 1 : 0;
    c.suck = 0;
  }
}

// 用道具。返回 1=用出去了（钱已扣），0=用不了（冷却/钱不够）
// 三个道具都不看运气：网必中、诱饵改道、静电按住时间。
int gameUseItem(Game& g, uint8_t idx) {
  if (idx > 2) return 0;
  int cost = ITEM_COST[idx];
  // 用不了要给声音：静默的按键会让人以为坏掉了
  if (g.itemCd[idx] > 0.f || g.phase != PH_RADAR || g.wallet < cost) {
    pushSfx(g, SFX_EMPTY);
    if (g.wallet < cost) g.noMoneyT = 0.75f;   // 冷却中不闪，只有真没钱才闪
    return 0;
  }
  if (idx != ITEM_DECOY) return 0;          // 网和静电已删，只剩打窝

  // ---- 打窝 ----
  // 这是钓鱼佬的动作，不是技能：饵抛出去 → 落水 → 鱼慢慢围过来 → 窝料散完鱼也走。
  // 窝只负责"把鱼叫过来"，钓不钓得到是你自己的事 —— 打窝不一定上鱼。
  g.wallet   -= cost;
  g.invested += (float)cost;
  g.itemCd[idx] = ITEM_CD[idx];

  float a = g.cannonAng * (3.14159265f / 180.f);
  g.decoyX    = CANNON_X + cosf(a) * DECOY_REACH;
  g.decoyY    = CANNON_Y - sinf(a) * DECOY_REACH;
  g.decoyT    = DECOY_LIFE;
  g.decoyFly  = DECOY_FLY;
  g.decoyFeed = 1.f;
  pushSfx(g, SFX_PAY);                      // 先听见钱被扣掉
  pushSfx(g, SFX_WHOOSH);                   // 再是"嗖"的一声出手
  return 1;
}


// 打中了，但鱼把网挣开跑掉：没有收益，但要有画面和声音（这就是"赌"的代价）
static void enterEscape(Game& g, int idx) {
  Fish& f = g.fish[idx];
  // 挣脱后脱力 1.4s（速度 0.25x）→ 玩家有时间补第二枪，再打中就跑
  f.stun = 1.4f;
  f.scared = 2.6f;
  if (g.dry < 255) g.dry++;         // 这一炮又是空的
  g.catchKind = f.kind;
  g.catchFlip = (f.dir < 0) ? 1 : 0;
  g.catchX = f.x;
  g.catchY = f.y;
  g.catchShake = 0.22f;
  g.phase = PH_MISS;
  g.phaseT = 0.f;
  g.slow = 0.30f;
  g.missFlag = 1;
  g.bullet.active = 0;
  pushSfx(g, SFX_ESCAPE);
}

static void updateBullet(Game& g, float dt) {
  Bullet& b = g.bullet;
  if (!b.active) return;

  // 子步推进：炮弹每帧飞 ~17px（520*0.033），小鱼只有 22px 宽，
  // 点检测会让炮弹"穿过去"。按 5px 一步细分成若干子步再判定。
  const float step = BULLET_SPEED * dt;
  int sub = (int)(step / 5.f) + 1;
  if (sub > 10) sub = 10;
  const float d0 = b.dist;

  for (int s = 1; s <= sub; s++) {
    b.dist = d0 + step * (float)s / (float)sub;
    float a = b.ang * (3.14159265f/180.f);
    b.x = CANNON_X + cosf(a) * b.dist;
    b.y = CANNON_Y - sinf(a) * b.dist;

    for (int i = 0; i < MAX_FISH; i++) {
      Fish& f = g.fish[i];
      if (!f.alive) continue;
      // 椭圆判定：鳗鱼横长竖窄，用圆会误判
      // 判定放宽：原来半径只有 15px，而小鱼本身才 22px 宽、炮弹 520px/s，
      // 玩家瞄得再准也常常擦边而过（实测命中率仅 17%）。给足容错。
      // 容错从 +10 收到 +4：小鱼判定半径曾经是 21px（鱼本身才 22 宽），
      // 等于炮弹擦着边就把鱼吃了，玩家看着像"前面没鱼也能中"。
      float ex = (f.x - b.x) / (KW[f.kind]*0.50f + 7.f);
      float ey = (f.y - b.y) / (KH[f.kind]*0.62f + 7.f);
      bool hit = (ex*ex + ey*ey <= 1.f);
      if (hit) {
        // ===== 捕鱼达人的判定（原版公式）=====
        // 系统权值由"收支比"决定：玩家赚得多就收权（甚至归零），亏得多就放宽。
        // 系统权值：收支比越低（玩家越赚）越收紧，越高（越亏）越放宽。
        // 原版是街机，<=0.5（赚一倍）直接归零；掌机上那样会把人逼到抓不到鱼、
        // 只能干等下一条的境地，所以最低给 0.15 而不是 0 —— 收紧但不死锁。
        // 加一对虚拟初始值：不然开局 invested=0 → payGain=0 → 系统误判"你亏惨了"，
        // 前 60 秒白送一波，然后立刻回落，玩家会觉得被耍了。
        float baseInv = 120.f, baseRet = 96.f;      // 相当于开局就已经是 1.25 的 ROI
        float payGain = (g.invested + baseInv) / (g.returned + baseRet);
        float sysPow;
        // 下限 0.50 而不是 0：LV1 的炮弹权值只有 0.35，若系统权值也压到 0.35，
        // 最大威力 12.25 仍打不过小鱼权值 13 —— 一旦收紧就永远抓不到任何鱼（死锁）。
        // 掌机不是街机。系统权值本质是个负反馈：它会把 ROI 拉回设定点。
        // 原来设定点是 1.0（原版街机就是吃币），所以玩家永远在"刚好不亏"处磨。
        // 现在把目标挪到 1.25：赢 25% 才开始收紧，亏 10% 立刻放宽。
        // 成长曲线：分数越高，目标 ROI 越低。前期让你尝到甜头，中后期靠本事，
        // 别变成刷钱 —— 一路往上冲的增长，玩十分钟就腻了。
        float target = 1.26f;
        if (g.score >  300) target = 1.16f;
        if (g.score >  900) target = 1.08f;
        if (g.score > 1800) target = 1.02f;
        float want = 1.f / target;                  // payGain 的目标值
        // 下限从 1.10 收到 0.75：1.10 太松，LV3 乘上去还是 1.87 威力，等于永远开着外挂，
        // 玩家越赚系统越夹不住（正反馈），十分钟翻九倍。
        if (payGain <= want - 0.10f)      sysPow = 0.75f;
        else if (payGain >= want + 0.12f) sysPow = 1.70f;
        else sysPow = 0.75f + (payGain - (want - 0.10f)) * 4.32f;

        int   roll  = 1 + (int)(rndf(g) * 100.f);                       // rand()%100 + 1
        // 穿透炮威力分散：一炮要穿两层，每层的判定力打折
        float spread = (b.pierce > 0 || b.killed > 0) ? 0.62f : 1.0f;
        float power = (float)roll * CANNON_POWER[g.cannonLv] * sysPow * spread;
        // 潮汐价值 ×1.5，鱼权值同步 ×1.5 → 概率不变（靠数量取胜，不靠赔率）
        float wgt   = (float)KWGT[f.kind] * (g.tideOn ? 1.5f : 1.f);
        if (power > wgt) {
          if (b.pierce > 0) {
            // 穿透：这条先记账、消失，炮弹继续飞；特写留给最后一条
            int gain = settleCatch(g, i);
            b.gainSum += gain;
            b.killed++;
            if (KSCORE[f.kind] > KSCORE[b.bestKind]) b.bestKind = f.kind;
            b.pierce--;
            continue;
          }
          enterCatch(g, i);
        } else {
          // 打不动：直接告诉他按几号，别让他自己猜"是不是这鱼无敌"
          int needLv = -1;
          for (int L = 0; L < 3; L++)
            if ((float)KWGT[f.kind] < CANNON_POWER[L] * 100.f * sysPow * 0.80f) { needLv = L; break; }
          if (needLv > (int)g.cannonLv) { g.hintT = 2.0f; g.hintLv = (uint8_t)(needLv + 1); }
          enterEscape(g, i);
        }
        return;
      }
    }
  }
  if (b.dist > BULLET_MAXD || b.x < -40 || b.x > WORLD_W+40 || b.y < -40 || b.y > WORLD_H+40) {
    b.active = 0;
    if (b.killed > 0) {
      // 穿透后飞空：已经吃下的鱼要给结算画面（用最值钱那条做特写）
      g.catchKind = b.bestKind;
      g.catchX = b.x; g.catchY = b.y;
      g.catchShake = 0.30f;
      g.phase = PH_CATCH;
      g.phaseT = 0.f;
      g.slow = 0.25f;
      g.missFlag = 0;
      g.flash = 1;
      pushSfx(g, SFX_CATCH);
      return;
    }
    g.missFlag = 2;                 // 完全落空
    if (g.dry < 255) g.dry++;       // 打空也算一次没收获
    pushSfx(g, SFX_MISSHOT);
    g.phase = PH_RESULT;
    g.phaseT = 0.f;
  }
}

static void updateCamera(Game& g, float dt) {
  float prev = g.zoom;
  switch (g.phase) {
    case PH_RADAR:
    case PH_OVER:
      g.zoom = 1.f/3.f; g.camX = 0.f; g.camY = 0.f; break;

    case PH_FLY:
      if (g.bullet.dist < 110.f) { g.zoom = 1.f/3.f; g.camX = 0.f; g.camY = 0.f; }
      else {
        g.zoom = 1.f;
        float tx = g.bullet.x - SCREEN_W*0.5f;
        float ty = g.bullet.y - SCREEN_H*0.5f;
        float k = 1.f - expf(-7.f * dt);
        g.camX += (tx - g.camX) * k;
        g.camY += (ty - g.camY) * k;
      }
      break;

    default: // CATCH / MISS / RESULT
      if (g.phase == PH_RESULT && (g.missFlag == 2 || g.missFlag == 3)) {
        g.zoom = 1.f;              // 落空 / 负伤：留在飞行视角
      } else {
        g.zoom = 3.f;
        g.camX = g.catchX - 40.f;
        g.camY = g.catchY - 23.f;
      }
      break;
  }
  (void)prev;
}

void gameUpdate(Game& g, const Input& in, float dt) {
  if (g.listOpen) return;      // 看鱼获时暂停（没有 game over，暂停不损失什么）

  // 顿帧：捕获命中那一下整屏冻住 65ms 再爆开——街机最有效的冲击手段
  if (g.hitStop > 0.f) { g.hitStop -= dt; return; }

  // 道具计时
  for (int i = 0; i < 3; i++) if (g.itemCd[i] > 0.f) { g.itemCd[i] -= dt; if (g.itemCd[i] < 0.f) g.itemCd[i] = 0.f; }
  // 道具请求（边沿触发，由平台层/模拟器每帧给一次）
  if (in.item >= 1 && in.item <= 3) { gameUseItem(g, (uint8_t)(in.item - 1)); }   // 范围硬校验
  if (g.decoyT > 0.f) {
    g.decoyT -= dt;
    if (g.decoyT < 0.f) g.decoyT = 0.f;
    if (g.decoyFly > 0.f) { g.decoyFly -= dt; if (g.decoyFly < 0.f) g.decoyFly = 0.f; }
    else g.decoyFeed -= dt / (DECOY_LIFE - DECOY_FLY);   // 落水后才开始耗窝料
    if (g.decoyFeed < 0.f) g.decoyFeed = 0.f;
  }
  if (g.inkT   > 0.f) { g.inkT   -= dt; if (g.inkT   < 0.f) g.inkT   = 0.f; }
  if (g.noMoneyT > 0.f) { g.noMoneyT -= dt; if (g.noMoneyT < 0.f) g.noMoneyT = 0.f; }
  if (g.hintT    > 0.f) { g.hintT    -= dt; if (g.hintT    < 0.f) g.hintT    = 0.f; }

  // 总游玩时长：跨局累计，看鱼获时不计时
  if (g.wallet > g.peakWallet) { g.peakWallet = g.wallet; if (g.peakWallet > sPeakWallet) sPeakWallet = g.peakWallet; }

  g.playAcc += dt;
  if (g.playAcc >= 1.f) {
    g.playAcc -= 1.f;
    g.playSec++;
    sPlaySec = g.playSec;
  }
  if (dt > 0.1f) dt = 0.1f;
  if (g.flash) g.flash--;
  if (g.muzzle > 0.f) g.muzzle -= dt;
  if (g.recoil > 0.f) { g.recoil -= dt * 22.f; if (g.recoil < 0.f) g.recoil = 0.f; }
  if (g.catchShake > 0.f) g.catchShake -= dt;
  if (g.comboT > 0.f) { g.comboT -= dt; if (g.comboT <= 0.f) g.combo = 0; }
  if (g.spawnTimer > 0.f) g.spawnTimer -= dt;

  if (g.scoreAnim < (float)g.score - 0.5f) {
    g.scoreAnim += ((float)g.score - g.scoreAnim) * 0.22f + 0.6f;
    if (g.scoreAnim > (float)g.score) g.scoreAnim = (float)g.score;
  } else if (g.scoreAnim > (float)g.score) g.scoreAnim = (float)g.score;

  bool aimable = (g.phase == PH_RADAR);
  if (aimable && in.useImu) {
    // 体感瞄准：平滑追向倾斜角，不用瞬时值（IMU 抖动会让炮口乱颤）
    float d = norm180(in.aimAng - g.cannonAng);
    g.cannonAng += d * fminf(1.f, dt * 14.f);
    if (g.cannonAng < 15.f)  g.cannonAng = 15.f;
    if (g.cannonAng > 165.f) g.cannonAng = 165.f;
    g.aimHold = 0.f;
  } else if (aimable && (in.left || in.right)) {
    g.aimHold += dt;
    float rate = (g.aimHold > 0.25f) ? 160.f : 60.f;
    if (in.left)  g.cannonAng += rate * dt;
    if (in.right) g.cannonAng -= rate * dt;
    if (g.cannonAng < 15.f)  g.cannonAng = 15.f;
    if (g.cannonAng > 165.f) g.cannonAng = 165.f;
    if (g.sfxCount == 0) pushSfx(g, SFX_AIM);
  } else g.aimHold = 0.f;

  // 金币见底：等 RECHARGE_SEC 秒补到 100（只补够打一炮，不是发大财）
  if (g.wallet < CANNON_COST[0]) {
    g.rechargeT += dt;
    if (g.rechargeT >= RECHARGE_SEC) { g.wallet = 25; g.rechargeT = 0.f; }   // 救济：够打一发最贵的炮（20）
  } else g.rechargeT = 0.f;

  switch (g.phase) {
    case PH_RADAR:
      if (g.tideOn) {
        g.tideT -= dt;
        if (g.tideFlash > 0) g.tideFlash--;
        if (g.tideT <= 0.f) {
          g.tideOn = 0;
          g.tideNext = 42.f + rndf(g) * 22.f;
          pushSfx(g, SFX_TIDE_END);
          // 多出来的槽随鱼游出自然消失，不强行清场
        }
      } else {
        if (!g.diverOn && g.diverNext < 9000.f) {
          g.diverNext -= dt;
          if (g.diverNext <= 0.f) spawnDiver(g);
        }
        // 鲨鱼事件：和潮汐错开触发
        if (!g.sharkOn) {
          g.sharkNext -= dt;
          if (g.sharkNext <= 0.f) spawnShark(g);
        }
        g.tideNext -= dt;
        if (g.tideNext <= 0.f) {
          g.tideOn = 1;
          g.tideT = 8.f;
          g.tideFlash = 26;
          g.tideDir = (rndf(g) < 0.5f) ? 1 : 0;
          g.spawnTimer = 0.f;
          // 只把多出来的 6 个槽填成潮汐鱼（它们从屏幕外远处游入），
          // 在场的老鱼交给潮流推走 —— 不重置，才不会"啪"一下全变
          for (int i = 14; i < MAX_FISH; i++) spawnTideFish(g, i);
          pushSfx(g, SFX_TIDE);
        }
      }
      updateFish(g, dt);
      if (in.fire) fire(g);
      break;

    case PH_FLY:
      updateFish(g, dt);
      updateBullet(g, dt);
      g.phaseT += dt;
      if (g.phaseT > 3.0f && g.bullet.active) { g.bullet.active = 0; g.phase = PH_RESULT; g.phaseT = 0.f; }
      break;

    case PH_CATCH: {
      g.phaseT += dt;
      bool suck = (g.phaseT > CATCH_HOLD);
      for (int i = 0; i < MAX_COINS; i++) {
        CoinFx& c = g.coins[i];
        if (c.life <= 0.f) continue;
        if (suck) {
          c.suck = 1;
          float k = (4.0f + i*0.55f) * dt;
          c.x += (40.f - c.x) * k;      // 飞向左上角 SCORE 数字
          c.y += (7.f  - c.y) * k;
        } else {
          c.vx *= 0.955f;
          c.vy = c.vy * 0.955f + 34.f * dt;
          c.x += c.vx * dt;
          c.y += c.vy * dt;
        }
        c.life -= dt;
      }
      if (g.phaseT > CATCH_TOTAL) { g.phase = PH_RADAR; g.phaseT = 0.f; g.slow = 1.f; g.missFlag = 0; }
      break;
    }

    case PH_MISS:
      g.phaseT += dt;
      if (g.phaseT > MISS_TOTAL) { g.phase = PH_RADAR; g.phaseT = 0.f; g.slow = 1.f; g.missFlag = 0; }
      break;

    case PH_RESULT:
      g.phaseT += dt;
      updateFish(g, dt);
      if (g.phaseT > 0.50f) {
        g.phase = PH_RADAR;          // 不再有"币归零即结束"，改成充能等待
        g.phaseT = 0.f;
      }
      break;

    case PH_OVER:
      updateFish(g, dt * 0.4f);
      if (in.fire) { int best = g.best; gameInit(g, best); }
      break;
  }

  if (g.score > g.best) g.best = g.score;
  g.sweep += dt * 12.f;
  updateCamera(g, dt);
}

void gameSetCannon(Game& g, uint8_t lv) {
  if (lv > 2) lv = 2;
  g.cannonLv = lv;
}

// 外部（main）想播个音效时用
void pushSfxQueued(Game& g, uint8_t id) { pushSfx(g, id); }

// ---- 鱼获本的外部读写（main 用它对接 NVS）----
void gameGetLog(const Game& g, uint16_t* c, uint32_t* v, uint32_t* tf, uint32_t* tv,
                uint32_t* playSec) {
  for (int i = 0; i < FK_COUNT; i++) { c[i] = g.logCount[i]; v[i] = g.logValue[i]; }
  *tf = g.logTotalFish;
  *tv = g.logTotalValue;
  *playSec = g.playSec;
}

void gameSetLog(Game& g, const uint16_t* c, const uint32_t* v, uint32_t tf, uint32_t tv,
                uint32_t playSec) {
  for (int i = 0; i < FK_COUNT; i++) {
    g.logCount[i] = sLogC[i] = c[i];
    g.logValue[i] = sLogV[i] = v[i];
  }
  g.logTotalFish  = sLogTF = tf;
  g.logTotalValue = sLogTV = tv;
  g.playSec = sPlaySec = playSec;
}

// main 从 NVS 读回历史峰值时用
void gameSetPeak(Game& g, int peak) {
  if (peak > sPeakWallet) sPeakWallet = peak;
  if (peak > g.peakWallet) g.peakWallet = peak;
}
