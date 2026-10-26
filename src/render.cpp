#include "render.h"
#include "palette.h"
#include "font5x7.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint16_t* FB = nullptr;
static float gTime = 0.f;                 // 秒，供水波/呼吸使用
static float gSinTab[257];
static inline float fsin(float a) {       // 查表 sinf，比 libm 快 5~10 倍
  float k = a * 40.7436654f;              // 256 / (2π)
  int i = (int)k;
  float f = k - (float)i;
  i &= 255;
  return gSinTab[i] + (gSinTab[i+1] - gSinTab[i]) * f;
}
static const float D2R = 3.14159265f / 180.f;
static const float R2D = 180.f / 3.14159265f;
static const float CATCH_HOLD_SEC  = 0.26f;   // 与 game.cpp 的 CATCH_HOLD 对齐
static const float CATCH_TOTAL_SEC = 1.30f;
static const float MISS_TOTAL_SEC  = 0.90f;   // 与 game.cpp 的 MISS_TOTAL 对齐

// ---------------- 基本绘制 ----------------
static inline void px(int x, int y, uint16_t c) {
  if ((unsigned)x < (unsigned)SCREEN_W && (unsigned)y < (unsigned)SCREEN_H)
    FB[y*SCREEN_W + x] = c;
}
static inline void rect(int x, int y, int w, int h, uint16_t c) {
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = (x+w) > SCREEN_W ? SCREEN_W : (x+w);
  int y1 = (y+h) > SCREEN_H ? SCREEN_H : (y+h);
  for (int j = y0; j < y1; j++) { uint16_t* r = FB + j*SCREEN_W; for (int i = x0; i < x1; i++) r[i] = c; }
}
static inline void hline(int x, int y, int w, uint16_t c) { rect(x, y, w, 1, c); }
// 描边矩形（rect 是实心填充，当外框用会把内容整个盖住）
static inline void boxOutline(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  hline(x, y, w, c);
  hline(x, y + h - 1, w, c);
  for (int j = 1; j < h - 1; j++) { px(x, y + j, c); px(x + w - 1, y + j, c); }
}
static void disc(int cx, int cy, int r, uint16_t c, bool ring = false) {
  int r2 = r*r, ri2 = ring ? (r-2)*(r-2) : -1;
  for (int j = cy-r; j <= cy+r; j++)
    for (int i = cx-r; i <= cx+r; i++) {
      int d = (i-cx)*(i-cx) + (j-cy)*(j-cy);
      if (d <= r2 && (!ring || d >= ri2)) px(i, j, c);
    }
}

// ---------------- 字体 ----------------
static const Glyph* gGlyph[128];
static int textW(const char* s, int sc) { int n = (int)strlen(s); return n ? n*6*sc - sc : 0; }
static void text(const char* s, int x, int y, uint16_t col, int sc = 1, int shadow = 0) {
  if (shadow) text(s, x+sc, y+sc, RAMP(0), sc, 0);
  int cx = x;
  for (const char* p = s; *p; p++) {
    char c = *p;
    if (c >= 'a' && c <= 'z') c -= 32;
    const Glyph* g = ((unsigned char)c < 128) ? gGlyph[(int)c] : nullptr;
    if (g) for (int j = 0; j < 7; j++) {
      uint8_t row = g->row[j];
      for (int i = 0; i < 5; i++) if (row & (1 << (4-i))) rect(cx+i*sc, y+j*sc, sc, sc, col);
    }
    cx += 6*sc;
  }
}

// ---------------- 噪声 ----------------
static inline uint32_t ihash(int x, int y, uint32_t s) {
  uint32_t h = (uint32_t)x*374761393u + (uint32_t)y*668265263u + s*2246822519u;
  h = (h ^ (h>>13)) * 1274126177u;
  return h ^ (h>>16);
}
static inline float hf(int x, int y, uint32_t s) { return (ihash(x,y,s) & 0xFFFF) * (1.f/65535.f); }
static inline float vnoise(float x, float y, uint32_t s) {
  int xi = (int)x, yi = (int)y;
  if (x < 0.f && (float)xi != x) xi--;
  if (y < 0.f && (float)yi != y) yi--;
  float fx = x - (float)xi, fy = y - (float)yi;
  fx = fx*fx*(3.f-2.f*fx); fy = fy*fy*(3.f-2.f*fy);
  float a = hf(xi,yi,s), b = hf(xi+1,yi,s), c = hf(xi,yi+1,s), d = hf(xi+1,yi+1,s);
  float ab = a + (b-a)*fx, cd = c + (d-c)*fx;
  return ab + (cd-ab)*fy;
}

// ---------------- 鱼精灵 ----------------
struct Spr { int w, h; uint8_t m[112*48]; };   // 最大的精灵是鲨鱼 110x46
static Spr gSpr[FK_COUNT];

static const uint16_t* palOf(uint8_t k) {
  switch (k) {
    case FK_SMALL: return PAL_SMALL;
    case FK_MID:   return PAL_MID;
    case FK_EEL:   return PAL_EEL;
    case FK_GOLD:  return PAL_GOLD;
    case FK_MANTA: return PAL_MANTA;
    case FK_SQUID: return PAL_SQUID;
    case FK_SHARK: return PAL_SHARK;
    case FK_DIVER: return PAL_DIVER;
    default:       return PAL_MON;
  }
}

static void buildFish(int kind) {
  const int L = fishWorldW(kind), Hh = fishWorldH(kind);
  const bool stripe = (kind == FK_GOLD);
  const bool big = (kind == FK_GOLD || kind == FK_MON);
  Spr& s = gSpr[kind];
  s.w = L; s.h = Hh;
  memset(s.m, 0, sizeof(s.m));
  float cx = L*0.60f, cy = Hh*0.50f, rx = L*0.34f, ry = Hh*0.40f, tail_x = L*0.23f;
  for (int y = 0; y < Hh; y++) for (int x = 0; x < L; x++) {
    float pxx = x+0.5f, pyy = y+0.5f; int v = 0;
    float ex = (pxx-cx)/rx, ey = (pyy-cy)/ry;
    if (ex*ex + ey*ey <= 1.f) v = 2;
    if (pxx < tail_x) {
      float t = (tail_x-pxx)/tail_x;
      float half = ry*(0.50f + 1.55f*t);
      if (fabsf(pyy-cy) <= half && !(t > 0.30f && fabsf(pyy-cy) < ry*0.50f)) v = (t > 0.55f) ? 3 : 2;
    }
    if (pxx >= 0.34f*L && pxx <= 0.74f*L) {
      float fh = ry*(0.95f - fabsf(pxx-0.54f*L)/(0.20f*L)*0.55f);
      if (pyy >= cy-ry-1.2f && pyy <= cy-ry+fh*0.9f) v = 2;
    }
    if (pxx >= 0.38f*L && pxx <= 0.60f*L && pyy >= cy+ry-1.0f && pyy <= cy+ry+ry*0.40f) v = 2;
    if (pxx > L-1.6f && fabsf(pyy-cy) < ry*0.42f) v = 2;
    s.m[y*L+x] = (uint8_t)v;
  }
  // 三色体积（上亮下暗，光从水面来）
  for (int x = 0; x < L; x++) {
    int top = -1, bot = -1;
    for (int y = 0; y < Hh; y++) if (s.m[y*L+x]) { if (top < 0) top = y; bot = y; }
    if (top < 0) continue;
    int hgt = bot-top+1;
    for (int y = top; y <= bot; y++) {
      uint8_t v = s.m[y*L+x];
      if (!v || v == 3) continue;
      float r = (y-top)/(float)hgt;
      s.m[y*L+x] = (r < 0.30f) ? 3 : ((r > 0.74f) ? 1 : 2);
    }
  }
  if (stripe) {
    for (int x = 0; x < L; x++) {
      if (!(x > 0.36f*L && x < 0.86f*L)) continue;
      int k = (int)(x - 0.36f*L) % 6;
      if (k != 2 && k != 3) continue;
      for (int y = 0; y < Hh; y++) {
        uint8_t v = s.m[y*L+x];
        if (v == 2) s.m[y*L+x] = 3;
        else if (v == 3 && y > Hh*0.34f) s.m[y*L+x] = 2;
      }
    }
  }
  int er = big ? 2 : 1;
  int exx = (int)(cx + rx*0.66f), eyy = (int)(cy - ry*0.26f);
  for (int dy = -er; dy <= er; dy++) for (int dx = -er; dx <= er; dx++)
    if (dx*dx + dy*dy <= er*er) {
      int yy = eyy+dy, xx = exx+dx;
      if (yy >= 0 && yy < Hh && xx >= 0 && xx < L && s.m[yy*L+xx]) s.m[yy*L+xx] = 1;
    }
  { int yy = eyy-er, xx = exx-er;
    if (yy >= 0 && yy < Hh && xx >= 0 && xx < L && s.m[yy*L+xx]) s.m[yy*L+xx] = 3; }
  { int cyi = (int)cy;
    if (cyi >= 0 && cyi < Hh) s.m[cyi*L + L-1] = 1;
    if (cyi-1 >= 0) s.m[(cyi-1)*L + L-1] = 1; }
}

