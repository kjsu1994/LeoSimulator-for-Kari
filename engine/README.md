# Local engine

Windows 실행 파일 engine.exe는 OMNeT++/INET/WSL/Docker 없이 실행됩니다.
개발 빌드는 `python engine/build.py`입니다. 빌드 때만 WSL Ubuntu의 MinGW를 사용하며
설치 PC에는 컴파일러가 필요하지 않습니다. JSON 헤더는 nlohmann/json 3.12.0입니다.

## 알고리즘 추가

1. algorithms.h의 Router 규격으로 함수를 구현합니다. 입력은 인접 리스트와 목적지이며,
   출력은 각 노드의 next/hops/cost/delay 벡터입니다. 도달 불가는 next=-1, cost=무한대입니다.
2. routingAlgorithms()에 ID와 경로 계산 함수·링크 가중치 함수를 등록합니다.
3. 기존 INI의 Config에 `*.satellite[*].net.routing.algorithm = "등록ID"`를 추가합니다.
   이 선택 키는 로컬 엔진용이며, 기존 INI에는 추가하지 않아도 됩니다.
4. 엔진을 다시 빌드하고 고정 그래프·동률·경로 없음 검사와 전체 실행 검사를 수행합니다.

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
sent = received + dropped + inFlight입니다. noUplink는 송신 전 실패한 시도입니다.
계획 경로는 routes.csv에 기록하며 수신 성공 기록과 구분합니다.

## 라이선스

배포 프로그램은 GPL-3.0으로 제공하며 상위 GPL/LGPL 및 JSON MIT 고지를 보존합니다.
루트 LICENSE, THIRD_PARTY_NOTICES.txt, licenses/를 함께 제공합니다.
별도 프로세스라는 이유만으로 모든 배포 의무가 없어진다고 간주하지 않습니다.
코드/빌드 자료는 함께 제공하며 기관 납품 조건에 따른 이용 의무 검토는 별도입니다.
