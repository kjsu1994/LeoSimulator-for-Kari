// SPDX-License-Identifier: GPL-3.0-only
// Native execution adapter, 2026-11-17. Vendored routing cores remain unchanged.
#include "geometry.h"
#include "vendor/routing/mcsr/planner.h"
#include "vendor/routing/congestion/zones.h"
#include <deque>
#include <filesystem>
#include <unordered_map>
#include <set>
#include <sstream>
#include <regex>
#include <chrono>
#include <array>
#include <optional>
#include "control.h"
double zoneCoefficient(const leocongestion::Zone &z,int id,double lat,double lon) {
  if(z.shape!=leocongestion::Zone::Box||std::abs(z.lonMax-z.lonMin)<360.-1e-9)return leocongestion::coefficient(z,id,lat,lon);
  if(lat<z.latMin||lat>z.latMax)return 0;
  double half=(z.latMax-z.latMin)/2,centre=(z.latMax+z.latMin)/2;
  double r=std::max(half>0?std::abs(lat-centre)/half:0.,std::abs(leocongestion::normLon(lon))/180.);
  return 1-(1-z.edgeFraction)*std::min(1.,r);
}
leocongestion::LinkLoadMap zoneLoads(const std::vector<leocongestion::Zone> &zones,double time,const std::vector<std::pair<int,int>> &links,const leocongestion::PositionFn &position,const leocongestion::QueueModel &q) {
  using namespace leocongestion;std::vector<Zone> ordinary,world;
  for(auto &z:zones)(z.shape==Zone::Box&&std::abs(z.lonMax-z.lonMin)>=360.-1e-9?world:ordinary).push_back(z);
  auto out=computeLinkLoads(ordinary,time,links,position,q);
  for(auto &z:world)if(z.activeAt(time)&&z.rho>0){std::map<int,double> factors;auto factor=[&](int id){if(!factors.count(id)){double lat=0,lon=0;factors[id]=position(id,lat,lon)?zoneCoefficient(z,id,lat,lon):0;}return factors[id];};
    for(auto [a,b]:links)for(auto [u,v]:{std::make_pair(a,b),std::make_pair(b,a)}){double cu=factor(u),cv=factor(v),k=z.dir==Direction::Out?cu:z.dir==Direction::In?cv:z.rule==LinkRule::Any?std::max(cu,cv):std::min(cu,cv),rho=z.rho*k;if(rho<=0)continue;
      auto &l=out[{u,v}];l.rho=std::max(l.rho,rho);l.extraDelay=std::max(l.extraDelay,q.delay(rho)+z.penaltyDelayMs/1000.);l.jitter=std::max(l.jitter,std::hypot(q.jitter(rho),z.penaltyJitterMs/1000.));}}
  return out;
}
constexpr char ENGINE_VERSION[] = "1.0.0";
constexpr char MODEL_VERSION[] = "leo-research-1.0.0";
using Tick = long long;
constexpr double TICKS = 1e11;
Tick ticks(double seconds) { return llround(seconds*TICKS); }
struct Plan {
  std::vector<int> path;
  double delay=INFINITY, dataTime=0;
  bool reserved=false;
  std::shared_ptr<const std::vector<mcsr::DomPath>> front;
};
struct Packet {
  long long id=0; int source=0,dest=0,hops=0,current=0; Tick created=0;
  int bytes=0; Plan plan; size_t cursor=0; double plannedAt=0;
  mcsr::FailureSet excluded;
  ControlPlane::Msg control;std::shared_ptr<ControlAssembly> assembly;int fragment=0,ttl=0;Tick enqueued=0;unsigned long long pass=0;std::vector<std::pair<unsigned long long,unsigned>> visits;
};
struct Context {
  J c; int n,g; double now=0; long epoch=0,islEpoch=0,congestionEpoch=0;
  mcsr::OpCounts operations;
  std::map<std::string,double> computeMs;std::map<std::string,long long> computations;
  std::function<unsigned long long(int)> revision,topologyRevision;
  std::function<mcsr::Graph(int)> graph;
  std::function<std::vector<mcsr::Planner::Exit>(int)> exits;
  std::function<std::vector<std::pair<int,double>>(int)> uplinks;
  std::function<mcsr::FailureSet(int)> failures;
  std::function<void(J)> record;
};
struct ComputeTimer {
  Context &c;std::string kind;std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
  ComputeTimer(Context &ctx,std::string name):c(ctx),kind(std::move(name)){c.computations[kind]++;}
  ~ComputeTimer(){c.computeMs[kind]+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}
};
class Routing {
public:
  virtual ~Routing()=default;
  virtual Plan forward(Packet &p,int node,bool repair=false)=0;
  virtual int selectUplink(int source,int dest)=0;
  virtual bool sourceRouted() const {return false;}
};
class DijkstraRouting:public Routing {
  Context &x; bool hop; long stamp=-1;
  std::map<std::pair<int,unsigned long long>,Tree> cache;
  Tree &tree(int dest,int viewer,const mcsr::FailureSet &extra=mcsr::FailureSet{}) {
    if(stamp!=x.epoch) { cache.clear();stamp=x.epoch; }
    auto failures=x.failures(viewer); failures.links.insert(extra.links.begin(),extra.links.end()); unsigned long long hash=0;
    for(auto k:failures.links) hash^=k*11400714819323198485ull;
    auto key=std::make_pair(dest,hash^(x.revision?x.revision(viewer):0));
    if(!cache.count(key)) {
      ComputeTimer measure(x,"dijkstra-tree");MCSR_OP(computations,1);MCSR_OP(planMiss,1);
      auto graph=x.graph(viewer);std::vector<std::vector<Edge>> a(x.n+1);
      for(int u=0;u<x.n;u++) for(auto e:graph.adj[u])if(!failures.blocksLink(u,e.to))a[e.to].push_back({u,e.t.D/1000});
      for(auto e:x.exits(dest)) { a[e.sat].push_back({x.n,e.gsl.D/1000});a[x.n].push_back({e.sat,e.gsl.D/1000}); }
      cache.emplace(key,dijkstra(a,x.n,hop));
    }
    else {MCSR_OP(planHit,1);}
    return cache.at(key);
  }
public:
  DijkstraRouting(Context &ctx):x(ctx),hop(x.c.value("algorithm",std::string())=="dijkstra-hop") {}
  Plan forward(Packet &p,int node,bool repair=false) override {
    auto &t=tree(p.dest,node,p.excluded);Plan out;
    if(!std::isfinite(t.cost[node]))return out;
    int v=node;for(int i=0;i<=x.n && v>=0 && v<x.n;i++){out.path.push_back(v);v=t.next[v];}
    if(v!=x.n) {out.path.clear();return out;}
    out.delay=t.delay[node];out.dataTime=x.now;return out;
  }
  int selectUplink(int src,int dst) override {
    int best=-1;double value=INFINITY;
    for(auto [sat,delay]:x.uplinks(src)) {
      auto &t=tree(dst,sat);double cost=(hop?1+1e-6*delay:delay)+t.cost[sat];
      if(cost<value || (cost==value && sat<best)){value=cost;best=sat;}
    }
    return best;
  }
};
class McsrRouting:public Routing {
  Context &x; mcsr::Service service; mcsr::SelectionParams selection;
  using Planner=mcsr::Planner;
  std::shared_ptr<Planner> current;
  int K=0,L=0;double sampled=-1;long epoch=-1,congestionEpoch=-1;
  std::map<std::tuple<const Planner*,int,int>,Plan> plans;
  long groundEpoch=-1;
  std::map<std::pair<unsigned long long,long long>,std::shared_ptr<Planner>> delivered;
  struct LocalView {std::weak_ptr<Planner> planner;unsigned long long topology=0;long geometry=-1;double sampled=-1;};
  std::map<int,LocalView> localViews;
  std::shared_ptr<Planner> build(int viewer=-2) {
    ComputeTimer measure(x,"mcsr-snapshot");MCSR_OP(computations,1);
    mcsr::PlannerConfig c;c.planes=x.c["planes"];c.satsPerPlane=x.n/c.planes;c.interPlaneSpacing=x.c["phasing"];
    c.labelCap=x.c.value("labelCap",32);c.fixedK=K;c.fixedL=L;
    if(x.c.value("floodModel",std::string("paper"))=="consistent")c.flood=mcsr::FloodModel::Consistent;
    auto p=std::shared_ptr<Planner>(Planner::create(c,x.graph(viewer)).release());
    if(!K){K=p->srdPlan().K;L=p->srdPlan().L;}
    sampled=x.now;epoch=x.c.value("snapshotOnDelayUpdate",true)?x.epoch:x.islEpoch;congestionEpoch=x.congestionEpoch;
    current=p;plans.clear();return p;
  }
  std::shared_ptr<Planner> view(int node) {
    if(x.c.value("controlMode",std::string("packet"))=="packet") {
      double refresh=x.c.value("refreshInterval",0.);auto topology=x.topologyRevision(node);
      auto &local=localViews[node];bool geometry=x.c.value("snapshotOnDelayUpdate",true)&&local.geometry!=x.epoch;
      if(local.topology==topology&&!geometry&&(refresh<=0||x.now-local.sampled<refresh-1e-10))if(auto p=local.planner.lock())return p;
      long long sample=x.c.value("snapshotOnDelayUpdate",true)?x.epoch:refresh>0?static_cast<long long>(x.now/refresh):0;
      auto key=std::make_pair(x.revision(node),sample);auto at=delivered.find(key);std::shared_ptr<Planner> p;
      if(at!=delivered.end())p=at->second;
      else {p=build(node);if(delivered.size()>=64){delivered.clear();plans.clear();}delivered[key]=p;}
      local={p,topology,x.epoch,x.now};return p;
    }
    long e=x.c.value("snapshotOnDelayUpdate",true)?x.epoch:x.islEpoch;
    double interval=x.c.value("refreshInterval",0.);
    if(!current || epoch!=e || congestionEpoch!=x.congestionEpoch || (interval>0 && x.now-sampled>=interval-1e-10))build();
    return current;
  }
public:
  McsrRouting(Context &ctx):x(ctx) {
    auto id=x.c.value("algorithm",std::string("mcsr-ls"));service=id=="mcsr-ts"?mcsr::Service::TS:id=="mcsr-hr"?mcsr::Service::HR:mcsr::Service::LS;
    selection.hrJitterMs=x.c.value("hrJitterMs",15.);double bound=x.c.value("tsDelayBoundMs",-1.);if(bound>=0)selection.tsDelayBoundMs=bound;
    K=x.c.contains("commonSrd")?x.c["commonSrd"]["K"].get<int>():x.c.value("srdK",0);L=x.c.contains("commonSrd")?x.c["commonSrd"]["L"].get<int>():x.c.value("srdL",0);auto p=build();
    if(x.c.contains("commonSrd")&&p->srdPlan().srdOf!=x.c["commonSrd"]["srdOf"].get<std::vector<int>>())throw std::runtime_error("Routing SRD differs from common mapping");
    x.record({{"event","srd"},{"K",K},{"L",L},{"srdOf",p->srdPlan().srdOf}});
  }
  bool sourceRouted() const override {return true;}
  Plan forward(Packet &packet,int node,bool repair=false) override {
    ComputeTimer measure(x,repair?"mcsr-repair":"mcsr-plan");
    auto p=view(node);auto f=x.failures(node);
    f.links.insert(packet.excluded.links.begin(),packet.excluded.links.end());
    if(repair && x.c.value("reservedPaths",false) && packet.plan.front) {
      std::vector<mcsr::DomPath> candidates;
      for(auto path:*packet.plan.front) {
        auto at=std::find(path.sats.begin(),path.sats.end(),node);
        if(at==path.sats.end())continue;
        path.sats.erase(path.sats.begin(),at);if(f.blocksPath(path.sats))continue;
        auto exits=x.exits(packet.dest);auto e=std::find_if(exits.begin(),exits.end(),[&](auto e){return e.sat==path.sats.back();});if(e==exits.end())continue;
        auto tuple=e->gsl;bool valid=true;
        for(size_t i=1;i<path.sats.size();i++) {
          auto &edges=p->graph().adj[path.sats[i-1]];auto edge=std::find_if(edges.begin(),edges.end(),[&](auto e){return e.to==path.sats[i];});
          if(edge==edges.end()){valid=false;break;}tuple=mcsr::extend(tuple,edge->t);
        }
        if(valid){path.t=tuple;candidates.push_back(std::move(path));}
      }
      if(auto best=mcsr::selectPath(candidates,service,selection)){MCSR_OP(reserveSwitches,1);return {best->sats,best->t.D/1000,sampled,true,packet.plan.front};}
    }
    if(groundEpoch!=x.epoch){plans.clear();groundEpoch=x.epoch;}
    auto key=std::make_tuple(p.get(),node,packet.dest);
    if(!repair && f.empty() && plans.count(key)){MCSR_OP(planHit,1);return plans.at(key);}
    MCSR_OP(planMiss,1);if(repair)MCSR_OP(recomputes,1);
    auto result=p->plan(node,x.exits(packet.dest),service,&f,selection);Plan out;
    if(result.ok){out.path=result.path.sats;out.delay=result.path.t.D/1000;out.front=result.front;out.dataTime=x.now;}
    if(!repair && f.empty())plans[key]=out;return out;
  }
  int selectUplink(int src,int dst) override {
    ComputeTimer measure(x,"mcsr-access");
    int best=-1;double cost=INFINITY;
    std::optional<std::vector<Planner::Exit>> exits;
    for(auto [sat,up]:x.uplinks(src)) {
      auto planner=view(sat);
      if(groundEpoch!=x.epoch){plans.clear();groundEpoch=x.epoch;}
      // Access selection is delay-based for all service classes, as in the reference module.
      auto key=std::make_tuple(planner.get(),sat,-dst-1);Plan plan;
      auto cached=plans.find(key);
      if(cached!=plans.end())plan=cached->second;
      else {if(!exits)exits=x.exits(dst);auto f=x.failures(sat);auto r=planner->plan(sat,*exits,mcsr::Service::LS,&f);if(r.ok)plan={r.path.sats,r.path.t.D/1000,sampled,false,r.front};plans[key]=plan;}
      double value=up+plan.delay;if(value<cost || (value==cost && sat<best)){cost=value;best=sat;}
    }
    return best;
  }
};
using RoutingFactory=std::function<std::unique_ptr<Routing>(Context&)>;
std::map<std::string,RoutingFactory> routingFactories() {
  auto dj=[](Context &x){return std::make_unique<DijkstraRouting>(x);};auto mc=[](Context &x){return std::make_unique<McsrRouting>(x);};
  return {{"dijkstra-delay",dj},{"dijkstra-hop",dj},{"mcsr-ls",mc},{"mcsr-ts",mc},{"mcsr-hr",mc}};
}