// 潜水员：手绘人形。面罩 + 躯干 + 前伸手臂 + 腿 + 脚蹼 + 上浮气泡。
static void drawDiverShape(int sx, int sy, int z, uint16_t outline) {
  const uint16_t* p = PAL_DIVER;
  const int W = 18, H = 30;
  if (z <= 0) z = 1;

  // 躯干（外亮内暗，让轮廓从深水里跳出来）
  rect(sx + 6*z, sy + 8*z, 6*z, 11*z, p[3]);
  rect(sx + 7*z, sy + 9*z, 4*z, 9*z, p[2]);
  // 腰带（区分上身/下身，人形更容易读）
  rect(sx + 6*z, sy + 17*z, 6*z, 1*z, p[1]);
  // 面罩
  disc(sx + 9*z, sy + 4*z, 3*z, p[3]);
  disc(sx + 9*z, sy + 4*z, 2*z, p[2]);
  px(sx + 8*z, sy + 3*z, p[3]);
  // 前伸的手臂（亮色，最容易读成"人"的部分）
  rect(sx + 1*z, sy + 10*z, 5*z, 2*z, p[3]);
  rect(sx + 12*z, sy + 10*z, 5*z, 2*z, p[3]);
  // 腿 + 脚蹼
  rect(sx + 7*z, sy + 19*z, 1*z, 6*z, p[2]);
  rect(sx + 10*z, sy + 19*z, 1*z, 6*z, p[2]);
  rect(sx + 5*z, sy + 25*z, 3*z, 2*z, p[3]);
  rect(sx + 10*z, sy + 25*z, 3*z, 2*z, p[3]);
  // 上浮气泡（随时间往上飘）
  for (int k = 0; k < 3; k++) {
    int by = (int)(sy + (2 - k*3)*z - fmodf(gTime * 9.f + k * 5.f, 13.f) * z);
    if (by > sy - 6*z) px(sx + (7 + k*2)*z, by, p[3]);
  }
}

// 鱿鱼：尖头 + 竖长身体 + 5 条摆动触手（fish_g 画的是横鱼，这里必须自己画）
static void drawSquidShape(int sx, int sy, int z, uint16_t outline) {
  const uint16_t* p = PAL_SQUID;
  if (z <= 0) z = 1;
  const int cx = sx + 13*z;
  // 头（上尖下宽）
  for (int j = 0; j < 15*z; j++) {
    int half = (j * 9 * z) / (15*z);
    for (int i = -half; i <= half; i++) px(cx + i, sy + j, (i > -half+2 && i < half-2) ? p[2] : p[1]);
  }
  // 身体
  rect(sx + 8*z, sy + 15*z, 11*z, 9*z, p[2]);
  rect(sx + 10*z, sy + 16*z, 7*z, 7*z, p[1]);
  // 眼睛
  px(sx + 9*z,  sy + 9*z, p[3]);
  px(sx + 17*z, sy + 9*z, p[3]);
  // 触手：随时间左右摆
  for (int t = 0; t < 5; t++) {
    int bx = sx + (7 + t*3)*z;
    for (int k = 0; k < 16*z; k++) {
      int wob = (int)(sinf(gTime * 3.2f + t * 0.9f + k * 0.22f) * 2.2f * z);
      px(bx + wob, sy + 24*z + k, (k < 6*z) ? p[2] : p[1]);
    }
  }

}

// 图鉴行首的迷你图标：按整数步长降采样；鱿鱼/潜水员走简化形状
static void drawMini(int x, int y, int maxW, int maxH, uint8_t kind, bool got = true) {
  static const uint16_t SIL[4] = {0, 0x18C3, 0x18C3, 0x2945};   // 未捕获：统一灰剪影
  const uint16_t* pal = got ? palOf(kind) : SIL;
  // 这两种没有可降采样的精灵，按图标框（24×16）手工排布，
  // 否则会比别的鱼小一大圈
  if (kind == FK_SQUID) {
    rect(x + 9,  y + 1, 7, 7, pal[2]);       // 外套膜
    rect(x + 11, y,     3, 2, pal[3]);       // 尖顶
    rect(x + 9,  y + 8, 2, 6, pal[1]);       // 触手 ×3
    rect(x + 12, y + 8, 2, 7, pal[1]);
    rect(x + 15, y + 8, 2, 6, pal[1]);
    return;
  }
  if (kind == FK_DIVER) {
    rect(x + 9,  y + 1, 6, 5, pal[3]);       // 头盔
    rect(x + 8,  y + 2, 2, 3, pal[1]);
    rect(x + 7,  y + 6, 9, 6, pal[2]);       // 躯干
    rect(x + 1,  y + 7, 6, 2, pal[3]);       // 手臂
    rect(x + 16, y + 7, 6, 2, pal[3]);
    rect(x + 8,  y + 12, 3, 4, pal[1]);      // 腿
    rect(x + 13, y + 12, 3, 4, pal[1]);
    return;
  }
  const Spr& s2 = gSpr[kind];
  int step = (s2.w + maxW - 1) / maxW;
  if (step < 1) step = 1;
  int dw = s2.w / step, dh = s2.h / step;
  if (dw > maxW) dw = maxW;
  if (dh > maxH) dh = maxH;
  for (int j = 0; j < dh; j++)
    for (int i = 0; i < dw; i++) {
      uint8_t v = s2.m[(j*step)*s2.w + (i*step)];
      if (v) px(x + i, y + j, pal[v & 3]);
    }
}

