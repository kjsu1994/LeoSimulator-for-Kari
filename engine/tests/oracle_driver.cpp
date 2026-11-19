// SPDX-License-Identifier: GPL-3.0-only
#include "../vendor/json.hpp"
#include "../vendor/routing/mcsr/router.h"
#include <iostream>
using J=nlohmann::json;
J path(const mcsr::DomPath* p){if(!p)return nullptr;return {{"path",p->sats},{"tuple",{p->t.D,p->t.A,p->t.J}}};}
int main(){
  J cases;std::cin>>cases;J results=J::array();
  for(const auto &c:cases){
    int n=c["n"],s=c["source"],d=c["destination"];mcsr::Graph g(n);
    for(const auto &e:c["edges"])g.addDirected(e[0],e[1],{e[2],e[3],e[4]});
    auto plan=mcsr::makePlan(c["planes"],c["perPlane"],c["K"],c["L"]);
    mcsr::OpCounts counts;mcsr::OpScope scope(&counts);
    mcsr::Router router(c["neighbors"].get<std::vector<std::vector<int>>>(),g,plan,c.value("labelCap",100000));
    auto front=router.dominantPaths(s,d);J paths=J::array();for(const auto &p:front)paths.push_back(path(&p));
    mcsr::SelectionParams p;p.hrJitterMs=c.value("hrJitterMs",15.);p.tsDelayBoundMs=c.value("tsDelayBoundMs",1e300);
    results.push_back({{"front",paths},{"srdOf",plan.srdOf},{"capHits",counts.capEvict+counts.capReject},
      {"LS",path(mcsr::selectPath(front,mcsr::Service::LS,p))},{"TS",path(mcsr::selectPath(front,mcsr::Service::TS,p))},{"HR",path(mcsr::selectPath(front,mcsr::Service::HR,p))}});
  }
  std::cout<<results.dump();
}
