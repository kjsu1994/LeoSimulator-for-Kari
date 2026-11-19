// SPDX-License-Identifier: GPL-3.0-only
#include "vendor/json.hpp"
#include "vendor/routing/mcsr/router.h"
#include <queue>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <cmath>
#include <cassert>
using J=nlohmann::json;
#include "control.h"
int main(){
  ControlAssembly a;a.parts.resize(3,false);
  assert(!a.accept(2));assert(!a.accept(0));assert(!a.accept(2));assert(a.received==2);assert(a.accept(1));assert(!a.accept(1));
  ControlAssembly lost;lost.parts.resize(2,false);assert(!lost.accept(0));lost.failed=true;assert(!lost.accept(1));assert(!lost.done);
  using Hop=std::tuple<int,int,ControlPlane::Msg,int>;std::queue<Hop> hops;int sends=0;
  J cfg={{"algorithm","mcsr-ls"},{"csibEnabled",true}};
  ControlPlane plane(cfg,4,{{0,1},{1,2},{2,3}},{0,0,0,0},{3},[]{return 0.;},[](double,std::function<void()>){},
    [&](int from,int to,ControlPlane::Msg m,int ttl){hops.push({from,to,m,ttl});sends++;},
    [](int u){std::vector<LinkReport> out;for(int v=0;v<4;v++)if(std::abs(u-v)==1)out.push_back({u,v});return out;},[](J){},[](int,int owner){return owner;},[](int,int){return 1.;},[]{return 0L;});
  auto adv=std::make_shared<Advertisement>(0,2,0.,std::vector<LinkReport>{{0,1,true,2.,1.,0.}});
  auto message=std::make_shared<ControlMessage>();message->kind="report";message->origin=0;message->target=3;message->region=0;message->round=1;message->nonce=100;message->records={adv};
  plane.receive(0,-1,message,4);
  while(!hops.empty()){auto [from,to,m,ttl]=hops.front();hops.pop();plane.receive(to,from,m,ttl);}
  assert(sends==3);assert(plane.views[1].info.empty());assert(plane.views[2].info.empty());assert(plane.views[3].info.count(0));assert(plane.revision(3)!=0);
  Advertisement refreshed=*adv;refreshed.version++;
  assert(refreshed.contentHash==adv->contentHash);
  assert(Advertisement(0,3,0.,{{0,1,true,3.,1.,0.}}).contentHash!=adv->contentHash);
}