// 帮助页：一屏说清所有按键。和鱼获本同一套版式（2 倍标题 + 分隔线）。
static void drawHelp() {
  static const char* KEY[7] = { ", /", "SPACE", "1 2 3", "I", "M", "L", "H" };
  static const char* WHAT[7] = { "AIM LEFT/RIGHT", "FIRE", "CANNON LEVEL", "TILT AIM ON/OFF",
                                 "SOUND ON/OFF", "CATCH LOG", "THIS HELP" };
  rect(0, 0, SCREEN_W, SCREEN_H, PAL_BG1[0]);
  text("HELP", 6, 3, PAL_FX[3], 2, 1);
  text("KEYS", SCREEN_W - 6 - textW("KEYS", 2), 3, PAL_BG2[2], 2, 1);
  hline(0, 20, SCREEN_W, PAL_BG1[2]);

  for (int i = 0; i < 7; i++) {
    int y = 25 + i * 14;
    text(KEY[i], 8, y, PAL_FX[2], 1, 0);           // 键名：金色
    text(WHAT[i], 62, y, PAL_BG2[3], 1, 0);        // 说明：青白
  }

  hline(0, SCREEN_H - 20, SCREEN_W, PAL_BG1[2]);
  text("h", 6, SCREEN_H - 15, PAL_FX[2], 1, 0);      // 键名同样用金色
  text("CLOSE", 6 + 14, SCREEN_H - 15, PAL_BG2[2], 1, 0);
  text("l=CATCH LOG", SCREEN_W - 6 - textW("l=CATCH LOG", 1), SCREEN_H - 15, PAL_BG1[3], 1, 0);
}

// 图鉴第 1 页：总账。这些数字塞底栏会越来越挤，单独给一页正好。
static void drawStats(const Game& g) {
  const int PAGES = 4;
  rect(0, 0, SCREEN_W, SCREEN_H, PAL_BG1[0]);
  text("STATS", 6, 3, PAL_FX[3], 2, 1);
  char b[28];
  snprintf(b, sizeof(b), "%d/%d", g.listPage + 1, PAGES);
  text(b, SCREEN_W - 6 - textW(b, 2), 3, PAL_BG2[3], 2, 1);
  hline(0, 20, SCREEN_W, PAL_BG1[2]);

  static const char* LBL[4] = { "BEST SCORE", "PEAK COINS", "FISH CAUGHT", "PLAY TIME" };
  for (int i = 0; i < 4; i++) {
    int y = 26 + i * 22;
    text(LBL[i], 8, y + 6, PAL_BG2[2], 1, 0);
    if (i == 0)      snprintf(b, sizeof(b), "%d", g.best);
    else if (i == 1) snprintf(b, sizeof(b), "%d", g.peakWallet);
    else if (i == 2) snprintf(b, sizeof(b), "%u", (unsigned)g.logTotalFish);
    else {
      uint32_t ps = g.playSec;
      if (ps >= 3600)    snprintf(b, sizeof(b), "%luh%lum", (unsigned long)(ps/3600), (unsigned long)(ps%3600/60));
      else if (ps >= 60) snprintf(b, sizeof(b), "%lum%lus", (unsigned long)(ps/60), (unsigned long)(ps%60));
      else               snprintf(b, sizeof(b), "%lus", (unsigned long)ps);
    }
    text(b, SCREEN_W - 8 - textW(b, 2), y + 2, (i == 3) ? PAL_BG2[3] : PAL_FX[3], 2, 1);
  }

  hline(0, SCREEN_H - 20, SCREEN_W, PAL_BG1[2]);
  text(",/", 6, SCREEN_H - 15, PAL_FX[2], 1, 0);
  text("NEXT", 6 + 20, SCREEN_H - 15, PAL_BG2[2], 1, 0);
  text("l", SCREEN_W - 6 - textW("l CLOSE", 1), SCREEN_H - 15, PAL_FX[2], 1, 0);
  text("CLOSE", SCREEN_W - 6 - 26, SCREEN_H - 15, PAL_BG2[2], 1, 0);
}

// 鱼获图鉴：第 1 页总账，第 2 页起才是鱼列表（每页 4 条 + 2 倍字体）。
static void drawList(const Game& g) {
  static const char* NAMES[FK_COUNT] = {
    "MINNOW", "SNAP", "EEL", "GOLD", "ODD", "MANTA", "SQUID", "SHARK", "DIVER"
  };
  if (g.listPage == 0) { drawStats(g); return; }
  const int PER_PAGE = 4;
  const int PAGES = 1 + (FK_COUNT + PER_PAGE - 1) / PER_PAGE;   // 统计页 + 鱼页
  const int page = g.listPage - 1;
  rect(0, 0, SCREEN_W, SCREEN_H, PAL_BG1[0]);

  text("CATCH LOG", 6, 3, PAL_FX[3], 2, 1);
  char b[32];
  snprintf(b, sizeof(b), "%d/%d", g.listPage + 1, PAGES);
  text(b, SCREEN_W - 6 - textW(b, 2), 3, PAL_BG2[3], 2, 1);
  hline(0, 20, SCREEN_W, PAL_BG1[2]);

  for (int r = 0; r < PER_PAGE; r++) {
    int k = page * PER_PAGE + r;
    if (k >= FK_COUNT) break;
    int y = 23 + r * 23;
    bool got = g.logCount[k] > 0;

    drawMini(6, y + 3, 24, 16, (uint8_t)k, got);
    text(NAMES[k], 34, y + 4, got ? PAL_BG2[3] : PAL_BG1[3], 2, 0);

    if (got) {
      // 数量是重点，但"x"保持小写小号，别喧宾夺主
      snprintf(b, sizeof(b), "%u", (unsigned)g.logCount[k]);
      int dw = textW(b, 2);
      text("x", SCREEN_W - 10 - dw - 7, y + 11, PAL_FX[3], 1, 0);
      text(b, SCREEN_W - 8 - dw, y + 5, PAL_FX[3], 2, 1);
    } else {
      text("--", SCREEN_W - 8 - textW("--", 2), y + 5, PAL_BG1[2], 2, 0);
    }
  }

  // 底部栏：与顶栏同高（20px）。左侧用 2x 数字压住重心，
  // 和顶部 2x 的 CATCH LOG 形成对称；右侧是时长与关闭提示。
  hline(0, SCREEN_H - 20, SCREEN_W, PAL_BG1[2]);
  // 底栏只留"总数 + 关闭"—— 金额和时长已经挪到第 1 页总账了
  snprintf(b, sizeof(b), "%u", (unsigned)g.logTotalFish);
  int fw = textW(b, 2);
  text(b, 6, SCREEN_H - 16, PAL_FX[3], 2, 1);
  text("FISH", 6 + fw + 4, SCREEN_H - 12, PAL_FX[2], 1, 0);
  text(",/", SCREEN_W - 6 - textW(",/  l CLOSE", 1), SCREEN_H - 15, PAL_FX[2], 1, 0);
  text("PAGE", SCREEN_W - 6 - textW("PAGE  l CLOSE", 1), SCREEN_H - 15, PAL_BG1[3], 1, 0);
  text("l", SCREEN_W - 6 - textW("l CLOSE", 1), SCREEN_H - 15, PAL_FX[2], 1, 0);
  text("CLOSE", SCREEN_W - 6 - 26, SCREEN_H - 15, PAL_BG2[2], 1, 0);
}

