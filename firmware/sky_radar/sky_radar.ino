/*
 * sky_radar — 머리 위 비행기 레이더 (NU40 DK + GC9A01 원형 240x240)
 *
 * 맥(host/radar.py)이 adsb.lol 에서 주변 비행기를 받아 USB 시리얼로 보내면
 * 보드는 그림만 그린다. 위치는 맥 쪽 설정에 고정.
 *
 * 배선 (color-picker 와 동일)
 *   VCC→3V3, GND→GND, SCL→47(P1.15), SDA→45(P1.13),
 *   CS→44(P1.12), DC→43(P1.11), RES→42(P1.10), BLK→41(P1.09)
 *
 * 시리얼 한 줄 명령 (115200)
 *   T <로컬 epoch 초>                  시간 맞추기
 *   L <위도> <경도>                     내 위치 (표시용 문자열)
 *   R <km>                              레이더 반경
 *   B                                   비행기 목록 시작
 *   A cs,type,alt,xkm,ykm,trk,gs,route  비행기 한 대 (x=동쪽+, y=북쪽+, gs=노트)
 *   E                                   목록 끝 → 화면 반영
 *   S <0|1|2|9>                         화면 고정: 0 레이더 1 상세 2 시계, 9 자동
 */
#include <Arduino_GFX_Library.h>
#include "FreeSansBold9pt7b.h"
#include "FreeSansBold12pt7b.h"
#include "FreeSansBold24pt7b.h"

#define LCD_CS  44
#define LCD_DC  43
#define LCD_RST 42
#define LCD_BLK 41

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS);
Arduino_GFX *panel = new Arduino_GC9A01(bus, LCD_RST, 0, true);
Arduino_Canvas *g = new Arduino_Canvas(240, 240, panel);

// 색 (시안과 동일)
#define C_BG    0x0000
#define C_RING  0x2A4B
#define C_FG    0xE77E
#define C_DIM   0x7C94
#define C_CYAN  0x3E9F
#define C_MAG   0xFA7B
#define C_GREEN 0x5FF1
#define C_AMBER 0xFD88

const int CX = 120, CY = 120, R = 92;

struct Plane { char cs[9]; char type[6]; long alt; float x, y; int trk; int gs; char route[16]; };
#define MAXP 40
Plane planes[MAXP], incoming[MAXP];
int nPlanes = 0, nIncoming = 0;
uint32_t listAt = 0;          // 목록 받은 millis
float rangeKm = 60;
char locStr[32] = "37.5500N 126.9900E";
uint32_t epochBase = 0, epochAtMs = 0;
int screenMode = 9;           // 9 = 자동
char lastStr[8] = "--:--";

// ── 시간 ──────────────────────────────────────────
struct DT { int y, mo, d, wd, h, mi, s; };
DT nowDT() {
  uint32_t t = epochBase + (millis() - epochAtMs) / 1000;
  DT r; long days = t / 86400; long sec = t % 86400;
  r.h = sec / 3600; r.mi = (sec / 60) % 60; r.s = sec % 60;
  r.wd = (days + 4) % 7;  // 1970-01-01 = 목
  long z = days + 719468, era = z / 146097, doe = z - era * 146097;
  long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  r.d = doy - (153 * mp + 2) / 5 + 1; r.mo = mp < 10 ? mp + 3 : mp - 9; r.y = yoe + era * 400 + (r.mo <= 2);
  return r;
}

// ── 그리기 도우미 ─────────────────────────────────
void textC(const char *s, int x, int y, const GFXfont *f, uint16_t col, int size = 1) {
  // y = 글자 세로 가운데
  g->setFont(f); g->setTextSize(size); g->setTextColor(col);
  int16_t x1, y1; uint16_t w, h;
  g->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  g->setCursor(x - w / 2 - x1, y - h / 2 - y1);
  g->print(s);
}
void textL(const char *s, int x, int y, uint16_t col, bool right) {
  g->setFont(NULL); g->setTextSize(1); g->setTextColor(col);
  int w = strlen(s) * 6;
  g->setCursor(right ? x - w : x, y - 4);
  g->print(s);
}

