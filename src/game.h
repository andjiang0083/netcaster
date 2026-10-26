// NETCASTER — 平台无关游戏核心（不许 include M5 任何头）
#pragma once
#include <stdint.h>

#define SCREEN_W 240
#define SCREEN_H 135
#define WORLD_W  720
#define WORLD_H  405
#define CANNON_X 360
#define CANNON_Y 360

enum FishKind { FK_SMALL = 0, FK_MID, FK_EEL, FK_GOLD, FK_MON,
                FK_MANTA, FK_SQUID, FK_SHARK, FK_DIVER, FK_COUNT };
enum Phase    { PH_RADAR = 0, PH_FLY, PH_CATCH, PH_MISS, PH_RESULT, PH_OVER };

#define MAX_FISH  20      // 潮汐时要能塞下整群鱼
#define MAX_COINS 14
#define SFX_QUEUE 8

// 音效 id（平台层负责发声）
enum Sfx {
  SFX_NONE = 0, SFX_COININ, SFX_AIM, SFX_FIRE, SFX_ESCAPE,
  SFX_CATCH, SFX_COINDROP, SFX_EMPTY, SFX_BET, SFX_MISSHOT,
  SFX_CATCH_MID,   // 中鱼/金鱼：更亮的叮
  SFX_CATCH_BIG,   // 怪鱼：低频起步的厚重上行
  SFX_TIDE,        // 潮汐来了
  SFX_TIDE_END,    // 潮汐退去
  SFX_DIVER,       // 抓到潜水员（滑稽）
  SFX_WHOOSH,      // 抛投："嗖" —— 饵团出手的那一下
  SFX_PAY          // 扣钱：柜员机那种"叮-嗒"。不响就以为不要钱
};

struct Fish {
  float   x, y;
  float   vx, vy;
  uint8_t kind;
  int8_t  dir;        // +1 朝右 / -1 朝左
  uint8_t alive;
  float   scared;     // >0 受惊加速
  float   stun;       // >0 挣脱后的脱力：游得极慢，给玩家补枪窗口
  float   bob;        // 摆尾相位
  float   wobble;
  uint8_t group;      // 鱼群编号（255 = 独游）—— 小鱼成群同向、一起受惊
  uint8_t frozen;     // >0 = 被炮口吓住，僵在原地"装死"（每帧递减）
  uint8_t tic;        // 偶发小动作的倒计时（打哈欠之类），0 = 空闲
};

struct Bullet {
  uint8_t active;
  float   x, y, ang, dist;
  float   px, py;      // 上一帧位置：子弹一帧走 17px，单点判定会直接穿过小鱼
  int8_t  target;     // 弱追踪目标索引，-1 无
  uint8_t pierce;     // 剩余可穿透条数（LV3 = 1）
  uint8_t killed;     // 本炮已穿杀条数
  int     gainSum;    // 本炮累计收益（穿透多杀时合并结算）
  uint8_t bestKind;   // 本炮最值钱的鱼（用它做特写）
};

struct CoinFx {
  float x, y, vx, vy, life;   // 屏幕坐标（只在 CATCH 用）
  uint8_t big;
  uint8_t suck;               // 0=向外爆 1=被吸向计分
};

struct Input {
  bool  left, right, fire, esc;
  bool  useImu;      // true 时用 aimAng 直接指定炮口角（体感瞄准）
  float aimAng;      // 目标角度（度）
  uint8_t item;      // 本帧请求使用的道具：0=无 1=渔网 2=诱饵 3=静电（边沿触发）
};

struct Game {
  Fish    fish[MAX_FISH];
  Bullet  bullet;
  CoinFx  coins[MAX_COINS];

