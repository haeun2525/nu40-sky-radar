# NU40 머리 위 비행기 레이더

맥이 adsb.lol(예비: adsb.fi)에서 주변 비행기를 받아 USB 시리얼로 NU40 DK에 보내고,
보드는 GC9A01 원형 240×240 화면에 그린다. 키 필요 없음.

- 위치: `host/.env` 에 `LAT=` `LON=` 로 고정 (없으면 서울 강남구 일대), `--lat --lon` 으로도 변경
- 반경 60km, 5초마다 조회, 사이엔 속도·방향으로 위치 추정해 부드럽게 이동
- 노선(ICN-NRT 등)은 adsbdb 편명 조회

## 화면
| 번호 | 화면 | 내용 |
|---|---|---|
| 0 | 레이더 | 링 20·40·60km, 비행기 아이콘(기수=진행 방향), 최근접은 마젠타+편명·고도 |
| 1 | 상세 | 최근접 편명·기종·노선·고도·거리, 바깥 링 아이콘 = 방향 |
| 2 | 시계 | 큰 시계·초 링·좌표, 데이터 없거나 끊기면 자동 |
| 9 | 자동 | 레이더 15초 / 상세 5초 (기본) |

시안: https://claude.ai/artifact/VRLXGJE7YuajMo94V9Wr5x

## 배선 (GC9A01)
VCC→3V3, GND→GND, SCL→47(P1.15), SDA→45(P1.13), CS→44(P1.12), DC→43(P1.11), RES→42(P1.10), BLK→41(P1.09)

## 실행
```sh
python3 host/radar.py               # 자동
python3 host/radar.py --screen 0    # 레이더 고정 (촬영용)
pkill -f radar.py                   # 끄기
```
pyserial 필요(`pip3 install pyserial`).

## 굽기
```sh
./build.sh upload    # 리셋(S5) 더블탭
```
★ 이 보드의 업로드 드라이브 이름은 `NRF52BOOT`가 아니라 **`BARAM-NU40`**. build.sh는 둘 다 감지.
cp가 I/O error를 내도 보드가 재부팅하며 정상으로 구워진다.

## 상태 (2026-11-06)
- 펌웨어 구움, 맥에서 실시간 데이터 전송 확인(17~23대)
- 남은 것: 실물 화면 색·글자 잘림 확인, 서보 포인터, 폰 GPS 이동 위치(보류)
