#!/usr/bin/env python3
"""
radar.py — adsb.lol 에서 내 위치 주변 비행기를 받아 NU40(sky_radar)로 USB 시리얼 전송

    python3 host/radar.py                   # 자동 화면 (15초 레이더 / 5초 상세)
    python3 host/radar.py --screen 0        # 0 레이더, 1 상세, 2 시계 고정
    python3 host/radar.py --lat 37.55 --lon 126.99 --range 60

위치는 고정값(아래 기본값 또는 --lat/--lon). 키 필요 없음.
"""
import argparse, glob, json, math, sys, threading, time, urllib.request

try:
    import serial
except ImportError:
    sys.exit("pyserial 이 필요합니다: pip3 install pyserial")

UA = {"User-Agent": "nu40-sky-radar"}
# 내 위치: host/.env 의 LAT=, LON= (깃에 안 올라감). 없으면 서울 강남구 일대.
import os
_env = {}
_envp = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env")
if os.path.exists(_envp):
    for _l in open(_envp):
        if "=" in _l and not _l.startswith("#"):
            k, v = _l.strip().split("=", 1); _env[k] = v
ap = argparse.ArgumentParser()
ap.add_argument("--lat", type=float, default=float(_env.get("LAT", 37.50)))
ap.add_argument("--lon", type=float, default=float(_env.get("LON", 127.06)))
ap.add_argument("--range", type=float, default=60, help="레이더 반경 km")
ap.add_argument("--screen", type=int, default=9, help="0 레이더 1 상세 2 시계 9 자동")
ap.add_argument("--every", type=float, default=5, help="조회 간격 초")
ap.add_argument("--port", default=None)
a = ap.parse_args()

routes = {}  # 편명 → "ICN-NRT" (adsbdb, 백그라운드 조회)
def lookup_route(cs):
    routes[cs] = ""
    try:
        with urllib.request.urlopen(urllib.request.Request(f"https://api.adsbdb.com/v0/callsign/{cs}", headers=UA), timeout=8) as r:
            fr = json.load(r)["response"]["flightroute"]
            routes[cs] = f'{fr["origin"]["iata_code"]}-{fr["destination"]["iata_code"]}'
    except Exception:
        pass

def fetch():
    nm = math.ceil(a.range / 1.852) + 5
    for url in (f"https://api.adsb.lol/v2/point/{a.lat}/{a.lon}/{nm}",
                f"https://opendata.adsb.fi/api/v2/lat/{a.lat}/lon/{a.lon}/dist/{nm}"):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=10) as r:
                d = json.load(r)
            return d.get("ac") or d.get("aircraft") or []
        except Exception as e:
            print("조회 실패:", url.split('/')[2], e)
    return None

def planes_lines(ac):
    out = []
    k = math.cos(math.radians(a.lat)) * 111.32
    for p in ac:
        if p.get("lat") is None or p.get("alt_baro") in (None, "ground"):
            continue
        x = (p["lon"] - a.lon) * k; y = (p["lat"] - a.lat) * 110.57
        if math.hypot(x, y) > a.range * 1.1:
            continue
        cs = (p.get("flight") or p.get("r") or p.get("hex", "")).strip()[:8] or "?"
        if cs not in routes:
            threading.Thread(target=lookup_route, args=(cs,), daemon=True).start()
        typ = (p.get("t") or "")[:5]
        out.append(f'A {cs},{typ},{int(p["alt_baro"])},{x:.2f},{y:.2f},{int(p.get("track") or 0)},{int(p.get("gs") or 0)},{routes.get(cs, "")}')
    return out[:40]

def find_port():
    if a.port: return a.port
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports: sys.exit("보드를 못 찾았습니다 (/dev/cu.usbmodem*). USB 연결을 확인하세요.")
    return ports[0]

def main():
    port = find_port(); print("보드:", port, "| 위치:", a.lat, a.lon, "| 반경", a.range, "km")
    s = None; next_fetch = 0; last_lines = []
    while True:
        try:
            if s is None:
                s = serial.Serial(port, 115200, timeout=1); time.sleep(1.5)
                tz = -time.timezone if not time.localtime().tm_isdst else -time.altzone
                send = lambda t: s.write((t + "\n").encode())
                send(f"T {int(time.time()) + tz}"); send(f"L {a.lat} {a.lon}"); send(f"R {a.range}"); send(f"S {a.screen}")
            if time.time() >= next_fetch:
                next_fetch = time.time() + a.every
                ac = fetch()
                if ac is not None:
                    last_lines = planes_lines(ac)
                    s.write(("B\n" + "".join(l + "\n" for l in last_lines) + "E\n").encode())
                    tz = -time.timezone if not time.localtime().tm_isdst else -time.altzone
                    s.write(f"T {int(time.time()) + tz}\n".encode())
                    print(time.strftime("%H:%M:%S"), f"{len(last_lines)}대", " ".join(l[2:].split(",")[0] for l in last_lines[:6]))
            time.sleep(0.2)
        except (serial.SerialException, OSError) as e:
            print("시리얼 끊김, 다시 연결:", e); s = None; time.sleep(2)
            try: port = find_port()
            except SystemExit: pass

if __name__ == "__main__":
    try: main()
    except KeyboardInterrupt: print("\n종료")
