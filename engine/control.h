// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Explicit experimental wire protocol; no estimated-time delivery shortcuts.
struct LinkReport { int a,b; bool up=true; double extra=0,A=1,J=0; };
struct Advertisement { int origin; unsigned long long version;double observedAt=0; std::vector<LinkReport> links; };
struct ControlMessage {
  std::string kind; int origin=0,target=-1,region=-1,ds=-1; unsigned long long nonce=0,round=0;
  std::vector<std::shared_ptr<const Advertisement>> records;
  int payloadBytes=32;double dataTime=0;
};
class ControlPlane {
public:
  using Msg=std::shared_ptr<const ControlMessage>;
  struct View { mcsr::FailureSet down;std::map<int,std::shared_ptr<const Advertisement>> info; unsigned long long revision=0,topologyRevision=0; };
  std::vector<View> views;
  long long generated=0,hopAccepted=0,arrived=0,dropped=0,wireBytes=0; double busySeconds=0;
  std::map<std::string,long long> generatedByKind,droppedByReason;
private:
  int n;J cfg; unsigned long long nonce=0,roundId=0;
  std::vector<std::vector<int>> neighbors,members;
  std::vector<int> region,ds,knownOwner;std::vector<unsigned long long> ownerRevision;
  std::vector<std::map<int,double>> heard;
  std::vector<unsigned long long> versions;std::vector<bool> statusDirty,metricDirty;double lastMonitor=0;
  std::vector<std::shared_ptr<const Advertisement>> local,initial;
  std::vector<std::map<std::pair<std::string,int>,unsigned long long>> seen;
  struct Collection { unsigned long long round=0; bool vote=false;std::set<int> replies; };
  std::vector<Collection> collecting;
  std::function<double()> now;
  std::function<void(double,std::function<void()>)> schedule;
  std::function<void(int,int,Msg,int)> send;
  std::function<std::vector<LinkReport>(int)> sample;
  std::function<void(J)> record;
  std::function<int(int,int)> elect;
  static unsigned long long mix(unsigned long long x){x+=0x9e3779b97f4a7c15ull;x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;x=(x^(x>>27))*0x94d049bb133111ebull;return x^(x>>31);}
  static unsigned long long content(const Advertisement &a) {
    unsigned long long h=0;for(auto &r:a.links){h^=mix((static_cast<unsigned long long>(r.a)<<32)|unsigned(r.b));h=mix(h^unsigned(r.up));for(auto v:{r.extra,r.A,r.J})h=mix(h^std::hash<double>{}(v));}return h;
  }
  void apply(int node,const std::vector<std::shared_ptr<const Advertisement>>& records) {
    auto &v=views[node];bool changed=false;
    for(auto &a:records){auto at=v.info.find(a->origin);if(at!=v.info.end()&&at->second->version>=a->version)continue;
      auto previous=at==v.info.end()?initial[a->origin]:at->second;
      auto oldContent=content(*previous),newContent=content(*a);if(oldContent!=newContent)v.revision^=mix(oldContent)^mix(newContent);
      v.info[a->origin]=a;for(auto &r:a->links){auto key=mcsr::linkKey(r.a,r.b);bool before=v.down.links.count(key),after=!r.up||!perceived(node,r.b,r.a).up;if(after)v.down.links.insert(key);else v.down.links.erase(key);if(before!=after)v.topologyRevision^=mix(key);}
      changed=true;}
    if(changed)record({{"event","knowledge-applied"},{"node",node},{"revision",v.revision}});
  }
  Msg make(std::string kind,int origin,int target,int scope,std::vector<std::shared_ptr<const Advertisement>> records={},unsigned long long round=0,int designated=-1) {
    auto m=std::make_shared<ControlMessage>();m->kind=kind;m->origin=origin;m->target=target;m->region=scope;m->nonce=++nonce;m->round=round;m->ds=designated;m->records=std::move(records);m->dataTime=now();
    long long bytes=cfg.value("controlHeaderBytes",32);for(auto &a:m->records)bytes+=a->links.size()*cfg.value("controlRecordBytes",48);
    if(kind=="csib")bytes=std::max<long long>(bytes,cfg.value("csibMessageBytes",0));
    if(bytes>2147483623)throw std::runtime_error("Control message exceeds wire length limit");m->payloadBytes=int(bytes);generated++;generatedByKind[kind]++;return m;
  }
  void emit(Msg m){receive(m->origin,-1,m,n);}
  void forward(int node,int from,Msg m,int ttl) {
    if(ttl<=0)return;
    for(int v:neighbors[node])if(v!=from&&(m->region<0||region[v]==m->region))send(node,v,m,ttl-1);
  }
  std::vector<std::shared_ptr<const Advertisement>> recordsAt(int node,bool regional=false) {
    std::vector<std::shared_ptr<const Advertisement>> out;for(auto &[origin,a]:views[node].info)if(!regional||region[origin]==region[node])out.push_back(a);return out;
  }
  std::shared_ptr<const Advertisement> observe(int u,bool metrics=false) {
    auto rows=sample(u);for(auto &r:rows)r.up=now()-heard[u][r.b] < cfg.value("slsInterval",.2)*cfg.value("slsMissCount",3)-1e-10;
    auto old=local[u];if(old&&!metrics)for(size_t i=0;i<rows.size();i++){rows[i].extra=old->links[i].extra;rows[i].A=old->links[i].A;rows[i].J=old->links[i].J;}bool changed=!old||old->links.size()!=rows.size();
    if(old&&!changed)for(size_t i=0;i<rows.size();i++){auto &a=rows[i];auto &b=old->links[i];if(a.up!=b.up||std::abs(a.extra-b.extra)>1e-6||std::abs(a.A-b.A)>.001||std::abs(a.J-b.J)>.001){changed=true;break;}}
    if(!changed)return old;
    if(old)for(size_t i=0;i<rows.size();i++)if(rows[i].up!=old->links[i].up){statusDirty[u]=true;record({{"event","link-detected"},{"a",u},{"b",rows[i].b},{"up",rows[i].up}});}
    metricDirty[u]=metricDirty[u]||metrics;
    auto a=std::make_shared<Advertisement>();a->origin=u;a->version=++versions[u];a->observedAt=now();a->links=std::move(rows);local[u]=a;apply(u,{a});return a;
  }
  bool hierarchical()const{return cfg.value("csibEnabled",false)&&cfg.value("algorithm",std::string()).find("mcsr-")==0;}
  void announce(int u,unsigned long long round=0,bool voting=false) {
    auto observed=observe(u,round>0);auto fresh=std::make_shared<Advertisement>(*observed);fresh->version=++versions[u];fresh->observedAt=now();local[u]=fresh;apply(u,{fresh});auto a=local[u];emit(make(hierarchical()?(voting?"vote":"report"):"lsa",u,hierarchical()?knownOwner[u]:-1,hierarchical()?region[u]:-1,{a},round));
  }
  void begin(int k,bool vote=false) {
    if(collecting[k].round){collecting[k].vote|=vote;return;}
    auto id=++roundId;collecting[k]={id,vote,{}};
    emit(make(vote?"vote-request":"request",ds[k],-1,k,{},id));
    schedule(now()+cfg.value("controlTimeout",3.),[this,k,id]{if(collecting[k].round==id)finish(k);});
  }
  void distribute(int k) {
    int owner=ds[k];auto records=recordsAt(owner);
    record({{"event","csib-generated"},{"srd",k},{"ds",owner},{"version",roundId},{"dataTime",now()},{"computeDone",now()}});
    emit(make("csib",owner,-1,k,records,roundId,owner));
    emit(make("summary",owner,-1,-1,recordsAt(owner,true)));
  }
  void finish(int k) {
    auto c=collecting[k];if(!c.round)return;collecting[k]={};int owner=ds[k];
    if(c.replies.size()!=members[k].size())record({{"event","control-timeout"},{"srd",k},{"received",c.replies.size()},{"expected",members[k].size()}});
    int winner=owner;
    if(c.vote&&c.replies.size()==members[k].size())winner=elect(k,owner);
    if(winner>=0&&winner!=owner){knownOwner[owner]=winner;ownerRevision[owner]=nonce+1;emit(make("handoff",owner,winner,k,recordsAt(owner),c.round,winner));}
    else {double delay=cfg.value("processingMode",std::string("neutral"))=="assumed"?cfg.value("csibComputeDelay",0.):0;
      if(delay>0)schedule(now()+delay,[this,k]{distribute(k);});else distribute(k);}
  }
  void heartbeat() {
    for(int u=0;u<n;u++) {
      auto m=make("hello",u,-1,region[u]);for(int v:neighbors[u])send(u,v,m,0);
      auto old=local[u];auto a=observe(u);
      if(old!=a)announce(u);
    }
    schedule(now()+cfg.value("slsInterval",.2),[this]{heartbeat();});
  }
  void refresh() {
    double monitor=cfg.value("csibMonitorInterval",0.);bool metrics=!hierarchical()||(monitor>0&&now()-lastMonitor>=monitor-1e-10);
    if(metrics)lastMonitor=now();
    for(int u=0;u<n;u++) {auto old=local[u];auto a=observe(u,metrics);if(statusDirty[u]||(metrics&&(metricDirty[u]||old!=a)))announce(u);}
    schedule(now()+cfg.value("controlRefresh",5.),[this]{refresh();});
  }
  void periodic() {
    if(hierarchical())for(int k=0;k<int(members.size());k++)begin(k,cfg.value("csibVote",true));
    schedule(now()+cfg.value("csibPeriod",180.),[this]{periodic();});
  }
public:
  ControlPlane(J config,int nodes,std::vector<std::pair<int,int>> edges,std::vector<int> partition,std::vector<int> owners,
    std::function<double()> clock,std::function<void(double,std::function<void()>)> timer,
    std::function<void(int,int,Msg,int)> tx,std::function<std::vector<LinkReport>(int)> measure,
    std::function<void(J)> event,std::function<int(int,int)> vote)
    :views(nodes),n(nodes),cfg(std::move(config)),neighbors(n),region(std::move(partition)),ds(std::move(owners)),heard(n),versions(n),statusDirty(n,false),metricDirty(n,false),local(n),initial(n),seen(n),now(clock),schedule(timer),send(tx),sample(measure),record(event),elect(vote) {
    if(region.empty()){region.assign(n,0);ds={0};}
    members.resize(ds.size());collecting.resize(ds.size());for(int u=0;u<n;u++)members.at(region[u]).push_back(u);for(size_t k=0;k<ds.size();k++)if(ds[k]<0)ds[k]=members[k].front();knownOwner.resize(n);ownerRevision.assign(n,0);for(int u=0;u<n;u++)knownOwner[u]=ds[region[u]];
    for(auto [a,b]:edges){neighbors[a].push_back(b);neighbors[b].push_back(a);heard[a][b]=heard[b][a]=0;}
    for(int u=0;u<n;u++) {std::sort(neighbors[u].begin(),neighbors[u].end());neighbors[u].erase(std::unique(neighbors[u].begin(),neighbors[u].end()),neighbors[u].end());auto a=std::make_shared<Advertisement>();a->origin=u;a->version=1;versions[u]=1;a->links=sample(u);local[u]=a;initial[u]=a;}
    // The preinstalled snapshot is immutable; later information only arrives by wire.
    schedule(cfg.value("slsInterval",.2),[this]{heartbeat();});schedule(cfg.value("controlRefresh",5.),[this]{refresh();});
    if(hierarchical())schedule(cfg.value("csibPeriod",180.),[this]{periodic();});
  }
  bool matches(int node,int a,int b,bool up,double time)const {
    for(auto [u,v]:{std::make_pair(a,b),std::make_pair(b,a)}){auto at=views[node].info.find(u);if(at==views[node].info.end()||at->second->observedAt<time)return false;bool found=false;for(auto &r:at->second->links)if(r.b==v){if(r.up!=up)return false;found=true;}if(!found)return false;}return true;
  }
  unsigned long long revision(int node)const{return node>=0&&node<n?views[node].revision:0;}
  LinkReport perceived(int viewer,int a,int b)const {
    LinkReport row{a,b};if(viewer<0||viewer>=n)return row;
    auto at=views[viewer].info.find(a);auto known=at==views[viewer].info.end()?initial[a]:at->second;
    for(auto r:known->links)if(r.b==b)return r;return row;
  }
  void receive(int node,int from,Msg m,int ttl) {
    if(m->kind=="hello"){if(from>=0)heard[node][from]=now();return;}
    auto key=std::make_pair(m->kind,m->origin);auto &last=seen[node][key];if(last>=m->nonce)return;last=m->nonce;
    int k=region[node];
    if(m->kind=="lsa")apply(node,m->records);
    else if(m->kind=="report"||m->kind=="vote") {
      if(!hierarchical())return;
      apply(node,m->records);
      if(node==m->target&&node!=ds[k]&&knownOwner[node]!=node){auto redirected=make(m->kind,m->origin,knownOwner[node],k,m->records,m->round);receive(node,-1,redirected,n);return;}
      if(node==m->target&&node==ds[k]) {
        if(!collecting[k].round&&m->round==0){begin(k);}
        if(m->round==collecting[k].round)collecting[k].replies.insert(m->origin);
        if(collecting[k].replies.size()==members[k].size())finish(k);
      }
    } else if(m->kind=="request"||m->kind=="vote-request"){bool boundary=std::any_of(neighbors[node].begin(),neighbors[node].end(),[&](int v){return region[v]!=region[node];});announce(node,m->round,m->kind=="vote-request"&&boundary);}
    else if(m->kind=="summary") {
      if(node==ds[k]&&k!=region[m->origin]){apply(node,m->records);emit(make("csib",node,-1,k,recordsAt(node),m->nonce,node));}
    } else if(m->kind=="csib") {
      if(m->nonce>ownerRevision[node]){knownOwner[node]=m->ds;ownerRevision[node]=m->nonce;}
      apply(node,m->records);record({{"event","csib-applied"},{"node",node},{"ds",m->ds},{"version",m->round},{"dataTime",m->dataTime}});
    } else if(m->kind=="handoff"&&node==m->target) {apply(node,m->records);ds[k]=node;knownOwner[node]=node;distribute(k);}
    forward(node,from,m,ttl);
  }
};
