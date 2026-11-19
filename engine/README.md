# Local engine

Windows 실행 파일 engine.exe는 OMNeT++/INET/WSL/Docker 없이 실행됩니다.
개발 빌드는 `python engine/build.py`입니다. 빌드 때만 WSL Ubuntu의 MinGW를 사용하며
설치 PC에는 컴파일러가 필요하지 않습니다. JSON 헤더는 nlohmann/json 3.12.0입니다.

## 알고리즘 추가

1. main.cpp의 Routing 인터페이스(selectUplink, forward, onNetworkEvent, onTimer)를
   구현합니다. Context가 현재 그래프·접속 후보·장애 정보와 이벤트 예약을 제공합니다.
2. routingFactories()에 고유 ID와 어댑터 생성자를 등록합니다.
3. native_engine.py의 ALGORITHMS와 필요한 설정 계약을 등록합니다.
   기존 INI에서 `*.satellite[*].net.routing.algorithm = "등록ID"`로 선택합니다.
4. 패킷 보존·장애·동일 송신 입력 검사와 verify_workloads.py를 수행합니다.

Dijkstra는 algorithms.h, MCSR은 vendor/routing/mcsr의 원본 Planner/Router/
Resilience/CSIB를 사용합니다. 어댑터와 실행 기반은 main.cpp와 control.h에 구현합니다.
원본 헤더 변경 여부는 vendor/routing-origin.json으로 확인할 수 있습니다.

기본은 기존 metric="delay" 또는 "hop"이며 INI 형식과 키를 유지합니다.
새 Config도 기존처럼 extends를 사용할 수 있습니다. NED나 OMNeT++ API를 구현하는
플러그인은 받지 않습니다. 경로 계산은 임의로 UI에서 다시 선택하지 않습니다.

## 출처와 차이

vendor/Global/Utilities/libnorad는 원본의 순수 궤도 코드를 그대로 복사한 것입니다.
vendor-origin.json이 파일 해시를 기록합니다. 원본 프로젝트는 실행·빌드에 필요 없습니다.
algorithms.h는 Sat_Net_DijkstraRouting::computeTree의 지연/홉 가중치와 낮은
선행 노드 ID 동률 규칙을 이식했습니다. main.cpp의 Walker 위상, 지상 이동,
앙각 연결, 송신 위성 선택은 원본 모델 규칙을 사용합니다.

이벤트 큐·패킷 객체·연결별 전송 예약은 새 실행 기반입니다. 원본의 OMNeT++ 객체를
재사용하지 않습니다. 고정 시드에서 자체 결과는 재현되지만 원본과 난수 소비 순서가
다를 수 있어 CSV 바이트 일치를 보장하지 않습니다. 큐/연결 해제 경계의 원본 완전
동등성도 아직 인증하지 않았습니다. 지원 범위는 Walker-Delta 지상 노드 모델이며
LoRa 무선 물리계층, 충돌, IoT, 에너지, TLE와 다른 FLoRaSat 모델은 제외합니다.

요약 통계는 summary.json, 진단은 events.csv에 저장됩니다. 패킷 보존 관계는
attempted = received + dropped + inFlight입니다. sent는 접속·초기 경로 선택에 성공한 시도이며,
sent = received + admittedDropped + inFlight입니다. noUplink는 송신 전 실패한 시도입니다.
계획 경로는 routes.csv에 기록하며 수신 성공 기록과 구분합니다.

## 라이선스

배포 프로그램은 GPL-3.0으로 제공하며 상위 GPL/LGPL 및 JSON MIT 고지를 보존합니다.
루트 LICENSE, THIRD_PARTY_NOTICES.txt, licenses/를 함께 제공합니다.
별도 프로세스라는 이유만으로 모든 배포 의무가 없어진다고 간주하지 않습니다.
코드/빌드 자료는 함께 제공하며 기관 납품 조건에 따른 이용 의무 검토는 별도입니다.

유한 방향별 FIFO 큐의 기본 용량은 500패킷입니다. 0은 무제한입니다.
링크 장애 시 대기 큐는 폐기하며 이미 전파 중인 패킷은 도착할 수 있습니다.
summary의 수신 지연은 큐·직렬화·전파·복구 처리 지연을 포함합니다.
지터는 흐름별 RFC 방식의 도착 간격 변동 추정값이며, 라우팅 비용 지터와 구별합니다.
시계는 10^-11초 정수이며 송신·장애·환경 손실 난수를 분리합니다.

## 실행 기반

control.h의 실험 제어 프로토콜은 실제 Packet으로 동일 FIFO를 사용합니다.
Context.graph(viewer)는 실제 전달된 광고만 반영하며 궤도 기반 전파 거리만 공통으로 예측합니다.
--prepare는 데이터·제어 송신 없이 기준 물리 사건만 산출합니다. Python runner가 이 결과를
고정해 재사용합니다. 격자 내 계획 개폐와 중첩 고장을 물리 전이로 합칩니다.
중립 모드와 가정값 모드를 구분하며 원본 mcsr 헤더는 수정하지 않습니다.
발생 = 수신 + 실패 + 전송 중, sent = 발생 - 접속/초기경로 실패입니다.
데이터와 제어 직렬화 중 단절은 폐기하고 이미 전파 중인 프레임은 도착할 수 있습니다.
algorithm operations는 원본 OpScope와 Dijkstra의 대응 카운터를 사용합니다.

## 연구 검증 확장 (4.1.0)

원본 MCSR 헤더는 유지합니다. `tests/oracle_driver.cpp`는 입력 그래프에 대한 실제 코어 결과를 내보내며, `verify_oracle.py`는 제품의 계산 함수를 재사용하지 않고 경로를 열거해 비교합니다. `tests/test_kernel.cpp`는 시험 빌드에서만 노출한 패킷 커널을 사용해 직렬화·FIFO·큐 초과·장애 시각 순서·종료 관측과 비대칭 Dijkstra 비용을 확인합니다. 일반 실행 파일에 시험용 설정이나 공개 API를 추가하지 않습니다.

`warmupSeconds`·`drainSeconds`가 모두 0이면 기존 계측을 유지합니다. 측정 코호트는 별도 `measurement` 하위 형식으로 기록합니다. `OpCounts::each`로 전체 연산 계측과 라벨 제한 횟수를 내보냅니다. Dijkstra는 목적지 기준 역방향 탐색에 역방향 인접 목록을 전달합니다. 이 변경은 방향별 지연이 다른 경우의 비용 계산을 수정하며 엔진·모델 버전으로 구분합니다.

전체 검증 명령은 루트 `verify_research.py`입니다. 이력은 `data/validation/`에 남고 사용자 `data/runs`와 분리합니다. 기본 실행에는 상세 추적이나 반복 실험을 강제하지 않습니다.