struct FlowStats {
  long long sourceDropped=0;
  long long attempted=0,sent=0,received=0,dropped=0,bytes=0,queued=0;
  double delaySum=0,mean=0,m2=0,maxDelay=0,jitter=0,lastTransit=-1,lastRx=-1,maxGap=0;
  long long hops=0,outages=0,lastSent=0,lastReceived=0,lastDropped=0,lastBytes=0,lastSourceDropped=0;
  double lastDelaySum=0;
};
struct Link {
  int a=0,b=0; bool active=false,nominal=false,transmitting=false;
  double prop=0,previousProp=0,rate=0,extra=0,extraJ=0,rho=0,availability=1,loss=0,capacityScale=1;
  double lastArrival=-1,lastInterval=-1,jitter=0,jitterAt=-1;
  unsigned long long generation=0;std::shared_ptr<Packet> current;Tick txStarted=0;double wireDuration=0;
  double lastTransit=-1;
  std::deque<std::shared_ptr<Packet>> queue;
};
struct Failure {int count=0;double since=0;std::vector<double> known;};
enum class EventKind { Other, Geometry, Control, Data, Sample };
struct Event {
  Tick time;long long seq;std::function<void()> action;EventKind kind=EventKind::Other;
  bool operator<(const Event &b)const{return time==b.time?seq>b.seq:time>b.time;}
};
class Simulation {
#ifdef LEO_KERNEL_TEST
public:
#endif
  J c;std::string out;int n,g;Tick now=0,limit;long long sequence=0,packetId=0,routeId=0,noUplink=0,maxQueue=0,replanned=0,reserved=0;
  unsigned long long trafficHash=14695981039346656037ull;
  const int queueCapacity,controlPayloadBytes;
  const bool packetTrace,packetJitter,assumedProcessing;
  const double controlReassemblyTimeout,reportProcessingDelay,lsaProcessingDelay,controlProcessingDelay;
  double gap=0;std::vector<Event> events;
  std::mt19937 trafficRng,failureRng;
  std::vector<std::unique_ptr<cOrbitA>> orbits;std::vector<cEci> sat;std::vector<Position> ground;
  std::vector<std::unordered_map<int,Link>> links;
  std::vector<std::pair<int,int>> nominalIsls;
  std::map<mcsr::LinkKey,Failure> failures;
  std::map<std::pair<int,int>,FlowStats> flows,cohort;
  Tick measurementStart=0,measurementEnd=0;bool measuring=false;
  std::map<std::pair<int,int>,std::vector<int>> lastPaths;
  std::vector<int> srdOf;int initialK=0,initialL=0;
  std::map<std::string,std::set<unsigned long long>> srdTargets;
  std::set<mcsr::LinkKey> syntheticFailures;
  double congestionSampled=-1;
  std::vector<leocongestion::Zone> zones;
  std::ofstream csv,trace,network,series,failureLog,controlSeries;
  Context context;
  std::unique_ptr<Routing> router;
  std::unique_ptr<ControlPlane> control;
  std::vector<J> physicalEvents;
  using FlowKey=std::pair<int,int>;
  struct RecoveryWindow {long long attempted=0,received=0;double lastReceipt=0;};
  struct RecoveryFlow {double first=INFINITY;std::map<Tick,RecoveryWindow> windows;};
  struct Recovery {
    double time;int a,b;bool up;long long lastPacketId;
    std::map<FlowKey,RecoveryFlow> flows;
    std::vector<double> applied;
    double detected=INFINITY,until=INFINITY;
  };
  std::map<mcsr::LinkKey,size_t> activeRecovery;
  std::map<mcsr::LinkKey,std::map<FlowKey,Tick>> lastLinkUse;
  bool trackRecovery=false;
  unsigned long long linkRandomDraws=0;
  std::vector<Recovery> recovery;
  double seconds()const{return now/TICKS;}
  double random(){return double(trafficRng())/4294967296.;}
  using ProfileClock = std::chrono::steady_clock;
  struct EventTiming { unsigned long long count=0, samples=0; double milliseconds=0; };
  static constexpr unsigned PROFILE_INTERVAL=256;
  bool profiling=false;
  unsigned long long processedEvents=0;
  std::array<EventTiming,5> eventTimings{};
  ProfileClock::time_point profileStart=ProfileClock::now(), profileWritten=profileStart;
  double queuePopMilliseconds=0, groundLinksMilliseconds=0;
  unsigned long long queuePopSamples=0, groundLinkUpdates=0;
  static double milliseconds(ProfileClock::duration duration) {
    return std::chrono::duration<double,std::milli>(duration).count();
  }
  void writeProfile(ProfileClock::time_point nowWall) {
    J kinds=J::object();
    const std::array<const char*,5> names={"other","geometry","control","data","sample"};
    for(size_t i=0;i<names.size();++i) {
      const auto &t=eventTimings[i];
      kinds[names[i]]={{"events",t.count},{"samples",t.samples},{"sampledMs",t.milliseconds},
                       {"estimatedMs",t.samples?t.milliseconds*t.count/t.samples:0}};
    }
    J data={{"simTime",seconds()},{"elapsedMs",milliseconds(nowWall-profileStart)},{"events",kinds},
            {"queuePopSamples",queuePopSamples},{"queuePopSampledMs",queuePopMilliseconds},
            {"queuePopEstimatedMs",queuePopSamples?queuePopMilliseconds*processedEvents/queuePopSamples:0},
            {"groundLinkUpdates",groundLinkUpdates},{"groundLinksMs",groundLinksMilliseconds},
            {"routingInclusiveMs",context.computeMs},{"routingCalls",context.computations},{"cpuThreads",1}};
    if(control)data["controlFrames"]={{"attempted",control->hopAccepted},{"arrived",control->arrived},
                                    {"lsaAttempts",control->byKindJson().value("lsa",J::object()).value("frameAttempts",0LL)}};
    std::ofstream(out+"/profile.tmp")<<data.dump(2);
    std::error_code error;
    std::filesystem::rename(out+"/profile.tmp",out+"/profile.json",error);
    if(error) {
      std::filesystem::remove(out+"/profile.json",error);
      std::filesystem::rename(out+"/profile.tmp",out+"/profile.json",error);
    }
    profileWritten=nowWall;
  }
  void schedule(double t,std::function<void()> fn,EventKind kind=EventKind::Other) {
    if(!std::isfinite(t)||t<seconds()-1e-10)throw std::runtime_error("Invalid event time");
    Tick tick=ticks(t);
    if(tick>=limit)return;
    events.push_back({tick,sequence++,std::move(fn),kind});
    std::push_heap(events.begin(),events.end());
  }
  void record(J e) {
    if(control && e.value("event",std::string())=="knowledge-applied") {int node=e["node"];for(auto &event:recovery)if(seconds()<=event.until&&!std::isfinite(event.applied[node])&&control->matches(node,event.a,event.b,event.up,event.time))event.applied[node]=seconds();}
    if(e.value("event",std::string())=="link-detected")for(auto &event:recovery)if(seconds()<=event.until&&e["up"]==event.up&&seconds()>=event.time&&((e["a"]==event.a&&e["b"]==event.b)||(e["b"]==event.a&&e["a"]==event.b)))event.detected=std::min(event.detected,seconds());
    if(e.value("event",std::string())=="knowledge-applied")return;
    e["time"]=seconds();network<<e.dump()<<'\n';
    if(e.value("event",std::string())=="srd"){srdOf=e["srdOf"].get<std::vector<int>>();initialK=e["K"];initialL=e["L"];}
  }
  Link *find(int a,int b) {
    if(a<0||a>=n+g)return nullptr;auto at=links[a].find(b);return at==links[a].end()?nullptr:&at->second;
  }
  double knownAt(const Failure &f,int viewer)const {
    if(viewer==-1)return f.since;
    if(viewer==-2)return f.since+c.value("slsInterval",.2)*c.value("slsMissCount",3);
    return f.known.empty()?f.since:f.known.at(viewer);
  }
  mcsr::FailureSet knownFailures(int viewer) {
    mcsr::FailureSet f;
    if(control&&viewer>=0)return control->views.at(viewer).down;
    for(auto &[key,state]:failures)if(state.count && seconds()+1e-10>=knownAt(state,viewer))f.links.insert(key);
    return f;
  }
  mcsr::Graph graph(int viewer) {
    mcsr::Graph result(n);auto f=knownFailures(viewer);
    bool mc=c["algorithm"].get<std::string>().find("mcsr")==0;
    for(int u=0;u<n;u++)for(auto &[v,l]:links[u])if(v<n && l.nominal && (l.active || failures.count(mcsr::linkKey(u,v))) && !f.blocksLink(u,v)) {
      if(control&&viewer>=0){auto r=control->perceived(viewer,u,v);if(!r.up)continue;result.addDirected(u,v,{1000*(l.prop+((mc||c.value("congestionAware",false))?r.extra:0.)),r.A,r.J});continue;}
      int cap=queueCapacity;if(cap==0)cap=c.value("routingQueueCapacity",100);
      double a=std::max(0.,1-double(l.queue.size())/cap);a=std::min(a,l.availability);
      double j=seconds()-l.jitterAt<=c.value("jitterWindow",30.)?l.jitter:0;
      if(c.value("jitterSource",std::string("arrival"))=="delayChange")j=std::abs(l.prop-l.previousProp);
      double d=l.prop+((mc||c.value("congestionAware",false))?(l.extra+(c.value("jitterModel",std::string("packet"))=="packet"?std::sqrt(3.)*l.extraJ:0.)):0.);
      result.addDirected(u,v,{1000*d,a,1000*(c.value("jitterModel",std::string("packet"))=="cost"?std::hypot(j,l.extraJ):j)});
    }
    return result;
  }
  std::vector<mcsr::Planner::Exit> exits(int dest) {
    std::vector<mcsr::Planner::Exit> result;
    for(auto &[id,l]:links[n+dest])if(l.active)result.push_back({id,{l.prop*1000,1.,0.}});
    std::sort(result.begin(),result.end(),[](auto a,auto b){return a.sat<b.sat;});return result;
  }
  std::vector<std::pair<int,double>> uplinks(int source) {
    std::vector<std::pair<int,double>> result;
    for(auto &[id,l]:links[n+source])if(l.active)result.push_back({id,l.prop});
    std::sort(result.begin(),result.end());return result;
  }
  void linkState(int a,int b,double delay,double rate,bool nominal=true) {
    auto &l=links[a][b];l.a=a;l.b=b;l.previousProp=l.prop;l.prop=delay;l.rate=rate*l.capacityScale;l.nominal=nominal;
    auto at=failures.find(mcsr::linkKey(a,b));l.active=nominal && (a>=n||b>=n||at==failures.end()||!at->second.count);
  }
  void update(bool reschedule=true) {
    double t=seconds();
    for(int i=0;i<n;i++)orbits[i]->getPosition((gap+t)/60,&sat[i]);
    for(int i=0;i<g;i++)ground[i]=station(c["stations"][i],t);
    for(int u=0;u<n+g;u++)for(auto &[v,l]:links[u])if(u>=n||v>=n){l.nominal=false;l.active=false;}
    int planes=c["planes"],per=n/planes,phase=c["phasing"];
    if(nominalIsls.empty())for(int i=0;i<n;i++)for(int k=0;k<2;k++) {
      if((k==0 && c.value("intraDisabled",false))||(k==1 && c.value("interDisabled",false)))continue;
      int q=i/per,j=i%per;int other=k==0?q*per+(j+1)%per:q==planes-1?(j+phase)%per:(q+1)*per+j;
      nominalIsls.push_back({i,other});
    }
    for(auto [a,b]:nominalIsls) {
      auto x=sat[a].getPos(),y=sat[b].getPos();double d=std::sqrt(sqr(x.m_x-y.m_x)+sqr(x.m_y-y.m_y)+sqr(x.m_z-y.m_z))*c.value("islDelay",.0000033356);
      linkState(a,b,d,c.value("islDatarate",1e8));linkState(b,a,d,c.value("islDatarate",1e8));
    }
    const auto groundStart=profiling?ProfileClock::now():ProfileClock::time_point{};
    std::vector<int> counts(n+g);
    for(int i=0;i<g;i++) {
      auto p=ground[i];cSite site(p.lat,p.lon,p.alt);
      auto date=sat[0].getDate();auto origin=site.getPosition(date).getPos();double theta=date.toLMST(site.getLon());
      double sx=std::cos(site.getLat())*std::cos(theta),sy=std::cos(site.getLat())*std::sin(theta),sz=std::sin(site.getLat()),threshold=std::sin(c["minimumElevation"].get<double>()*RADS_PER_DEG);
      for(int j=0;j<n;j++) {
        auto position=sat[j].getPos();double dx=position.m_x-origin.m_x,dy=position.m_y-origin.m_y,dz=position.m_z-origin.m_z;
        double range=std::sqrt(dx*dx+dy*dy+dz*dz);bool visible=(sx*dx+sy*dy+sz*dz)/range>=threshold;
        if(c.value("validateGeometry",false)&&j%31==0){auto reference=site.getLookAngle(sat[j]);if(visible!=(reference.m_El/RADS_PER_DEG>=c["minimumElevation"].get<double>())||std::abs(range-reference.m_Range)>1e-9)throw std::runtime_error("Cached geometry differs from original libnorad");}
        if(!visible)continue;
        if(++counts[n+i]>c.value("maxGroundLinks",40)||++counts[j]>c.value("maxGroundLinks",40))throw std::runtime_error("Ground link capacity exceeded");
        double d=range*c.value("groundlinkDelay",.0000033356);
        linkState(n+i,j,d,c.value("groundlinkDatarate",5e7));linkState(j,n+i,d,c.value("groundlinkDatarate",5e7));
      }
    }
    if(profiling){groundLinksMilliseconds+=milliseconds(ProfileClock::now()-groundStart);++groundLinkUpdates;}
    for(int u=0;u<n+g;u++)for(auto &[v,l]:links[u])if((u>=n||v>=n)&&!l.active){clearQueue(l,"link-down-in-queue");abort(l);}
    context.epoch++;context.now=t;applyCongestion(false);
    if(reschedule)schedule(t+c.value("updateInterval",1.),[this]{update();},EventKind::Geometry);
  }
  double coefficient(const J &target,int id) {
    auto kind=target.value("kind",std::string());auto p=target.value("params",J::object());
    if(kind=="satellite")return id==p.value("sat",-1)?1:0;
    if(kind=="zone") {
      auto k=p.value("zone_kind",std::string());int per=n/c["planes"].get<int>();
      if(k=="plane_range")return id/per>=p.value("plane_from",0)&&id/per<=p.value("plane_to",0)&&id%per>=p.value("sat_from",0)&&id%per<=p.value("sat_to",0)?1:0;
      if(k=="srd")return !srdOf.empty()&&srdOf[id]>=p.value("srd_from",0)&&srdOf[id]<=p.value("srd_to",0)?1:0;
    }
    leocongestion::Zone z;
    if(kind=="region"||kind=="zone") {
      auto pos=sat[id].toGeo();
      if(p.value("shape",std::string("rect"))=="circle") {z.shape=leocongestion::Zone::Circle;z.lat=p.value("center_lat",0.);z.lon=p.value("center_lon",0.);z.radiusKm=p.value("radius_km",1.);}
      else {z.shape=leocongestion::Zone::Box;z.latMin=p.value("lat_min",-90.);z.latMax=p.value("lat_max",90.);z.lonMin=p.value("lon_min",-180.);z.lonMax=p.value("lon_max",180.);}
      z.edgeFraction=p.value("profile",std::string("uniform"))=="hotspot"?p.value("edge_pct",20.)/100:1.;
      return zoneCoefficient(z,id,pos.m_Lat/RADS_PER_DEG,pos.m_Lon/RADS_PER_DEG);
    }
    return 0;
  }
  double factor(const J &target,int a,int b) {
    auto p=target.value("params",J::object());auto kind=target.value("kind",std::string());
    auto frozen=srdTargets.find(target.dump());if(frozen!=srdTargets.end())return frozen->second.count((static_cast<unsigned long long>(a)<<32)|unsigned(b))?1:0;
    if(kind=="link") {
      int u=p.value("sat_a",-1),v=p.value("sat_b",-1);
      return (a==u&&b==v)||(p.value("bidirectional",true)&&a==v&&b==u)?1:0;
    }
    if(kind=="path_hops") {
      auto path=c.value("scenario",J::object()).value("baseline",J::object()).value("route",std::vector<int>{});
      int lo=p.value("hop_from",1),hi=p.value("hop_to",1);
      for(int i=lo;i<=hi&&i<int(path.size());i++)if((a==path[i-1]&&b==path[i])||(b==path[i-1]&&a==path[i]))return 1;
      return 0;
    }
    double fa=coefficient(target,a),fb=coefficient(target,b);auto d=p.value("direction",std::string("both"));
    return d=="out"?fa:d=="in"?fb:p.value("link_rule",std::string("any"))=="both"?std::min(fa,fb):std::max(fa,fb);
  }
  void applyCongestion(bool force=true) {
    if(!force && congestionSampled>=0 && (c.value("congestionRefresh",5.)==0 || seconds()-congestionSampled+1e-10<c.value("congestionRefresh",5.)))return;
    congestionSampled=seconds();std::set<mcsr::LinkKey> newFailures;
    bool environment=c.value("congestionMode",std::string("actual"))=="environment";
    leocongestion::QueueModel q;q.packetBytes=c.value("congestionPacketBytes",1380);q.capacityPackets=c.value("congestionBuffer",500);q.islDatarate=c.value("islDatarate",1e8);
    auto position=[this](int id,double &lat,double &lon){auto p=sat[id].toGeo();lat=p.m_Lat/RADS_PER_DEG;lon=p.m_Lon/RADS_PER_DEG;return true;};
    auto loads=environment?zoneLoads(zones,seconds(),nominalIsls,position,q):leocongestion::LinkLoadMap{};
    auto entries=c.value("scenario",J::object()).value("entries",J::array());
    for(int a=0;a<n;a++)for(auto &[b,l]:links[a])if(b<n) {
      double rho=0,extra=0,jitter=0,available=1,scale=1,loss=0;bool fail=false;
      auto at=loads.find({a,b});if(at!=loads.end()){rho=at->second.rho;extra=at->second.extraDelay;jitter=at->second.jitter;available=1-rho;scale=std::max(.01,1-rho);}
      for(const auto &e:entries) {
        double start=e.value("start_s",0.);if(seconds()<start || (e.contains("end_s")&&!e["end_s"].is_null()&&seconds()>=e["end_s"].get<double>()))continue;
        double f=factor(e.at("target"),a,b);if(e.value("fail",false))f=std::max(f,factor(e.at("target"),b,a));if(f<=0)continue;
        if(e.value("fail",false)){fail=true;continue;}
        if(!environment)continue;
        double ramp=e.value("ramp_s",0.);double r=e.value("rho",.6)*f*(ramp>0?std::min(1.,(seconds()-start)/ramp):1.);
        auto pen=e.value("penalty",J::object());if(pen.is_null())pen=J::object();
        rho=std::max(rho,r);
        if(e.value("use_capacity",false))scale=std::min(scale,std::max(.01,1-r));
        if(e.value("use_buffer",false))available=std::min(available,1-r);
        double d=e.value("use_queue",false)?q.delay(r):0.,j=e.value("use_queue",false)?q.jitter(r):0.;
        extra=std::max(extra,d+pen.value("delay_ms",0.)/1000);jitter=std::max(jitter,std::hypot(j,pen.value("jitter_ms",0.)/1000));loss=std::max(loss,pen.value("loss_pct",0.)*f/100);
      }
      bool changed=std::abs(rho-l.rho)>1e-10||extra!=l.extra||jitter!=l.extraJ||loss!=l.loss||available!=l.availability;
      l.rho=rho;l.extra=extra;l.extraJ=jitter;l.availability=available;l.loss=loss;l.capacityScale=scale;l.rate=c.value("islDatarate",1e8)*scale;
      if(changed){context.congestionEpoch++;record({{"event","congestion"},{"a",a},{"b",b},{"rho",rho},{"delay",extra},{"jitter",jitter},{"loss",loss},{"capacityScale",scale},{"availability",available}});}
      if(fail)newFailures.insert(mcsr::linkKey(a,b));
    }
    if(c.value("physicalPrepared",false))return;
    for(auto key:newFailures)if(!syntheticFailures.count(key))fail(int(key>>32),int(key));
    for(auto key:syntheticFailures)if(!newFailures.count(key))fail(int(key>>32),int(key),true);
    syntheticFailures=std::move(newFailures);
  }

