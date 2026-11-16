# LEO Network Simulator

## 사용

sim-ui.exe 또는 start_ui.bat을 실행합니다. 실행 구성 선택 → INI 편집 → 저장 →
새로고침 → 실행 순서입니다. 계산은 UI 폴더의 engine/engine.exe가 수행합니다.
leo-main, OMNeT++, INET, WSL, Docker는 실행 시 필요하지 않습니다.
배포 시 engine/, settings/, 화면 파일, replay-model.json을 EXE와 함께 둡니다.
Python은 start_ui.bat에서만 필요하며 EXE에는 포함돼 있습니다. WebView2는 시스템
런타임을 사용합니다. 로그인·계정·DB·외부 서비스는 없습니다.

HTML 직접 열기는 저장된 결과 재생만 지원합니다. 새 계산과 메모장 INI 편집은
EXE로 실행하세요. INI 편집은 settings/simulations/Dijkstra132/omnetpp.ini를 엽니다.
원본 INI 형식, include, extends, 기존 키를 유지하며 JSON 편집은 필요 없습니다.
별도 Config를 추가할 수 있습니다. 지원하지 않는 계산식/모델은 오류로 알립니다.

## 결과

실행마다 data/runs/<고유ID>/에 routes.csv, config/의 실제 INI 트리,
manifest.json, engine-input.json, summary.json, events.csv, run.log를 보관합니다.
manifest에는 엔진 ID leo-native와 버전이 기록됩니다. 원본 FLoRaSat 실행 결과라고
표시하지 않습니다. 이전 원본 결과 폴더도 계속 열 수 있습니다.

실행 결과 열기에서 manifest.json이 있는 폴더를 선택하면 설정에 맞춰 지도와 CSV를
재생합니다. CSV만 열기는 경로/홉/예측 지연 표를 표시합니다. 다른 실행의 CSV를
기존 좌표에 임의로 겹치지 않습니다. CSV에는 실제 위치·연결 설정이 없습니다.
파일 누락·변경이나 지원하지 않는 모델이면 이전 지도를 유지합니다.
보관된 INI는 새 설정을 편집해도 변경되지 않습니다.

지도는 설정 기반 재현입니다. 실제 송수신/실측 지연 카드는 표시하지 않습니다.
원본 결과와 native 결과는 엔진/난수 처리 차이가 있을 수 있습니다. 원본 알고리즘
전체와 동일하거나 연구 타당성이 검증됐다고 주장하지 않습니다.

## 개발

알고리즘 확장과 지원 범위는 engine/README.md, 궤도 재생 검증 출처는
REPLAY_NOTES.md에 있습니다. 원본 모델 코드는 변경하지 않습니다.

```
python -m unittest discover -s . -p "test_*.py" -v
node test_replay.js
node test_browser.js
python engine/build.py
```

개발용 C++ 빌드는 WSL의 MinGW를 사용합니다. 완성된 Windows 엔진은 WSL 없이 동작합니다.
단독 컴파일된 엔진은 고정 그래프의 지연/홉 선택·동률·경로 없음 검사를 통과합니다.
Python 검사는 반복 실행·패킷 보존·고부하 큐·최대 홉 폐기·INI 입력 검증을 포함합니다.
원본과 전체 이벤트·유실·난수열의 완전 동등성 검증은 완료되지 않았습니다.

## 배포와 소스

이 프로그램은 GPL-3.0으로 소스와 함께 제공합니다. 제삼자 파일은 각 원래 조건을
유지합니다. LICENSE, THIRD_PARTY_NOTICES.txt, licenses/를 삭제하지 마세요.
사용·수정·재배포 조건은 해당 원문에 있습니다. 원본 프로젝트 전체를 독점 소유한다고
주장하지 않으며 납품처에도 적용 라이선스에 따른 권리가 유지됩니다.

배포 파일과 동일한 코드를 다시 빌드하려면 Windows Python 3.12에서
`python -m pip install -r requirements-build.txt` 후 `python build_release.py`를
실행합니다. C++ 개발 빌드에만 WSL Ubuntu의 g++-mingw-w64-x86-64가 필요합니다.
빌드 결과는 dist/의 고유한 폴더와 ZIP이며 기존 사용자 결과는 포함하지 않습니다.
배포 폴더에는 전체 소스·INI·라이선스·테스트와 새로 계산한 예제 결과가 포함됩니다.
