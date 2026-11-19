// SPDX-License-Identifier: GPL-3.0-only
#define LEO_KERNEL_TEST
#include "../main.cpp"
#include <cassert>
// Analytic FIFO fixtures: no orbit, routing or control events are executed.
int main(int argc,char **argv){
  if(argc!=2)return 2;std::string out=argv[1];
  auto config=[](){J station={{"lat",0},{"lon",0},{"enabled",false},{"mobile",false},{"packetSize",976},{"sendInterval",1.}};
    return J{{"numSatellites",12},{"planes",4},{"phasing",1},{"altitude",550},{"inclination",53},{"simTimeLimit",5.},{"minimumElevation",10.},{"algorithm","dijkstra-delay"},{"controlMode","time"},{"stations",J::array({station,station})},{"queueCapacity",1}};};
  for(int variant=0;variant<5;variant++){
    Simulation sim(config(),out);while(!sim.events.empty())sim.events.pop();sim.now=0;sim.sequence=0;
    for(auto &row:sim.links)row.clear();Link link;link.a=0;link.b=13;link.active=true;link.prop=.01;link.rate=8000;sim.links[0][13]=link;
    auto &stats=sim.flows[{0,1}];stats.attempted=stats.sent=variant==0?3:1;
    auto send=[&](int id){auto p=std::make_shared<Packet>();p->id=id;p->source=0;p->dest=1;p->bytes=976;sim.transmit(p,0,13);};
    auto fail=[&](){auto &l=sim.links[0][13];l.active=false;sim.abort(l);sim.clearQueue(l,"test-failure");};
    // Equal-time events follow insertion order; a frame already propagating may arrive.
    if(variant==2)sim.schedule(1.,fail);
    send(1);if(variant==0){send(2);send(3);}
    if(variant==1)sim.schedule(.5,fail);
    if(variant==3)sim.schedule(1.,fail);
    if(variant==4){sim.measuring=true;sim.measurementStart=0;sim.measurementEnd=ticks(1.);sim.cohort[{0,1}].attempted=sim.cohort[{0,1}].sent=1;}
    sim.run();
    if(variant==0){assert(stats.received==2&&stats.dropped==1);assert(std::abs(stats.mean-1.51)<2e-11);}
    if(variant==1||variant==2)assert(stats.received==0&&stats.dropped==1);
    if(variant==3||variant==4){assert(stats.received==1&&stats.dropped==0);assert(std::abs(stats.mean-1.01)<2e-11);}
    if(variant==4)assert((sim.cohort[{0,1}].received==1));
  }
  Context x;x.n=3;x.g=1;x.c={{"algorithm","dijkstra-delay"}};
  x.failures=[](int){return mcsr::FailureSet{};};
  x.graph=[](int){mcsr::Graph g(3);g.addDirected(0,1,{1,1,0});g.addDirected(1,2,{2,1,0});g.addDirected(1,0,{10,1,0});g.addDirected(2,1,{20,1,0});return g;};
  x.exits=[](int){return std::vector<mcsr::Planner::Exit>{{2,{.5,1,0}}};};
  DijkstraRouting routing(x);Packet packet;packet.dest=0;auto plan=routing.forward(packet,0);
  assert((plan.path==std::vector<int>{0,1,2}));assert(std::abs(plan.delay-.0035)<1e-12);
  std::cout<<"Analytic kernel: FIFO, overflow, serialization, propagation, failure ordering, drain passed\n";
}
