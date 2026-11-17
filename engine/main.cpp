// SPDX-License-Identifier: GPL-3.0-only
// Native execution adapter, 2026-11-17. Vendored routing cores remain unchanged.
#include "geometry.h"
#include "vendor/routing/mcsr/planner.h"
#include "vendor/routing/mcsr/csib.h"
#include "vendor/routing/congestion/zones.h"
#include <deque>
#include <filesystem>
#include <unordered_map>
#include <set>
#include <sstream>
#include <regex>
#include <chrono>
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
};
struct Context {
  J c; int n,g; double now=0; long epoch=0,islEpoch=0,congestionEpoch=0;
  std::function<mcsr::Graph(int)> graph;
  std::function<std::vector<mcsr::Planner::Exit>(int)> exits;
  std::function<std::vector<std::pair<int,double>>(int)> uplinks;
  std::function<mcsr::FailureSet(int)> failures;
  std::function<void(double,std::function<void()>)> schedule;
  std::function<void(J)> record;
};
class Routing {
public:
  virtual ~Routing()=default;
  virtual Plan forward(Packet &p,int node,bool repair=false)=0;
  virtual int selectUplink(int source,int dest)=0;
  virtual bool sourceRouted() const {return false;}
  virtual void onNetworkEvent(const J &event) {}
  virtual void onTimer(const std::string &kind) {}
};
class DijkstraRouting:public Routing {
  Context &x; bool hop; long stamp=-1;
  std::map<std::pair<int,unsigned long long>,Tree> cache;
  Tree &tree(int dest,int viewer,const mcsr::FailureSet &extra=mcsr::FailureSet{}) {
    if(stamp!=x.epoch) { cache.clear();stamp=x.epoch; }
    auto failures=x.failures(viewer); failures.links.insert(extra.links.begin(),extra.links.end()); unsigned long long hash=0;
    for(auto k:failures.links) hash^=k*11400714819323198485ull;
    auto key=std::make_pair(dest,hash);
    if(!cache.count(key)) {
      auto graph=x.graph(viewer);std::vector<std::vector<Edge>> a(x.n+1);
      for(int u=0;u<x.n;u++) for(auto e:graph.adj[u])if(!failures.blocksLink(u,e.to))a[u].push_back({e.to,e.t.D/1000});
      for(auto e:x.exits(dest)) { a[e.sat].push_back({x.n,e.gsl.D/1000});a[x.n].push_back({e.sat,e.gsl.D/1000}); }
      cache.emplace(key,dijkstra(a,x.n,hop));
    }
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
  std::map<double,std::shared_ptr<Planner>> snapshots;
  std::unique_ptr<mcsr::SrdPlan> fixedPlan;
  std::unique_ptr<mcsr::CsibModel> csib;
  int K=0,L=0;double sampled=-1;long epoch=-1,congestionEpoch=-1;
  std::map<std::tuple<const Planner*,int,int>,Plan> plans;
  long groundEpoch=-1;
  std::shared_ptr<Planner> build() {
    mcsr::PlannerConfig c;c.planes=x.c["planes"];c.satsPerPlane=x.n/c.planes;c.interPlaneSpacing=x.c["phasing"];
    c.labelCap=x.c.value("labelCap",32);c.fixedK=K;c.fixedL=L;
    if(x.c.value("floodModel",std::string("paper"))=="consistent")c.flood=mcsr::FloodModel::Consistent;
    auto p=std::shared_ptr<Planner>(Planner::create(c,x.graph(-2)).release());
    if(!K){K=p->srdPlan().K;L=p->srdPlan().L;}
    sampled=x.now;epoch=x.c.value("snapshotOnDelayUpdate",true)?x.epoch:x.islEpoch;congestionEpoch=x.congestionEpoch;
    current=p;plans.clear();return p;
  }
  std::shared_ptr<Planner> view(int node) {
    if(csib) {auto v=csib->viewAt(node,x.now);return snapshots.at(v?v->dataTime:0);}
    long e=x.c.value("snapshotOnDelayUpdate",true)?x.epoch:x.islEpoch;
    double interval=x.c.value("refreshInterval",0.);
    if(!current || epoch!=e || congestionEpoch!=x.congestionEpoch || (interval>0 && x.now-sampled>=interval-1e-10))build();
    return current;
  }
  void publish(const std::string &kind,int a=-1,int b=-1,double failedAt=0) {
    auto p=build();snapshots[x.now]=p;
    auto f=x.failures(-2);
    if(kind=="periodic")csib->periodic(x.now,p->router(),p->graph(),f);
    else if(kind=="failure")csib->onLinkFailure(failedAt,a,b,p->router(),p->graph(),f);
    else csib->monitor(x.now,p->router(),p->graph(),f);
    for(int k=0;k<fixedPlan->r;k++)for(const auto &v:csib->versions(k))if(v.dataTime==x.now) {
      x.record({{"event","csib-generated"},{"srd",k},{"version",v.id},{"ds",v.ds},{"dataTime",v.dataTime},{"computeDone",v.computeDone}});
      for(size_t i=0;i<v.members.size();i++)if(std::isfinite(v.arrival[i]) && v.arrival[i]>=x.now) {
        int node=v.members[i],id=v.id;double data=v.dataTime;
        x.schedule(v.arrival[i],[this,node,id,data]{x.record({{"event","csib-applied"},{"node",node},{"version",id},{"dataTime",data}});});
      }
    }
    csib->prune(x.now);std::set<double> keep{0};
    for(int k=0;k<fixedPlan->r;k++)for(auto &v:csib->versions(k))keep.insert(v.dataTime);
    for(auto i=snapshots.begin();i!=snapshots.end();)if(!keep.count(i->first))i=snapshots.erase(i);else ++i;
  }
public:
  McsrRouting(Context &ctx):x(ctx) {
    auto id=x.c.value("algorithm",std::string("mcsr-ls"));service=id=="mcsr-ts"?mcsr::Service::TS:id=="mcsr-hr"?mcsr::Service::HR:mcsr::Service::LS;
    selection.hrJitterMs=x.c.value("hrJitterMs",15.);double bound=x.c.value("tsDelayBoundMs",-1.);if(bound>=0)selection.tsDelayBoundMs=bound;
    K=x.c.value("srdK",0);L=x.c.value("srdL",0);auto p=build();
    if(x.c.value("csibEnabled",false)) {
      fixedPlan=std::make_unique<mcsr::SrdPlan>(p->srdPlan());mcsr::CsibParams cp;
      cp.periodSec=x.c.value("csibPeriod",180.);cp.computeMs=1000*x.c.value("csibComputeDelay",0.);
      cp.reportProcMs=1000*x.c.value("csibReportProcDelay",0.);cp.hopProcMs=1000*x.c.value("csibHopProcDelay",0.);
      cp.messageBytes=x.c.value("csibMessageBytes",0);cp.islBitsPerSecond=x.c.value("islDatarate",1e8);cp.useVote=x.c.value("csibVote",true);
      double d=x.c.value("csibDetectDelay",-1.);if(d<0)d=x.c.value("slsInterval",.2)*x.c.value("slsMissCount",3);
      cp.detectMs=1000*d;csib=std::make_unique<mcsr::CsibModel>(*fixedPlan,cp);
      csib->initialize(p->router(),p->graph());snapshots[0]=p;
      x.schedule(cp.periodSec,[this]{onTimer("periodic");});
      double m=x.c.value("csibMonitorInterval",0.);if(m>0)x.schedule(m,[this]{onTimer("monitor");});
    }
    x.record({{"event","srd"},{"K",K},{"L",L},{"srdOf",p->srdPlan().srdOf}});
  }
  void onTimer(const std::string &kind) override {
    publish(kind);double interval=x.c.value(kind=="periodic"?"csibPeriod":"csibMonitorInterval",0.);
    if(interval>0)x.schedule(x.now+interval,[this,kind]{onTimer(kind);});
  }
  void onNetworkEvent(const J &event) override {
    if(!csib)return;
    if(event.value("event",std::string())=="failure-detected")publish("failure",event["a"],event["b"],event["failedAt"]);
    else if(event.value("event",std::string())=="restore")publish("monitor");
  }
  bool sourceRouted() const override {return true;}
  Plan forward(Packet &packet,int node,bool repair=false) override {
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
      if(auto best=mcsr::selectPath(candidates,service,selection))return {best->sats,best->t.D/1000,sampled,true,packet.plan.front};
    }
    if(groundEpoch!=x.epoch){plans.clear();groundEpoch=x.epoch;}
    auto key=std::make_tuple(p.get(),node,packet.dest);
    if(!repair && f.empty() && plans.count(key))return plans.at(key);
    auto result=p->plan(node,x.exits(packet.dest),service,&f,selection);Plan out;
    if(result.ok){out.path=result.path.sats;out.delay=result.path.t.D/1000;out.front=result.front;out.dataTime=csib?csib->viewAt(node,x.now)->dataTime:sampled;}
    if(!repair && f.empty())plans[key]=out;return out;
  }
  int selectUplink(int src,int dst) override {
    int best=-1;double cost=INFINITY;
    for(auto [sat,up]:x.uplinks(src)) {
      Packet p;p.source=src;p.dest=dst;auto planner=view(sat);auto f=x.failures(sat);
      if(groundEpoch!=x.epoch){plans.clear();groundEpoch=x.epoch;}
      // Access selection is delay-based for all service classes, as in the reference module.
      auto key=std::make_tuple(planner.get(),sat,-dst-1);Plan plan;
      if(plans.count(key))plan=plans.at(key);
      else {auto r=planner->plan(sat,x.exits(dst),mcsr::Service::LS,&f);if(r.ok)plan={r.path.sats,r.path.t.D/1000,sampled,false,r.front};plans[key]=plan;}
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
  long long attempted=0,sent=0,received=0,dropped=0,bytes=0,queued=0;
  double delaySum=0,mean=0,m2=0,maxDelay=0,jitter=0,lastTransit=-1,lastRx=-1,maxGap=0;
  long long hops=0,outages=0,lastSent=0,lastReceived=0,lastDropped=0,lastBytes=0;
  double lastDelaySum=0;
};
struct Link {
  int a=0,b=0; bool active=false,nominal=false,transmitting=false;
  double prop=0,previousProp=0,rate=0,extra=0,extraJ=0,rho=0,availability=1,loss=0,capacityScale=1;
  double lastArrival=-1,lastInterval=-1,jitter=0,jitterAt=-1;
  std::deque<std::shared_ptr<Packet>> queue;
};
struct Failure {int count=0;double since=0;std::vector<double> known;};
struct Event {
  Tick time;long long seq;std::function<void()> action;
  bool operator<(const Event &b)const{return time==b.time?seq>b.seq:time>b.time;}
};
class Simulation {
  J c;std::string out;int n,g;Tick now=0,limit;long long sequence=0,packetId=0,routeId=0,noUplink=0,maxQueue=0,replanned=0,reserved=0;
  unsigned long long trafficHash=14695981039346656037ull;
  double gap=0;std::priority_queue<Event> events;
  std::mt19937 trafficRng,failureRng;
  std::vector<std::unique_ptr<cOrbitA>> orbits;std::vector<cEci> sat;std::vector<Position> ground;
  std::vector<std::unordered_map<int,Link>> links;
  std::vector<std::pair<int,int>> nominalIsls;
  std::map<mcsr::LinkKey,Failure> failures;
  std::map<std::pair<int,int>,FlowStats> flows;
  std::map<std::pair<int,int>,std::vector<int>> lastPaths;
  std::vector<int> lastRoute,srdOf;
  std::set<mcsr::LinkKey> syntheticFailures;
  double congestionSampled=-1;
  std::vector<leocongestion::Zone> zones;
  std::ofstream csv,trace,network,series,failureLog;
  Context context;
  std::unique_ptr<Routing> router;
  double seconds()const{return now/TICKS;}
  double random(){return double(trafficRng())/4294967296.;}
  void schedule(double t,std::function<void()> fn) {
    if(!std::isfinite(t)||t<seconds()-1e-10)throw std::runtime_error("Invalid event time");
    Tick tick=ticks(t);if(tick<limit)events.push({tick,sequence++,std::move(fn)});
  }
  void record(J e) {
    e["time"]=seconds();network<<e.dump()<<'\n';
    if(e.value("event",std::string())=="srd")srdOf=e["srdOf"].get<std::vector<int>>();
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
    for(auto &[key,state]:failures)if(state.count && seconds()+1e-10>=knownAt(state,viewer))f.links.insert(key);
    return f;
  }
  mcsr::Graph graph(int viewer) {
    mcsr::Graph result(n);auto f=knownFailures(viewer);
    bool mc=c["algorithm"].get<std::string>().find("mcsr")==0;
    for(int u=0;u<n;u++)for(auto &[v,l]:links[u])if(v<n && l.nominal && (l.active || failures.count(mcsr::linkKey(u,v))) && !f.blocksLink(u,v)) {
      int cap=c.value("queueCapacity",500);if(cap==0)cap=c.value("routingQueueCapacity",100);
      double a=std::max(0.,1-double(l.queue.size())/cap);a=std::min(a,l.availability);
      double j=seconds()-l.jitterAt<=c.value("jitterWindow",30.)?l.jitter:0;
      if(c.value("jitterSource",std::string("arrival"))=="delayChange")j=std::abs(l.prop-l.previousProp);
      double d=l.prop+((mc||c.value("congestionAware",false))?l.extra:0.);
      result.addDirected(u,v,{1000*d,a,1000*std::hypot(j,l.extraJ)});
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
  void update() {
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
    for(int u=0;u<n+g;u++)for(auto &[v,l]:links[u])if((u>=n||v>=n)&&!l.active)clearQueue(l,"link-down-in-queue");
    context.epoch++;context.now=t;applyCongestion(false);
    schedule(t+c.value("updateInterval",1.),[this]{update();});
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
      return leocongestion::coefficient(z,id,pos.m_Lat/RADS_PER_DEG,pos.m_Lon/RADS_PER_DEG);
    }
    return 0;
  }
  double factor(const J &target,int a,int b) {
    auto p=target.value("params",J::object());auto kind=target.value("kind",std::string());
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
    auto loads=environment?leocongestion::computeLinkLoads(zones,seconds(),nominalIsls,position,q):leocongestion::LinkLoadMap{};
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
    for(auto key:newFailures)if(!syntheticFailures.count(key))fail(int(key>>32),int(key));
    for(auto key:syntheticFailures)if(!newFailures.count(key))fail(int(key>>32),int(key),true);
    syntheticFailures=std::move(newFailures);
  }

  void tracePacket(const std::shared_ptr<Packet> &p,const std::string &event,const std::string &detail) {
    if(c.value("packetTrace",false))trace<<seconds()<<','<<event<<','<<p->source<<','<<p->dest<<','<<detail<<','<<p->id<<','<<p->hops<<'\n';
  }
  FlowStats &flow(const std::shared_ptr<Packet> &p){return flows[{p->source,p->dest}];}
  void drop(const std::shared_ptr<Packet> &p,const std::string &reason) {flow(p).dropped++;dropReasons[reason]++;tracePacket(p,"drop",reason);}
  std::map<std::string,long long> dropReasons;
  void clearQueue(Link &l,const std::string &reason) {while(!l.queue.empty()){auto p=l.queue.front();l.queue.pop_front();flow(p).queued--;drop(p,reason);}}
  void start(Link &l,const std::shared_ptr<Packet> &p) {
    if(!l.active){drop(p,"link-down");return;}
    if(l.loss>0) {
      std::seed_seq seed{unsigned(c.value("seed",0)),unsigned(p->id),unsigned(p->id>>32),unsigned(l.a),unsigned(l.b),0x4c4f5353u};std::mt19937 rng(seed);
      if(double(rng())/4294967296.<l.loss){drop(p,"injected-loss");return;}
    }
    l.transmitting=true;int a=l.a,b=l.b;double delay=l.prop+l.extra;
    Tick duration=std::max<Tick>(1,ticks((p->bytes+24)*8/l.rate));
    schedule((now+duration)/TICKS,[this,p,a,b,delay]{
      auto &l=links[a].at(b);l.transmitting=false;
      schedule(seconds()+delay,[this,p,a,b]{arrive(p,b,a);});
      if(!l.active)clearQueue(l,"link-down-in-queue");
      drain(l);
    });
  }
  void drain(Link &l) {
    while(!l.transmitting&&!l.queue.empty()){auto next=l.queue.front();l.queue.pop_front();flow(next).queued--;start(l,next);}
  }
  void transmit(const std::shared_ptr<Packet> &p,int a,int b) {
    auto l=find(a,b);if(!l||!l->active){drop(p,"link-down");return;}
    if(!l->transmitting&&l->queue.empty()){start(*l,p);return;}
    int cap=c.value("queueCapacity",500);
    if(cap && l->queue.size()>=size_t(cap)){drop(p,"queue-overflow");return;}
    l->queue.push_back(p);flow(p).queued++;maxQueue=std::max(maxQueue,(long long)l->queue.size());
  }
  void writeRoute(const std::shared_ptr<Packet> &p,int node) {
    if(p->plan.path.empty())return;auto key=std::make_pair(p->source,p->dest);
    bool write=c.value("routeRecording",std::string("full"))=="full"||lastPaths[key]!=p->plan.path;
    lastPaths[key]=p->plan.path;lastRoute=p->plan.path;
    if(!write)return;
    double up=0;if(auto l=find(n+p->source,node))up=l->prop;
    csv<<seconds()<<','<<++routeId<<",GS"<<p->source<<','<<p->dest<<','<<p->plan.path.front()<<','<<p->plan.path.back()<<','<<p->plan.path.size()-1<<','<<up*1000<<','<<p->plan.delay*1000<<','<<(up+p->plan.delay)*1000<<',';
    for(size_t i=0;i<p->plan.path.size();i++)csv<<(i?"-":"")<<p->plan.path[i];csv<<'\n';
    tracePacket(p,"route",std::to_string(routeId));
  }
  void dispatch(const std::shared_ptr<Packet> &p,int node,bool afterRepair=false) {
    if(router->sourceRouted()&&!p->plan.path.empty()&&seconds()-p->plannedAt>c.value("flowTimeout",120.)){p->plan=Plan{};p->cursor=0;}
    bool first=p->plan.path.empty();
    if(!router->sourceRouted()||first) {p->plan=router->forward(*p,node);p->plannedAt=seconds();p->cursor=0;}
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
      if(next<n)p->excluded.links.insert(mcsr::linkKey(node,next));
      auto plan=router->forward(*p,node,true);if(plan.path.empty()){drop(p,"no-route");return;}
      replanned++;if(plan.reserved)reserved++;
      double delay=router->sourceRouted()?(plan.reserved?c.value("reserveDelay",.001):c.value("repairDelay",.005)):c.value("spfDelay",.05);
      p->plan=std::move(plan);p->plannedAt=seconds();p->cursor=0;writeRoute(p,node);
      schedule(seconds()+delay,[this,p,node]{dispatch(p,node,true);});return;
    }
    transmit(p,node,next);
  }
  void arrive(const std::shared_ptr<Packet> &p,int node,int from) {
    if(node>=n) {
      auto &f=flow(p);f.received++;f.bytes+=p->bytes;double d=seconds()-p->created/TICKS;
      f.delaySum+=d;double delta=d-f.mean;f.mean+=delta/f.received;f.m2+=delta*(d-f.mean);f.maxDelay=std::max(f.maxDelay,d);f.hops+=std::max(0,p->hops-1);
      if(f.lastTransit>=0)f.jitter+=(std::abs(d-f.lastTransit)-f.jitter)/16.;f.lastTransit=d;
      if(f.lastRx>=0){double gap=seconds()-f.lastRx;f.maxGap=std::max(f.maxGap,gap);if(gap>c["stations"][p->dest].value("gapThreshold",.1))f.outages++;}f.lastRx=seconds();
      tracePacket(p,"receive",std::to_string(d));return;
    }
    if(from<n) {auto l=find(from,node);if(l){if(l->lastArrival>=0){double interval=seconds()-l->lastArrival;if(l->lastInterval>=0)l->jitter+=(std::abs(interval-l->lastInterval)-l->jitter)/16.;l->lastInterval=interval;}l->lastArrival=seconds();l->jitterAt=seconds();}}
    p->current=node;if(++p->hops>c.value("maxHops",64)){drop(p,"max-hops");return;}dispatch(p,node);
  }
  void hashValue(unsigned long long value){for(int i=0;i<8;i++){trafficHash^=value&255;trafficHash*=1099511628211ull;value>>=8;}}
  void send(int source) {
    auto &s=c["stations"][source];int dest=s.value("dstStation",-1);
    if(dest<0) {
      std::vector<int> candidates;auto type=s.value("dstType",std::string("station"));
      for(int j=0;j<g;j++)if(j!=source&&(type=="any"||(c["stations"][j].value("mobile",false)?"mobile":"station")==type))candidates.push_back(j);
      if(!candidates.empty())dest=candidates[std::min(candidates.size()-1,size_t(random()*candidates.size()))];
    }
    auto &f=flows[{source,dest}];f.attempted++;long long id=++packetId;
    for(auto v:{(unsigned long long)id,(unsigned long long)now,(unsigned long long)source,(unsigned long long)dest,(unsigned long long)s["packetSize"].get<int>()})hashValue(v);
    int best=dest<0?-1:router->selectUplink(source,dest);
    if(best<0)noUplink++;
    else {f.sent++;auto p=std::make_shared<Packet>();p->id=id;p->source=source;p->dest=dest;p->bytes=s["packetSize"];p->created=now;tracePacket(p,"send",std::to_string(p->bytes));transmit(p,n+source,best);}
    Tick next=now+ticks(s["sendInterval"].get<double>());
    if(next<=now)throw std::runtime_error("Traffic interval is below time resolution");
    if(next<limit&&next<ticks(s.value("endTime",limit/TICKS)))schedule(next/TICKS,[this,source]{send(source);});
  }
  void fail(int a,int b,bool restore=false) {
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
    for(auto [u,v]:{std::make_pair(a,b),std::make_pair(b,a)}){auto &l=links[u].at(v);l.active=restore&&l.nominal;if(!restore)clearQueue(l,"link-down-in-queue");}
    context.islEpoch++;context.epoch++;record({{"event",restore?"restore":"failure"},{"a",a},{"b",b}});
    failureLog<<seconds()<<','<<(restore?"restore":"fail")<<','<<a<<','<<b<<'\n';
    if(router) {
      if(restore)router->onNetworkEvent({{"event","restore"}});
      else {double failedAt=seconds(),detect=c.value("csibDetectDelay",-1.);if(detect<0)detect=c.value("slsInterval",.2)*c.value("slsMissCount",3);
        schedule(seconds()+detect,[this,a,b,failedAt]{context.epoch++;router->onNetworkEvent({{"event","failure-detected"},{"a",a},{"b",b},{"failedAt",failedAt}});});}
    }
  }
  void failureSchedule() {
    for(const auto &e:c.value("failureReplayEvents",J::array())) {double t=e["time"];int a=e["a"],b=e["b"];bool up=e.value("event",std::string())=="restore";schedule(t,[this,a,b,up]{fail(a,b,up);});}
    if(!c.value("failureReplayEvents",J::array()).empty())return;
    std::string text=c.value("failureSchedule",std::string());for(char &ch:text)if(ch==','||ch==';')ch=' ';
    std::istringstream input(text);std::string item;
    while(input>>item) {
      std::smatch m;if(!std::regex_match(item,m,std::regex(R"((route|([0-9]+)-([0-9]+))@([0-9.eE+-]+?)(?:\+([0-9.eE-]+))?)")))throw std::runtime_error("Invalid failure schedule: "+item);
      bool route=m[1]=="route";int a=route?-1:std::stoi(m[2]),b=route?-1:std::stoi(m[3]);double t=std::stod(m[4]),duration=m[5].matched?std::stod(m[5]):0;
      schedule(t,[this,a,b,route,duration]()mutable{if(route){if(lastRoute.size()<2){record({{"event","failure-skipped"},{"reason","no-active-route"}});return;}int k=(lastRoute.size()-1)/2;a=lastRoute[k];b=lastRoute[k+1];}fail(a,b);if(duration>0)schedule(seconds()+duration,[this,a,b]{fail(a,b,true);});});
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
      series<<seconds()<<','<<key.first<<','<<key.second<<','<<f.sent-f.lastSent<<','<<rx<<','<<f.dropped-f.lastDropped<<','<<(f.bytes-f.lastBytes)*8/window/1e6<<','<<delay<<','<<f.queued<<'\n';
      f.lastSent=f.sent;f.lastReceived=f.received;f.lastDropped=f.dropped;f.lastBytes=f.bytes;f.lastDelaySum=f.delaySum;
      sent+=f.sent;received+=f.received;dropped+=f.dropped;
    }
    J status={{"time",seconds()},{"limit",limit/TICKS},{"sent",sent},{"received",received},{"dropped",dropped},{"progress",seconds()/(limit/TICKS)}};
    std::ofstream(out+"/progress.tmp")<<status.dump();
    std::error_code ec;std::filesystem::rename(out+"/progress.tmp",out+"/progress.json",ec);
    if(ec){std::filesystem::remove(out+"/progress.json",ec);std::filesystem::rename(out+"/progress.tmp",out+"/progress.json",ec);}
    schedule(seconds()+1,[this]{sample();});
  }
public:
  Simulation(J cfg,const std::string &directory):c(std::move(cfg)),out(directory),n(c["numSatellites"]),g(c["stations"].size()),limit(ticks(c["simTimeLimit"])),trafficRng(c.value("seed",23542)),failureRng(c.value("failureSeed",4242)),sat(n),ground(g),links(n+g) {
    if(n<6||n>20000||g<1||g>5000||limit<=0||c["planes"].get<int>()<2||n%c["planes"].get<int>())throw std::runtime_error("Invalid engine dimensions");
    auto year=[](int y){return y<57?y+2000:y+1900;};cJulian base(year(c.value("baseYear",20)),c.value("baseDay",1.5)),epoch(year(c.value("epochYear",20)),c.value("epochDay",1.5));gap=base.spanSec(epoch);
    int per=n/c["planes"].get<int>();for(int i=0;i<n;i++)orbits.emplace_back(new cOrbitA("sat",c.value("epochYear",20),c.value("epochDay",1.5),c["altitude"],c.value("eccentricity",.000001),c["inclination"],std::fmod((i/per)*360.*c["phasing"].get<int>()/n+(i%per)*360./per,360.),c.value("bstar",.00042243),.00001846,i,c["planes"],per,(i/per)*360./c["planes"].get<int>(),0));
    csv.open(out+"/routes.csv");trace.open(out+"/events.csv");network.open(out+"/network-events.jsonl");series.open(out+"/timeseries.csv");failureLog.open(out+"/failures.csv");
    if(!csv||!trace||!network||!series||!failureLog)throw std::runtime_error("Cannot create output files");
    csv<<std::setprecision(14)<<"time,route,source,station,firstSat,exitSat,satHops,uplinkMs,satToStationMs,totalMs,path\n";
    trace<<std::setprecision(14)<<"time,event,source,destination,detail,packet_id,hops\n";
    series<<std::setprecision(14)<<"time,source,destination,sent,received,dropped,throughputMbps,delayMs,queued\n";failureLog<<std::setprecision(14)<<"time,event,satA,satB\n";
    context.c=c;context.n=n;context.g=g;context.graph=[this](int v){return graph(v);};context.exits=[this](int d){return exits(d);};context.uplinks=[this](int s){return uplinks(s);};context.failures=[this](int v){return knownFailures(v);};context.schedule=[this](double t,auto f){schedule(t,f);};context.record=[this](J e){record(e);};
    zones=leocongestion::parseZones(c.value("congestionZones",std::string()));update();
    auto factories=routingFactories();auto name=c.value("algorithm",std::string("dijkstra-delay"));if(!factories.count(name))throw std::runtime_error("Unknown routing algorithm");router=factories.at(name)(context);
    if(srdOf.empty() && c.value("scenario",J::object()).dump().find("\"srd\"")!=std::string::npos){mcsr::PlannerConfig pc;pc.planes=c["planes"];pc.satsPerPlane=per;auto p=mcsr::Planner::create(pc,graph(-1));srdOf=p->srdPlan().srdOf;}
    failureSchedule();
    for(const auto &e:c.value("scenario",J::object()).value("entries",J::array()))for(auto key:{"start_s","end_s"})if(e.contains(key)&&!e[key].is_null()){double t=e[key];if(t>=0)schedule(t,[this]{applyCongestion();});}
    for(auto &z:zones){schedule(z.start,[this]{applyCongestion();});if(z.duration>0)schedule(z.start+z.duration,[this]{applyCongestion();});}
    for(int i=0;i<g;i++)if(c["stations"][i].value("enabled",false)) {
      auto &s=c["stations"][i];double start=s.contains("startTime")?s["startTime"].get<double>():s.contains("startRange")?s["startRange"][0].get<double>()+random()*(s["startRange"][1].get<double>()-s["startRange"][0].get<double>()):1+4*random();
      if(ticks(s["sendInterval"].get<double>())<1)throw std::runtime_error("Traffic interval below resolution");if(start<s.value("endTime",limit/TICKS))schedule(start,[this,i]{send(i);});
    }
    schedule(1,[this]{sample();});
  }
  void run() {
    while(!events.empty()){auto e=events.top();events.pop();now=e.time;context.now=seconds();e.action();}
    now=limit;context.now=seconds();sample();J rows=J::array();long long sent=0,received=0,dropped=0,attempted=0,bytes=0;double delaySum=0;
    for(auto &[key,f]:flows){sent+=f.sent;received+=f.received;dropped+=f.dropped;attempted+=f.attempted;bytes+=f.bytes;delaySum+=f.delaySum;
      rows.push_back({{"source",key.first},{"destination",key.second},{"attempted",f.attempted},{"sent",f.sent},{"received",f.received},{"dropped",f.dropped},{"inFlight",f.sent-f.received-f.dropped},{"deliveryRate",f.sent?100.*f.received/f.sent:0},{"offeredDeliveryRate",f.attempted?100.*f.received/f.attempted:0},{"meanDelayMs",1000*f.mean},{"delayStdMs",f.received>1?1000*std::sqrt(f.m2/(f.received-1)):0},{"maxDelayMs",f.maxDelay*1000},{"jitterMs",f.jitter*1000},{"meanHops",f.received?double(f.hops)/f.received:0},{"maxGapSeconds",f.maxGap},{"outages",f.outages},{"throughputMbps",f.bytes*8/(limit/TICKS)/1e6}});}
    if(received+dropped>sent)throw std::runtime_error("Packet conservation violated");
    std::ostringstream hash;hash<<std::hex<<trafficHash;
    J result={{"engine","leo-native"},{"version","2.0.0"},{"algorithm",c["algorithm"]},{"attempted",attempted},{"sent",sent},{"received",received},{"dropped",dropped},{"inFlight",sent-received-dropped},{"noUplink",noUplink},{"delaySumSeconds",delaySum},{"meanDelayMs",received?1000*delaySum/received:0},{"deliveryRate",sent?100.*received/sent:0},{"throughputMbps",bytes*8/(limit/TICKS)/1e6},{"maxQueuePackets",maxQueue},{"replanned",replanned},{"reservedSwitches",reserved},{"trafficHash",hash.str()},{"trafficHashAlgorithm","fnv1a64"},{"dropReasons",dropReasons},{"flows",rows},{"routeRecording",c.value("routeRecording",std::string("full"))},{"packetTrace",c.value("packetTrace",false)},{"experimental",c.value("experimental",false)||!c.value("scenario",J::object()).value("entries",J::array()).empty()||!zones.empty()},{"congestionMode",c.value("congestionMode",std::string("actual"))}};
    std::ofstream(out+"/summary.json")<<result.dump(2);
  }
};
int main(int argc,char **argv) {
  try{if(argc!=3)throw std::runtime_error("Usage: engine.exe input.json output-directory");J c;std::ifstream(argv[1])>>c;Simulation simulation(c,argv[2]);simulation.run();return 0;}
  catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