  void tracePacket(const std::shared_ptr<Packet> &p,const std::string &event,const std::string &detail) {
    if(packetTrace)trace<<seconds()<<','<<event<<','<<p->source<<','<<p->dest<<','<<detail<<','<<p->id<<','<<p->hops<<'\n';
  }
  bool inMeasurement(Tick time)const{return measuring&&time>=measurementStart&&time<measurementEnd;}
  FlowStats &flow(const std::shared_ptr<Packet> &p){return flows[{p->source,p->dest}];}
  void drop(const std::shared_ptr<Packet> &p,const std::string &reason) {if(p->control){if(control){control->dropped++;control->droppedByReason[reason]++;control->count(p->control->kind,&ControlCounts::frameDrops);if(p->assembly&&!p->assembly->failed&&!p->assembly->done){p->assembly->failed=true;control->count(p->control->kind,&ControlCounts::messageHopDrops);}}return;}flow(p).dropped++;if(inMeasurement(p->created))cohort[{p->source,p->dest}].dropped++;dropReasons[reason]++;tracePacket(p,"drop",reason);}
  std::map<std::string,long long> dropReasons;
  void clearQueue(Link &l,const std::string &reason) {while(!l.queue.empty()){auto p=l.queue.front();l.queue.pop_front();if(!p->control)flow(p).queued--;drop(p,reason);}}
  double uniform(const Packet &p,const Link &l,unsigned salt) {
    ++linkRandomDraws;
    std::seed_seq seed{unsigned(c.value("seed",0)),unsigned(p.id),unsigned(p.id>>32),unsigned(l.a),unsigned(l.b),unsigned(p.pass),unsigned(p.fragment),salt};std::mt19937 rng(seed);return double(rng())/4294967296.;
  }
  void start(Link &l,const std::shared_ptr<Packet> &p) {
    if(!l.active){drop(p,"link-down");return;}
    if(l.loss>0||l.extraJ>0){auto key=(static_cast<unsigned long long>(l.a)<<32)|unsigned(l.b);auto at=std::find_if(p->visits.begin(),p->visits.end(),[&](auto v){return v.first==key;});if(at==p->visits.end()){p->visits.push_back({key,1});p->pass=1;}else p->pass=++at->second;}
    l.transmitting=true;l.current=p;l.txStarted=now;auto generation=++l.generation;int a=l.a,b=l.b;
    double delay=l.prop+l.extra;if(packetJitter&&l.extraJ>0)delay+=uniform(*p,l,0x4a495454u)*2*std::sqrt(3.)*l.extraJ;
    bool lost=l.loss>0&&uniform(*p,l,0x4c4f5353u)<l.loss;
    Tick duration=std::max<Tick>(1,ticks((p->bytes+24)*8/l.rate));l.wireDuration=duration/TICKS;
    const auto kind=p->control?EventKind::Control:EventKind::Data;
    schedule((now+duration)/TICKS,[this,p,a,b,delay,generation,lost,kind]{
      auto &l=links[a].at(b);if(l.generation!=generation)return;
      if(p->control&&control){control->wireBytes+=p->bytes+24;control->busySeconds+=l.wireDuration;control->count(p->control->kind,&ControlCounts::wireBytes,p->bytes+24);}
      l.transmitting=false;l.current.reset();
      schedule(seconds()+delay,[this,p,a,b,lost]{if(lost)drop(p,"injected-loss");else arrive(p,b,a);},kind);
      if(!l.active)clearQueue(l,"link-down-in-queue");drain(l);
    },kind);
  }
  void accountPartialControl(Link &l) {
    if(!l.current||!l.current->control||!control)return;
    double busy=seconds()-l.txStarted/TICKS;auto bytes=std::min<long long>(l.current->bytes+24,static_cast<long long>(busy/l.wireDuration*(l.current->bytes+24)));
    control->busySeconds+=busy;control->wireBytes+=bytes;control->count(l.current->control->kind,&ControlCounts::wireBytes,bytes);
  }
  void abort(Link &l) {
    if(!l.current)return;
    auto p=l.current;accountPartialControl(l);
    l.generation++;l.transmitting=false;l.current.reset();drop(p,"link-down-in-transmission");
  }
  void drain(Link &l) {
    while(!l.transmitting&&!l.queue.empty()){auto next=l.queue.front();l.queue.pop_front();if(!next->control)flow(next).queued--;start(l,next);}
  }
  void transmit(const std::shared_ptr<Packet> &p,int a,int b) {
    p->enqueued=now;if(p->control&&control){control->hopAccepted++;control->count(p->control->kind,&ControlCounts::frameAttempts);if(packetTrace)record({{"event","control-frame"},{"from",a},{"to",b},{"kind",p->control->kind},{"nonce",p->control->nonce},{"fragment",p->fragment},{"bytes",p->bytes+24}});}
    auto l=find(a,b);if(!l||!l->active){drop(p,"link-down");return;}
    if(!l->transmitting&&l->queue.empty()){start(*l,p);return;}
    int cap=queueCapacity;
    if(cap && l->queue.size()>=size_t(cap)){drop(p,"queue-overflow");return;}
    l->queue.push_back(p);if(!p->control)flow(p).queued++;maxQueue=std::max(maxQueue,(long long)l->queue.size());
  }
  void writeRoute(const std::shared_ptr<Packet> &p,int node) {
    if(p->plan.path.empty())return;auto key=std::make_pair(p->source,p->dest);
    bool write=c.value("routeRecording",std::string("full"))=="full"||lastPaths[key]!=p->plan.path;
    lastPaths[key]=p->plan.path;
    if(!write)return;
    double up=0;if(auto l=find(n+p->source,node))up=l->prop;
    csv<<seconds()<<','<<++routeId<<",GS"<<p->source<<','<<p->dest<<','<<p->plan.path.front()<<','<<p->plan.path.back()<<','<<p->plan.path.size()-1<<','<<up*1000<<','<<p->plan.delay*1000<<','<<(up+p->plan.delay)*1000<<',';
    for(size_t i=0;i<p->plan.path.size();i++)csv<<(i?"-":"")<<p->plan.path[i];csv<<'\n';
    tracePacket(p,"route",std::to_string(routeId));
  }
  Plan measuredForward(Packet &p,int node,bool repair=false){ComputeTimer measure(context,repair?"repair":"plan");return router->forward(p,node,repair);}
  void dispatch(const std::shared_ptr<Packet> &p,int node,bool afterRepair=false) {
    if(router->sourceRouted()&&!p->plan.path.empty()&&seconds()-p->plannedAt>c.value("flowTimeout",120.)){p->plan=Plan{};p->cursor=0;}
    bool first=p->plan.path.empty();
    if(!router->sourceRouted()||first) {p->plan=measuredForward(*p,node);p->plannedAt=seconds();p->cursor=0;}
    if(p->plan.path.empty()){drop(p,"no-route");return;}
    if(first)writeRoute(p,node);
    if(router->sourceRouted()) {
      while(p->cursor<p->plan.path.size()&&p->plan.path[p->cursor]!=node)p->cursor++;
      if(p->cursor==p->plan.path.size()){drop(p,"route-loop");return;}
    }
    int next=(router->sourceRouted()?p->cursor+1:1)<p->plan.path.size()?p->plan.path[router->sourceRouted()?p->cursor+1:1]:n+p->dest;
    auto l=find(node,next);
    if(!l||!l->active) {
      if(afterRepair){drop(p,"no-route");return;}
      if(control && next<n && !knownFailures(node).blocksLink(node,next)){transmit(p,node,next);return;}
      if(next<n)p->excluded.links.insert(mcsr::linkKey(node,next));
      auto plan=measuredForward(*p,node,true);if(plan.path.empty()){drop(p,"no-route");return;}
      replanned++;if(plan.reserved)reserved++;
      double delay=c.value("processingMode",std::string("neutral"))=="neutral"?0:router->sourceRouted()?(plan.reserved?c.value("reserveDelay",.001):c.value("repairDelay",.005)):c.value("spfDelay",.05);
      p->plan=std::move(plan);p->plannedAt=seconds();p->cursor=0;writeRoute(p,node);
      schedule(seconds()+delay,[this,p,node]{dispatch(p,node,true);});return;
    }
    transmit(p,node,next);
  }
  void observeRecovery(const FlowKey &key,Tick created,long long id,bool received) {
    for(auto [link,index]:activeRecovery){
      auto &event=recovery[index];auto flow=event.flows.find(key);
      if(flow==event.flows.end()||id<=event.lastPacketId)continue;
      auto &state=flow->second;
      auto &window=state.windows[(created-ticks(event.time))/ticks(1.)];
      if(received){state.first=std::min(state.first,seconds());++window.received;window.lastReceipt=seconds();}
      else ++window.attempted;
    }
  }
  J recoveryFlowJson(const Recovery &event,const FlowKey &key,const RecoveryFlow &flow)const {
    double end=std::isfinite(event.until)?event.until:limit/TICKS;
    J first=std::isfinite(flow.first)?J(flow.first-event.time):J(nullptr),stable=nullptr;
    Tick previous=-2;double previousReceipt=0;
    for(auto [bin,window]:flow.windows){
      double boundary=event.time+bin+1;
      if(ticks(boundary)>ticks(end)||window.attempted<3||100*window.received<95*window.attempted)continue;
      if(bin==previous+1){stable=std::max({boundary,previousReceipt,window.lastReceipt})-event.time;break;}
      previous=bin;previousReceipt=window.lastReceipt;
    }
    return {{"source",key.first},{"destination",key.second},{"affected",true},
            {"receivedAt",std::isfinite(flow.first)?J(flow.first):J(nullptr)},
            {"recoverySeconds",first},{"firstPacketRecoverySeconds",first},
            {"stableRecoverySeconds",stable},{"censoredAt",end}};
  }
  void arrive(const std::shared_ptr<Packet> &p,int node,int from) {
    if(trackRecovery&&!p->control&&from<n&&node<n)lastLinkUse[mcsr::linkKey(from,node)][{p->source,p->dest}]=now;
    if(from<n&&node<n){auto l=find(from,node);if(l){double transit=seconds()-p->enqueued/TICKS;if(l->lastTransit>=0)l->jitter+=(std::abs(transit-l->lastTransit)-l->jitter)/16.;l->lastTransit=transit;l->jitterAt=seconds();}}
    if(p->control){
      control->arrived++;control->count(p->control->kind,&ControlCounts::frameArrivals);auto assembly=p->assembly;
      if(assembly->failed||assembly->done)return;
      if(!assembly->started&&assembly->fragments>1){assembly->started=true;auto weak=assembly;auto kind=p->control->kind;
        schedule(seconds()+controlReassemblyTimeout,[this,weak,kind]{if(auto a=weak)if(!a->done&&!a->failed){a->failed=true;control->count(kind,&ControlCounts::messageHopDrops);control->count(kind,&ControlCounts::reassemblyTimeouts);}});}
      if(!assembly->accept(p->fragment))return;if(packetTrace)record({{"event","control-message"},{"from",from},{"node",node},{"kind",p->control->kind},{"nonce",p->control->nonce},{"fragments",assembly->fragments}});control->count(p->control->kind,&ControlCounts::messageHopArrivals);double proc=0;
      if(assumedProcessing)proc=(p->control->kind=="report"||p->control->kind=="vote")?reportProcessingDelay:p->control->kind=="lsa"?lsaProcessingDelay:controlProcessingDelay;
      if(proc>0)schedule(seconds()+proc,[this,p,node,from]{control->receive(node,from,p->control,p->ttl);});else control->receive(node,from,p->control,p->ttl);return;}
    if(node>=n) {
      auto &f=flow(p);f.received++;f.bytes+=p->bytes;double d=seconds()-p->created/TICKS;
      if(inMeasurement(p->created)){auto &m=cohort[{p->source,p->dest}];m.received++;m.bytes+=p->bytes;m.delaySum+=d;if(m.lastTransit>=0)m.jitter+=(std::abs(d-m.lastTransit)-m.jitter)/16.;m.lastTransit=d;}
      f.delaySum+=d;double delta=d-f.mean;f.mean+=delta/f.received;f.m2+=delta*(d-f.mean);f.maxDelay=std::max(f.maxDelay,d);f.hops+=std::max(0,p->hops-1);
      if(f.lastTransit>=0)f.jitter+=(std::abs(d-f.lastTransit)-f.jitter)/16.;f.lastTransit=d;
      if(f.lastRx>=0){double gap=seconds()-f.lastRx;f.maxGap=std::max(f.maxGap,gap);if(gap>c["stations"][p->dest].value("gapThreshold",.1))f.outages++;}f.lastRx=seconds();
      if(!activeRecovery.empty())observeRecovery({p->source,p->dest},p->created,p->id,true);
      tracePacket(p,"receive",std::to_string(d));return;
    }
    p->current=node;if(++p->hops>c.value("maxHops",64)){drop(p,"max-hops");return;}dispatch(p,node);
  }
  void hashValue(unsigned long long value){for(int i=0;i<8;i++){trafficHash^=value&255;trafficHash*=1099511628211ull;value>>=8;}}
  void send(int source) {
    if(measuring&&now>=measurementEnd)return;
    auto &s=c["stations"][source];int dest=s.value("dstStation",-1);
    if(dest<0) {
      std::vector<int> candidates;auto type=s.value("dstType",std::string("station"));
      for(int j=0;j<g;j++)if(j!=source&&(type=="any"||(c["stations"][j].value("mobile",false)?"mobile":"station")==type))candidates.push_back(j);
      if(!candidates.empty())dest=candidates[std::min(candidates.size()-1,size_t(random()*candidates.size()))];
    }
    auto &f=flows[{source,dest}];f.attempted++;if(inMeasurement(now))cohort[{source,dest}].attempted++;long long id=++packetId;
    if(!activeRecovery.empty())observeRecovery({source,dest},now,id,false);
    for(auto v:{(unsigned long long)id,(unsigned long long)now,(unsigned long long)source,(unsigned long long)dest,(unsigned long long)s["packetSize"].get<int>()})hashValue(v);
    int best;{ComputeTimer measure(context,"access");best=dest<0?-1:router->selectUplink(source,dest);}
    if(best<0){noUplink++;f.sourceDropped++;if(inMeasurement(now))cohort[{source,dest}].sourceDropped++;dropReasons[dest<0?"no-destination":uplinks(source).empty()?"no-access":"no-route-at-source"]++;}
    else {f.sent++;if(inMeasurement(now))cohort[{source,dest}].sent++;auto p=std::make_shared<Packet>();p->id=id;p->source=source;p->dest=dest;p->bytes=s["packetSize"];p->created=now;tracePacket(p,"send",std::to_string(p->bytes));transmit(p,n+source,best);}
    Tick next=now+ticks(s["sendInterval"].get<double>());
    if(next<=now)throw std::runtime_error("Traffic interval is below time resolution");
    if(next<limit&&(!measuring||next<measurementEnd)&&next<ticks(s.value("endTime",limit/TICKS)))schedule(next/TICKS,[this,source]{send(source);});
  }
  void fail(int a,int b,bool restore=false,std::string cause="failure") {
    if(a<0||b<0||a>=n||b>=n||!find(a,b))throw std::runtime_error("Failure target is not an ISL");
    auto &f=failures[mcsr::linkKey(a,b)];
    if(restore){if(f.count>0)--f.count;if(f.count)return;}
    else {if(f.count++>0)return;f.since=seconds();f.known.assign(n,seconds());
      if(c.value("failureKnowledge",std::string("instant"))=="propagate") {
        std::vector<int> distances(n,-1);std::queue<int> q;distances[a]=distances[b]=0;q.push(a);q.push(b);
        while(!q.empty()){int u=q.front();q.pop();for(auto &[v,l]:links[u])if(v<n&&l.nominal&&distances[v]<0){distances[v]=distances[u]+1;q.push(v);}}
        double detect=c.value("slsInterval",.2)*c.value("slsMissCount",3),per=c.value("floodHopDelay",.002);int scope=c.value("failureScopeHops",6);
        for(int i=0;i<n;i++)f.known[i]=(distances[i]<0||(scope>0&&distances[i]>scope))?INFINITY:seconds()+detect+distances[i]*per;
      }
    }
    auto key=mcsr::linkKey(a,b);
    Recovery event{seconds(),a,b,restore,packetId};
    auto previous=activeRecovery.find(key);
    if(previous!=activeRecovery.end()){
      auto &prior=recovery[previous->second];prior.until=seconds();
      if(restore)for(const auto &[flow,state]:prior.flows)event.flows.try_emplace(flow);
      activeRecovery.erase(previous);
    }
    if(!restore){
      auto used=lastLinkUse.find(key);
      if(used!=lastLinkUse.end())for(auto [flow,time]:used->second)
        if(now-time<=ticks(std::max(1.,2*c["stations"][flow.first].value("sendInterval",1.))))event.flows.try_emplace(flow);
      auto observe=[&](const std::shared_ptr<Packet>& p){if(p&&!p->control)event.flows.try_emplace(FlowKey{p->source,p->dest});};
      for(auto [u,v]:{std::make_pair(a,b),std::make_pair(b,a)}){
        auto &link=links[u].at(v);observe(link.current);
        for(const auto &packet:link.queue)observe(packet);
      }
    }
    for(auto [u,v]:{std::make_pair(a,b),std::make_pair(b,a)}){auto &l=links[u].at(v);l.active=restore&&l.nominal;if(!restore){clearQueue(l,"link-down-in-queue");abort(l);}}
    context.islEpoch++;if(!control)context.epoch++;record({{"event",restore?"restore":"failure"},{"a",a},{"b",b},{"cause",cause}});
    physicalEvents.push_back({{"time",seconds()},{"event",restore?"restore":"failure"},{"a",a},{"b",b},{"cause",cause}});
    event.applied.assign(n,INFINITY);
    activeRecovery[key]=recovery.size();
    recovery.push_back(std::move(event));
    failureLog<<seconds()<<','<<(restore?"restore":"failure")<<','<<a<<','<<b<<'\n';
    if(router && !control && !restore) {
      double detect=c.value("csibDetectDelay",-1.);
      if(detect<0)detect=c.value("slsInterval",.2)*c.value("slsMissCount",3);
      schedule(seconds()+detect,[this]{context.epoch++;});
    }
  }
  void failureSchedule() {
    if(c.value("physicalPrepared",false)) {for(const auto &e:c.at("physicalEvents")){double t=e["time"];int a=e["a"],b=e["b"];bool up=e["event"]=="restore";std::string cause=e.value("cause",std::string("failure"));schedule(t,[this,a,b,up,cause]{fail(a,b,up,cause);});}return;}
    for(const auto &e:c.value("scheduledEvents",J::array())){double t=e["time"];int a=e["a"],b=e["b"];bool up=e["event"]=="restore";schedule(t,[this,a,b,up]{fail(a,b,up,"scheduled");});}
    for(const auto &e:c.value("failureReplayEvents",J::array())) {double t=e["time"];int a=e["a"],b=e["b"];bool up=e.value("event",std::string())=="restore";schedule(t,[this,a,b,up]{fail(a,b,up);});}
    if(!c.value("failureReplayEvents",J::array()).empty())return;
    std::string text=c.value("failureSchedule",std::string());for(char &ch:text)if(ch==','||ch==';')ch=' ';
    std::istringstream input(text);std::string item;
    while(input>>item) {
      std::smatch m;if(!std::regex_match(item,m,std::regex(R"((route|([0-9]+)-([0-9]+))@([0-9.eE+-]+?)(?:\+([0-9.eE-]+))?)")))throw std::runtime_error("Invalid failure schedule: "+item);
      bool route=m[1]=="route";int a=route?-1:std::stoi(m[2]),b=route?-1:std::stoi(m[3]);double t=std::stod(m[4]),duration=m[5].matched?std::stod(m[5]):0;
      schedule(t,[this,a,b,route,duration]()mutable{if(route){update(false);int src=c.value("failureRouteSrc",-1),dst=c.value("failureRouteDst",-1);
      if(src<0)for(auto &s:c["stations"])if(s.value("enabled",false)&&s.value("dstStation",-1)>=0){src=s["id"];dst=s["dstStation"];break;}
      if(src>=0&&src<g&&dst<0)dst=c["stations"][src].value("dstStation",-1);
      if(src<0||src>=g||dst<0||dst>=g||src==dst)throw std::runtime_error("route@ requires a fixed reference source and destination");
      Context reference=context;reference.c["algorithm"]="dijkstra-delay";reference.graph=[this](int){mcsr::Graph g(n);for(int u=0;u<n;u++)for(auto &[v,l]:links[u])if(v<n&&l.nominal&&l.active)g.addDirected(u,v,{l.prop*1000,1.,0.});return g;};reference.failures=[this](int){return knownFailures(-1);};
      DijkstraRouting oracle(reference);int first=oracle.selectUplink(src,dst);Packet p;p.dest=dst;auto path=first<0?std::vector<int>{}:oracle.forward(p,first).path;
      if(path.size()<2)throw std::runtime_error("Reference route has no ISL to cut");int k=(path.size()-1)/2;a=path[k];b=path[k+1];}fail(a,b);if(duration>0)schedule(seconds()+duration,[this,a,b]{fail(a,b,true);});});
    }
    int count=c.value("randomFailures",0);double lo=c.value("randomStart",0.),hi=c.value("randomEnd",0.),duration=c.value("randomDuration",0.);
    if(count && hi<=lo)throw std::runtime_error("Invalid random failure window");
    for(int i=0;i<count;i++) {
      double t=lo+(hi-lo)*double(failureRng())/4294967296.;auto [a,b]=nominalIsls.at(failureRng()%nominalIsls.size());
      schedule(t,[this,a,b,duration]{fail(a,b);if(duration>0)schedule(seconds()+duration,[this,a,b]{fail(a,b,true);});});
    }
  }
  double lastSample=0;
  void sample() {
    if(seconds()<=lastSample)return;
    double window=seconds()-lastSample;lastSample=seconds();
    long long sent=0,received=0,dropped=0;
    for(auto &[key,f]:flows) {
      long long rx=f.received-f.lastReceived;double delay=rx?(f.delaySum-f.lastDelaySum)/rx*1000:0;
      series<<seconds()<<','<<key.first<<','<<key.second<<','<<f.sent-f.lastSent<<','<<rx<<','<<f.dropped+f.sourceDropped-f.lastDropped-f.lastSourceDropped<<','<<(f.bytes-f.lastBytes)*8/window/1e6<<','<<delay<<','<<f.queued<<'\n';
      f.lastSent=f.sent;f.lastReceived=f.received;f.lastDropped=f.dropped;f.lastSourceDropped=f.sourceDropped;f.lastBytes=f.bytes;f.lastDelaySum=f.delaySum;
      sent+=f.sent;received+=f.received;dropped+=f.dropped;
    }
    if(control){auto &c=*control;controlSeries<<seconds()<<','<<c.generated<<','<<c.hopAccepted<<','<<c.arrived<<','<<c.dropped<<','<<c.wireBytes<<','<<c.busySeconds<<','<<c.hopAccepted-c.arrived-c.dropped<<'\n';}
    J status={{"time",seconds()},{"limit",limit/TICKS},{"sent",sent},{"received",received},{"dropped",dropped},{"progress",seconds()/(limit/TICKS)}};
    std::ofstream(out+"/progress.tmp")<<status.dump();
    std::error_code ec;std::filesystem::rename(out+"/progress.tmp",out+"/progress.json",ec);
    if(ec){std::filesystem::remove(out+"/progress.json",ec);std::filesystem::rename(out+"/progress.tmp",out+"/progress.json",ec);}
    schedule(seconds()+1,[this]{sample();},EventKind::Sample);
  }
public:
  Simulation(J cfg,const std::string &directory,bool profile=false):c(std::move(cfg)),out(directory),n(c["numSatellites"]),g(c["stations"].size()),limit(ticks(c["simTimeLimit"])),
    queueCapacity(c.value("queueCapacity",500)),controlPayloadBytes(c.value("controlMtu",1500)-40),
    packetTrace(c.value("packetTrace",false)),packetJitter(c.value("jitterModel",std::string("packet"))=="packet"),
    assumedProcessing(c.value("processingMode",std::string("neutral"))=="assumed"),
    controlReassemblyTimeout(c.value("controlReassemblyTimeout",3.)),reportProcessingDelay(c.value("csibReportProcDelay",0.)),
    lsaProcessingDelay(c.value("floodHopDelay",0.)),controlProcessingDelay(c.value("csibHopProcDelay",0.)),
    trafficRng(c.value("seed",23542)),failureRng(c.value("failureSeed",4242)),sat(n),ground(g),links(n+g),profiling(profile) {
    if(n<6||n>20000||g<1||g>5000||limit<=0||c["planes"].get<int>()<2||n%c["planes"].get<int>())throw std::runtime_error("Invalid engine dimensions");
    if(c.value("controlMode",std::string("packet"))=="time" && c.value("csibEnabled",false))
      throw std::runtime_error("CSIB requires packet control mode");
    if(c.contains("modelVersion")&&c["modelVersion"]!=MODEL_VERSION)throw std::runtime_error("Engine model version mismatch");
    trackRecovery=!c.value("prepareOnly",false)&&(
      !c.value("physicalEvents",J::array()).empty()||!c.value("failureReplayEvents",J::array()).empty()||
      !c.value("scheduledEvents",J::array()).empty()||c.value("randomFailures",0)>0||
      !c.value("failureSchedule",std::string()).empty()||!c.value("scenario",J::object()).value("entries",J::array()).empty());
    measurementStart=ticks(c.value("warmupSeconds",0.));measurementEnd=limit-ticks(c.value("drainSeconds",0.));
    if(measurementStart<0||measurementEnd>limit||measurementStart>=measurementEnd)throw std::runtime_error("Invalid measurement window");
    measuring=measurementStart>0||measurementEnd<limit;
    mcsr::OpScope initialCount(&context.operations);
    auto year=[](int y){return y<57?y+2000:y+1900;};cJulian base(year(c.value("baseYear",20)),c.value("baseDay",1.5)),epoch(year(c.value("epochYear",20)),c.value("epochDay",1.5));gap=base.spanSec(epoch);
    int per=n/c["planes"].get<int>();for(int i=0;i<n;i++)orbits.emplace_back(new cOrbitA("sat",c.value("epochYear",20),c.value("epochDay",1.5),c["altitude"],c.value("eccentricity",.000001),c["inclination"],std::fmod((i/per)*360.*c["phasing"].get<int>()/n+(i%per)*360./per,360.),c.value("bstar",.00042243),.00001846,i,c["planes"],per,(i/per)*360./c["planes"].get<int>(),0));
    csv.open(out+"/routes.csv");trace.open(out+"/events.csv");network.open(out+"/network-events.jsonl");series.open(out+"/timeseries.csv");failureLog.open(out+"/failures.csv");controlSeries.open(out+"/control-series.csv");controlSeries<<"time,generated,hopAccepted,arrived,dropped,wireBytes,busySeconds,inFlight\n";
    if(!csv||!trace||!network||!series||!failureLog||!controlSeries)throw std::runtime_error("Cannot create output files");
    csv<<std::setprecision(14)<<"time,route,source,station,firstSat,exitSat,satHops,uplinkMs,satToStationMs,totalMs,path\n";
    trace<<std::setprecision(14)<<"time,event,source,destination,detail,packet_id,hops\n";
    series<<std::setprecision(14)<<"time,source,destination,sent,received,dropped,throughputMbps,delayMs,queued\n";failureLog<<std::setprecision(14)<<"time,event,satA,satB\n";
    context.c=c;context.revision=[this](int node){return control?control->revision(node):0;};context.topologyRevision=[this](int node){return control&&node>=0?control->views.at(node).topologyRevision:0;};context.n=n;context.g=g;context.graph=[this](int v){return graph(v);};context.exits=[this](int d){return exits(d);};context.uplinks=[this](int s){return uplinks(s);};context.failures=[this](int v){return knownFailures(v);};context.record=[this](J e){record(e);};
    zones=leocongestion::parseZones(c.value("congestionZones",std::string()));update();
    if(!c.contains("commonSrd")){
      mcsr::PlannerConfig pc;pc.planes=c["planes"];pc.satsPerPlane=per;pc.interPlaneSpacing=c["phasing"];pc.fixedK=c.value("srdK",0);pc.fixedL=c.value("srdL",0);
      mcsr::Graph geometry(n);for(int u=0;u<n;u++)for(auto &[v,l]:links[u])if(v<n&&l.nominal)geometry.addDirected(u,v,{l.prop*1000,1.,0.});
      auto planner=mcsr::Planner::create(pc,geometry);auto &plan=planner->srdPlan();c["commonSrd"]={{"K",plan.K},{"L",plan.L},{"srdOf",plan.srdOf}};
    }
    srdOf=c["commonSrd"]["srdOf"].get<std::vector<int>>();initialK=c["commonSrd"]["K"];initialL=c["commonSrd"]["L"];
    if(int(srdOf.size())!=n||initialK<1||initialL<1)throw std::runtime_error("Invalid common SRD");
    for(auto &entry:c.value("scenario",J::object()).value("entries",J::array())){auto target=entry.at("target");if(target.value("kind",std::string())!="zone"||target.value("params",J::object()).value("zone_kind",std::string())!="srd")continue;
      auto key=target.dump();std::set<unsigned long long> selected;
      for(auto [a,b]:nominalIsls)for(auto [u,v]:{std::make_pair(a,b),std::make_pair(b,a)})if(factor(target,u,v)>0)selected.insert((static_cast<unsigned long long>(u)<<32)|unsigned(v));srdTargets[key]=selected;}
    J targets=J::object();for(auto &[key,values]:srdTargets){J rows=J::array();for(auto edge:values)rows.push_back({int(edge>>32),int(edge)});targets[key]=rows;}
    if(c["commonSrd"].contains("targets")&&c["commonSrd"]["targets"]!=targets)throw std::runtime_error("Common SRD targets changed");c["commonSrd"]["targets"]=targets;
    std::ofstream(out+"/common-srd.json")<<c["commonSrd"].dump(2);
    context.c=c;auto factories=routingFactories();auto name=c.value("algorithm",std::string("dijkstra-delay"));if(!factories.count(name))throw std::runtime_error("Unknown routing algorithm");router=factories.at(name)(context);
    applyCongestion();
    if(!c.value("prepareOnly",false)&&c.value("controlMode",std::string("packet"))=="packet") {
      std::vector<int> owners;if(!srdOf.empty()){int count=*std::max_element(srdOf.begin(),srdOf.end())+1;owners.assign(count,-1);
        mcsr::PlannerConfig pc;pc.planes=c["planes"];pc.satsPerPlane=per;pc.interPlaneSpacing=c["phasing"];pc.fixedK=initialK;pc.fixedL=initialL;auto planner=mcsr::Planner::create(pc,graph(-1));
        for(int k=0;k<count;k++)owners[k]=planner->router().electDs(k,mcsr::FailureSet{});
      }
      control=std::make_unique<ControlPlane>(c,n,nominalIsls,srdOf,owners,[this]{return seconds();},[this](double t,auto f){schedule(t,f,EventKind::Control);},
        [this](int a,int b,ControlPlane::Msg message,int ttl){
          int payload=controlPayloadBytes;if(payload<1)throw std::runtime_error("Invalid control MTU");
          int count=1+(message->payloadBytes-1)/payload;auto assembly=std::make_shared<ControlAssembly>(count);control->count(message->kind,&ControlCounts::messageHopAttempts);
          for(int part=0;part<count;part++){auto p=std::make_shared<Packet>();p->control=message;p->assembly=assembly;p->fragment=part;p->ttl=ttl;p->id=-(long long)message->nonce;p->bytes=std::min(payload,message->payloadBytes-part*payload)+16;p->pass=part;transmit(p,a,b);}
        },
        [this](int u){std::vector<LinkReport> rows;for(auto &[v,l]:links[u])if(v<n&&l.nominal){int cap=queueCapacity;double A=cap?std::max(0.,1-double(l.queue.size())/cap):1.;rows.push_back({u,v,true,l.extra+(c.value("jitterModel",std::string("packet"))=="packet"?std::sqrt(3.)*l.extraJ:0.),std::min(A,l.availability),(c.value("jitterModel",std::string("packet"))=="cost"?std::hypot(c.value("jitterSource",std::string("arrival"))=="delayChange"?std::abs(l.prop-l.previousProp):l.jitter,l.extraJ):(c.value("jitterSource",std::string("arrival"))=="delayChange"?std::abs(l.prop-l.previousProp):l.jitter))*1000});}std::sort(rows.begin(),rows.end(),[](auto a,auto b){return a.b<b.b;});return rows;},
        [this](J event){record(event);},[this,per](int k,int viewer){ComputeTimer measure(context,"ds-vote");mcsr::PlannerConfig pc;pc.planes=c["planes"];pc.satsPerPlane=per;pc.interPlaneSpacing=c["phasing"];pc.fixedK=initialK;pc.fixedL=initialL;auto planner=mcsr::Planner::create(pc,graph(viewer));return planner->router().voteDs(k);},[this](int a,int b){return links[a].at(b).prop;},[this]{return context.epoch;});
    }
    failureSchedule();
    for(const auto &e:c.value("scenario",J::object()).value("entries",J::array()))for(auto key:{"start_s","end_s"})if(e.contains(key)&&!e[key].is_null()){double t=e[key];if(t>=0)schedule(t,[this]{applyCongestion();});}
    for(auto &z:zones){schedule(z.start,[this]{applyCongestion();});if(z.duration>0)schedule(z.start+z.duration,[this]{applyCongestion();});}
    for(int i=0;i<g;i++)if(!c.value("prepareOnly",false)&&c["stations"][i].value("enabled",false)) {
      auto &s=c["stations"][i];double start=s.contains("startTime")?s["startTime"].get<double>():s.contains("startRange")?s["startRange"][0].get<double>()+random()*(s["startRange"][1].get<double>()-s["startRange"][0].get<double>()):1+4*random();
      if(ticks(s["sendInterval"].get<double>())<1)throw std::runtime_error("Traffic interval below resolution");if(start<s.value("endTime",limit/TICKS))schedule(start,[this,i]{send(i);});
    }
    schedule(1,[this]{sample();},EventKind::Sample);
  }
  void run() {
    mcsr::OpScope count(&context.operations);
    while(!events.empty()){
      const bool measureQueue=profiling&&processedEvents%PROFILE_INTERVAL==0;
      const auto begin=measureQueue?ProfileClock::now():ProfileClock::time_point{};
      std::pop_heap(events.begin(),events.end());
      auto e=std::move(events.back());events.pop_back();
      if(measureQueue){queuePopMilliseconds+=milliseconds(ProfileClock::now()-begin);++queuePopSamples;}
      now=e.time;context.now=seconds();
      if(!profiling){e.action();continue;}
      auto &timing=eventTimings[static_cast<size_t>(e.kind)];
      const bool measureAction=timing.count++%PROFILE_INTERVAL==0;
      const auto actionStart=measureAction?ProfileClock::now():ProfileClock::time_point{};
      e.action();
      if(measureAction){timing.milliseconds+=milliseconds(ProfileClock::now()-actionStart);++timing.samples;}
      if(++processedEvents%PROFILE_INTERVAL==0){
        const auto time=ProfileClock::now();
        if(time-profileWritten>=std::chrono::seconds(1))writeProfile(time);
      }
    }
    now=limit;context.now=seconds();for(auto &row:links)for(auto &[v,l]:row)accountPartialControl(l);sample();J rows=J::array();long long sent=0,received=0,dropped=0,attempted=0,bytes=0;double delaySum=0;
    for(auto &[key,f]:flows){sent+=f.sent;received+=f.received;dropped+=f.dropped;attempted+=f.attempted;bytes+=f.bytes;delaySum+=f.delaySum;
      rows.push_back({{"source",key.first},{"destination",key.second},{"attempted",f.attempted},{"sent",f.sent},{"received",f.received},{"dropped",f.dropped+f.sourceDropped},{"sourceDropped",f.sourceDropped},{"inFlight",f.sent-f.received-f.dropped},{"deliveryRate",f.attempted?100.*f.received/f.attempted:0},{"admittedDeliveryRate",f.sent?100.*f.received/f.sent:0},{"offeredDeliveryRate",f.attempted?100.*f.received/f.attempted:0},{"meanDelayMs",1000*f.mean},{"delayStdMs",f.received>1?1000*std::sqrt(f.m2/(f.received-1)):0},{"maxDelayMs",f.maxDelay*1000},{"jitterMs",f.jitter*1000},{"meanHops",f.received?double(f.hops)/f.received:0},{"maxGapSeconds",f.maxGap},{"outages",f.outages},{"throughputMbps",f.bytes*8/(limit/TICKS)/1e6}});}
    if(received+dropped>sent)throw std::runtime_error("Packet conservation violated");
    std::ostringstream hash;hash<<std::hex<<trafficHash;
    J result={{"engine","leo-native"},{"version",ENGINE_VERSION},{"algorithm",c["algorithm"]},{"attempted",attempted},{"sent",sent},{"received",received},{"dropped",dropped+noUplink},{"admittedDropped",dropped},{"inFlight",sent-received-dropped},{"noUplink",noUplink},{"delaySumSeconds",delaySum},{"meanDelayMs",received?1000*delaySum/received:0},{"deliveryRate",attempted?100.*received/attempted:0},{"admittedDeliveryRate",sent?100.*received/sent:0},{"throughputMbps",bytes*8/(limit/TICKS)/1e6},{"maxQueuePackets",maxQueue},{"replanned",replanned},{"reservedSwitches",reserved},{"trafficHash",hash.str()},{"trafficHashAlgorithm","fnv1a64"},{"dropReasons",dropReasons},{"flows",rows},{"routeRecording",c.value("routeRecording",std::string("full"))},{"packetTrace",c.value("packetTrace",false)},{"experimental",c.value("experimental",false)||!c.value("scenario",J::object()).value("entries",J::array()).empty()||!zones.empty()},{"congestionMode",c.value("congestionMode",std::string("actual"))}};
    result["appliedProcessingDelays"]=J::object();result["requestedProcessingDelays"]=J::object();for(auto key:{"reserveDelay","repairDelay","spfDelay","csibComputeDelay","csibReportProcDelay","csibHopProcDelay","floodHopDelay"}){result["requestedProcessingDelays"][key]=c.value(key,0.);result["appliedProcessingDelays"][key]=c.value("processingMode",std::string("neutral"))=="neutral"?0:c.value(key,0.);}
    if(!attempted)result["deliveryRate"]=nullptr;
    result["controlProtocolVersion"]=2;result["metricVersion"]=4;result["modelVersion"]=MODEL_VERSION;result["linkRandomDraws"]=linkRandomDraws;result["processingMode"]=c.value("processingMode",std::string("neutral"));result["controlMode"]=c.value("controlMode",std::string("packet"));result["jitterModel"]=c.value("jitterModel",std::string("packet"));
    result["cpuThreads"]=1;
    result["computeMs"]=context.computeMs;result["computations"]=context.computations;
    auto &o=context.operations;result["operations"]=J::object();o.each([&](const char* key,std::uint64_t value){result["operations"][key]=value;});
    result["approximation"]={{"labelCap",c.value("labelCap",32)},{"capEvict",o.capEvict},{"capReject",o.capReject},{"limited",o.capEvict+o.capReject>0}};
    if(measuring){
      long long attempted=0,received=0,dropped=0,sent=0,bytes=0,jitterCount=0;double delay=0,jitter=0;
      for(auto &[key,m]:cohort){attempted+=m.attempted;received+=m.received;dropped+=m.dropped+m.sourceDropped;sent+=m.sent;bytes+=m.bytes;delay+=m.delaySum;if(m.received>1){jitter+=m.jitter*m.received;jitterCount+=m.received;}}
      result["measurement"]={{"schema",1},{"metricVersion","cohort-1"},{"start",measurementStart/TICKS},{"end",measurementEnd/TICKS},{"observedUntil",limit/TICKS},{"attempted",attempted},{"sent",sent},{"received",received},{"dropped",dropped},{"inFlight",attempted-received-dropped},{"deliveryRate",attempted?J(100.*received/attempted):J(nullptr)},{"meanDelayMs",received?J(1000*delay/received):J(nullptr)},{"jitterMs",jitterCount?J(1000*jitter/jitterCount):J(nullptr)},{"throughputMbps",bytes*8/((measurementEnd-measurementStart)/TICKS)/1e6}};
      if(attempted<received+dropped)throw std::runtime_error("Measurement conservation failed");
    }
    if(control){auto &c=*control;result["control"]={{"generated",c.generated},{"generatedByKind",c.generatedByKind},{"hopAccepted",c.hopAccepted},{"arrived",c.arrived},{"dropped",c.dropped},{"inFlight",c.hopAccepted-c.arrived-c.dropped},{"wireBytes",c.wireBytes},{"busySeconds",c.busySeconds},{"dropReasons",c.droppedByReason},{"byKind",c.byKindJson()}};}
    J events=J::array();for(auto &e:physicalEvents)events.push_back(e);std::ofstream(out+"/physical-events.json")<<events.dump();
    result["recoveryPolicy"]={{"version",1},{"windowSeconds",1},{"consecutiveWindows",2},{"minimumPacketsPerWindow",3},{"minimumDeliveryRatePct",95},{"affectedBasis","recent-link-arrival-or-queued-or-transmitting"}};
    J recoveries=J::array();for(auto &e:recovery){J rows=J::array();
      for(const auto &[key,flow]:e.flows)rows.push_back(recoveryFlowJson(e,key,flow));
      double applied=0;int count=0;for(auto t:e.applied)if(std::isfinite(t)){count++;applied=std::max(applied,t);}
      recoveries.push_back({{"detectedSeconds",std::isfinite(e.detected)?J(e.detected-e.time):J(nullptr)},{"appliedNodes",count},{"expectedNodes",n},{"convergenceSeconds",count==n?J(applied-e.time):J(nullptr)},{"time",e.time},{"a",e.a},{"b",e.b},{"restore",e.up},{"flows",rows}});}result["recovery"]=recoveries;
    if(control&&control->hopAccepted<control->arrived+control->dropped)throw std::runtime_error("Control conservation failed");
    if(attempted!=received+dropped+noUplink+(sent-received-dropped))throw std::runtime_error("Offered packet conservation failed");
    std::ofstream(out+"/summary.json")<<result.dump(2);
    if(profiling)writeProfile(ProfileClock::now());
  }
};
#ifndef LEO_KERNEL_TEST
int main(int argc,char **argv) {
  try{if(argc!=3&&argc!=4)throw std::runtime_error("Usage: engine.exe input.json output-directory");J c;std::ifstream(argv[1])>>c;if(argc==4&&std::string(argv[3])=="--prepare"){c["prepareOnly"]=true;c["algorithm"]="dijkstra-delay";c["controlMode"]="time";c["csibEnabled"]=false;}Simulation simulation(c,argv[2],argc==4&&std::string(argv[3])=="--profile");simulation.run();return 0;}
  catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}

#endif
