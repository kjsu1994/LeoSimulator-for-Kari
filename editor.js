// SPDX-License-Identifier: GPL-3.0-only
// Native controls use the server's applied-setting contract; INI remains the source.
const nativeUI={config:null,scenario:{schema:1,method:"environment",entries:[]},entryIndex:-1,drawing:null,baseline:null};
const byId=id=>document.getElementById(id);
function node(tag,attrs={},text){const e=document.createElement(tag);for(const [k,v]of Object.entries(attrs)){if(k==="class")e.className=v;else if(k in e)e[k]=v;else e.setAttribute(k,v);}if(text!==undefined)e.textContent=text;return e;}
function control(label,id,value,type="number",options={}){
  const wrap=node("label",{class:"editor-field"});wrap.append(node("span",{},label));let e;
  if(type==="select"){e=node("select",{id});for(const [key,title]of Object.entries(options))e.add(new Option(title,key));e.value=String(value);}
  else e=node("input",{id,type,value:value??"",step:"any",...options});
  wrap.append(e);return wrap;
}
function group(title,open=false){const e=node("details",{class:"editor-group",open});e.append(node("summary",{},title));const content=node("div",{class:"editor-grid"});e.append(content);return [e,content];}
function value(id){return byId(id)?.value;}
function numeric(id){const v=Number(value(id));if(!Number.isFinite(v)||value(id)==="")throw Error("입력값을 확인하세요.");return v;}
function modeChanged(){const mode=value("edit-congestionMode");byId("scenarioModeText").textContent=mode==="actual"?"실제 송신과 큐의 대기·폐기를 기록합니다.":"외부 혼잡 효과를 링크 상태에 반영합니다.";}
function updateMcsrControls(){
  const algorithm=value("edit-algorithm"),mcsr=algorithm.startsWith("mcsr-"),packet=value("edit-controlMode")==="packet",assumed=value("edit-processingMode")==="assumed",csib=mcsr&&byId("edit-csibEnabled").checked,environment=value("edit-congestionMode")==="environment";
  for(const group of document.querySelectorAll("[data-mcsr]"))group.hidden=!mcsr;
  const conditions={
    hrJitterMs:algorithm==="mcsr-hr",tsDelayBoundMs:algorithm==="mcsr-ts",jitterWindow:mcsr,jitterSource:mcsr,
    csibVote:csib,csibPeriod:csib,csibMonitorInterval:csib,csibMessageBytes:csib,
    csibComputeDelay:csib&&assumed,csibReportProcDelay:csib&&assumed&&packet,csibHopProcDelay:csib&&assumed&&packet,csibDetectDelay:csib&&!packet,
    reserveDelay:mcsr&&assumed,repairDelay:mcsr&&assumed,spfDelay:!mcsr&&assumed,floodHopDelay:assumed||!packet,
    failureKnowledge:!packet,failureScopeHops:!packet&&value("edit-failureKnowledge")==="propagate",
    routingQueueCapacity:mcsr&&!packet&&Number(value("edit-queueCapacity"))===0,
    controlRefresh:packet,controlTimeout:packet,controlHeaderBytes:packet,controlRecordBytes:packet,controlMtu:packet,controlReassemblyTimeout:packet,controlResyncInterval:packet,
    congestionRefresh:environment,congestionBuffer:environment,congestionPacketBytes:environment,congestionZones:environment,congestionAware:environment&&!mcsr
  };
  for(const [key,enabled]of Object.entries(conditions)){const input=byId("edit-"+key);if(input)input.closest("label").hidden=!enabled;}
  for(const input of byId("settingsFields").querySelectorAll("[id^='edit-']"))input.disabled=!!input.closest("[hidden]");
  const alternatives=[value("edit-processingMode")==="assumed"&&"처리 지연 가정",!packet&&"시간 기반 제어",value("edit-jitterModel")==="cost"&&"비용 기반 지터"].filter(Boolean);
  byId("settingsModelStatus").textContent=alternatives.length?"모델 검증 · "+alternatives.join(" · "):"";
  byId("settingsModelStatus").hidden=!alternatives.length;
}
function openSettings(cfg){
  nativeUI.config=cfg;nativeUI.scenario=structuredClone(cfg.scenario||{schema:1,method:"environment",entries:[]});nativeUI.entryIndex=-1;
  const container=byId("settingsFields");container.replaceChildren();byId("settingsError").textContent="";
  const [basic,grid]=group("기본 설정",true);container.append(basic);
  grid.append(control("알고리즘","edit-algorithm",cfg.algorithm,"select",cfg.capabilities.algorithms));
  byId("edit-algorithm").closest("label").hidden=true;
  const basicKeys=["numSatellites","planes","altitude","inclination","simTimeLimit","seed"];
  const addField=(grid,f)=>grid.append(control(f.label,"edit-"+f.key,cfg[f.key],"number",{min:f.min,max:f.max,step:f.type==="int"?"1":"any"}));
  for(const f of cfg.capabilities.fields.filter(f=>basicKeys.includes(f.key)))addField(grid,f);
  const [models,mg]=group("모델 검증");container.append(models);
  for(const [key,label,choices]of [["processingMode","처리 지연",{neutral:"추가 지연 없음",assumed:"가정값 적용"}],["controlMode","제어 통신",{packet:"실제 패킷",time:"시간 가정"}],["jitterModel","지터",{packet:"실제 지연 변동",cost:"라우팅 비용 주입"}],["islMode","ISL 연결",{fixed:"고정 격자",scheduled:"계획된 개폐"}]])mg.append(control(label,"edit-"+key,cfg[key]||Object.keys(choices)[0],"select",choices));
  for(const [key,label]of [["controlRefresh","제어 갱신 (s)"],["controlTimeout","수집 제한 시간 (s)"],["controlHeaderBytes","제어 헤더 (B)"],["controlRecordBytes","상태 레코드 (B)"],["controlMtu","제어 MTU (B)"],["controlReassemblyTimeout","재조립 제한 (s)"],["controlResyncInterval","재동기화 주기 (s)"]])mg.append(control(label,"edit-"+key,cfg[key],"number",{min:key==="controlMtu"?64:key.endsWith("Bytes")?1:1e-11,step:key.endsWith("Bytes")||key==="controlMtu"?1:"any"}));
  const scheduleLabel=node("label",{class:"file-btn"},"ISL 개폐 일정 가져오기"),scheduleFile=node("input",{type:"file",accept:".csv",hidden:true});scheduleLabel.append(scheduleFile);mg.append(scheduleLabel);
  scheduleFile.onchange=async()=>{try{nativeUI.scenario.scheduled_events=readEventCsv(await scheduleFile.files[0].text());byId("edit-islMode").value="scheduled";scheduleLabel.firstChild.textContent=`ISL 일정 ${nativeUI.scenario.scheduled_events.length}개`;byId("settingsError").textContent="";}catch(e){byId("settingsError").textContent=e.message;}};
  if(cfg.islScheduleFile)mg.append(control("기존 일정 경로","edit-islScheduleFile",cfg.islScheduleFile,"text"));
  const [traffic,tf]=group("트래픽",true);container.append(traffic);
  const enabled=cfg.stations.filter(s=>s.enabled);const first=enabled[0]||cfg.stations[0];
  const senderWrap=node("details",{class:"sender-list"});senderWrap.append(node("summary",{},`송신 노드 (${enabled.length}개 선택)`));
  const search=node("input",{type:"search",placeholder:"노드 검색",'aria-label':"송신 노드 검색"});senderWrap.append(search);
  const list=node("div",{class:"sender-options"});
  for(const s of cfg.stations){const label=node("label");label.append(node("input",{type:"checkbox",value:s.id,checked:s.enabled,class:"sender-check"}),document.createTextNode(`${s.id} · ${s.name}`));list.append(label);}
  search.oninput=()=>{for(const label of list.children)label.hidden=!label.textContent.toLowerCase().includes(search.value.toLowerCase());};
  list.onchange=()=>senderWrap.querySelector("summary").textContent=`송신 노드 (${list.querySelectorAll("input:checked").length}개 선택)`;
  senderWrap.append(list);tf.append(senderWrap);
  tf.append(control("목적지","edit-destination",first.dstStation,"select",{"-1":"무작위",...Object.fromEntries(cfg.stations.map(s=>[s.id,`${s.id} · ${s.name}`]))}));
  tf.append(control("패킷 크기 (B)","edit-packetSize",first.packetSize,"number",{min:1,step:1}));
  tf.append(control("송신률 (Mbps)","edit-bitrate",first.packetSize*8/first.sendInterval/1e6,"number",{min:.000001}));
  tf.append(control("송신 간격 (s)","edit-sendInterval",first.sendInterval,"number",{min:1e-11}));
  tf.append(control("송신 시작 (s)","edit-startTime",first.startTime??"","number",{min:0,placeholder:"기존 시작 방식 유지"}));
  tf.append(control("송신 종료 (s)","edit-endTime",first.endTime,"number",{min:0}));
  byId("edit-bitrate").oninput=()=>{const r=Number(value("edit-bitrate")),b=Number(value("edit-packetSize"));if(r>0&&b>0)byId("edit-sendInterval").value=b*8/(r*1e6);};
  byId("edit-packetSize").oninput=()=>byId("edit-bitrate").oninput();
  byId("edit-sendInterval").oninput=()=>{const t=Number(value("edit-sendInterval"));if(t>0)byId("edit-bitrate").value=Number(value("edit-packetSize"))*8/t/1e6;};
  byId("edit-simTimeLimit").oninput=()=>{if(Number(value("edit-endTime"))===cfg.simTimeLimit)byId("edit-endTime").value=value("edit-simTimeLimit");};
  const common=["islDatarate","groundlinkDatarate","minimumElevation","queueCapacity","updateInterval"];
  const [links,lf]=group("링크·큐");container.append(links);for(const f of cfg.capabilities.fields.filter(f=>common.includes(f.key)))addField(lf,f);
  const [mcsr,mf]=group("MCSR");mcsr.dataset.mcsr="1";container.append(mcsr);
  const csibKeys=cfg.capabilities.fields.filter(f=>f.key.startsWith("csib")).map(f=>f.key);
  const experimental=["srdK","srdL"];
  for(const f of cfg.capabilities.fields.filter(f=>!basicKeys.includes(f.key)&&!common.includes(f.key)&&!csibKeys.includes(f.key)&&!experimental.includes(f.key)&&!["slsInterval","slsMissCount","floodHopDelay","spfDelay"].includes(f.key)))addField(mf,f);
  mf.append(control("지터 측정 방식","edit-jitterSource",cfg.jitterSource,"select",{arrival:"도착 지연 변동",delayChange:"전파 지연 변화"}));
  for(const key of ["reservedPaths","snapshotOnDelayUpdate"]){const label=node("label",{class:"check-field"});label.append(node("input",{id:"edit-"+key,type:"checkbox",checked:cfg[key]}),document.createTextNode(key==="reservedPaths"?"예비 경로 사용":"전파 지연 갱신 시 상태 재계산"));mf.append(label);}
  const [csib,cf]=group("DS·CSIB");csib.dataset.mcsr="1";container.append(csib);
  for(const key of ["csibEnabled","csibVote"]){const label=node("label",{class:"check-field"});label.append(node("input",{id:"edit-"+key,type:"checkbox",checked:cfg[key]}),document.createTextNode(key==="csibEnabled"?"DS·CSIB 사용":"DS 투표 사용"));cf.append(label);}
  for(const f of cfg.capabilities.fields.filter(f=>csibKeys.includes(f.key)))addField(cf,f);
  const [srd,sg]=group("공통 SRD");
  for(const f of cfg.capabilities.fields.filter(f=>experimental.includes(f.key)))addField(sg,f);
  mg.append(control("MCSR 경로 계산 방식","edit-floodModel",cfg.floodModel,"select",{paper:"참조 방식",consistent:"축 일관 방식 (추가 실험)"}));
  const [fault,ff]=group("장애·복구");container.append(fault);
  ff.append(control("장애 일정","edit-failureSchedule",cfg.failureSchedule,"text",{placeholder:"12-13@60+30 또는 route@60+30"}));
  ff.append(control("장애 정보 전달","edit-failureKnowledge",cfg.failureKnowledge,"select",{instant:"즉시",propagate:"감지·전파 후"}));
  for(const f of cfg.capabilities.fields.filter(f=>["slsInterval","slsMissCount","floodHopDelay","spfDelay"].includes(f.key)))addField(ff,f);
  for(const [key,label]of Object.entries({randomFailures:"무작위 장애 수",randomStart:"시작 (s)",randomEnd:"종료 (s)",randomDuration:"복구까지 (s, 0=영구)",failureSeed:"장애 시드"}))ff.append(control(label,"edit-"+key,cfg[key]));
  const importFault=node("label",{class:"file-btn"},"장애 기록 가져오기");const faultFile=node("input",{type:"file",accept:".csv",hidden:true});importFault.append(faultFile);ff.append(importFault);
  faultFile.onchange=async()=>{try{const text=await faultFile.files[0].text();const rows=text.trim().split(/\r?\n/);if(rows.shift()!=="time,event,satA,satB")throw Error("장애 CSV 형식을 확인하세요.");nativeUI.scenario.failure_events=rows.map(s=>{const[t,event,a,b]=s.split(",");return{time:Number(t),event,a:Number(a),b:Number(b)};});setStatus(`장애 기록 ${rows.length}개를 설정에 포함합니다.`);}catch(e){byId("settingsError").textContent=e.message;}};
  if(cfg.failureReplay)ff.append(control("기존 장애 기록 경로","edit-failureReplay",cfg.failureReplay,"text"));
  const [congestion,cg]=group("혼잡·환경 효과");container.append(congestion);
  cg.classList.add("scenario-editor");cg.append(control("시험 방식","edit-congestionMode",cfg.congestionMode,"select",{actual:"실제 큐",environment:"환경 효과 주입"}));cg.append(node("p",{id:"scenarioModeText",class:"quiet"}));
  cg.append(control("지역 갱신 간격 (s)","edit-congestionRefresh",cfg.congestionRefresh,"number",{min:1e-11}));
  cg.append(control("모형 버퍼 B (패킷)","edit-congestionBuffer",cfg.congestionBuffer,"number",{min:1,step:1}));
  cg.append(control("모형 패킷 크기 (B)","edit-congestionPacketBytes",cfg.congestionPacketBytes,"number",{min:1,step:1}));
  cg.append(control("기존 zones 설정","edit-congestionZones",cfg.congestionZones,"text"));
  const aware=node("label",{class:"check-field"});aware.append(node("input",{type:"checkbox",id:"edit-congestionAware",checked:cfg.congestionAware}),document.createTextNode("Dijkstra 비용에 환경 지연 반영"));cg.append(aware);
  cg.append(node("div",{id:"scenarioEntries",class:"scenario-entries"}),node("div",{id:"entryForm",class:"entry-form"}));
  buildEntryForm();refreshEntries();byId("edit-congestionMode").onchange=()=>{modeChanged();updateMcsrControls();};modeChanged();
  const [recording,rf]=group("기록");container.append(recording);rf.append(control("경로 기록","edit-routeRecording",cfg.routeRecording,"select",{changes:"변경된 경로",full:"모든 계획 경로"}));
  const pt=node("label",{class:"check-field"});pt.append(node("input",{id:"edit-packetTrace",type:"checkbox",checked:cfg.packetTrace}),document.createTextNode("전체 패킷 기록 (대용량)"));rf.append(pt);
  const [advanced,ag]=group("고급 설정");advanced.id="settingsAdvanced";ag.classList.add("settings-sections");container.append(advanced);
  const [routing,rg]=group("경로 계산");routing.dataset.mcsr="1";ag.append(routing);
  const [protocol,pg]=group("제어 통신");ag.append(protocol);
  // Keep the common SRD editable for Dijkstra-targeted scenarios as well.
  ag.append(srd);
  const move=(keys,target)=>{for(const key of keys){const input=byId("edit-"+key);if(input)target.append(input.closest("label"));}};
  move(["labelCap","routingQueueCapacity","refreshInterval","flowTimeout","jitterWindow","jitterSource","snapshotOnDelayUpdate"],rg);
  move(["slsInterval","slsMissCount","controlRefresh","controlTimeout","controlHeaderBytes","controlRecordBytes","controlMtu","controlReassemblyTimeout","controlResyncInterval","csibPeriod","csibMonitorInterval","csibMessageBytes"],pg);
  move(["reserveDelay","repairDelay","spfDelay","floodHopDelay","failureKnowledge","failureScopeHops","csibComputeDelay","csibReportProcDelay","csibHopProcDelay","csibDetectDelay"],mg);
  byId("edit-floodModel").closest("label").dataset.mcsr="1";
  move(["islMode","islScheduleFile"],lf);lf.append(scheduleLabel);
  ag.append(models,recording);
  container.insertBefore(node("p",{id:"settingsModelStatus",class:"model-status",role:"status",hidden:true}),basic);
  for(const key of ["algorithm","processingMode","controlMode","jitterModel","csibEnabled","failureKnowledge","queueCapacity"])byId("edit-"+key).addEventListener("change",updateMcsrControls);
  updateMcsrControls();byId("settingsDialog").showModal();
}
const cities={"서울":[37.5665,126.978],"도쿄":[35.6762,139.6503],"베이징":[39.9042,116.4074],"싱가포르":[1.3521,103.8198],"시드니":[-33.8688,151.2093],"뭄바이":[19.076,72.8777],"두바이":[25.2048,55.2708],"모스크바":[55.7558,37.6173],"런던":[51.5074,-.1278],"파리":[48.8566,2.3522],"프랑크푸르트":[50.1109,8.6821],"카이로":[30.0444,31.2357],"라고스":[6.5244,3.3792],"요하네스버그":[-26.2041,28.0473],"뉴욕":[40.7128,-74.006],"로스앤젤레스":[34.0522,-118.2437],"멕시코시티":[19.4326,-99.1332],"상파울루":[-23.5505,-46.6333],"부에노스아이레스":[-34.6037,-58.3816],"산티아고":[-33.4489,-70.6693]};
function buildEntryForm(entry=null){
  const form=byId("entryForm");form.replaceChildren();const target=entry?.target,params=target?.params||{};
  let kind=target?.kind||"link";if(kind==="zone")kind=params.zone_kind;if(kind==="region")kind=params.shape;
  form.append(control("대상","entry-kind",kind,"select",{link:"링크",satellite:"위성",plane_range:"궤도면·위성 범위",latlon_box:"위경도 구역",srd:"SRD 범위",path_hops:"기준 경로의 홉",rect:"지도 사각형",circle:"지도 원·도시 반경"}));
  const area=node("div",{id:"entry-target",class:"editor-grid"});form.append(area);
  function targetFields(){
    area.replaceChildren();const k=value("entry-kind"),n=nativeUI.config.numSatellites;
    const fields=k==="link"?[["위성 A","sat_a",0],["위성 B","sat_b",1]]:k==="satellite"?[["위성 번호","sat",0]]:k==="plane_range"?[["궤도면 시작","plane_from",0],["궤도면 끝","plane_to",0],["면 내 위성 시작","sat_from",0],["면 내 위성 끝","sat_to",0]]:k==="srd"?[["SRD 시작","srd_from",0],["SRD 끝","srd_to",0]]:k==="path_hops"?[["시작 홉","hop_from",1],["종료 홉","hop_to",1]]:k==="circle"?[["중심 위도","center_lat",37.5665],["중심 경도","center_lon",126.978],["반경 (km)","radius_km",1500]]:[["최소 위도","lat_min",30],["최대 위도","lat_max",45],["시작 경도","lon_min",120],["끝 경도","lon_max",140]];
    for(const [label,key,d]of fields)area.append(control(label,"target-"+key,params[key]??d));
    if(k==="circle"){
      area.append(control("도시","entry-city","","select",{"":"직접 입력",...Object.fromEntries(Object.keys(cities).map(s=>[s,s]))}));byId("entry-city").onchange=()=>{const p=cities[value("entry-city")];if(p){byId("target-center_lat").value=p[0];byId("target-center_lon").value=p[1];}};
    }
    if(["rect","circle","latlon_box"].includes(k)){
      const draw=node("button",{type:"button"},"지도에서 지정");draw.onclick=()=>{nativeUI.drawing={shape:k==="circle"?"circle":"rect",start:null};playing=false;byId("settingsDialog").close();setStatus("지도에서 범위를 드래그하세요. Esc로 취소합니다.");};area.append(draw);
      area.append(control("범위 내 세기","target-profile",params.profile||"uniform","select",{uniform:"균일",hotspot:"중심이 강한 핫스팟"}),control("경계 세기 (%)","target-edge_pct",params.edge_pct??20,"number",{min:0,max:100}));
    }
    if(k==="path_hops"){
      const chosen=selectedRouteId?findRouteById(routes,selectedRouteId):null;const options={"":"경로 선택",...Object.fromEntries(routes.slice(-100).map(r=>[r[1],`${r[0].toFixed(2)}s · ${r[2]}→${r[3]} · ${r[4]}홉`]))};
      area.append(control("기준 경로","entry-route",chosen?.[1]??"","select",options));
    }
    area.append(control("방향","target-direction",params.direction||"both","select",k==="link"?{both:"양방향",out:"A → B",in:"B → A"}:{both:"양방향",out:"나가는 방향",in:"들어오는 방향"}));
    area.append(control("링크 적용","target-link_rule",params.link_rule||"any","select",{any:"한쪽 끝 포함",both:"양쪽 끝 포함"}));
  }
  byId("entry-kind").onchange=targetFields;targetFields();
  const time=node("div",{class:"editor-grid"});form.append(time);
  time.append(control("시작 (s)","entry-start",entry?.start_s??0,"number",{min:0}),control("종료 (s, 빈칸=끝까지)","entry-end",entry?.end_s??"","number",{min:0}),control("점진 증가 (s)","entry-ramp",entry?.ramp_s??0,"number",{min:0}),control("혼잡도 (%)","entry-rho",(entry?.rho??.6)*100,"number",{min:1,max:99}));
  for(const [key,label,defaultValue]of [["fail","링크 장애",false],["use_capacity","용량 감소",true],["use_buffer","가용 버퍼 감소",false],["use_queue","M/M/1 지연·지터",false]]){const wrap=node("label",{class:"check-field"});wrap.append(node("input",{id:"entry-"+key,type:"checkbox",checked:entry?!!entry[key]:defaultValue}),document.createTextNode(label));form.append(wrap);}
  const penalty=node("div",{class:"editor-grid"});for(const [key,label,max]of [["delay_ms","추가 지연 (ms)",100000],["jitter_ms","추가 지연 표준편차 (ms)",100000],["loss_pct","패킷 손실률 (%)",100]])penalty.append(control(label,"penalty-"+key,entry?.penalty?.[key]??0,"number",{min:0,max}));form.append(penalty);
  byId("entry-fail").onchange=()=>{const fail=byId("entry-fail").checked;byId("target-direction").disabled=fail;if(fail)byId("target-direction").value="both";for(const key of ["use_capacity","use_buffer","use_queue"]){byId("entry-"+key).disabled=fail;if(fail)byId("entry-"+key).checked=false;}for(const e of penalty.querySelectorAll("input")){e.disabled=fail;if(fail)e.value=0;}if(fail)byId("entry-ramp").value=0;};byId("entry-fail").onchange();
  const actions=node("div",{class:"entry-actions"});const add=node("button",{type:"button",class:"btn-primary"},nativeUI.entryIndex<0?"조건 추가":"조건 수정");
  add.onclick=()=>{try{const e=entryFromForm();if(nativeUI.entryIndex<0)nativeUI.scenario.entries.push(e);else nativeUI.scenario.entries[nativeUI.entryIndex]=e;nativeUI.entryIndex=-1;refreshEntries();buildEntryForm();byId("settingsError").textContent="";}catch(error){byId("settingsError").textContent=error.message;}};actions.append(add);
  const upload=node("label",{class:"file-btn"},"시나리오 가져오기"),file=node("input",{type:"file",accept:".json",hidden:true});upload.append(file);file.onchange=async()=>{try{const s=JSON.parse(await file.files[0].text());if(!Array.isArray(s.entries))throw Error("시나리오 형식을 확인하세요.");nativeUI.scenario=s;refreshEntries();}catch(e){byId("settingsError").textContent=e.message;}};
  const download=node("button",{type:"button"},"시나리오 내보내기");download.onclick=()=>{const a=node("a",{href:URL.createObjectURL(new Blob([JSON.stringify(nativeUI.scenario,null,2)],{type:"application/json"})),download:"congestion_scenario.json"});a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000);};actions.append(upload,download);form.append(actions);
}
function entryFromForm(){
  const k=value("entry-kind");const p={};for(const input of byId("entry-target").querySelectorAll("input[id^=target-],select[id^=target-]"))p[input.id.slice(7)]=input.type==="number"?Number(input.value):input.value;
  let kind=k;if(["plane_range","latlon_box","srd"].includes(k)){kind="zone";p.zone_kind=k;}if(["rect","circle"].includes(k)){kind="region";p.shape=k;}if(k==="link")p.bidirectional=p.direction==="both";
  if(k==="path_hops") {const r=findRouteById(routes,Number(value("entry-route")));if(nativeUI.baseline)nativeUI.scenario.baseline=structuredClone(nativeUI.baseline);else {if(!r)throw Error("기준 경로를 선택하세요.");nativeUI.scenario.baseline={route:r[6],runId:resultMeta?.runId,time:r[0]};}}
  const e={target:{kind,params:p},start_s:numeric("entry-start"),end_s:value("entry-end")===""?null:numeric("entry-end"),ramp_s:numeric("entry-ramp"),rho:numeric("entry-rho")/100};
  if(e.start_s<0||e.start_s>=numeric("edit-simTimeLimit")||(e.end_s!==null&&e.end_s<=e.start_s))throw Error("조건의 시작·종료 시각을 확인하세요.");
  for(const key of ["fail","use_capacity","use_buffer","use_queue"])e[key]=byId("entry-"+key).checked;
  const pen=Object.fromEntries(["delay_ms","jitter_ms","loss_pct"].map(key=>[key,numeric("penalty-"+key)]));if(Object.values(pen).some(v=>v>0))e.penalty=pen;
  if(!e.fail){byId("edit-congestionMode").value="environment";modeChanged();}
  return e;
}
function refreshEntries(){const list=byId("scenarioEntries");list.replaceChildren();nativeUI.scenario.entries.forEach((e,i)=>{const row=node("div",{class:"scenario-entry"});row.append(node("span",{},`${i+1}. ${e.target.kind} · ${e.start_s}s–${e.end_s??"끝"} · ${e.fail?"장애":`${Math.round(e.rho*100)}%`}`));const edit=node("button",{type:"button"},"편집"),remove=node("button",{type:"button"},"삭제");edit.onclick=()=>{nativeUI.entryIndex=i;buildEntryForm(e);};remove.onclick=()=>{nativeUI.scenario.entries.splice(i,1);nativeUI.entryIndex=-1;refreshEntries();};row.append(edit,remove);list.append(row);});}
byId("btnSettings").onclick=async()=>{try{const r=await fetch(`/api/config?config=${encodeURIComponent(cfgNameSel.value)}`,{cache:"no-store"});const cfg=await r.json();if(!r.ok)throw Error(cfg.error);openSettings(cfg);}catch(e){setStatus(e.message);}};
for(const button of document.querySelectorAll("[data-close]"))button.onclick=()=>byId(button.dataset.close).close();
byId("settingsForm").onsubmit=async event=>{
  event.preventDefault();const save=event.submitter||byId("settingsForm").querySelector("[type=submit]");save.disabled=true;
  try {
    const cfg=nativeUI.config,first=cfg.stations.find(s=>s.enabled)||cfg.stations[0],changes={senders:[...document.querySelectorAll(".sender-check:checked")].map(e=>Number(e.value))};
    if(value("edit-algorithm")!==cfg.algorithm)changes.algorithm=value("edit-algorithm");
    for(const [key,original]of Object.entries({destination:first.dstStation,packetSize:first.packetSize,sendInterval:first.sendInterval,endTime:first.endTime,startTime:first.startTime})){if(value("edit-"+key)!==""&&numeric("edit-"+key)!==original)changes[key]=numeric("edit-"+key);}
    for(const f of cfg.capabilities.fields){const input=byId("edit-"+f.key);if(input&&!input.disabled)changes[f.key]=numeric("edit-"+f.key);}
    for(const key of Object.keys(cfg.capabilities.booleans)){const input=byId("edit-"+key);if(input&&!input.disabled)changes[key]=input.checked;}
    for(const key of Object.keys(cfg.capabilities.choices)){const input=byId("edit-"+key);if(input&&!input.disabled)changes[key]=input.value;}
    for(const key of ["congestionMode","failureSchedule","failureKnowledge","congestionZones","routeRecording","failureReplay"]){const input=byId("edit-"+key);if(input&&!input.disabled)changes[key]=input.value;}
    for(const key of ["randomFailures","randomStart","randomEnd","randomDuration","failureSeed","congestionRefresh","congestionBuffer","congestionPacketBytes"]){const input=byId("edit-"+key);if(input&&!input.disabled)changes[key]=numeric("edit-"+key);}
    for(const key of ["processingMode","controlMode","jitterModel","islMode","islScheduleFile"]){const e=byId("edit-"+key);if(e&&!e.disabled)changes[key]=e.value;}
    for(const key of ["controlRefresh","controlTimeout","controlHeaderBytes","controlRecordBytes","controlMtu","controlReassemblyTimeout","controlResyncInterval"]){const input=byId("edit-"+key);if(input&&!input.disabled)changes[key]=numeric("edit-"+key);}
    changes.packetTrace=byId("edit-packetTrace").checked;
    for(const key of Object.keys(changes))if(changes[key]===cfg[key])delete changes[key];
    const response=await fetch("/api/config",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({configName:cfg.activeConfig,changes,scenario:nativeUI.scenario})});const result=await response.json();if(!response.ok)throw Error(result.error||"저장하지 못했습니다.");
    applyLoadedConfig(result,{announce:true});byId("settingsDialog").close();setStatus("INI에 저장했습니다. 실행하면 새 설정이 적용됩니다.");
  }catch(e){byId("settingsError").textContent=e.message;}finally{save.disabled=false;}
};
function mapCoordinates(e){const r=canvas.getBoundingClientRect(),scale=Math.min(r.width/W,r.height/H),x=(e.clientX-r.left-(r.width-W*scale)/2)/scale,y=(e.clientY-r.top-(r.height-H*scale)/2)/scale;return {lat:Math.max(-90,Math.min(90,90-y/H*180)),lon:Math.max(-180,Math.min(180,x/W*360-180))};}
canvas.addEventListener("pointerdown",e=>{if(nativeUI.drawing){nativeUI.drawing.start=mapCoordinates(e);canvas.setPointerCapture(e.pointerId);}});
canvas.addEventListener("pointerup",e=>{const d=nativeUI.drawing;if(!d?.start)return;const end=mapCoordinates(e);nativeUI.drawing=null;
  if(d.shape==="rect"){const lons=[d.start.lon,end.lon];byId("target-lat_min").value=Math.min(d.start.lat,end.lat);byId("target-lat_max").value=Math.max(d.start.lat,end.lat);byId("target-lon_min").value=Math.abs(lons[0]-lons[1])>180?Math.max(...lons):Math.min(...lons);byId("target-lon_max").value=Math.abs(lons[0]-lons[1])>180?Math.min(...lons):Math.max(...lons);}
  else {const rad=Math.PI/180,a=d.start.lat*rad,b=end.lat*rad,h=Math.sin((b-a)/2)**2+Math.cos(a)*Math.cos(b)*Math.sin((end.lon-d.start.lon)*rad/2)**2;byId("target-center_lat").value=d.start.lat;byId("target-center_lon").value=d.start.lon;byId("target-radius_km").value=Math.max(1,6371.0088*2*Math.asin(Math.min(1,Math.sqrt(h))));}
  byId("settingsDialog").showModal();});