static void drawFish(int sx, int sy, int zoom, uint8_t kind, bool flip, uint16_t outline, bool hasOutline) {
  if (kind == FK_DIVER) { drawDiverShape(sx, sy, zoom, outline); return; }
  if (kind == FK_SQUID) { drawSquidShape(sx, sy, zoom, hasOutline ? outline : 0); return; }
  const Spr& s = gSpr[kind];
  const uint16_t* pal = palOf(kind);
  const int w = s.w, h = s.h;
  if (hasOutline) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
      int xx = flip ? (w-1-i) : i;
      if (s.m[j*w+xx]) continue;
      bool nb = false;
      if (i > 0   && s.m[j*w + (flip ? (w-1-(i-1)) : (i-1))]) nb = true;
      if (i < w-1 && s.m[j*w + (flip ? (w-1-(i+1)) : (i+1))]) nb = true;
      if (j > 0   && s.m[(j-1)*w+xx]) nb = true;
      if (j < h-1 && s.m[(j+1)*w+xx]) nb = true;
      if (nb) rect(sx+i*zoom, sy+j*zoom, zoom, zoom, outline);
    }
  }
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
    int xx = flip ? (w-1-i) : i;
    uint8_t v = s.m[j*w+xx];
    if (v) rect(sx+i*zoom, sy+j*zoom, zoom, zoom, pal[v]);
  }
}

// ---------------- 金币 ----------------
static void coinIcon(int x, int y, int r = 4, int frame = 0) {
  static const float wmul[4] = {1.f, 0.70f, 0.36f, 0.70f};
  float w = r * wmul[frame & 3];
  for (int dy = -r-1; dy <= r+1; dy++) for (int dx = -r-1; dx <= r+1; dx++) {
    float e = (dx*dx)/(w*w) + (dy*dy)/(float)(r*r);
    if (e <= 1.f) px(x+dx, y+dy, PAL_FX[2]);
    else if (e <= 1.7f) px(x+dx, y+dy, PAL_FX[3]);
  }
  if (w > r*0.6f) { px(x-1, y-1, PAL_FX[1]); px(x-1, y-2, PAL_FX[0]); }
}

// ---------------- HUD ----------------
static void hud(const Game& g, bool hot = false) {
  rect(0, 0, SCREEN_W, 16, RAMP(0));
  hline(0, 15, SCREEN_W, hot ? PAL_FX[2] : PAL_BG2[2]);
  text("SCORE", 4, 5, PAL_BG2[2], 1);
  char b[16];
  snprintf(b, sizeof(b), "%d", (int)(g.scoreAnim + 0.5f));
  text(b, 38, 1, hot ? PAL_FX[1] : PAL_UI[3], 2);
  if (g.combo > 1) {
    snprintf(b, sizeof(b), "X%d", g.combo);
    text(b, 104, 1, PAL_FX[2], 2);
    int n = g.combo > 6 ? 6 : g.combo;
    for (int k = 0; k < n; k++) rect(106+k*4, 12, 3, 2, PAL_FX[3]);
  }
  // 金币：够 100 才能开炮
  coinIcon(150, 8);
  snprintf(b, sizeof(b), "%d", g.wallet);
  text(b, 158, 5, (g.wallet >= CANNON_COST[0]) ? PAL_FX[1] : PAL_BG2[3], 1);
  {
    char lb[16];
    snprintf(lb, sizeof(lb), "LV%d %d", g.cannonLv + 1, CANNON_COST[g.cannonLv]);
    text(lb, 198, 5, (g.wallet >= CANNON_COST[g.cannonLv]) ? PAL_FX[3] : PAL_BG2[3], 1);
  }
}

// 底部：金币 / 充能条 / 开火提示
static void bottomHints(const Game& g, const char* fireHint) {
  const int bx = 6, by = 121, bw = 70;      // 长度 2/3、高度 1/2：别抢水面
  bool ready = (g.wallet >= CANNON_COST[0]);
  rect(bx-1, by-1, bw+2, 5, RAMP(0));
  int fill = ready ? bw : (int)(bw * (g.rechargeT / RECHARGE_SEC));
  if (fill > 0) rect(bx, by, fill, 3, ready ? PAL_FX[1] : PAL_BG2[3]);
  for (int k = 1; k < 4; k++) rect(bx + bw*k/4, by, 1, 3, RAMP(0));
  if (!ready) {
    char b[20];
    int left = (int)(RECHARGE_SEC - g.rechargeT) + 1;
    snprintf(b, sizeof(b), "NO COIN  %dS", left);
    text(b, bx, 128, PAL_FX[3], 1, 1);
  } else if (g.dry >= 4) {
    text("FREE SHOT", bx, 128, PAL_FX[2], 1, 1);   // 连续空炮的补偿
  } else {
    text("READY", bx, 128, PAL_BG3[2], 1, 1);
  }
  if (fireHint) text(fireHint, 150, 122, PAL_FX[3], 1, 1);
}

// ---------------- 雷达 ----------------
static void radarBg() {
  const float t = gTime;
  for (int y = 16; y < SCREEN_H; y += 2) {
    for (int x = 0; x < SCREEN_W; x += 2) {
      // 1) 极慢漂移的深浅斑（水体本身在呼吸）
      float n = vnoise(x*0.018f + t*0.28f, y*0.024f - t*0.16f, 3u);
      // 2) 相位被噪声扰动，避免出现规则斜纹
      float ph = (vnoise(x*0.010f + t*0.05f, y*0.010f, 5u) - 0.5f) * 4.2f;
      // 3) 两个方向的正弦**相乘**：只有交点处亮 => 格子状波光，不是条纹
      float w = fsin(x*0.052f + y*0.027f + t*0.85f + ph)
              * fsin(x*0.029f - y*0.046f - t*0.66f + ph);
      float v = n*0.78f + w*0.34f;
      uint16_t c;
      if (v > 0.86f)      c = RAMP(2);       // 波光交点：一闪
      else if (v > 0.61f) c = RAMP(1);
      else                c = RAMP(0);
      px(x,y,c); px(x+1,y,c); px(x,y+1,c); px(x+1,y+1,c);
    }
  }
}

static float norm180f(float d) {
  while (d >  180.f) d -= 360.f;
  while (d < -180.f) d += 360.f;
  return d;
}

static void drawCannon(float cx, float cy, float angDeg, bool aimTicks) {
  if (aimTicks) for (int sgn = -1; sgn <= 1; sgn += 2) {
    float a = (angDeg + sgn*7.f)*D2R;
    for (float t = 20.f; t < 62.f; t += 1.8f)
      px((int)(cx+cosf(a)*t), (int)(cy-sinf(a)*t), PAL_BG1[3]);
  }
  float a = angDeg*D2R;
  disc((int)cx, (int)cy, 7, RAMP(0));
  disc((int)cx, (int)cy, 6, PAL_UI[2]);
  disc((int)cx-1, (int)cy-1, 4, PAL_UI[3]);
  disc((int)cx-1, (int)cy-1, 2, PAL_UI[2]);
  for (float t = 2.f; t < 16.f; t += 0.4f)
    for (float o = -2.6f; o <= 2.6f; o += 0.65f)
      px((int)(cx+cosf(a)*t + sinf(a)*o), (int)(cy-sinf(a)*t + cosf(a)*o),
         (fabsf(o) < 1.6f) ? PAL_UI[3] : PAL_UI[1]);
  disc((int)(cx+cosf(a)*15), (int)(cy-sinf(a)*15), 3, PAL_UI[2]);
  disc((int)(cx+cosf(a)*15), (int)(cy-sinf(a)*15), 1, PAL_FX[2]);
}

