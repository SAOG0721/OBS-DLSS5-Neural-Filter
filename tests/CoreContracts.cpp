// SPDX-License-Identifier: GPL-3.0-only
#include "Settings.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace neural;
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        Settings s,previous;
        s.pass[2].tone=1.5f; check(s.firstDirty(previous)==1,"Hidden pass invalidated active chain");
        s.passes=3; check(s.firstDirty(previous)==1,"Increasing passes did not start at suffix");
        previous=s; s.pass[1].structure=1.7f; check(s.firstDirty(previous)==1,"Wrong dirty suffix");
        previous=s; s.mix=.5f; check(s.firstDirty(previous)==3,"Outlet adjustment invalidated NR");
        s.mix=0; check(s.bypass(),"Mix zero was not bypass");
        s.mix=1; s.strength=0; check(s.bypass(),"Zero strength was not bypass");
        s.strength=1; s.view=2; check(!s.bypass(),"Split view bypassed NR");
        s.strength=std::numeric_limits<float>::quiet_NaN(); s.af=99; s.percent=-12; s.passes=0;
        s.sanitize(); check(s.strength==1 && s.af==4 && s.percent==25 && s.passes==1,"Config sanitization failed");
        s.scaling=true; s.percent=25; check(inferenceExtent({1920,1080},s)==Extent{480,270},"Inference sizing incorrect");
        check(!safeExtent({32,18}) && safeExtent({3840,2160}) && !safeExtent({7680,4320}),"Safety envelope failed");
        check(!temporalPair(1,1,false) && !temporalPair(2,1,false),"Duplicate or reversed time advanced");
        check(!temporalPair(1,300000001,false) && !temporalPair(1,16000001,true),"Long gap/reset reused history");
        check(temporalPair(1,16000001,false),"Valid temporal pair rejected");
        const float a=historyWeight(1,16666668,80,1),b=historyWeight(1,33333335,80,1);
        check(std::abs(a*a-b)<1e-5f,"Temporal weighting depends on render count");
        check(historyWeight(4,4,80,1)==0,"Duplicate render accumulated temporal history");
        FrameKey key{1,2,3,{1920,1080}}, changed=key;
        changed.generation++; check(key!=changed,"Resource generation missing from frame identity");
        std::cout << "CoreContracts: behavior checks passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
