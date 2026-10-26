// NETCASTER — PC 模拟器：跑逻辑 + 渲染，关键帧导出 PPM
#include "game.h"
#include "render.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static uint16_t fb[SCREEN_W * SCREEN_H];

static void dumpPPM(const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) { printf("cannot write %s\n", path); return; }
  fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
  for (int i = 0; i < SCREEN_W*SCREEN_H; i++) {
    uint16_t c = fb[i];
    unsigned char rgb[3] = {
      (unsigned char)(((c >> 11) & 31) * 255 / 31),
      (unsigned char)(((c >> 5)  & 63) * 255 / 63),
      (unsigned char)(( c        & 31) * 255 / 31)
    };
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

static void drain(Game& g) { uint8_t s; while (gamePopSfx(g, s)) {} }

// 鱼权值（和 game.cpp 的 KWGT 保持一致；那边是 internal，模拟器自己带一份）
static int kwgtOf(int k) {
  static const int W[FK_COUNT] = { 13, 48, 37, 60, 65, 64, 52, 68, 0 };
  return (k >= 0 && k < FK_COUNT) ? W[k] : 0;
}

int main(int argc, char** argv) {
  const char* prefix = (argc > 1) ? argv[1] : "frame";
  renderInit();

  Game g; gameInit(g, 0);
  const float dt = 1.f/30.f;
  int dRadar = 0, dFly = 0, dCatch = 0, dOver = 0;
  int cooldown = 0;

  for (int frame = 0; frame < 30*14; frame++) {
    Input in; memset(&in, 0, sizeof(in));


    if (g.phase == PH_RADAR) {
      // 像人一样瞄准：找角偏差最小的鱼，转到 ±3° 内再开火
      int best = -1; float bestDiff = 1e9f;
      for (int i = 0; i < MAX_FISH; i++) {
        const Fish& f = g.fish[i];
        if (!f.alive) continue;
        float dx = f.x - CANNON_X, dy = CANNON_Y - f.y;
        float d = sqrtf(dx*dx + dy*dy);
        if (d > 330.f || d < 70.f) continue;
        float a = atan2f(dy, dx) * 57.29578f;
        float diff = fabsf(a - g.cannonAng);
        if (diff < bestDiff) { bestDiff = diff; best = i; }
      }
      if (best >= 0) {
        const Fish& f = g.fish[best];
        float dx = f.x - CANNON_X, dy = CANNON_Y - f.y;
        float a = atan2f(dy, dx) * 57.29578f;
        if (a > g.cannonAng + 3.f)      in.left  = true;
        else if (a < g.cannonAng - 3.f) in.right = true;
        else if (cooldown <= 0)         in.fire  = true;
      }
    }
    if (g.phase == PH_OVER) in.fire = true;
    if (in.fire) cooldown = 40;

    gameUpdate(g, in, dt);
    if (cooldown > 0) cooldown--;
    drain(g);

    char path[128];
    if (g.phase == PH_RADAR && !dRadar && frame > 4) {
      renderFrame(fb, g, frame*dt); snprintf(path, sizeof(path), "%s_1_radar.ppm", prefix);
      dumpPPM(path);
      renderFrame(fb, g, frame*dt + 2.5f);            // 同一画面 2.5 秒后：验证水波在动
      snprintf(path, sizeof(path), "%s_1b_radar_t2.ppm", prefix);
      dumpPPM(path);
      dRadar = 1;
    }
    if (g.phase == PH_FLY && g.zoom > 0.9f && !dFly) {
      renderFrame(fb, g, frame*dt); snprintf(path, sizeof(path), "%s_2_fly.ppm", prefix);
      dumpPPM(path); dFly = 1;
    }
    if (g.phase == PH_CATCH && g.phaseT > 0.08f && !dCatch) {
      renderFrame(fb, g, frame*dt); snprintf(path, sizeof(path), "%s_3_catch.ppm", prefix);
      dumpPPM(path); dCatch = 1;
    }
    if (g.phase == PH_OVER && !dOver) {
      renderFrame(fb, g, frame*dt); snprintf(path, sizeof(path), "%s_4_over.ppm", prefix);
      dumpPPM(path); dOver = 1;
    }
  }
  printf("play  : score=%d wallet=%d best=%d shots-dumped=%d%d%d%d\n",
         g.score, g.wallet, g.best, dRadar, dFly, dCatch, dOver);

  // 稳定性压测：6 分钟不崩、金币守恒可查
  Game g2; gameInit(g2, 0);
  int maxCombo = 0;
  for (int frame = 0; frame < 30*360; frame++) {
    Input in; memset(&in, 0, sizeof(in));

    if (g2.phase == PH_RADAR) {
      if ((frame/11) % 2) in.left = true; else in.right = true;
      if (frame % 33 == 0) in.fire = true;
    }
    if (g2.phase == PH_OVER) in.fire = true;
    gameUpdate(g2, in, dt);
    drain(g2);
    if (g2.combo > maxCombo) maxCombo = g2.combo;
  }
  printf("stress: 6min ok score=%d wallet=%d best=%d maxcombo=%d\n",
         g2.score, g2.wallet, g2.best, maxCombo);

  // LV3 穿透对比：同一套 AI，只换炮台等级。单次结果方差太大，
  // 每个等级跑 5 局取平均（每局 3 分钟）。
  for (int lv = 0; lv <= 2; lv++) {
    double sumW = 0, sumFish = 0, sumRoi = 0, sumScore = 0;
    const int RUNS = 5;
    for (int run = 0; run < RUNS; run++) {
      Game p; gameInit(p, 0);
      p.cannonLv = (uint8_t)lv;
      for (int sec = 1; sec <= 180; sec++) {
        for (int f = 0; f < 30; f++) {
          Input in; memset(&in, 0, sizeof(in));
          int best = -1; float bestd = 1e9f;
          for (int i = 0; i < MAX_FISH; i++) {
            const Fish& fi = p.fish[i];
            if (!fi.alive) continue;
            float dx = fi.x - CANNON_X, dy = CANNON_Y - fi.y;
            float d = sqrtf(dx*dx + dy*dy);
            if (d < 70.f || d > 330.f) continue;
            if (d < bestd) { bestd = d; best = i; }
          }
          if (p.phase == PH_RADAR && best >= 0) {
            const Fish& fi = p.fish[best];
            float dx = fi.x - CANNON_X, dy = CANNON_Y - fi.y;
            float d = sqrtf(dx*dx + dy*dy);
            float tof = d / 420.f;
            float px = fi.x + fi.vx * tof, py = fi.y + fi.vy * tof;
            float a = atan2f(CANNON_Y - py, px - CANNON_X) * 57.29578f;
            if (a > p.cannonAng + 1.2f) in.left = true;
            else if (a < p.cannonAng - 1.2f) in.right = true;
            else in.fire = true;
          }
          if (p.phase == PH_OVER) in.fire = true;
          gameUpdate(p, in, dt);
          drain(p);
        }
      }
      sumW      += p.wallet;
      sumFish   += p.logTotalFish;
      sumScore  += p.score;
      sumRoi    += (double)p.returned / (p.invested > 1.f ? p.invested : 1.f);
    }
    printf("pierce: LV%d cost=%d  wallet(avg)=%.0f  fish(avg)=%.1f  score(avg)=%.0f  roi(avg)=%.2f  [%d runs/3min]\n",
           lv+1, lv==0?35:(lv==1?100:200), sumW/RUNS, sumFish/RUNS, sumScore/RUNS, sumRoi/RUNS, RUNS);
  }

  // 经济曲线：AI 会瞄准，代表"技术较好的玩家"上限
  {
    Game e; gameInit(e, 0);
    int w0 = e.wallet;
    printf("econ  : start=%d", w0);
    for (int sec = 1; sec <= 600; sec++) {
      for (int f = 0; f < 30; f++) {
        Input in; memset(&in, 0, sizeof(in));
        if (e.phase == PH_RADAR) {
          int best = -1; float bd = 1e9f;
          for (int i = 0; i < MAX_FISH; i++) {
            const Fish& fi = e.fish[i];
            if (!fi.alive) continue;
            float dx = fi.x - CANNON_X, dy = CANNON_Y - fi.y;
            float d = sqrtf(dx*dx + dy*dy);
            if (d > 330.f || d < 70.f) continue;
            // 真人会按目标换炮台：小鱼用 1，中鱼用 2，大鱼用 3
            float a = atan2f(dy, dx) * 57.29578f;
            float diff = fabsf(a - e.cannonAng);
            if (diff < bd) { bd = diff; best = i; }
          }
          // ===== 打窝策略：鱼稀了才撒一把（模拟懂钓鱼的玩家）=====
          {
            int aliveN = 0;
            for (int i = 0; i < MAX_FISH; i++) if (e.fish[i].alive) aliveN++;
            if (e.itemCd[0] <= 0.f && e.wallet >= 25 && aliveN < 6) in.item = 1;   // → gameUseItem(0) 打窝
          }
          if (best >= 0 && in.item == 0) {
            const Fish& fi = e.fish[best];
            int kw = kwgtOf(fi.kind);
            int wantLv = (kw > 55) ? 2 : (kw > 24) ? 1 : 0;
            if (e.wallet < (wantLv == 0 ? 4 : (wantLv == 1 ? 10 : 18))) wantLv = 0;
            e.cannonLv = (uint8_t)wantLv;
            float dx = fi.x - CANNON_X, dy = CANNON_Y - fi.y;
            float d  = sqrtf(dx*dx + dy*dy);
            float tof = d / 420.f;                     // 真实弹速 420（原写 520，提前量一直少算）
            float px = fi.x + fi.vx * tof;             // 提前量：鱼在弹到时的位置
            float py = fi.y + fi.vy * tof;
            float a = atan2f(CANNON_Y - py, px - CANNON_X) * 57.29578f;
            if (a > e.cannonAng + 1.2f) in.left = true;
            else if (a < e.cannonAng - 1.2f) in.right = true;
            else in.fire = true;
          }
        }
        if (e.phase == PH_OVER) in.fire = true;
        gameUpdate(e, in, dt);
        drain(e);
      }
      if (sec % 60 == 0) printf("  t=%ds wallet=%d score=%d roi=%.2f", sec, e.wallet, e.score, (double)e.returned/(e.invested>1.f?e.invested:1.f));
    }
    printf("\n");
  }
  // ===== 打窝后鱼必须恢复自由（曾经窝散了鱼还保持低速，直到潮汐推醒）=====
  {
    Game w; gameInit(w, 0);
    w.cannonAng = 55.f;
    w.wallet = 400;                        // 窝 300，给够钱才测得到
    int ok = gameUseItem(w, 0);           // 打窝
    Input z{};
    for (int f = 0; f < 30 * 9; f++) { gameUpdate(w, z, 1.f/30.f); drain(w); }
    int slow = 0, alive = 0;
    for (int i = 0; i < MAX_FISH; i++) {
      const Fish& fi = w.fish[i];
      if (!fi.alive) continue;
      alive++;
      if (fabsf(fi.vx) < 6.f) slow++;     // 基础速度 26~46，这里几乎为 0 = 卡住
    }
    printf("decoy : 打窝(%s) 9 秒后 → %d/%d 条几乎不动 (必须为 0)\n",
           ok ? "成功" : "失败", slow, alive);
    if (slow > 0) printf("        *** 失败：窝散了鱼还在低速，会被误认为'鱼不活了' ***\n");
  }

  // ===== 静置测试：一枪不放、一件道具不用，鱼不能凭空消失 =====
  // （真机上曾出现"放桌上一会儿鱼自己命中"：Input 未初始化，item 是栈上垃圾值）
  {
    Game q; gameInit(q, 0);
    Input z{};                       // 全零：没按键、没开火、没道具
    int fish0 = q.logTotalFish, score0 = q.score, w0 = q.wallet;
    for (int sec = 0; sec < 300; sec++)
      for (int f = 0; f < 30; f++) { gameUpdate(q, z, 1.f/30.f); drain(q); }
    printf("idle  : 5min 无输入 → 捕获 %d 条 | 分数 +%d | 金币 %d→%d  (必须全为 0)\n",
           q.logTotalFish - fish0, q.score - score0, w0, q.wallet);
    if (q.logTotalFish != fish0 || q.score != score0 || q.wallet != w0)
      printf("        *** 失败：静置时发生了捕获/得分，有脏输入 ***\n");
  }

  // 鱼种陈列（检查新加的鳗鱼精灵有没有画歪）
  {
    Game d; gameInit(d, 0);
    d.phase = PH_FLY; d.zoom = 1.f; d.camX = 262.f; d.camY = 182.f;
    for (int i = 0; i < MAX_FISH; i++) d.fish[i].alive = 0;
    for (int i = 0; i < FK_COUNT; i++) {
      d.fish[i].alive = 1;
      d.fish[i].kind = (uint8_t)i;
      d.fish[i].x = 305.f + (i % 4) * 62.f;
      d.fish[i].y = 208.f + (i / 4) * 64.f;
      d.fish[i].dir = (i & 1) ? -1 : 1;
    }
    d.bullet.active = 0;
    renderFrame(fb, d, 1.0f);
    dumpPPM("frame_5_fish.ppm");

    // 鱿鱼墨汁：强制一团墨盖在场上，检查遮挡范围和边缘
    {
      Game ig; gameInit(ig, 0);
      for (int i = 0; i < MAX_FISH; i++) if (ig.fish[i].alive && (i & 1)) ig.fish[i].alive = 0;
      ig.inkT = 0.55f; ig.squidX = 372.f; ig.squidY = 205.f;
      ig.zoom = 1.f/3.f;   // 雷达是 1/3 缩放
      renderFrame(fb, ig, 1.0f);
      dumpPPM("frame_9_ink.ppm");
    }
    // 打窝：飞行中 & 落水后（窝料多 / 快散）
    {
      Game dg2; gameInit(dg2, 0);
      dg2.cannonAng = 50.f; dg2.decoyX = 430.f; dg2.decoyY = 190.f;
      dg2.decoyT = DECOY_LIFE; dg2.decoyFly = 0.22f; dg2.decoyFeed = 1.f;
      dg2.zoom = 1.f/3.f;
      renderFrame(fb, dg2, 1.0f); dumpPPM("frame_12_decoy_fly.ppm");
    }
    {
      Game dg3; gameInit(dg3, 0);
      dg3.decoyX = 430.f; dg3.decoyY = 190.f;
      dg3.decoyT = 5.0f; dg3.decoyFly = 0.f; dg3.decoyFeed = 0.75f;
      dg3.zoom = 1.f/3.f;
      renderFrame(fb, dg3, 1.0f); dumpPPM("frame_13_decoy.ppm");
    }

    // 潜水员竖大拇指（彩蛋：这海里唯一会谢你的东西）
    {
      Game dg; gameInit(dg, 0);
      dg.phase = PH_CATCH; dg.catchKind = FK_DIVER; dg.catchFlip = 0;
      dg.catchX = 360.f; dg.catchY = 360.f; dg.lastGain = 10;
      dg.phaseT = 0.45f; dg.catchShake = 0.f; dg.combo = 1;
      renderFrame(fb, dg, 0.45f);
      dumpPPM("frame_10_thumb.ppm");
    }
    // 帮助页
    {
      Game hg; gameInit(hg, 0);
      hg.helpOpen = 1;
      renderFrame(fb, hg, 0.f);
      dumpPPM("frame_8_help.ppm");
    }

    // 鱼获图鉴（假数据，主要检查排版与对齐）
    {
      Game lg; gameInit(lg, 0);
      lg.listOpen = 1;
      lg.logCount[0]=24; lg.logValue[0]=3840;
      lg.logCount[2]=9;  lg.logValue[2]=2520;
      lg.logCount[6]=7;  lg.logValue[6]=3500;
      lg.logCount[5]=3;  lg.logValue[5]=4200;
      lg.logCount[7]=1;  lg.logValue[7]=4000;
      lg.logCount[8]=2;  lg.logValue[8]=200;
      lg.logTotalFish=46; lg.logTotalValue=18260;
      lg.playSec = 2*3600u + 34*60u; lg.peakWallet = 1250;
      for (int pg = 0; pg < 4; pg++) {
        lg.listPage = (uint8_t)pg;
        renderFrame(fb, lg, 0.f);
        char nm[32]; snprintf(nm, sizeof(nm), "frame_7_list_p%d.ppm", pg+1);
        dumpPPM(nm);
      }
    }

    // 挣脱特写（0.45s 处：鱼已经在跑、碎片散开）
    d.phase = PH_MISS; d.slow = 0.30f;
    d.zoom = 3.f; d.catchX = 360.f; d.catchY = 260.f; d.catchKind = FK_GOLD; d.catchFlip = 0;
    d.camX = d.catchX - 40.f; d.camY = d.catchY - 23.f;
    const float tm[3] = {0.18f, 0.46f, 0.74f};
    const char* nm[3] = {"frame_6a_struggle.ppm", "frame_6b_break.ppm", "frame_6c_flee.ppm"};
    for (int k = 0; k < 3; k++) { d.phaseT = tm[k]; renderFrame(fb, d, 1.0f); dumpPPM(nm[k]); }
    d.phaseT = 0.46f;
    renderFrame(fb, d, 1.0f);
    dumpPPM("frame_6_escape.ppm");
  }
  return 0;
}