static void drawRadar(const Game& g) {
  radarBg();
  const float cx = CANNON_X * g.zoom, cy = CANNON_Y * g.zoom;

  // 瞄准指示：只一条短虚线，别盖住水面波光
  {
    float a = g.cannonAng*D2R;
    for (float t = 17.f; t < 64.f; t += 4.6f)
      px((int)(cx+cosf(a)*t), (int)(cy-sinf(a)*t), PAL_FX[2]);
  }

  const int RR[4] = {26, 52, 78, 104};
  for (int k = 0; k < 4; k++) {
    bool main = (k == 1);
    uint16_t col = main ? PAL_BG2[3] : PAL_BG2[1];
    for (int deg = 0; deg < 360; deg += 2) {
      if ((deg/2) % 5 == 0) continue;
      float a = deg*D2R;
      for (int o = 0; o < (main ? 2 : 1); o++) {
        int r = RR[k]+o;
        px((int)(cx+cosf(a)*r), (int)(cy+sinf(a)*r), col);
      }
    }
  }

  for (int i = 0; i < MAX_FISH; i++) {
    const Fish& f = g.fish[i];
    if (!f.alive) continue;
    float sx = f.x*g.zoom, sy = f.y*g.zoom;
    float ang = atan2f(cy-sy, sx-cx)*R2D;
    bool bright = fabsf(norm180f(g.cannonAng - ang)) < 44.f;
    const uint16_t* pal = palOf(f.kind);
    int dim = 0;
    int r = 2;
    if (f.kind == FK_MID) r = 3; else if (f.kind == FK_GOLD) r = 4; else if (f.kind == FK_MON) r = 5;
    else if (f.kind == FK_SHARK) r = 8; else if (f.kind == FK_DIVER) r = 3;
    else if (f.kind == FK_MANTA) r = 7; else if (f.kind == FK_SQUID) r = 3;
    disc((int)sx, (int)sy, r, pal[(1-dim) < 0 ? 0 : (1-dim)]);
    if (r >= 3) { disc((int)sx, (int)sy, r-1, pal[bright?(3-dim):(2-dim)]); px((int)sx, (int)sy, pal[3-dim]); }
    else disc((int)sx, (int)sy, 1, pal[bright?(3-dim):(2-dim)]);
    for (int k = 1; k <= 3; k++) px((int)(sx - f.dir*(r+k+1)), (int)sy, pal[bright?(1-dim):0]);
  }

  for (int deg = 0; deg < 360; deg += 5) if ((deg/5) % 2 == 0) {
    float a = deg*D2R;
    px((int)(cx+cosf(a)*12), (int)(cy+sinf(a)*12), PAL_BG3[1]);
    px((int)(cx+cosf(a)*8),  (int)(cy+sinf(a)*8),  PAL_BG2[2]);
  }
  drawCannon(cx, cy + g.recoil, g.cannonAng, false);
  // 炮口闪光
  if (g.muzzle > 0.f) {
    float a = g.cannonAng*D2R;
    float mx = cx + cosf(a)*16.f, my = cy + g.recoil - sinf(a)*16.f;
    int r = 3 + (int)(g.muzzle * 34.f);
    disc((int)mx, (int)my, r, PAL_FX[3], true);
    disc((int)mx, (int)my, r-2, PAL_FX[1], true);
    px((int)mx, (int)my, PAL_FX[0]);
  }

  // 钱不够：闪一下。音效会被 BGM 压住，画面不会
  if (g.noMoneyT > 0.f && fmodf(g.noMoneyT, 0.25f) > 0.12f) {
    const char* nm = "NOT ENOUGH";
    int tw = (int)strlen(nm) * 6;
    text(nm, (SCREEN_W - tw)/2, 100, PAL_FX[0], 1, 1);
  }

  // ---- 打窝：一颗饵从炮口抛出去，落水，然后冒泡、圈子慢慢缩小 ----
  // 不画出来就是"按一下鱼就来了"，赤裸；画出来才是钓佬的动作。
  if (g.decoyT > 0.f) {
    float sx = g.decoyX * g.zoom, sy = g.decoyY * g.zoom;
    if (g.decoyFly > 0.f) {
      float t  = 1.f - g.decoyFly / DECOY_FLY;             // 0 出手 → 1 落水
      float cx = CANNON_X * g.zoom, cy = CANNON_Y * g.zoom;
      float x  = cx + (sx - cx) * t;
      float y  = cy + (sy - cy) * t - sinf(t * 3.14159f) * 9.f;   // 抛物弧
      disc((int)x, (int)y, 1, PAL_GOLD[3]);
      px((int)(x - (x-cx) * 0.08f), (int)(y + 2), PAL_GOLD[1]);   // 一点拖尾
    } else {
      float feed = g.decoyFeed;
      float life = 1.f - g.decoyT / DECOY_LIFE;
      // 落水涟漪：只在头 0.5 秒
      if (life < 0.10f) {
        int rr = (int)(life / 0.10f * 24.f);
        for (int deg = 0; deg < 360; deg += 9) {
          float a = deg * D2R;
          px((int)(sx + cosf(a)*rr), (int)(sy + sinf(a)*rr), PAL_FX[2]);
        }
      }
      // 一把饵：中心一团 + 周围撒开的碎屑。一颗孤零零的点读不出"打窝"
      disc((int)sx, (int)sy, 3, PAL_GOLD[2]);
      px((int)sx, (int)sy, PAL_GOLD[3]);
      for (int k = 0; k < 8; k++) {
        float a2 = (float)k * 45.f * D2R + g.decoyT * 0.6f;
        int bx = (int)(sx + cosf(a2) * (6.f + (float)(k & 1) * 3.5f));
        int by = (int)(sy + sinf(a2) * (5.f + (float)((k >> 1) & 1) * 3.f));
        px(bx, by, (k & 1) ? PAL_GOLD[3] : PAL_GOLD[2]);
      }
      // 气泡：三个相位错开的，往上飘；窝料越少飘得越稀
      for (int k = 0; k < 3; k++) {
        float ph = fmodf(g.decoyT * (1.4f + k * 0.31f) + (float)k * 0.33f, 1.f);
        if (ph < feed) {
          int bx = (int)(sx + sinf(ph * 5.f + (float)k * 2.1f) * 4.f);
          int by = (int)(sy - ph * 13.f);
          px(bx, by, PAL_FX[3]);
          if (ph > 0.25f) px(bx, by + 1, PAL_GOLD[1]);
        }
      }
      // 号召力圈：密一点的点圈（雷达圈是疏虚线，不能长得一样）
      int RR = (int)(DECOY_R * (0.45f + 0.55f * feed) * g.zoom);
      for (int deg = 0; deg < 360; deg += 5) {
        float a = deg * D2R;
        px((int)(sx + cosf(a)*RR), (int)(sy + sinf(a)*RR), PAL_GOLD[2]);
      }
    }
  }

  // 鱿鱼墨汁：一团散开的黑，把鱼藏起来。短、可预期，只盖战场不盖 HUD
  if (g.inkT > 0.f) {
    float sx = g.squidX * g.zoom, sy = g.squidY * g.zoom;
    float k  = 1.f - g.inkT / 1.2f;
    int   R  = (int)(18.f + k * 54.f);
    for (int yy = -R; yy <= R; yy++) {
      for (int xx = -R; xx <= R; xx++) {
        int X = (int)sx + xx, Y = (int)sy + yy;
        if (X < 1 || X >= SCREEN_W-1 || Y < 17 || Y >= SCREEN_H-11) continue;
        float d   = sqrtf((float)(xx*xx + yy*yy));
        // 轮廓按角度抖：三层不同频率叠加，才不会读成一个圆
        float ang = atan2f((float)yy, (float)xx);
        float wob = 0.74f
                  + 0.20f * sinf(ang*3.0f + g.squidX*0.03f)
                  + 0.12f * sinf(ang*5.7f - g.squidY*0.05f)
                  + 0.08f * sinf(ang*9.1f + 1.7f);
        float edge = (float)R * wob;
        if (d > edge) continue;
        float t = d / (edge > 1.f ? edge : 1.f);
        if (t < 0.52f)                          px(X, Y, PAL_UI[0]);           // 墨核：实心
        else if (t < 0.78f) { if (((X*3 + Y*5) & 3) != 0) px(X, Y, PAL_UI[0]); } // 渐变：网纹
        else if (((X*7 + Y*11) & 7) < 2)        px(X, Y, PAL_UI[0]);           // 边缘：稀疏散点
        // 暗青反光镶边：黑墨甩在黑水里本来什么都看不见，这一圈是它的"形体"
        if (t > 0.86f && (((X*5 + Y*7) & 3) == 0)) px(X, Y, PAL_BG2[1]);
      }
    }
    // 飞溅：几滴甩出去的墨，位置固定（随机的话每帧都会跳）
    static const int8_t SP[6][2] = {{58,-14},{-52,20},{34,52},{-30,-46},{70,26},{-64,-8}};
    for (int k = 0; k < 6; k++) {
      int X = (int)sx + SP[k][0], Y = (int)sy + SP[k][1];
      if (X < 1 || X >= SCREEN_W-1 || Y < 17 || Y >= SCREEN_H-11) continue;
      int rr = (k & 1) ? 1 : 2;
      disc(X, Y, rr, PAL_UI[0]);
    }
  }

  rect(0, 16, SCREEN_W, 1, PAL_UI[2]);
  rect(0, SCREEN_H-10, SCREEN_W, 1, PAL_UI[1]);
  rect(0, 16, 1, SCREEN_H-16, PAL_UI[1]);
  rect(SCREEN_W-1, 16, 1, SCREEN_H-16, PAL_UI[1]);

  hud(g);
  text("RADAR", 6, 20, PAL_BG2[2], 1, 1);
  if (g.tideOn) {
    // 潮汐提示：前 0.9 秒闪烁，之后常亮，倒计时最后 2 秒变红
    bool blink = (g.tideFlash > 0) ? ((g.tideFlash / 4) % 2 == 0) : true;
    if (blink) {
      uint16_t c = (g.tideT < 2.f) ? PAL_FX[3] : PAL_FX[1];
      char tb[16];
      snprintf(tb, sizeof(tb), "SCHOOL %d", (int)(g.tideT + 1));
      text(tb, 176, 20, c, 1, 1);
    }
  }
  text(g.imuAim ? "TILT AIM" : ",  /  AIM", 60, 20, g.imuAim ? PAL_FX[2] : PAL_BG1[3], 1, 1);
  bottomHints(g, "4 BAIT 25  SPACE FIRE  i");
}

