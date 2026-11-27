// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Settings.h"
#include <array>
#include <optional>

namespace neural::detail {
// A cycle is an OBS video input identity, not a render request. This clock is
// shared by inference cadence and residual expiry, including dropped work.
struct InputClock {
    FrameKey key{};
    std::uint64_t serial=0,epoch=1,time=0;
    bool observed=false;
    bool observe(FrameKey current,std::uint64_t now,bool reset) noexcept {
        const bool changed=!observed || current.cycle!=key.cycle ||
            current.generation!=key.generation || current.inputRevision!=key.inputRevision;
        const bool discontinuity=observed && (current.generation!=key.generation ||
            current.inputRevision!=key.inputRevision || (changed &&
            (current.cycle<key.cycle || !temporalPair(time,now,false))));
        if (reset || discontinuity) ++epoch;
        if (changed) { ++serial; key=current; time=now; observed=true; }
        return changed;
    }
};
inline int safeMinimumPercent(Extent source) noexcept {
    for (int percent=25;percent<=100;percent+=5) {
        const Extent candidate{static_cast<unsigned>(static_cast<std::uint64_t>(source.width)*percent/100),
            static_cast<unsigned>(static_cast<std::uint64_t>(source.height)*percent/100)};
        if (safeExtent(candidate)) return percent;
    }
    return 100;
}
struct AdaptiveResolution {
    static constexpr unsigned kTierCount=16;
    struct FailedTier {
        double referenceWork=0,referenceScalable=0,referenceBusyRate=0;
        unsigned retryWindows=0;
        bool blocked=false;
    };
    struct Trial {
        int from=100,to=100;
        bool up=false;
        double baselineWork=0,baselineScalable=0,baselineAge=0,baselineBusyRate=0;
        unsigned windows=0,good=0,bad=0;
    };
    int percent=100,proposal=100;
    unsigned cycles=0,samples=0,headroomWindows=0;
    std::uint64_t lastCycle=0,lastCompleted=0,previousBusy=0;
    double totalMs=0,totalScalableMs=0,totalFixedMs=0;
    std::optional<Trial> trial,pendingTrial;
    std::array<FailedTier,kTierCount> failedDown{},failedUp{};
    bool restoring=false;
    std::string reason="manual";
    static unsigned tierIndex(int value) noexcept {
        return static_cast<unsigned>(std::clamp((value-25)/5,0,static_cast<int>(kTierCount-1)));
    }
    void clearWindow() noexcept { cycles=samples=0; totalMs=totalScalableMs=totalFixedMs=0; }
    void invalidateMeasurements(const char* why="measurements reset") {
        clearWindow(); headroomWindows=0;
        trial.reset(); pendingTrial.reset();
        failedDown={}; failedUp={}; restoring=false;
        proposal=percent;
        reason=why;
    }
    void switched(int value) noexcept {
        const bool wasRestoring=restoring;
        percent=proposal=value; clearWindow();
        if (wasRestoring) {
            restoring=false; trial.reset(); pendingTrial.reset(); headroomWindows=0;
            reason="restored "+std::to_string(value)+"%";
            return;
        }
        if (pendingTrial && pendingTrial->to==value) {
            trial=pendingTrial; pendingTrial.reset();
            reason=(trial->up ? "verifying upgrade " : "verifying trial ")+std::to_string(value)+"%";
        } else {
            trial.reset(); pendingTrial.reset(); headroomWindows=0;
            reason="active "+std::to_string(value)+"%";
        }
    }
    // Sampling is keyed to completed full observations and independent input
    // cycles. Preview polling never changes the measurement window.
    void observe(std::uint64_t cycle,Extent source,const Settings& settings,
                 std::uint64_t complete,double fullMs,double lightMs,
                 std::uint64_t busy,double outputAgeMs,
                 double scalableFullMs=-1.0,double fixedFullMs=0.0) {
        if (!(settings.scaling && settings.resolutionControl==0)) {
            if (reason!="manual") invalidateMeasurements("manual");
            else { proposal=percent; clearWindow(); headroomWindows=0; }
            return;
        }
        if (cycle==lastCycle) return;
        lastCycle=cycle; ++cycles;
        if (complete!=lastCompleted && fullMs>0) {
            const double interval=settings.processingMode ? settings.updateInterval : 1;
            const double scalable=scalableFullMs>=0 ? scalableFullMs : fullMs;
            const double fixed=scalableFullMs>=0 ? fixedFullMs : 0.0;
            totalMs+=fullMs/interval+(settings.processingMode ? lightMs : 0);
            totalScalableMs+=scalable/interval;
            totalFixedMs+=fixed/interval+(settings.processingMode ? lightMs : 0);
            ++samples; lastCompleted=complete;
        }
        if (cycles<24 || samples<4 || proposal!=percent) return;
        const double avgWork=totalMs/samples;
        const double avgScalable=totalScalableMs/samples;
        const double avgFixed=totalFixedMs/samples;
        const double budget=1000.0/std::clamp(static_cast<double>(settings.targetFps),1.0,240.0);
        const auto busyDelta=busy>=previousBusy ? busy-previousBusy : 0;
        const double busyRate=static_cast<double>(busyDelta)/cycles;
        const bool pressure=avgWork>budget*.85 || busyDelta>cycles/3 || outputAgeMs>budget*3;
        const bool headroom=avgWork<budget*.58 && busyDelta==0 && outputAgeMs<budget*2;
        const bool fixedBottleneck=pressure && busyDelta<=cycles/3 && outputAgeMs<=budget*3 &&
            avgScalable<budget*.25 && avgFixed>budget*.65;
        const int minimum=safeMinimumPercent(source);

        if (trial) {
            auto& t=*trial;
            ++t.windows;
            if (!t.up) {
                const bool benefit=
                    (t.baselineWork-avgWork>=std::max(0.35,t.baselineWork*.04)) ||
                    (t.baselineScalable-avgScalable>=std::max(0.35,t.baselineScalable*.05)) ||
                    (busyRate<=t.baselineBusyRate*.75-0.04) ||
                    (outputAgeMs<=t.baselineAge-budget*.25);
                if (benefit) { ++t.good; t.bad=0; }
                else { ++t.bad; t.good=0; }
                if (headroom && avgWork<t.baselineWork*.65) ++headroomWindows;
                else if (!headroom) headroomWindows=0;

                if (t.good>=2) {
                    const int acceptedFrom=t.from;
                    if (!headroom || avgWork>=t.baselineWork*.80)
                        failedUp[tierIndex(acceptedFrom)]={avgWork,avgScalable,busyRate,0,true};
                    trial.reset();
                    reason="trial accepted "+std::to_string(percent)+"%";
                } else if (t.bad>=2 || t.windows>=4) {
                    failedDown[tierIndex(t.to)]={t.baselineWork,t.baselineScalable,t.baselineBusyRate,0,true};
                    restoring=true; proposal=t.from; headroomWindows=0;
                    reason="trial no benefit; restoring "+std::to_string(t.from)+"%";
                } else {
                    reason="verifying trial "+std::to_string(percent)+"%";
                }
            } else {
                const bool passed=!pressure;
                if (passed) { ++t.good; t.bad=0; }
                else { ++t.bad; t.good=0; }
                if (t.good>=2) {
                    trial.reset(); headroomWindows=0;
                    reason="upgrade accepted "+std::to_string(percent)+"%";
                } else if (t.bad>=2 || t.windows>=4) {
                    failedUp[tierIndex(t.to)]={t.baselineWork,t.baselineScalable,t.baselineBusyRate,0,true};
                    restoring=true; proposal=t.from; headroomWindows=0;
                    reason="upgrade overloaded; restoring "+std::to_string(t.from)+"%";
                } else {
                    reason="verifying upgrade "+std::to_string(percent)+"%";
                }
            }
            previousBusy=busy; clearWindow();
            return;
        }

        if (percent>minimum) {
            auto& down=failedDown[tierIndex(std::max(minimum,percent-5))];
            if (down.blocked) {
                const bool worsened=pressure && (
                    avgWork>down.referenceWork*1.12+.35 ||
                    avgScalable>down.referenceScalable*1.15+.35 ||
                    busyRate>down.referenceBusyRate+.08);
                if (worsened) {
                    if (++down.retryWindows>=2) down={};
                } else down.retryWindows=0;
            }
        }
        if (percent<100) {
            auto& up=failedUp[tierIndex(percent+5)];
            if (up.blocked) {
                const bool eased=headroom && avgWork<up.referenceWork*.80;
                if (eased) {
                    if (++up.retryWindows>=2) up={};
                } else up.retryWindows=0;
            }
        }

        if (fixedBottleneck) {
            headroomWindows=0;
            reason="fixed-bottleneck; keeping "+std::to_string(percent)+"%";
        } else if (pressure && percent>minimum) {
            const int candidate=std::max(minimum,percent-5);
            headroomWindows=0;
            if (!failedDown[tierIndex(candidate)].blocked) {
                pendingTrial=Trial{percent,candidate,false,avgWork,avgScalable,outputAgeMs,busyRate,0,0,0};
                proposal=candidate;
                reason="over budget; warming "+std::to_string(proposal)+"%";
            } else {
                reason="retry blocked; keeping "+std::to_string(percent)+"%";
            }
        } else if (headroom && percent<100) {
            const int candidate=percent+5;
            if (!failedUp[tierIndex(candidate)].blocked) {
                if (++headroomWindows>=5) {
                    pendingTrial=Trial{percent,candidate,true,avgWork,avgScalable,outputAgeMs,busyRate,0,0,0};
                    proposal=candidate; headroomWindows=0;
                    reason="sustained headroom; warming "+std::to_string(proposal)+"%";
                } else {
                    reason="measuring headroom "+std::to_string(percent)+"%";
                }
            } else {
                headroomWindows=0;
                reason="upgrade blocked; keeping "+std::to_string(percent)+"%";
            }
        } else {
            if (!headroom) headroomWindows=0;
            reason=percent==minimum && pressure ? "minimum safe extent" : "measuring "+std::to_string(percent)+"%";
        }
        previousBusy=busy; clearWindow();
    }
};
} // namespace neural::detail
