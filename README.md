# LEO Network Simulator

## 사용

sim-ui.exe 또는 start_ui.bat을 실행합니다. 실행 구성 선택 → 시나리오 설정 → INI에 저장 → 실행 순서입니다.
INI 파일 버튼으로 메모장 편집도 가능합니다. 계산은 UI 폴더의 engine/engine.exe가 수행합니다.
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
manifest.json, normalized.json, engine-input.json, summary.json, timeseries.csv,
network-events.jsonl, failures.csv, run.log를 보관합니다. 전체 패킷 기록을 선택한
경우 events.csv도 기록합니다. 외부 시나리오·장애 입력은 inputs/에 함께 보관합니다.
manifest에는 엔진 ID leo-native와 버전이 기록됩니다. 원본 FLoRaSat 실행 결과라고
표시하지 않습니다. 이전 원본 결과 폴더도 계속 열 수 있습니다.

실행 결과 열기에서 manifest.json이 있는 폴더를 선택하면 설정에 맞춰 지도와 CSV를
재생합니다. CSV만 열기는 경로/홉/예측 지연 표를 표시합니다. 다른 실행의 CSV를
기존 좌표에 임의로 겹치지 않습니다. CSV에는 실제 위치·연결 설정이 없습니다.
파일 누락·변경이나 지원하지 않는 모델이면 이전 지도를 유지합니다.
보관된 INI는 새 설정을 편집해도 변경되지 않습니다.

지도 좌표는 보관된 설정과 SGP4로 재현하고 장애·혼잡·CSIB 상태는 기록된 이벤트를
반영합니다. 버전 2 결과에는 자체 엔진의 실제 패킷 송수신·폐기·지연·지터 통계와
시간별 그래프가 표시됩니다. CSV만으로 수신 성공이나 큐 상태를 추정하지 않습니다.
실행 비교에서 환경 해시와 송신 시도 해시가 같은지 확인할 수 있습니다.
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
엔진 빌드는 고정 그래프 검사와 원본 MCSR 순수 코어의 5개 자체 검사를 수행합니다.
`python verify_workloads.py`는 1,584위성·100지상국 조건에서 5개 알고리즘의
10~60Mbps 짧은 실행 및 10/35/60Mbps 600초 실행 총 70개를 검사합니다.
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

설정의 실제 큐 모드는 송신 패킷으로 부하를 생성합니다. 환경 효과 모드는 별도
지정한 혼잡도·지연·손실을 주입하는 시험이며 결과에 구분해 기록됩니다.
SRD K/L과 consistent 플러딩은 참조 프로파일과 구별되는 추가 실험 옵션입니다.
time 모드의 DS·CSIB는 참조 시간 모델입니다. packet 모드는 아래 독립 실험 규격을 사용합니다.

## 연구 비교 모델 v3

비교 실행은 선택한 INI와 시드로 물리 사건을 먼저 확정한 뒤 알고리즘을 순차 실행합니다.
data/experiments/<ID>/common.json과 physical-events.json에 공통 입력을 보관하고,
각 실행은 data/runs/<ID>/에 별도로 보관합니다. route@T는 지정된 고정 출발·목적 흐름의
전파 지연 최소 기준 경로에서 링크를 정합니다. 실제 알고리즘 경로를 따라 장애를 바꾸지 않습니다.
모호한 기준 흐름이나 ISL 없는 기준 경로는 준비 단계에서 거부합니다.

신규 결과는 leo-native-replay-v3입니다. 기존 v1/v2는 계속 읽되 같은 계측으로 판정하지 않습니다.
전달률은 수신/발생이며 접속·경로 선택 실패도 분모와 실패 수에 포함합니다.
정보 감지·정보 적용·영향 흐름의 사건 후 첫 수신을 구분하며 관측 종료 시 미수렴/미복구를 표시합니다.
추가 처리 지연은 neutral에서 모두 0이고, 기존 reserveDelay/repairDelay/spfDelay 등은
assumed에서만 적용합니다. 계산 시간은 호스트 측정이며 시뮬레이션 시간으로 자동 환산하지 않습니다.

실험 모델의 INI 키는 *.native.processingMode = "neutral" 또는 "assumed",
*.native.controlMode = "packet" 또는 "time", *.native.jitterModel = "packet" 또는 "cost",
*.native.islMode = "fixed" 또는 "scheduled"입니다. scheduled는 UI에서 CSV를 가져오거나
*.native.islScheduleFile로 지정합니다. CSV 열은 time,event,satA,satB이며 failure/restore,
이전 fail, 그리고 down/up을 읽습니다. 고정 격자 밖의 연결은 지원하지 않습니다.

packet 지터의 jitter_ms는 링크 추가 지연의 표준편차입니다. 지연은 0..2√3σ의 균등분포로
추가되며 평균 √3σ도 지연 비용에 반영됩니다. 라우팅용 J는 관측된 도착 지연 변동이며,
수신 지터와 구분하고 주입값을 다시 합산하지 않습니다. cost 모드는 라우팅 비용 주입만 합니다.

실제 제어 통신은 독립 실험 규격입니다. 초기 공통 정보는 수렴된 상태로 설치하고 이후를 측정합니다.
SLS는 기본 0.2초/3회 누락, controlRefresh=5초, controlTimeout=3초입니다.
메시지 기본 헤더는 controlHeaderBytes=32, 상태 레코드는 controlRecordBytes=48바이트이며
전송 헤더 24바이트를 추가합니다. 데이터와 제어는 동일 FIFO·용량을 공유합니다.
Dijkstra와 CSIB 비활성 MCSR은 상태 플러딩을 사용합니다. CSIB 활성 MCSR은 지역 수집,
상태 자료를 포함한 경계 투표, DS 간 요약, 지역 배포를 사용하며 코어의 DS 선택 함수를 재사용합니다.
보고 누락은 제한 시간까지 기다리고 투표 자료가 불완전하면 DS를 유지합니다.
제어 패킷 도착 전 물리 상태를 라우터에 제공하지 않습니다. 실제 제어 규격이 논문과 같다는 주장은 하지 않습니다.

physical-events.json은 실제 물리 전이, control-series.csv는 시간별 누적 제어 계측입니다.
상세 제어 프레임은 기본적으로 저장하지 않습니다. 원본 순수 코어의 해시와 라이선스는 유지됩니다.