// ---------------- 近景水 ----------------
static void waterPov(const Game& g) {
  float inv = 1.f/g.zoom;
  for (int sy = 0; sy < SCREEN_H; sy += 2) {
    float wy = g.camY + sy*inv;
    float depth = wy/(float)WORLD_H;
    if (depth < 0.f) depth = 0.f; if (depth > 1.f) depth = 1.f;
    float base = 2.7f - 1.8f*depth;
    for (int sx = 0; sx < SCREEN_W; sx += 2) {
      float wx = g.camX + sx*inv;
      float tt = gTime;
      float v = base + (vnoise(wx*0.011f + tt*0.25f, wy*0.011f - tt*0.15f, 7u) - 0.5f)*0.55f
                      + (vnoise(wx*0.034f + tt*0.4f,  wy*0.034f + tt*0.2f,  13u) - 0.5f)*0.25f;
      if (vnoise(wx*0.070f + tt*0.6f, wy*0.070f - tt*0.35f, 29u) > 0.80f) v += 0.8f;
      int i = (int)(v + 0.5f); if (i < 0) i = 0; if (i > 9) i = 9;
      uint16_t c = RAMP(i);
      px(sx,sy,c); px(sx+1,sy,c); px(sx,sy+1,c); px(sx+1,sy+1,c);
    }
  }
  // 浮游
  int gx0 = (int)(g.camX/21.f) - 1, gy0 = (int)(g.camY/21.f) - 1;
  int gnx = (int)(SCREEN_W*inv/21.f) + 3, gny = (int)(SCREEN_H*inv/21.f) + 3;
  for (int gy = gy0; gy < gy0+gny; gy++)
    for (int gx = gx0; gx < gx0+gnx; gx++) {
      uint32_t h = ihash(gx, gy, 41u);
      if ((h & 7) != 0) continue;
      float wx = gx*21.f + (float)((h>>3) & 15) + 1.f;
      float wy = gy*21.f + (float)((h>>7) & 15) + 1.f;
      int sx = (int)((wx - g.camX)*g.zoom), sy = (int)((wy - g.camY)*g.zoom);
      if ((unsigned)sx < (unsigned)SCREEN_W && (unsigned)sy < (unsigned)SCREEN_H && sy > 16)
        px(sx, sy, (h & 32) ? PAL_BG3[2] : PAL_BG3[1]);
    }
}

// ---------------- 飞行视图 ----------------
static void drawFly(const Game& g) {
  waterPov(g);
  for (int i = 0; i < MAX_FISH; i++) {
    const Fish& f = g.fish[i];
    if (!f.alive) continue;
    int w = fishWorldW(f.kind), h = fishWorldH(f.kind);
    int sx = (int)((f.x - w*0.5f - g.camX)*g.zoom);
    int sy = (int)((f.y - h*0.5f - g.camY)*g.zoom);
    int sw = (int)(w*g.zoom), sh = (int)(h*g.zoom);
    if (sx > SCREEN_W+20 || sx+sw < -20 || sy > SCREEN_H+20 || sy+sh < -20) continue;
    if (g.zoom < 0.5f) {
      const uint16_t* pal = palOf(f.kind);
      disc(sx+sw/2, sy+sh/2, 2, pal[2]);
    } else {
      drawFish(sx, sy, (int)(g.zoom + 0.5f), f.kind, f.dir < 0, palOf(f.kind)[0], true);
    }
  }
  if (g.bullet.active) {
    int sx = (int)((g.bullet.x - g.camX)*g.zoom);
    int sy = (int)((g.bullet.y - g.camY)*g.zoom);
    float a = g.bullet.ang*D2R;
    for (int i = 1; i <= 12; i++) {
      float t = i*0.62f;
      int bx = (int)(sx - cosf(a)*t), by = (int)(sy + sinf(a)*t);
      uint16_t c = (i < 4) ? PAL_FX[3] : ((i < 8) ? PAL_BG2[3] : PAL_BG2[2]);
      disc(bx, by, (i < 4) ? 2 : 1, c);
    }
    for (int k = 0; k < 6; k++) {
      float b = (k*60.f + 15.f)*D2R;
      px((int)(sx+cosf(b)*4), (int)(sy+sinf(b)*4), PAL_FX[1]);
    }
    disc(sx, sy, 3, PAL_FX[3]);
    disc(sx, sy, 2, PAL_FX[2]);
    px(sx-1, sy-1, PAL_FX[0]);
  }
  hud(g);
  text("FLY", 6, 20, PAL_BG1[3], 1, 1);
}