// 위에서 본 비행기: 동체·주날개·꼬리 (단위 좌표, 대칭)
const float SHAPE[][6] = {
  {0,-1.45, -.18,-1.1, .18,-1.1},
  {-.18,-1.1, .18,-1.1, .14,1.2}, {-.18,-1.1, .14,1.2, -.14,1.2},
  {.18,-.4, 1.35,.2, 1.35,.42}, {.18,-.4, 1.35,.42, .18,.18},
  {-.18,-.4, -1.35,.2, -1.35,.42}, {-.18,-.4, -1.35,.42, -.18,.18},
  {.14,.85, .58,1.15, .58,1.32}, {.14,.85, .58,1.32, .12,1.2},
  {-.14,.85, -.58,1.15, -.58,1.32}, {-.14,.85, -.58,1.32, -.12,1.2},
};
void drawPlane(float px, float py, float deg, uint16_t col, float s) {
  float a = deg * PI / 180, c = cos(a), sn = sin(a);
  for (auto &t : SHAPE) {
    int p[6];
    for (int i = 0; i < 3; i++) {
      float x = t[i*2] * s, y = t[i*2+1] * s;
      p[i*2] = round(px + x * c - y * sn); p[i*2+1] = round(py + x * sn + y * c);
    }
    g->fillTriangle(p[0], p[1], p[2], p[3], p[4], p[5], col);
  }
}

// 현재 위치 추정 (받은 뒤 지난 시간만큼 진행 방향으로 이동)
void planePos(const Plane &p, float &x, float &y) {
  float dt = (millis() - listAt) / 1000.0f; if (dt > 30) dt = 30;
  float km = p.gs * 1.852f / 3600.0f * dt, a = p.trk * PI / 180;
  x = p.x + km * sin(a); y = p.y + km * cos(a);
}
int nearestIdx() {
  int best = -1; float bd = 1e9;
  for (int i = 0; i < nPlanes; i++) { float x, y; planePos(planes[i], x, y); float d = hypot(x, y); if (d < bd) { bd = d; best = i; } }
  return best;
}
int inRange() { int n = 0; for (int i = 0; i < nPlanes; i++) { float x, y; planePos(planes[i], x, y); if (hypot(x, y) <= rangeKm) n++; } return n; }

void clockTop(const DT &t) {
  char b[8]; snprintf(b, sizeof b, "%02d:%02d", t.h, t.mi);
  textC(b, CX, 38, &FreeSansBold12pt7b, C_FG);
}
void coordBottom(int y = 200) { textC(locStr, CX, y, NULL, C_DIM); }

