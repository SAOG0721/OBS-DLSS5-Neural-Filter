// SPDX-License-Identifier: GPL-3.0-only
#include "BackendPolicy.h"
#include <iostream>
#include <stdexcept>
using namespace neural;
void require(bool value,const std::string& message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        detail::InputClock clock; FrameKey key{1,1,1,{512,288}};
        require(clock.observe(key,1000000000,false),"First cycle was not accepted");
        const auto first=clock.serial,epoch=clock.epoch;
        for (int i=0;i<20;++i) require(!clock.observe(key,1000000000,false),"Preview polling created an input cycle");
        require(clock.serial==first,"Preview polling advanced residual age");
        ++key.cycle; clock.observe(key,1016666667,false);
        require(clock.serial==first+1 && clock.epoch==epoch,"Independent cycle was not counted once");
        ++key.inputRevision; clock.observe(key,1033333334,false);
        require(clock.epoch==epoch+1,"Source revision retained the old residual epoch");
        --key.cycle; clock.observe(key,1050000001,false);
        require(clock.epoch==epoch+2,"Backward video clock retained the old residual epoch");
        ++key.cycle; clock.observe(key,1400000001,false);
        require(clock.epoch==epoch+3,"Long discontinuity retained the old residual epoch");
        require(detail::safeMinimumPercent({256,144})==100,"Minimum-size source can enter passthrough when adapting");
        require(detail::safeMinimumPercent({512,288})==50,"Small source safe tier is wrong");
        require(detail::safeMinimumPercent({1920,1080})==25,"HD safe tier is wrong");
        Settings settings; settings.scaling=true; settings.resolutionControl=0; settings.percent=63;
        detail::AdaptiveResolution adaptive;
        for (unsigned i=1;i<=24;++i) adaptive.observe(i,{1920,1080},settings,i,25,0,0,16);
        require(adaptive.proposal==95,"Over-budget work did not propose one 5% tier");
        require(settings.percent==63,"Adaptive policy overwrote saved manual percentage");
        adaptive.switched(95);
        for (unsigned i=25;i<=144;++i) adaptive.observe(i,{1920,1080},settings,i,3,0,0,16);
        require(adaptive.proposal==100,"Sustained headroom did not recover a tier");
        detail::AdaptiveResolution duplicate;
        for (unsigned i=0;i<500;++i) duplicate.observe(1,{1920,1080},settings,i+1,100,0,0,16);
        require(duplicate.proposal==100,"Render requests were mistaken for adaptive measurement cycles");
        detail::AdaptiveResolution minimum;
        for (unsigned i=1;i<=72;++i) minimum.observe(i,{256,144},settings,i,100,0,i,100);
        require(minimum.proposal==100,"Adaptive minimum extent was bypassed");
        settings.processingMode=1; settings.updateInterval=4;
        detail::AdaptiveResolution alternating;
        for (unsigned i=1;i<=24;++i) alternating.observe(i,{1920,1080},settings,i,40,1,0,16);
        require(alternating.proposal==100,"Alternating inference cost was charged on every input cycle");
        settings.processingMode=0; settings.targetFps=30;
        detail::AdaptiveResolution thirty;
        for (unsigned i=1;i<=24;++i) thirty.observe(i,{1920,1080},settings,i,25,0,0,32);
        require(thirty.proposal==100,"Adaptive budget ignored the host video rate");

        // Magpie 0.6.13 adaptive resolution trial, auto-restore, blocked-tier, and fixed-bottleneck contracts:
        settings.targetFps=60;
        // 1. Downshift trial accepted when scalable GPU cost drops meaningfully.
        detail::AdaptiveResolution trialAccepted;
        for (unsigned i=1;i<=24;++i) trialAccepted.observe(i,{1920,1080},settings,i,18,0,0,16,16,2);
        require(trialAccepted.proposal==95,"Downshift trial was not proposed");
        trialAccepted.switched(95);
        for (unsigned i=25;i<=72;++i) trialAccepted.observe(i,{1920,1080},settings,i,13,0,0,16,11,2);
        require(trialAccepted.percent==95 && trialAccepted.proposal==95 && !trialAccepted.trial.has_value(),
            "Beneficial downshift trial was not accepted: "+trialAccepted.reason);

        // 2. Downshift trial with NO relief auto-restores to 100% and blocks immediate retry to 95%.
        detail::AdaptiveResolution noBenefit;
        for (unsigned i=1;i<=24;++i) noBenefit.observe(i,{1920,1080},settings,i,18,0,0,16,16,2);
        require(noBenefit.proposal==95,"Initial downshift proposal failed");
        noBenefit.switched(95);
        for (unsigned i=25;i<=72;++i) noBenefit.observe(i,{1920,1080},settings,i,18,0,0,16,16,2);
        require(noBenefit.proposal==100 && noBenefit.restoring,
            "Unbeneficial downshift trial did not trigger auto-restore: "+noBenefit.reason);
        noBenefit.switched(100);
        for (unsigned i=73;i<=120;++i) noBenefit.observe(i,{1920,1080},settings,i,18,0,0,16,16,2);
        require(noBenefit.proposal==100,"Blocked downshift tier retried immediately without load worsening");

        // 3. Blocked tier unblocks after 2 consecutive windows of genuine load increase.
        for (unsigned i=121;i<=168;++i) noBenefit.observe(i,{1920,1080},settings,i,23,0,0,16,21,2);
        for (unsigned i=169;i<=192;++i) noBenefit.observe(i,{1920,1080},settings,i,23,0,0,16,21,2);
        require(noBenefit.proposal==95,"Blocked tier did not unblock after sustained load increase: "+noBenefit.reason);

        // 4. Fixed-bottleneck guard prevents downshift when scalable GPU work is small and fixed work dominates.
        detail::AdaptiveResolution fixedBottleneck;
        for (unsigned i=1;i<=48;++i) fixedBottleneck.observe(i,{1920,1080},settings,i,16,0,0,16,2,14);
        require(fixedBottleneck.proposal==100,"Fixed-shader bottleneck triggered pointless inference downshift");

        // 5. Upshift trial that immediately overloads restores to previous tier and blocks immediate re-upshift.
        detail::AdaptiveResolution badUpgrade;
        badUpgrade.switched(95);
        for (unsigned i=1;i<=120;++i) badUpgrade.observe(i,{1920,1080},settings,i,6,0,0,16,4,2);
        require(badUpgrade.proposal==100,"Headroom did not propose upshift trial");
        badUpgrade.switched(100);
        for (unsigned i=121;i<=168;++i) badUpgrade.observe(i,{1920,1080},settings,i,18,0,0,16,16,2);
        require(badUpgrade.proposal==95 && badUpgrade.restoring,
            "Overloaded upshift trial did not restore to 95%: "+badUpgrade.reason);
        badUpgrade.switched(95);
        for (unsigned i=169;i<=312;++i) badUpgrade.observe(i,{1920,1080},settings,i,6,0,0,16,4,2);
        require(badUpgrade.proposal==95,"Blocked upshift tier retried without meaningful headroom drop beyond failure baseline");

        std::cout << "OBS cycle identity, epochs, safe adaptive tiers, manual preservation, trial/restore, fixed-bottleneck and FPS budgets passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
