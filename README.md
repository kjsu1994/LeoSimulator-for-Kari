# FLoRaSat 실행 결과 뷰어

로컬 PC에서 사용하는 위성망 결과 재생 UI입니다. 로그인·인증·외부 서버가
필요하지 않으며, HTML/CSS/JavaScript와 기존 Python 서버만 사용합니다.
원본 leo-main은 읽기만 합니다. 상태 기록 기능을 추가하지 않습니다.

## 사용 방법

### HTML로 결과 재생

1. index.html을 Chrome 또는 Edge로 엽니다.
2. 상단의 **실행 결과 열기**에서 manifest.json이 있는 실행 ID 폴더를 선택합니다.
3. 위성·지상국·이동 노드·연결선과 CSV의 패킷 경로를 함께 재생합니다.
4. 상세 설정을 펼치면 그 실행에 보관된 INI 내용을 확인할 수 있습니다.

서버·OMNeT++·WSL·Docker 없이 저장된 결과를 재생할 수 있습니다.
처음 열면 마지막 성공 결과가 있으면 자동 표시합니다. CSV만 열기는 경로 표·
홉 수·예측 전파 지연을 확인하는 용도입니다. 다른 실행의 CSV를 열면 기존 지도는
참고 화면으로 고정해 유지하고 해당 CSV를 지도 위에 임의로 겹치지 않습니다.

### 새 시뮬레이션 실행

start_ui.bat 또는 최신 EXE를 실행하고 실행 구성을 선택합니다.
실행 버튼은 원본의 별도 사본을 Docker에서 빌드·실행합니다. 원본 파일을 편집하거나
원본 results 폴더를 덮어쓰지 않습니다. 계산에는 기존대로 WSL·Docker·OMNeT++가
필요합니다. 이 기능은 OMNeT++ 라이선스 문제를 해결하거나 대체하지 않습니다.

실행 전 소스·설정을 메모리로 캡처하고 그 내용으로 이미지를 빌드합니다.
이미지 식별 해시는 소스·INI 변경을 반영합니다. 지원하는 원본 모델과 다르면
임의의 기본값으로 재현하지 않고 중단합니다. 결과 재생용 폴더에 원본 C++ 소스를
복사하지 않습니다. 빌드용 소스는 별도 실행 환경에서만 사용합니다.

## 실행별 보관 구조

```
data/runs/<UTC시각-고유ID>/
  manifest.json
  routes.csv
  config/
    simulations/
      Dijkstra132/
        omnetpp.ini
        stations/EsaDsn-Mobiles.ini
      Custom_default/
        devices/WorldWide-30.ini
        configurations/
          StatisticRecording.ini
          BlindTx.ini
          BandSelection.ini
          Services.ini
  results/       # 실행 로그와 원본 통계 등 부가 산출물
  result.json    # UI 자동 로딩용 파생 캐시
```

폴더 하나가 실행 결과 한 세트입니다. INI는 원문 바이트와 상대 경로를 그대로
보관하며, include를 재귀적으로 따라 필요한 파일을 모두 포함합니다. 합치거나
다른 실행의 INI를 대신 사용하지 않습니다. 같은 설정을 다시 실행해도 ID는 다릅니다.

manifest.json에는 실행 ID·등록 시각·Config 이름·실행 번호·실제 명령·원본 모델
버전과 CSV/INI 경로·해시가 있습니다. 시드는 해당 실행의 INI에 보관됩니다.
해시는 인증 수단이 아니라 잘못 섞이거나 변경된 파일을 확인하기 위한 것입니다.
CSV만 보관하거나 최상위 INI만 복사해서는 완전한 지도 재현 자료가 되지 않습니다.

UI의 data/latest_result.js는 마지막 결과의 로컬 자동 열기 캐시입니다.
원본의 출력 파일이 아니며, 이 파일이 없어도 실행 결과 폴더를 직접 열 수 있습니다.
기존 상태 기록 결과와 이전 실행 폴더는 자동으로 삭제하지 않습니다.

