// NETCASTER — Cardputer 入口
#include <M5Cardputer.h>
#include <Preferences.h>
#include "game.h"
#include "render.h"
#include "audio.h"

static M5Canvas  canvas(&M5.Display);
static Game      g;
static uint16_t* fb = nullptr;
static Preferences prefs;

// ---- 省电：2 分钟没有任何按键就关屏+降频，任意键唤醒 ----
static uint32_t lastInputMs = 0;
static bool     powerSave   = false;
static const uint32_t IDLE_TO_SLEEP_MS = 120000UL;   // 2 分钟
static uint32_t  lastFrame = 0;
static uint32_t  prevKeys = 0;
static bool      prevSpace = false;
static int       lastBest = -1;
static bool      imuAim = true;        // 体感瞄准开关（i 键）—— 默认开，手感已被认可
static float     tiltF = 0.f;          // 倾斜低通滤波状态

#define FRAME_MS 33   // 30 fps

// 键盘边沿检测：keysState().word 每 ms 重建，不做边沿会每秒切换上千次
static uint32_t keyMask() {
  uint32_t m = 0;
  auto ks = M5Cardputer.Keyboard.keysState();
  for (auto k : ks.word) {
    if (k >= '1' && k <= '6') m |= 1UL << (k - '1');   // 1-3 炮台档，4-6 道具
    if (k == 'm' || k == 'M') m |= 1UL << 8;
    if (k == 'i' || k == 'I') m |= 1UL << 11;
    if (k == 'l' || k == 'L') m |= 1UL << 12;
    if (k == 'h' || k == 'H') m |= 1UL << 13;
    if (k == ' ' || k == 0x20) m |= 1UL << 9;
  }
  if (ks.enter) m |= 1UL << 10;
  return m;
}

static Input readInput() {
  Input in{};          // 必须值初始化：Input 是 POD，item 字段是后加的，
                       // 裸 `Input in;` 会给它一个栈上垃圾值 → 每帧随机触发道具（会自己撒网）
  in.left = in.right = in.fire = in.esc = false;
  in.useImu = false;
  in.aimAng = 90.f;

  auto& kb = M5Cardputer.Keyboard;
  in.left  = kb.isKeyPressed(',') || kb.isKeyPressed('a');
  in.right = kb.isKeyPressed('/') || kb.isKeyPressed('d');
  in.esc   = kb.isKeyPressed(KEY_BACKSPACE);

  uint32_t m = keyMask();
  uint32_t edge = m & ~prevKeys;
  prevKeys = m;

  if (edge & (1UL << 8)) audioSetMute(!audioMuted());
  if (edge & (1UL << 11)) imuAim = !imuAim;
  if (edge & (1UL << 12)) { g.listOpen = g.listOpen ? 0 : 1; g.helpOpen = 0; g.listPage = 0; pushSfxQueued(g, SFX_BET); }
  if (edge & (1UL << 13)) { g.helpOpen = g.helpOpen ? 0 : 1; g.listOpen = 0; pushSfxQueued(g, SFX_BET); }
  // 图鉴翻页：, 上一页 / 下一页（边沿触发，按住不会连翻）
  if (g.listOpen) {
    const int PAGES = 1 + (FK_COUNT + 3) / 4;   // 统计页 + 鱼页
    bool prevNow = M5Cardputer.Keyboard.isKeyPressed(',');
    bool nextNow = M5Cardputer.Keyboard.isKeyPressed('/');
    static bool prevLast = false, nextLast = false;
    if (prevNow && !prevLast && g.listPage > 0) { g.listPage--; pushSfxQueued(g, SFX_AIM); }
    if (nextNow && !nextLast && g.listPage < PAGES - 1) { g.listPage++; pushSfxQueued(g, SFX_AIM); }
    prevLast = prevNow; nextLast = nextNow;
  }
  // 数字键 1/2/3 切炮台等级（成本 35/100/200）
  for (int d = 0; d < 3; d++)
    if (edge & (1UL << d)) { gameSetCannon(g, (uint8_t)d); pushSfxQueued(g, SFX_BET); }
  // 数字键 4/5/6 用道具：4=渔网(180) 5=诱饵(60) 6=静电(120)
  for (int d = 3; d < 6; d++)
    if (edge & (1UL << d)) in.item = (uint8_t)(d - 2);

  if (imuAim) {
    float ax, ay, az;
    if (M5.Imu.getAccel(&ax, &ay, &az)) {
      // 低通 + 死区：IMU 噪声直接映射会让炮口持续发抖
      tiltF = tiltF * 0.86f + ax * 0.14f;
      float t = tiltF;
      if (t > -0.045f && t < 0.045f) t = 0.f;
      // 符号可在此处取反（真机若左右相反就改成 +）
      float ang = 90.f - t * 135.f;
      if (ang < 15.f)  ang = 15.f;
      if (ang > 165.f) ang = 165.f;
      in.useImu = true;
      in.aimAng = ang;
    }
  }

  bool spaceNow = (m & (1UL << 9)) != 0 || (m & (1UL << 10)) != 0;
  in.fire = spaceNow && !prevSpace;
  prevSpace = spaceNow;
  return in;
}