window.addEventListener("keydown",e=>{if(e.key==="Escape"&&nativeUI.drawing){nativeUI.drawing=null;byId("settingsDialog").showModal();}});

const metricLabels={attempted:"발생",sent:"송신",received:"수신",dropped:"폐기",inFlight:"전송 중",deliveryRate:"전달률 (%)",meanDelayMs:"평균 지연 (ms)",throughputMbps:"처리량 (Mbps)",replanned:"재계산",reservedSwitches:"예비 경로 전환",peakMemoryBytes:"최대 메모리 (B)",elapsedSeconds:"계산 시간 (s)"};
function metricText(v,d=2){return v===null||v===undefined||!Number.isFinite(Number(v))?"—":Number(v).toLocaleString("ko-KR",{maximumFractionDigits:d});}
function measured(s,key){return ["meanDelayMs","delayStdMs","maxDelayMs","jitterMs","meanHops"].includes(key)&&!s.received?null:s[key];}
function showMetrics(result){
  nativeUI.result=result;const raw=result?.summary,s=raw?offered(raw):null;byId("nativeMetrics").hidden=!s;if(!s)return;
  const grid=byId("metricValues");grid.replaceChildren();
  for(const key of ["attempted","received","dropped","inFlight","deliveryRate","meanDelayMs","throughputMbps","maxQueuePackets"]){const e=node("div");e.append(node("small",{},metricLabels[key]||"최대 대기 (패킷)"),node("strong",{},metricText(measured(s,key))));grid.append(e);}
  const body=byId("flowMetrics").tBodies[0];body.replaceChildren();byId("metricFlow").replaceChildren(new Option("전체 흐름","all"));
  for(const original of s.flows){const f=offered(original);const row=body.insertRow();for(const text of [`${f.source} → ${f.destination}`,`${metricText(f.received,0)} / ${metricText(f.attempted,0)}`,`${metricText(f.deliveryRate)}%`,`${metricText(measured(f,"meanDelayMs"))} ms`,`${metricText(measured(f,"jitterMs"))} ms`,metricText(measured(f,"meanHops"))])row.insertCell().textContent=text;byId("metricFlow").add(new Option(`${f.source} → ${f.destination}`,`${f.source}:${f.destination}`));}
  showResearchMetrics(s);drawMetrics();
}
function drawMetrics(){
  const result=nativeUI.result;if(!result?.summary)return;
  const chart=byId("qosChart"),c=chart.getContext("2d"),w=chart.width,h=chart.height,type=value("metricSeries"),flow=value("metricFlow"),groups=new Map();
  for(const r of result.series||[]){if(flow!=="all"&&flow!==`${r[1]}:${r[2]}`)continue;let g=groups.get(r[0]);if(!g){g={time:r[0],v:0,rx:0};groups.set(r[0],g);}if(type==="delay"){g.v+=r[7]*r[4];g.rx+=r[4];}else g.v+=r[type==="throughput"?6:type==="dropped"?5:8];}
  let points=[...groups.values()].map(g=>[g.time,type==="delay"?(g.rx?g.v/g.rx:null):g.v]);if(type.startsWith("control")){points=[];let previous=null;for(const row of result.controlSeries||[]){const elapsed=row[0]-(previous?.[0]||0),index=type==="controlBytes"?5:4,delta=row[index]-(previous?.[index]||0);points.push([row[0],type==="controlBytes"?delta*8/elapsed/1e6:delta]);previous=row;}}
  c.clearRect(0,0,w,h);c.font="20px sans-serif";c.fillStyle="#aebed2";
  const valid=points.filter(p=>p[1]!==null),max=Math.max(1,...valid.map(p=>p[1])),end=result.config.simTimeLimit,left=70,top=20,bottom=h-40;
  c.strokeStyle="#344458";c.beginPath();c.moveTo(left,top);c.lineTo(left,bottom);c.lineTo(w-15,bottom);c.stroke();c.fillText(metricText(max),4,top+16);c.fillText("0",left-22,bottom);c.fillText(`${metricText(end)} s`,w-100,h-6);
  c.strokeStyle="#57c9e8";c.lineWidth=2;c.beginPath();let pen=false;for(const [t,v]of points){if(v===null){pen=false;continue;}const x=left+t/end*(w-left-15),y=bottom-v/max*(bottom-top);if(pen)c.lineTo(x,y);else c.moveTo(x,y);pen=true;}c.stroke();
  byId("chartCaption").textContent=`${byId("metricSeries").selectedOptions[0].textContent} · ${byId("metricFlow").selectedOptions[0].textContent}`;
}
byId("metricSeries").onchange=drawMetrics;byId("metricFlow").onchange=drawMetrics;
window.addEventListener("sim-result",e=>showMetrics(e.detail));window.addEventListener("sim-clear",()=>showMetrics(null));window.addEventListener("sim-csv",e=>showMetrics(e.detail.matchesMap?resultMeta:null));
const comparisonInputs={};
function showComparison(a,b){
  const sa=offered(a.summary),sb=offered(b.summary);const ca=a.config,cb=b.config;
  const verified=a.manifest.format==="leo-native-replay-v4"&&b.manifest.format==="leo-native-replay-v4"&&sa.processingMode==="neutral"&&sb.processingMode==="neutral"&&sa.controlMode===sb.controlMode&&sa.jitterModel===sb.jitterModel&&a.manifest.physicalEventsHash===b.manifest.physicalEventsHash;
  const sameTraffic=sa.trafficHash===sb.trafficHash,ha=a.manifest.environmentHash,hb=b.manifest.environmentHash;
  byId("comparisonStatus").textContent=(verified&&ha&&ha===hb&&sameTraffic)?"동일한 환경·송신 조건입니다.":sameTraffic?"송신 조건은 동일합니다. 환경 설정 차이를 확인하세요.":"송신 조건이 다릅니다.";
  const content=byId("comparisonContent");content.replaceChildren();const table=node("table"),head=table.createTHead().insertRow();for(const t of ["지표",`${sa.algorithm} (기준)`,`${sb.algorithm} (비교)`,"차이"])head.append(node("th",{},t));
  const body=table.createTBody();for(const key of Object.keys(metricLabels)){const av=measured(sa,key),bv=measured(sb,key),row=body.insertRow();for(const text of [metricLabels[key],metricText(av),metricText(bv),av==null||bv==null?"—":metricText(bv-av)])row.insertCell().textContent=text;}
  content.append(table);
  const settings=node("details");settings.append(node("summary",{},"알고리즘 설정 차이"));for(const key of new Set([...Object.keys(a.manifest.algorithmSettings||{}),...Object.keys(b.manifest.algorithmSettings||{})])){const av=a.manifest.algorithmSettings?.[key],bv=b.manifest.algorithmSettings?.[key];if(av!==bv)settings.append(node("p",{class:"quiet"},`${key}: ${av??"—"} → ${bv??"—"}`));}content.append(settings);
  const capture=node("button",{type:"button"},"기준 경로를 혼잡 설정에 사용");capture.onclick=()=>{const route=a.routes[0];if(!route){byId("comparisonStatus").textContent="기준 결과에 경로가 없습니다.";return;}nativeUI.baseline={runId:a.runId,route:route[6],time:route[0]};byId("comparisonStatus").textContent=`기준 경로 ${route[6].join(" → ")} 선택됨`;render();};content.append(capture);
}
async function readComparisonFolder(files){
  return prepareBundle(await bundleFromFiles([...files]),msg=>byId("comparisonStatus").textContent=msg);
}
for(const [id,key]of [["baselineFolder","a"],["comparisonFolder","b"]])byId(id).onchange=async e=>{try{const result=await readComparisonFolder(e.target.files);if(!result.summary)throw Error("수신 통계가 포함된 실행 결과를 선택하세요.");comparisonInputs[key]=result;byId("comparisonStatus").textContent=`${key==="a"?"기준":"비교"} 실행: ${result.runId}`;if(comparisonInputs.a&&comparisonInputs.b)showComparison(comparisonInputs.a,comparisonInputs.b);}catch(err){byId("comparisonStatus").textContent=err.message;}};
byId("btnCompare").onclick=async()=>{
  byId("compareDialog").showModal();byId("comparisonStatus").textContent="";
  const online=location.protocol!=="file:";for(const id of ["baselineRun","scenarioRun","loadComparison"])byId(id).disabled=!online;if(!online)return;
  try{const response=await fetch("/api/runs");const list=await response.json();for(const id of ["baselineRun","scenarioRun"]){const select=byId(id);select.replaceChildren();for(const r of list)select.add(new Option(`${scenarioName(r.configName)} · ${algorithmName({algorithm:r.summary.algorithm})} · ${r.runId}`,r.runId));}if(nativeUI.result)byId("scenarioRun").value=nativeUI.result.runId;if(list.length>1)byId("baselineRun").selectedIndex=1;}catch(e){byId("comparisonStatus").textContent=e.message;}
};
byId("loadComparison").onclick=async()=>{try{const results=await Promise.all([value("baselineRun"),value("scenarioRun")].map(async id=>{const response=await fetch(`/api/result?runId=${encodeURIComponent(id)}&metadata=1`),result=await response.json();if(!response.ok||!result?.bundle)throw Error(result?.error||"실행 결과가 없습니다.");return prepareBundle(result.bundle);}));comparisonInputs.a=results[0];comparisonInputs.b=results[1];showComparison(...results);}catch(e){byId("comparisonStatus").textContent=e.message;}};
function drawScenarioOverlay(snapshot,time){
  if(!snapshot)return;const entries=resultMeta?.config?.scenario?.entries||nativeUI.config?.scenario?.entries||[];ctx.save();ctx.strokeStyle="#ffb65c";ctx.fillStyle="rgba(255,166,65,.13)";ctx.lineWidth=3;
  for(const e of entries){if(time<(e.start_s||0)||(e.end_s!=null&&time>=e.end_s))continue;const p=e.target?.params||{},kind=e.target?.kind;if(kind!=="region"&&!(kind==="zone"&&p.zone_kind==="latlon_box"))continue;
    if(p.shape==="circle"){const rad=p.radius_km/6371.0088,lat=p.center_lat*Math.PI/180,lon=p.center_lon*Math.PI/180;let previous=null;for(let i=0;i<=72;i++){const b=i/72*2*Math.PI,la=Math.asin(Math.sin(lat)*Math.cos(rad)+Math.cos(lat)*Math.sin(rad)*Math.cos(b)),lo=lon+Math.atan2(Math.sin(b)*Math.sin(rad)*Math.cos(lat),Math.cos(rad)-Math.sin(lat)*Math.sin(la)),point=[(lo*180/Math.PI+540)%360-180,la*180/Math.PI];if(previous)drawWrappedLine(...previous,...point,"#ffb65c",3,.7);previous=point;}}
    else{const ranges=p.lon_min>p.lon_max?[[p.lon_min,180],[-180,p.lon_max]]:[[p.lon_min,p.lon_max]];for(const [a,b]of ranges){const x=lonToX(a),y=latToY(p.lat_max),w=lonToX(b)-x,h=latToY(p.lat_min)-y;ctx.fillRect(x,y,w,h);ctx.strokeRect(x,y,w,h);}}
  }
  const baseline=nativeUI.baseline||resultMeta?.config?.scenario?.baseline;if(baseline?.route)for(let i=1;i<baseline.route.length;i++){const a=snapshot.nodes[baseline.route[i-1]],b=snapshot.nodes[baseline.route[i]];if(a&&b)drawWrappedLine(a[1],a[0],b[1],b[0],"#6ef2bb",5,.9);}ctx.restore();
}
if(location.protocol==="file:")byId("btnSettings").hidden=true;
if(typeof resultMeta!=="undefined"&&resultMeta)showMetrics(resultMeta);