## CSV로 알 수 있는 것과 없는 것

- CSV: 시간, 계획 경로 ID, 출발지·목적지 ID, 첫/마지막 위성, ISL 홉 수,
  상향/위성 구간/총 예측 전파 지연, 위성 경로.
- CSV에 없는 것: 위성·지상국 좌표, 궤도·항로·초기 시각, 연결 판정 설정,
  실제 수신 여부·실측 종단 간 지연.
- CSV+실행 INI: 지원하는 원본 모델에 맞춰 orbit.js에서 위치·연결을 재현하고,
  CSV의 계획 경로를 그 위에 표시합니다. 화면에 **설정 기반 재현**으로 표시합니다.
- 실제 송수신 수와 실측 종단 간 지연은 CSV로 추정하지 않고 **제공되지 않음**으로
  표시합니다. 이는 계획 경로 수와 다른 개념입니다. 커버리지 원은 기하학적 근사입니다.

## 수동 실행 결과 묶기

이미 CSV를 생성했다면 실행에 실제 사용한 src/와 simulations/가 있는 프로젝트
트리를 지정해 묶을 수 있습니다. 아래는 Windows PowerShell 예시입니다.

```powershell
cd O:\sim-ui
python result_bundle.py --csv "C:\실제결과경로\routes.csv" --source "O:\leo-main" --config Dijkstra132
```

원본과 CSV는 읽기만 하며, data/runs/<고유ID>/를 새로 만듭니다.
--source는 실행 후 수정된 설정이 아니라 **해당 실행에 사용한 설정·모델**이어야 합니다.
컨테이너에서 실행했다면 그 컨테이너의 설정이 기준입니다. 출처를 증명할 수 없는
수동 등록은 manifest에 manual-import-unverified로 표시하고 화면에서도 안내합니다.
명령행으로 계산 파라미터를 덮어쓴 결과나 다른 원본 버전은 현재 자동 재현 대상이
아닙니다. 지원하지 않는 값은 원본과 같다고 가정하지 않습니다.

## 지원 범위와 원본 보존

현재 버전은 전달받은 원본의 Walker-Delta, 균등 궤도면, 지상 노드 Dijkstra 모델을
지원합니다. Dijkstra132, Dijkstra132Hop, Dijkstra132Pair, Dijkstra132StationsOnly가
대상이며, IoT 단말/TLE/Walker-Star 등은 제외합니다.

이전 작업에서 추가했던 FloraMapDataSource.h/.cc/.ned의 stateLogFile 기능과
Dijkstra132Hop의 metric 변경은 되돌렸습니다. 원본의 Hop 설정은 실제로 delay로
되어 있으므로 UI도 그 실제 값을 표시합니다. 이름만 보고 hop으로 바꾸지 않습니다.
앞으로 원본을 수정하지 않고 sim-ui 안에서만 개선합니다.

원본 C++의 궤도 보정항·항력·기본값과 마지막 궤도면 연결 규칙을 orbit.js에 맞췄습니다.
132기·600초 기준 엔진 기록 602개 시점과 비교해 위치 오차 최대 약 4mm,
연결 목록은 모두 일치했습니다. 이 검증이 모든 FLoRaSat 설정의 동등성을 뜻하지는
않습니다. 원본 버전과 지원 범위를 명시적으로 확인합니다. 자세한 출처와 검증 범위는
REPLAY_NOTES.md에 있습니다.

## 검증

```
python -m unittest discover -s . -p "test_*.py" -v
node test_csv.js
node test_replay.js
node test_browser.js
```

브라우저 검증은 설치된 Chrome·Node를 사용합니다. HTML 폴더 선택, 상세 설정,
CSV 단독 재생, 해시/파일 누락 오류에서 지도 보존, 마우스 툴팁, 화면 크기별
배치를 검사합니다. 추가 테스트 라이브러리는 설치하지 않습니다.