void setup() {
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5Cardputer;   // 不是 board_M5CardputerADV
  M5Cardputer.begin(cfg);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(100);

  canvas.setColorDepth(16);
  if (!canvas.createSprite(SCREEN_W, SCREEN_H)) {
    M5.Display.fillScreen(TFT_RED);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("sprite alloc fail", 10, 60);
    for (;;) delay(1000);
  }
  fb = (uint16_t*)canvas.getBuffer();

  renderInit();
  audioInit();

  prefs.begin("netcaster", false);
  int best = prefs.getInt("best", 0);
  gameInit(g, best);
  // 读回鱼获本 + 总时长（跨局累计）
  {
    uint16_t c[FK_COUNT]; uint32_t v[FK_COUNT];
    size_t n = prefs.getBytesLength("logc");
    if (n == sizeof(c)) prefs.getBytes("logc", c, n); else memset(c, 0, sizeof(c));
    n = prefs.getBytesLength("logv");
    if (n == sizeof(v)) prefs.getBytes("logv", v, n); else memset(v, 0, sizeof(v));
    // 旧档是"金币 ×10"时代的（点一位数）：一次性折算，别让最高分永远够不着。
    // econ 键 = 2 表示已经是新单位，之后再也不会二次除。
    bool migrate = prefs.getInt("econ", 1) < 2;
    if (migrate) {
      for (int i = 0; i < FK_COUNT; i++) v[i] /= 10;
      prefs.putInt("econ", 2);
    }
    uint32_t tv = prefs.getUInt("logtv", 0);
    int      pk = prefs.getInt("peak", 0);
    if (migrate) { tv /= 10; pk /= 10; }
    gameSetLog(g, c, v, prefs.getUInt("logtf", 0), tv, prefs.getUInt("ptime", 0));
    gameSetPeak(g, pk);
  }
  lastBest = best;
  g.imuAim = 0;
}

void loop() {
  M5Cardputer.update();          // 每轮都跑，必须在帧率门之前

  uint32_t now = millis();

  // ---- 省电模式 ----
  {
    auto ks = M5Cardputer.Keyboard.keysState();
    bool keyDown = !ks.word.empty() || ks.enter || ks.del || ks.space || ks.fn;
    if (keyDown) {
      lastInputMs = now;
      if (powerSave) {                        // 任意键唤醒
        powerSave = false;
        setCpuFrequencyMhz(240);
        M5.Display.wakeup();
        M5.Display.setBrightness(100);
        audioSetMute(audioMuted());           // 恢复原静音状态
        lastFrame = millis();                 // 别让 dt 变成一大坨
      }
    }
    if (powerSave) { delay(60); return; }     // 睡着时不渲染、不更新

    if (now - lastInputMs > IDLE_TO_SLEEP_MS) {
      powerSave = true;
      M5.Speaker.stop();
      M5.Display.setBrightness(0);
      M5.Display.sleep();                     // 关显示面板 + 背光
      setCpuFrequencyMhz(40);                 // 降频到 40MHz
      delay(10);
      return;
    }
  }

  if (now - lastFrame < FRAME_MS) return;
  float dt = (now - lastFrame) * 0.001f;
  if (dt > 0.12f) dt = 0.12f;
  lastFrame = now;

  Input in = readInput();
  g.imuAim = imuAim ? 1 : 0;
  gameUpdate(g, in, dt);

  uint8_t s;
  while (gamePopSfx(g, s)) { if (s == SFX_CATCH) audioSetCombo(g.combo); audioPlay(s); }
  audioUpdate(now, g.phase == PH_RADAR || g.phase == PH_FLY, g.tideOn != 0);

  renderFrame(fb, g, now * 0.001f);
  // M5GFX 的 sprite buffer 是 swap565（字节交换），写进去的 RGB565 要翻一次
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
    uint16_t v = fb[i];
    fb[i] = (uint16_t)((v >> 8) | (v << 8));
  }
  canvas.pushSprite(0, 0);

  // 每 5 秒落一次 NVS（鱼获本 + 总时长）；NVS 写入次数有限，别每帧写
  static uint32_t lastLogSave = 0;
  if (millis() - lastLogSave > 5000) {
    lastLogSave = millis();
    uint16_t c[FK_COUNT]; uint32_t v[FK_COUNT]; uint32_t tf, tv, ps;
    gameGetLog(g, c, v, &tf, &tv, &ps);
    prefs.putBytes("logc", c, sizeof(c));
    prefs.putBytes("logv", v, sizeof(v));
    prefs.putUInt("logtf", tf);
    prefs.putUInt("logtv", tv);
    prefs.putUInt("ptime", ps);
    prefs.putInt("peak", g.peakWallet);
  }

  if (g.best != lastBest) {
    lastBest = g.best;
    prefs.putInt("best", g.best);
  }
}