function readEventCsv(text){const rows=text.replace(/^\uFEFF/,"").trim().split(/\r?\n/);if(rows.shift()!=="time,event,satA,satB")throw Error("CSV 열: time,event,satA,satB");return rows.map(row=>{const[t,event,a,b]=row.split(",");return{time:Number(t),event:({fail:"failure",down:"failure",up:"restore"})[event]||event,a:Number(a),b:Number(b)};});}
function offered(s){if((s.metricVersion||0)>=3||!Number.isFinite(s.attempted))return s;return{...s,deliveryRate:s.attempted?100*s.received/s.attempted:0,dropped:s.dropped+(s.noUplink??s.attempted-s.sent)};}
function showResearchMetrics(s){
  let detail=byId("researchMetrics");if(detail)detail.remove();if((s.metricVersion||0)<3)return;
  detail=node("details",{id:"researchMetrics"});detail.append(node("summary",{},"제어 통신·연산·복구"));
  const grid=node("div",{class:"metric-grid"});for(const [name,v]of Object.entries({"제어 프레임":s.control?.hopAccepted,"제어 전송 (MB)":s.control?.wireBytes/1e6,"제어 폐기":s.control?.dropped,"간선 검사":s.operations?.edgeChecks,"라벨 확장":s.operations?.labelExpand,"재계산":s.replanned})){const e=node("div");e.append(node("small",{},name),node("strong",{},metricText(v)));grid.append(e);}detail.append(grid);
  if(s.control?.byKind){const table=node("table"),head=table.createTHead().insertRow();for(const text of ["메시지","생성","홉 전달","홉 실패","전송 프레임","손실 프레임","전송 MB"])head.append(node("th",{},text));const body=table.createTBody();for(const [kind,m]of Object.entries(s.control.byKind)){const row=body.insertRow();for(const v of [kind,m.generated||0,m.messageHopArrivals||0,(m.messageHopDrops||0)+(m.routeDrops||0),m.frameAttempts||0,m.frameDrops||0,(m.wireBytes||0)/1e6])row.insertCell().textContent=typeof v==="number"?metricText(v):v;}detail.append(table);}
  for(const [name,time]of Object.entries(s.computeMs||{}))detail.append(node("p",{class:"quiet"},`${name}: ${metricText(time)} ms`));
  for(const event of s.recovery||[]){detail.append(node("p",{},`${event.time}s · ${event.a}–${event.b} · ${event.restore?"재연결":"단절"}`));detail.append(node("p",{class:"quiet"},`감지 ${event.detectedSeconds==null?"미감지":metricText(event.detectedSeconds)+"s"} · 정보 수렴 ${event.convergenceSeconds==null?"미수렴":metricText(event.convergenceSeconds)+"s"} (${event.appliedNodes}/${event.expectedNodes})`));for(const f of event.flows)detail.append(node("p",{class:"quiet"},`${f.source}→${f.destination}: ${!f.affected?"영향 경로 없음":f.recoverySeconds==null?"관측 종료 · 미복구":metricText(f.recoverySeconds)+"s"}`));}
  byId("nativeMetrics").append(detail);
}
byId("btnExperiment").onclick=async()=>{try{const response=await fetch(`/api/config?config=${encodeURIComponent(cfgNameSel.value)}`),cfg=await response.json();if(!response.ok)throw Error(cfg.error);const area=byId("experimentAlgorithms");area.replaceChildren();for(const [id,label]of Object.entries(cfg.capabilities.algorithms)){const e=node("label",{class:"check-field"});e.append(node("input",{type:"checkbox",class:"experiment-algorithm",value:id,checked:true}),document.createTextNode(algorithmName({algorithm:id})));area.append(e);}byId("experimentConditions").textContent=`${scenarioName(cfg.activeConfig)} · ${cfg.simTimeLimit}s · 시드 ${cfg.seed}`;byId("experimentError").textContent="";byId("experimentDialog").showModal();}catch(e){setStatus(e.message);}};
byId("experimentForm").onsubmit=e=>{e.preventDefault();const selected=[...document.querySelectorAll(".experiment-algorithm:checked")].map(e=>e.value);if(!selected.length){byId("experimentError").textContent="알고리즘을 선택하세요.";return;}byId("experimentDialog").close();runNative(selected);};
if(location.protocol==="file:")byId("btnExperiment").hidden=true;