// ---------------- 命中特写 ----------------
static void drawCatch(const Game& g) {
  waterPov(g);
  const float ccx = (g.catchX - g.camX)*g.zoom;
  const float ccy = (g.catchY - g.camY)*g.zoom;

  // 其它鱼（只在视野边缘，且离捕获点足够远，避免糊住主体）
  for (int i = 0; i < MAX_FISH; i++) {
    const Fish& f = g.fish[i];
    if (!f.alive) continue;
    float ddx = f.x - g.catchX, ddy = f.y - g.catchY;
    if (ddx*ddx + ddy*ddy < 55.f*55.f) continue;
    int w = fishWorldW(f.kind), h = fishWorldH(f.kind);
    int sx = (int)((f.x - w*0.5f - g.camX)*g.zoom);
    int sy = (int)((f.y - h*0.5f - g.camY)*g.zoom);
    if (sx > SCREEN_W+40 || sx+w*3 < -40 || sy > SCREEN_H+40 || sy+h*3 < -40) continue;
    drawFish(sx, sy, 3, f.kind, f.dir < 0, palOf(f.kind)[0], true);
  }

  int w = fishWorldW(g.catchKind), h = fishWorldH(g.catchKind);
  int sx = (int)((g.catchX - w*0.5f - g.camX)*g.zoom);
  int sy = (int)((g.catchY - h*0.5f - g.camY)*g.zoom);
  // 震动：大鱼抖得更狠（鲨鱼/魔鬼鱼是 ±2px，小鱼 ±1px）
  int shx = 0, shy = 0;
  if (g.catchShake > 0.f) {
    int amp = (fishWorldW(g.catchKind) >= 60) ? 2 : 1;
    int s = (int)(g.catchShake * 60.f);
    shx = (s % (amp*2+1)) - amp;
    shy = ((s / (amp*2+1)) % (amp*2+1)) - amp;
  }
  drawFish(sx+shx, sy+shy, 3, g.catchKind, g.catchFlip != 0, PAL_FX[1], true);
  drawFish(sx+shx, sy+shy, 3, g.catchKind, g.catchFlip != 0, PAL_FX[0], true);

  // 冲击环（贴着鱼身，随定格时间扩散）
  float hf2 = g.phaseT / 0.30f; if (hf2 > 1.f) hf2 = 1.f;
  for (int R = 0; R < 2; R++) {
    float rr = (R == 0 ? 44.f : 52.f) + hf2 * 16.f;
    uint16_t col = (R == 0) ? PAL_BG3[3] : PAL_BG3[1];
    for (int deg = 0; deg < 360; deg += 3) {
      float a = deg*D2R;
      px((int)(ccx+cosf(a)*rr*1.30f), (int)(ccy+sinf(a)*rr*0.80f), col);
    }
  }
  // 网：0.26s 内从外圈收拢到鱼身（真正的"收网"）
  float close = g.phaseT / CATCH_HOLD_SEC; if (close > 1.f) close = 1.f;
  float netBase = 96.f - 18.f * close;
  for (int R = 0; R < 2; R++) {
    float rr = netBase - (R == 0 ? 0.f : 6.f);
    uint16_t col = (R == 0) ? PAL_FX[1] : PAL_FX[0];
    for (int deg = 0; deg < 360; deg += 2) {
      float a = deg*D2R;
      px((int)(ccx+cosf(a)*rr*1.30f), (int)(ccy+sinf(a)*rr*0.80f), (deg % 6) ? col : PAL_BG3[3]);
    }
  }
  for (int k = 0; k < 8; k++) {
    float b = (k*45.f)*D2R;
    for (float t = 0.58f; t < 1.0f; t += 0.010f)
      px((int)(ccx+cosf(b)*netBase*1.30f*t), (int)(ccy+sinf(b)*netBase*0.80f*t),
         (t > 0.97f) ? PAL_FX[1] : PAL_BG2[3]);
  }
  for (int y = 16; y < SCREEN_H; y++) for (int x = 0; x < SCREEN_W; x++) {
    if (!(((x+y) % 13 == 0) || ((x-y) % 13 == 0))) continue;
    float ddx = (x-ccx)/1.30f, ddy = y-ccy;
    float d = sqrtf(ddx*ddx + ddy*ddy);
    if (d > netBase-16.f && d < netBase-2.f) px(x, y, PAL_BG2[2]);
  }
  // 碎块
  static const int8_t SH[10][2] = {{-92,-34},{-88,30},{78,-46},{88,26},{56,-52},{-58,46},{112,2},{-114,6},{24,54},{-28,-58}};
  for (int k = 0; k < 10; k++) {
    int px0 = (int)ccx + SH[k][0] + (int)(close * 6.f * (SH[k][0] > 0 ? 1 : -1));
    int py0 = (int)ccy + SH[k][1];
    rect(px0, py0, 3, 3, PAL_GOLD[2]);
    px(px0, py0, PAL_GOLD[3]);
  }
  // 金币：爆开阶段带长拖尾，吸入阶段变亮（飞向计分器）
  for (int i = 0; i < MAX_COINS; i++) {
    const CoinFx& c = g.coins[i];
    if (c.life <= 0.f) continue;
    int tail = c.suck ? 9 : 5;
    for (int t = 1; t <= tail; t++) {
      float k = 0.05f * t;
      uint16_t col = c.suck ? (t & 1 ? PAL_FX[2] : PAL_FX[3]) : (t & 1 ? PAL_FX[3] : PAL_FX[2]);
      px((int)(c.x + (ccx-c.x)*k), (int)(c.y + (ccy-c.y)*k), col);
    }
    coinIcon((int)c.x, (int)c.y, c.big ? 6 : 3, i);
  }
  // 结算：+N 大字居中上浮（一眼就知道"钱进来了"）
  float rise = g.phaseT / CATCH_TOTAL_SEC; if (rise > 1.f) rise = 1.f;
  int fy = 100 - (int)(rise * 12.f);
  char b[20];
  if (g.catchKind == FK_DIVER) {
    const char* oo = "OOPS! DIVER";
    text(oo, (SCREEN_W - (int)strlen(oo)*12)/2, fy, PAL_FX[3], 2, 1);
    // 他不挣扎，反而给你竖了个大拇指 —— 这海里唯一会谢你的东西
    {
      // 手臂从潜水员身上伸出来，再是一个明确的大拇指 —— 悬空的小图标会读成一串乱码
      int hx = SCREEN_W/2 + 30, hy = 72;
      uint16_t suit = palOf(FK_DIVER)[2];
      uint16_t skin = PAL_GOLD[2], line = PAL_GOLD[3];
      rect(hx-16, hy+7, 17, 4, suit);        // 潜水服手臂
      rect(hx-4,  hy+6, 5, 6, skin);         // 手腕
      rect(hx+1,  hy,    4, 7, skin);        // 拇指：竖起来
      rect(hx,    hy+5,  9, 8, skin);        // 拳头
      px(hx+2, hy+1, line);                  // 指甲
      px(hx+2, hy+8, line); px(hx+4, hy+8, line); px(hx+6, hy+8, line);   // 指节
      rect(hx, hy+12, 9, 1, line);           // 拳底
    }
  } else {
    snprintf(b, sizeof(b), "+%d", g.lastGain);
    int tw = (int)strlen(b) * 18;            // 3 倍字宽 18px
    text(b, (SCREEN_W - tw) / 2, fy, PAL_FX[3], 3, 1);
  }
  if (g.combo > 1) {
    snprintf(b, sizeof(b), "x%d", g.combo);
    // 连击数字跳动：刚抓到的一瞬间弹大，随后回落
    int cs = (g.phaseT < 0.20f) ? 3 : 2;
    int cw = (int)strlen(b) * 6 * cs;
    text(b, (SCREEN_W - cw)/2, fy - (cs == 3 ? 22 : 16), PAL_FX[0], cs, 1);
  }

  hud(g, true);
  text(g.catchKind == FK_DIVER ? "??? CAUGHT" : "CAUGHT", 6, 20, PAL_FX[2], 1, 1);
}