  float   cannonAng;        // 0=右, 90=上, 180=左
  float   aimHold;          // 按住调角时长（加速用）
  float   rechargeT;        // 金币不足 100 时的充能计时（秒）
  uint8_t dry;              // 连续没抓到的炮数：到 4 就免费一炮
  uint8_t imuAim;           // 1 = 体感瞄准模式（仅 UI 提示用）
  uint8_t cannonLv;         // 炮台等级 0/1/2（1~3 级），决定成本与炮弹权值
  uint8_t listOpen;         // 1 = 打开鱼获图鉴（暂停游戏）
  uint8_t listPage;         // 图鉴页码（每页 4 条）
  uint8_t helpOpen;         // 1 = 按键说明页（暂停游戏）
  uint8_t logDirty;         // 有新的渔获未写入 NVS
  // 鱼获本：跨局累计，钓到什么、多少条、值多少钱
  uint16_t logCount[FK_COUNT];
  uint32_t logValue[FK_COUNT];
  uint32_t logTotalFish;
  uint32_t logTotalValue;
  uint32_t playSec;         // 总游戏时长（累计秒，跨局）
  float    playAcc;         // 秒以下的小数累加器
  float    hitStop;         // >0 时整屏冻结（捕获瞬间的顿帧）
  int      peakWallet;      // 历史最高金币余额（跨局）
  float    huntPts;         // 全场猎杀池：每次命中累积，满 8 点后打大鱼必杀（跨目标，不用追鱼）
  uint8_t tideOn;           // 1 = 潮汐中（鱼群涌过）
  float   tideT;            // 潮汐剩余时间
  float   tideNext;         // 距下次潮汐的时间
  uint8_t tideFlash;        // 潮汐刚开始的提示闪烁计数
  uint8_t tideDir;          // 潮汐方向 1=向右 0=向左
  float   spawnTimer;       // 潮汐期的慢速补充计时（避免瞬间塞满）
  float   sharkNext;        // 距下次鲨鱼的时间
  uint8_t sharkOn;          // 鲨鱼在场中
  float   invested;         // 累计投入（每炮成本累加），用于自适应 RTP 控制
  float   returned;         // 累计赔付（抓到的价值累加）
  float   diverNext;        // 潜水员彩蛋倒计时（每局 10% 概率才有）
  uint8_t diverOn;
  int     score, wallet, best;
  float   scoreAnim;        // 分数滚动显示值
  int     combo;
  float   comboT;
  int     lastGain;

  Phase   phase;
  float   phaseT;
  float   slow;             // 1.0 正常 / 0.25 慢动作
  int8_t  catchKind;
  uint8_t catchFlip;        // 被抓住的那条鱼原本朝哪边
  uint8_t missFlag;         // 1 = 上次是挣脱（RESULT 阶段画挣脱帧）
  float   catchX, catchY;
  float   catchShake;
  float   muzzle;           // 炮口闪光剩余时间
  float   recoil;           // 后坐位移

  // 道具（4=渔网 5=诱饵 6=静电）：都不掷骰子，方差交给玩家决策
  float   itemCd[3];        // 各自冷却剩余秒
    float   decoyT;           // 窝的总寿命（含飞行）
  float   decoyFly;         // >0 = 饵团还在空中飞
  float   decoyFeed;        // 窝料余量 1→0：鱼越聚越少，最后散开
  float   decoyX, decoyY;   // 落点
  float   hintT;            // >0 = 正在提示"该换几号炮"（打不动大鱼时）
  uint8_t hintLv;           // 建议的等级 1/2/3
  float   noMoneyT;         // >0 = 刚被拒绝过（钱不够）：屏幕要闪一下，光靠音效不够
  float   inkT;             // 鱿鱼墨汁：雷达被遮住的剩余时间
  float   squidAimT;        // 炮口持续对着鱿鱼的累计秒数
  float   squidX, squidY;   // 喷墨位置

  float   camX, camY, zoom; // 由 phase 推导，供渲染读
  uint8_t flash;            // 换档闪白帧计数
  float   sweep;            // 雷达余辉相位

  uint32_t rng;
  uint8_t  sfx[SFX_QUEUE];
  uint8_t  sfxCount;
};

void  gameInit(Game& g, int bestScore);
void  gameUpdate(Game& g, const Input& in, float dt);
void  gameSetCannon(Game& g, uint8_t lv);   // 切炮台等级 0/1/2
int   gameUseItem(Game& g, uint8_t idx);   // 用道具 0=网 1=诱饵 2=静电；返回 1=用出去了
void  gameGetLog(const Game& g, uint16_t* c, uint32_t* v, uint32_t* tf, uint32_t* tv, uint32_t* playSec);
void  gameSetLog(Game& g, const uint16_t* c, const uint32_t* v, uint32_t tf, uint32_t tv, uint32_t playSec);
void  pushSfxQueued(Game& g, uint8_t id);   // 外部播一个音效
void  gameSetPeak(Game& g, int peak);       // 设置历史最高金币（main 从 NVS 读回）
#define SHOT_COST     10
// 道具的几何参数：渲染层也要用（网画多大、诱饵光晕多宽），放头文件里共用
extern const int CANNON_COST[3];   // 三级炮台开一炮的钱，HUD 和逻辑共用一份
extern const int ITEM_COST[3];

#define DECOY_R        85.f   // 吸引半径：只够招呼附近那几条，不是全场广播
#define DECOY_LIFE      6.0f
#define DECOY_FLY       0.40f

#define START_COINS   350     // 够买一袋窝料（300）或 35 发一级炮
#define RECHARGE_SEC  3.5f    // 掌机节奏：见底后 3.5 秒就给一炮的钱，别让人干等
bool  gamePopSfx(Game& g, uint8_t& out);   // true = 取到一个音效

// 供渲染使用的静态表
int   fishWorldW(uint8_t kind);
int   fishWorldH(uint8_t kind);
int   fishBaseScore(uint8_t kind);   // 捕获这条鱼给多少金币
void  audioSetCombo(int combo);   // 捕获音阶随连击升高（audio.cpp 实现）
