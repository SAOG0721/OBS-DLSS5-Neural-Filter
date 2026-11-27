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
        check(s.modelPreset==1 && s.processingMode==0 && s.updateInterval==2 && s.residualTransfer==0,
              "New and legacy settings lost the HT/every-frame defaults");
        check(!s.scaling && s.percent==100 && s.resolutionControl==1 && s.midtone==1 && s.chromaProtection==0,
              "New and legacy settings changed inference/color defaults");
        check(s.responseMs==80,"Legacy anti-flicker response default changed");
        s.pass[2].tone=1.5f; check(s.firstDirty(previous)==1,"Hidden pass invalidated active chain");
        s.passes=3; check(s.firstDirty(previous)==1,"Increasing passes did not start at suffix");
        previous=s; s.pass[1].structure=1.7f; check(s.firstDirty(previous)==1,"Wrong dirty suffix");
        previous=s; s.mix=.5f; check(s.firstDirty(previous)==3,"Outlet adjustment invalidated NR");
        previous=s; s.midtone=1.6f;
        check(s.firstDirty(previous)==3 && s.resourceKey()==previous.resourceKey() && s.outletKey()!=previous.outletKey(),
              "Midtone adjustment lost outlet-only cache behavior");
        previous=s; s.chromaProtection=.7f;
        check(s.firstDirty(previous)==3 && s.resourceKey()==previous.resourceKey() && s.outletKey()!=previous.outletKey(),
              "Chroma protection lost outlet-only cache behavior");
        previous=s; s.modelPreset=2;
        check(s.firstDirty(previous)==0 && s.resourceKey()!=previous.resourceKey(),"Model switch reused old-model NR");
        previous=s; s.processingMode=1;
        check(s.resourceKey()!=previous.resourceKey(),"Processing mode missing from structural settings");
        s.mix=0; check(s.bypass(),"Mix zero was not bypass");
        s.mix=1; s.strength=0; check(s.bypass(),"Zero strength was not bypass");
        s.strength=1; s.view=2; check(!s.bypass(),"Split view bypassed NR");
        s.strength=std::numeric_limits<float>::quiet_NaN(); s.af=99; s.percent=-12; s.passes=0;
        s.sanitize(); check(s.strength==1 && s.af==4 && s.percent==25 && s.passes==1,"Config sanitization failed");
        s.midtone=std::numeric_limits<float>::infinity(); s.chromaProtection=std::numeric_limits<float>::quiet_NaN();
        s.targetFps=0; s.modelPreset=90; s.resolutionControl=90; s.processingMode=-1; s.updateInterval=90; s.residualTransfer=90;
        s.sanitize();
        check(s.midtone==1 && s.chromaProtection==0 && s.targetFps==60 && s.modelPreset==2 &&
              s.resolutionControl==1 && s.processingMode==0 && s.updateInterval==4 && s.residualTransfer==1,
              "New settings sanitization failed");
        s.flow=0; s.af=3; s.responseMs=10; s.sanitize();
        check(s.af==3 && s.responseMs==10,"NR flow off removed independent flow anti-flicker or legacy 10 ms response");
        s.responseMs=500; s.sanitize(); check(s.responseMs==500,"Legacy 500 ms response rejected");
        s.responseMs=std::numeric_limits<float>::quiet_NaN(); s.sanitize(); check(s.responseMs==80,"Invalid response did not use legacy default");
        s.scaling=true; s.percent=25; check(inferenceExtent({1920,1080},s)==Extent{480,270},"Inference sizing incorrect");
        s.resolutionControl=0;
        check(s.adaptiveResolution() && inferenceExtent({1920,1080},s)==Extent{1920,1080} && s.percent==25,
              "Adaptive initial extent changed the saved manual ratio");
        s.scaling=false;
        check(!s.adaptiveResolution() && inferenceExtent({1920,1080},s)==Extent{1920,1080},"Disabled scaling activated adaptive resolution");
        s.scaling=true; s.resolutionControl=1;
        check(inferenceExtent({1920,1080},s)==Extent{480,270},"Returning to manual did not restore the saved ratio");
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