// ---------------- 挣脱特写：网罩住 → 崩断 → 冲出画面 ----------------
// 关键：这段必须"动"。静态鱼 + 文字永远读不出"它正在挣脱"。
static void drawMiss(const Game& g) {
  waterPov(g);
  float t = g.phaseT / MISS_TOTAL_SEC; if (t > 1.f) t = 1.f;
  int w = fishWorldW(g.catchKind), h = fishWorldH(g.catchKind);
  int dir = g.catchFlip ? -1 : 1;          // 朝哪边就从哪边逃

  const float baseX = (g.catchX - g.camX)*g.zoom;
  const float baseY = (g.catchY - g.camY)*g.zoom;

  float fx, fy;
  bool struggling = (t < 0.45f);
  if (struggling) {
    // 8Hz 大幅甩动：低帧率下这个频率才读得出"挣扎"
    fx = baseX + fsin(g.phaseT * 50.f) * 9.f;
    fy = baseY + fsin(g.phaseT * 71.f + 1.1f) * 5.f;
  } else {
    float k = (t - 0.45f) / 0.55f;
    fx = baseX + dir * k * k * 175.f;
    fy = baseY - k * k * 56.f;
  }

  int sx = (int)(fx - w*g.zoom*0.5f);
  int sy = (int)(fy - h*g.zoom*0.5f);

  // 脱网瞬间的速度线（只在冲出去之后画）
  if (!struggling) {
    for (int k = 1; k <= 3; k++) {
      int lx = (int)(fx - dir*(10 + k*9));
      hline(lx, (int)fy - 4 + k*4, 6, (k == 1) ? PAL_UI[3] : PAL_UI[1]);
    }
  }

  drawFish(sx, sy, 3, g.catchKind, g.catchFlip != 0, PAL_FX[3], true);

  // 网：罩在鱼身上的网格，跟着鱼一起抖
  if (struggling) {
    uint16_t netC = (fsin(g.phaseT * 30.f) > 0.f) ? PAL_UI[3] : PAL_UI[2];
    for (int yy = -h/2; yy <= h/2; yy++) {
      for (int xx = -w/2; xx <= w/2; xx += 2) {
        if (((xx + 128) % 6) == 0 || ((yy + 128) % 5) == 0)
          px((int)(fx + xx), (int)(fy + yy), netC);
      }
    }
  }

  // 崩断：碎片从鱼身上炸开、边飞边沉
  if (t > 0.38f && t < 0.82f) {
    float k = (t - 0.38f) / 0.44f;
    for (int i = 0; i < 16; i++) {
      float a = (float)i * 0.393f;
      float d = 6.f + k * 48.f;
      uint16_t c = (k < 0.5f) ? PAL_UI[3] : PAL_UI[1];
      px((int)(baseX + cosf(a)*d), (int)(baseY + sinf(a)*d*0.8f + k*k*16.f), c);
      px((int)(baseX + cosf(a)*d) + 1, (int)(baseY + sinf(a)*d*0.8f + k*k*16.f), c);
    }
  }

  if (t > 0.30f) text("GOT AWAY", 72, 100, PAL_FX[3], 2, 1);
  hud(g);
  text("NO COIN", 6, 20, PAL_FX[3], 1, 1);
}

// ---------------- 结束画面 ----------------
static void drawOver(const Game& g) {
  drawRadar(g);
  for (int y = 16; y < SCREEN_H; y += 2)
    for (int x = 0; x < SCREEN_W; x += 2) {
      uint16_t c = RAMP(1); px(x,y,c); px(x+1,y,c); px(x,y+1,c); px(x+1,y+1,c);
    }
  text("GAME OVER", 62, 46, PAL_FX[2], 2, 1);
  char b[24];
  snprintf(b, sizeof(b), "SCORE %d", g.score);
  text(b, 74, 68, PAL_UI[3], 1, 1);
  snprintf(b, sizeof(b), "BEST %d", g.best);
  text(b, 78, 80, PAL_FX[1], 1, 1);
  text("SPACE RESTART", 54, 104, PAL_BG3[2], 1, 1);
}

// ---------------- 入口 ----------------
void renderInit() {
  for (int i = 0; i <= 256; i++) gSinTab[i] = sinf(i * 6.2831853f / 256.f);
  for (int i = 0; i < 128; i++) gGlyph[i] = nullptr;
  for (int i = 0; i < FONT5x7_COUNT; i++) {
    unsigned char c = (unsigned char)FONT5x7[i].c;
    if (c < 128) gGlyph[c] = &FONT5x7[i];
  }
  for (int k = 0; k < FK_COUNT; k++) buildFish(k);
}

void renderFrame(uint16_t* fb, const Game& g, float timeSec) {
  FB = fb;
  gTime = timeSec;
  if (g.helpOpen) { drawHelp(); return; }    // 帮助页：整屏独占
  if (g.listOpen) { drawList(g); return; }   // 鱼获图鉴：整屏独占
  switch (g.phase) {
    case PH_RADAR:  drawRadar(g); break;
    case PH_FLY:    drawFly(g);   break;
    case PH_CATCH:  drawCatch(g); break;
    case PH_MISS:   drawMiss(g);  break;
    case PH_RESULT:
      if (g.missFlag == 2) {          // 落空：炮弹自己飞走，不弹任何结算文字
        drawFly(g);
      } else {
        drawCatch(g);
      }
      break;
    default:        drawOver(g);  break;
  }
  // 打不动时提示换炮：新玩家不会去按 h 看帮助，只会以为"这鱼打不死"
  if (g.hintT > 0.f && fmodf(g.hintT, 0.5f) > 0.18f) {
    char buf[16];
    snprintf(buf, sizeof(buf), "PRESS %d", g.hintLv);
    int tw = (int)strlen(buf) * 6;
    text(buf, (SCREEN_W - tw)/2, 92, PAL_FX[3], 1, 1);
  }
  // 开局一分钟：把换炮的键位摆出来
  if (g.playSec < 60.f && !g.helpOpen && !g.listOpen && g.phase == PH_RADAR) {
    text("1 2 3 POWER", 78, 122, PAL_BG3[2], 1, 1);
  }

  if (g.flash) {
    // 换档闪白：整屏提亮一档（只影响非黑像素）
    for (int i = 0; i < SCREEN_W*SCREEN_H; i++) {
      uint16_t c = fb[i];
      uint16_t r = (c >> 11) & 31, gg = (c >> 5) & 63, b = c & 31;
      if (r || gg || b) {
        r = (r < 30) ? r+2 : 31;
        gg = (gg < 62) ? gg+4 : 63;
        b = (b < 29) ? b+2 : 31;
        fb[i] = (uint16_t)((r<<11) | (gg<<5) | b);
      }
    }
  }
}