// ── A. 레이더 ─────────────────────────────────────
void drawRadar(const DT &t, float sweep) {
  g->fillScreen(C_BG);
  // 스캔선 꼬리
  for (int i = 0; i < 70; i++) {
    float a = (sweep - i * 0.5f - 90) * PI / 180;
    float k = 0.38f * (1 - i / 70.0f);
    uint16_t col = ((uint16_t)(0x07 * k) << 11) | ((uint16_t)(0x34 * k) << 5) | (uint16_t)(0x1F * k);
    g->drawLine(CX, CY, CX + R * cos(a), CY + R * sin(a), col);
  }
  g->drawCircle(CX, CY, R / 3, C_RING); g->drawCircle(CX, CY, R * 2 / 3, C_RING); g->drawCircle(CX, CY, R, C_RING);
  for (int a = 0; a < 360; a += 10) {
    float r2 = R + (a % 30 == 0 ? 6 : 3), th = (a - 90) * PI / 180;
    g->drawLine(CX + R * cos(th), CY + R * sin(th), CX + r2 * cos(th), CY + r2 * sin(th), C_RING);
  }
  textC("N", CX, CY - R - 13, NULL, C_FG);
  char b[24];
  snprintf(b, sizeof b, "%d", (int)(rangeKm / 3)); textL(b, CX + R / 3 + 2, CY + 6, C_DIM, false);
  snprintf(b, sizeof b, "%d", (int)(rangeKm * 2 / 3)); textL(b, CX + R * 2 / 3 + 2, CY + 6, C_DIM, false);

  int ni = nearestIdx();
  for (int i = 0; i < nPlanes; i++) {
    float x, y; planePos(planes[i], x, y);
    if (hypot(x, y) > rangeKm || i == ni) continue;
    drawPlane(CX + x / rangeKm * R, CY - y / rangeKm * R, planes[i].trk, planes[i].alt < 3000 ? C_AMBER : C_CYAN, 5);
  }
  if (ni >= 0) {
    Plane &p = planes[ni]; float x, y; planePos(p, x, y);
    if (hypot(x, y) <= rangeKm) {
      float px = CX + x / rangeKm * R, py = CY - y / rangeKm * R;
      drawPlane(px, py, p.trk, C_MAG, 6.5);
      g->drawCircle(px, py, 11, C_MAG);
      bool right = px > CX; int lx = right ? px - 15 : px + 15;
      textL(p.cs, lx, py - 8, C_MAG, right);
      snprintf(b, sizeof b, "%03ld", p.alt / 100); textL(b, lx, py + 3, C_MAG, right);
    }
  }
  g->fillCircle(CX, CY, 2, C_GREEN);
  g->fillRect(CX - 36, 27, 72, 22, C_BG); g->fillRect(CX - 64, 192, 128, 28, C_BG);
  clockTop(t); coordBottom();
  snprintf(b, sizeof b, "%d AIRCRAFT - %dKM", inRange(), (int)rangeKm);
  textC(b, CX, 213, NULL, C_CYAN);
}

// ── B. 가장 가까운 비행기 ─────────────────────────
void drawDetail(const DT &t) {
  g->fillScreen(C_BG);
  int ni = nearestIdx(); if (ni < 0) return;
  Plane &p = planes[ni]; float x, y; planePos(p, x, y);
  float d = hypot(x, y), brg = fmod(atan2(x, y) * 180 / PI + 360, 360);
  g->drawCircle(CX, CY, 108, C_RING);
  textC("N", CX, 20, NULL, C_DIM);
  float th = (brg - 90) * PI / 180;
  drawPlane(CX + 108 * cos(th), CY + 108 * sin(th), p.trk, C_MAG, 6.5);
  clockTop(t);
  textC(p.cs, CX, 74, &FreeSansBold24pt7b, C_MAG);
  char b[40]; snprintf(b, sizeof b, "%s  %s", p.type, p.route);
  textC(b, CX, 100, &FreeSansBold9pt7b, C_FG);
  g->drawFastHLine(52, 116, 136, C_RING);
  textC("ALT", 84, 130, NULL, C_DIM); textC("DIST", 156, 130, NULL, C_DIM);
  snprintf(b, sizeof b, "%ld", p.alt); textC(b, 84, 148, &FreeSansBold12pt7b, C_FG);
  snprintf(b, sizeof b, "%.1f", d);   textC(b, 156, 148, &FreeSansBold12pt7b, C_FG);
  textC("ft", 84, 166, NULL, C_DIM);
  snprintf(b, sizeof b, "km %d", (int)brg); textC(b, 156, 166, NULL, C_DIM);
  coordBottom();
  snprintf(b, sizeof b, "NEAREST OF %d", inRange()); textC(b, CX, 213, NULL, C_DIM);
}

// ── C. 대기 시계 ──────────────────────────────────
void drawClock(const DT &t) {
  g->fillScreen(C_BG);
  for (int s = 0; s < 60; s++) {
    float th = (s * 6 - 90) * PI / 180; bool on = s <= t.s; float r2 = s % 5 == 0 ? 112 : 109;
    uint16_t col = on ? C_GREEN : C_RING;
    g->drawLine(CX + 104 * cos(th), CY + 104 * sin(th), CX + r2 * cos(th), CY + r2 * sin(th), col);
    if (s % 5 == 0) g->drawLine(CX + 104 * cos(th) + 1, CY + 104 * sin(th), CX + r2 * cos(th) + 1, CY + r2 * sin(th), col);
  }
  const char *W[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
  char b[32]; snprintf(b, sizeof b, "%s %02d.%02d", W[t.wd], t.mo, t.d);
  textC(b, CX, 72, &FreeSansBold9pt7b, C_DIM);
  snprintf(b, sizeof b, "%02d:%02d", t.h, t.mi); textC(b, CX, 112, &FreeSansBold24pt7b, C_FG);
  snprintf(b, sizeof b, "KST  %02ds", t.s); textC(b, CX, 146, NULL, C_DIM);
  coordBottom(182);
  snprintf(b, sizeof b, "%d AIRCRAFT - LAST %s", inRange(), lastStr);
  textC(b, CX, 198, NULL, C_AMBER);
}

// ── 시리얼 ────────────────────────────────────────
char line[160]; int lineLen = 0;
void handle(char *s) {
  char c = s[0]; char *arg = s + 1; while (*arg == ' ') arg++;
  if (c == 'T') { epochBase = strtoul(arg, NULL, 10); epochAtMs = millis(); }
  else if (c == 'L') { float la, lo; if (sscanf(arg, "%f %f", &la, &lo) == 2) snprintf(locStr, sizeof locStr, "%.4f%c %.4f%c", fabs(la), la >= 0 ? 'N' : 'S', fabs(lo), lo >= 0 ? 'E' : 'W'); }
  else if (c == 'R') { float r = atof(arg); if (r > 1) rangeKm = r; }
  else if (c == 'S') { screenMode = atoi(arg); }
  else if (c == 'B') { nIncoming = 0; }
  else if (c == 'A' && nIncoming < MAXP) {
    Plane &p = incoming[nIncoming]; char *f[8]; int n = 0; char *tok = strtok(arg, ",");
    while (tok && n < 8) { f[n++] = tok; tok = strtok(NULL, ","); }
    if (n < 7) return;
    strncpy(p.cs, f[0], 8); p.cs[8] = 0; strncpy(p.type, f[1], 5); p.type[5] = 0;
    p.alt = atol(f[2]); p.x = atof(f[3]); p.y = atof(f[4]); p.trk = atoi(f[5]); p.gs = atoi(f[6]);
    if (n > 7) { strncpy(p.route, f[7], 15); p.route[15] = 0; } else p.route[0] = 0;
    nIncoming++;
  }
  else if (c == 'E') {
    memcpy(planes, incoming, sizeof(Plane) * nIncoming); nPlanes = nIncoming; listAt = millis();
    DT t = nowDT(); snprintf(lastStr, sizeof lastStr, "%02d:%02d", t.h, t.mi);
  }
}
void pollSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') { if (lineLen) { line[lineLen] = 0; handle(line); lineLen = 0; } }
    else if (lineLen < (int)sizeof line - 1) line[lineLen++] = ch;
  }
}

void setup() {
  pinMode(LCD_BLK, OUTPUT); digitalWrite(LCD_BLK, HIGH);
  Serial.begin(115200);
  if (!g->begin(24000000)) { while (1) delay(1000); }
  epochBase = 1793919600; epochAtMs = millis();  // 맥 연결 전 임시값
}

void loop() {
  pollSerial();
  static float sweep = 0; static uint32_t lastMs = millis();
  uint32_t ms = millis(); sweep = fmod(sweep + (ms - lastMs) * 0.09f, 360); lastMs = ms;
  DT t = nowDT();
  bool stale = listAt == 0 || millis() - listAt > 90000;
  int mode = screenMode;
  if (mode == 9) {
    if (stale || inRange() == 0) mode = 2;
    else mode = ((millis() / 1000) % 20) >= 15 ? 1 : 0;   // 15초 레이더, 5초 상세
  }
  if (mode == 1 && nPlanes == 0) mode = 2;
  if (mode == 0) drawRadar(t, sweep); else if (mode == 1) drawDetail(t); else drawClock(t);
  g->flush();
}
