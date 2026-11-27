// SPDX-License-Identifier: GPL-3.0-only
// Linear premultiplied OBS -> straight sRGB UNORM proxy -> signed linear delta.
// The exact quantized proxy is the residual baseline. No CPU image transfer.
cbuffer Parameters : register(b0) {
    uint2 sourceExtent;
    uint2 inferenceExtent;
    uint2 flowExtent;
    uint2 flowColorExtent;
    uint flowGrid;
    float flowDivisor;
    uint stageMode;
    uint view;
    float mix;
    float strength;
    float chroma;
    float lightness;
    float darken;
    float brighten;
    float hue;
    float darkProtect;
    float brightProtect;
    float compression;
    float lowGain;
    float highGain;
    uint debugView;
    uint afMode;
    float afWeight;
    float appearWeight;
    uint validHistory;
    uint validMotion;
    float testDelta;
    uint recoveryFlags;
    float midtone;
    float chromaProtection;
    uint residualReuse;
    uint residualAge;
};
#ifdef VERIFY_INPUT
Texture2D<float4> NewInput : register(t0);
Texture2D<float4> CachedInput : register(t1);
RWTexture2D<uint> Changed : register(u0);
[numthreads(1,1,1)]
void ClearChanged(uint3 id : SV_DispatchThreadID) { Changed[uint2(0,0)]=0; }
[numthreads(8,8,1)]
void CompareInput(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= sourceExtent)) return;
    if (any(asuint(NewInput.Load(int3(id.xy,0))) != asuint(CachedInput.Load(int3(id.xy,0)))))
        InterlockedOr(Changed[uint2(0,0)],1);
}
#else
Texture2D<float4> T0 : register(t0);
Texture2D<float4> T1 : register(t1);
Texture2D<float4> T2 : register(t2);
Texture2D<float4> T3 : register(t3);
Texture2D<float4> T4 : register(t4);
Texture2D<float4> T5 : register(t5);
Texture2D<int2> SparseFlow : register(t6);
RWTexture2D<float4> U0 : register(u0);
RWTexture2D<float4> U1 : register(u1);
RWTexture2D<float2> U2 : register(u2);
RWTexture2D<float> U3 : register(u3);
SamplerState linearClamp : register(s0);
float3 decode(float3 c) {
    return float3(c.r <= .04045 ? c.r / 12.92 : pow((c.r + .055) / 1.055, 2.4),
                  c.g <= .04045 ? c.g / 12.92 : pow((c.g + .055) / 1.055, 2.4),
                  c.b <= .04045 ? c.b / 12.92 : pow((c.b + .055) / 1.055, 2.4));
}
float3 encode(float3 c) {
    c = max(c, 0);
    return float3(c.r <= .0031308 ? 12.92*c.r : 1.055*pow(c.r, 1/2.4)-.055,
                  c.g <= .0031308 ? 12.92*c.g : 1.055*pow(c.g, 1/2.4)-.055,
                  c.b <= .0031308 ? 12.92*c.b : 1.055*pow(c.b, 1/2.4)-.055);
}
float3 straight(float4 c) { return c.a > 1e-6 ? c.rgb/c.a : 0; }
// Bjorn Ottosson's public-domain Oklab matrices.
float3 toLab(float3 c) {
    float3 l = float3(dot(c,float3(.4122214708,.5363325363,.0514459929)),
        dot(c,float3(.2119034982,.6806995451,.1073969566)),
        dot(c,float3(.0883024619,.2817188376,.6299787005)));
    l = sign(l)*pow(abs(l),1.0/3.0);
    return float3(dot(l,float3(.2104542553,.793617785,-.0040720468)),
        dot(l,float3(1.9779984951,-2.428592205,.4505937099)),
        dot(l,float3(.0259040371,.7827717662,-.808675766)));
}
float3 fromLab(float3 c) {
    float3 l = float3(c.x+.3963377774*c.y+.2158037573*c.z,
        c.x-.1055613458*c.y-.0638541728*c.z,c.x-.0894841775*c.y-1.291485548*c.z);
    l = l*l*l;
    return float3(dot(l,float3(4.0767416621,-3.3077115913,.2309699292)),
        dot(l,float3(-1.2684380046,2.6097574011,-.3413193965)),
        dot(l,float3(-.0041960863,-.7034186147,1.707614701)));
}
float3 controlled(float3 o, float3 residual, out float4 metrics) {
    float3 delta = residual*strength;
    metrics = float4(0,1,1,1);
    if (!all(isfinite(delta))) return 0;
    if (strength == 0 || all(delta == 0)) return 0;
    if (chroma == 1 && lightness == 1 && darken == 1 && brighten == 1 &&
        midtone == 1 && hue == 0 && darkProtect == 0 && brightProtect == 0 &&
        chromaProtection == 0 && compression == 0) return delta;
    float3 originalLab = toLab(o);
    float3 d = toLab(o+delta)-originalLab;
    d.x = (min(d.x,0)*darken + max(d.x,0)*brighten)*lightness;
    float shadow = 1-smoothstep(.08,.35,originalLab.x);
    float highlight = smoothstep(.72,.98,originalLab.x);
    d.x *= 1+(midtone-1)*(1-shadow-highlight);
    d.yz *= chroma;
    float c = length(originalLab.yz);
    float2 direction = originalLab.yz / max(c,1e-12);
    float2 candidate = originalLab.yz+d.yz;
    d.yz = lerp(candidate, direction*max(dot(candidate,direction),0),
                hue*smoothstep(0,.02,c))-originalLab.yz;
    d.yz *= 1-saturate(chromaProtection);
    float dark = darkProtect*shadow;
    float bright = brightProtect*highlight;
    // These textures and the available outward headroom are linear RGB.
    float3 outward = max(fromLab(originalLab+d)-o,0);
    float3 headroom = outward/(max(1-o,0)+1e-4);
    bright *= lerp(.5,1,smoothstep(0,.02,d.x));
    bright = max(bright,brightProtect*highlight*smoothstep(.5,2,max(headroom.r,max(headroom.g,headroom.b))));
    metrics.y = (1-dark)*(1-bright);
    d *= metrics.y;
    float magnitude = length(d), excess = max(magnitude-.04,0);
    float compressed = .04+excess/(1+excess/.12);
    metrics.z = magnitude > .04 ? lerp(1,compressed/max(magnitude,1e-12),compression) : 1;
    d *= metrics.z; metrics.x = d.x;
    if (all(d == 0)) return 0;
    // Keep signed, out-of-gamut corrections through interpolation and AF.
    return fromLab(originalLab+d)-o;
}
[numthreads(8,8,1)]
void Proxy(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= inferenceExtent)) return;
    float2 uv = (id.xy+.5)/float2(inferenceExtent);
    float4 c = T0.SampleLevel(linearClamp,uv,0);
    U0[id.xy] = float4(saturate(encode(straight(c))),1);
}
[numthreads(8,8,1)]
void ModelTest(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= inferenceExtent)) return;
    float4 c = T0.Load(int3(id.xy,0));
    U0[id.xy] = float4(saturate(c.rgb+testDelta),1);
}
[numthreads(8,8,1)]
void Delta(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= inferenceExtent)) return;
    float3 o = decode(T0.Load(int3(id.xy,0)).rgb);
    float3 n = decode(T1.Load(int3(id.xy,0)).rgb);
    float3 d = all(isfinite(o)) && all(isfinite(n)) ? n-o : 0;
    if (lowGain != 1 || highGain != 1) {
        float3 low = 0;
        float mass = 0;
        [unroll] for (int y=-1;y<=1;++y) [unroll] for (int x=-1;x<=1;++x) {
            int2 p = clamp(int2(id.xy)+int2(x,y),0,int2(inferenceExtent)-1);
            float3 g = decode(T0.Load(int3(p,0)).rgb);
            float3 result = decode(T1.Load(int3(p,0)).rgb);
            if (!all(isfinite(g)) || !all(isfinite(result))) continue;
            float3 difference = g-o;
            float spatial = (x == 0 ? 2 : 1)*(y == 0 ? 2 : 1);
            float w = spatial*exp(-dot(difference,difference)/.005);
            low += (result-g)*w; mass += w;
        }
        low /= max(mass,1e-12);
        d = low*lowGain + (d-low)*highGain;
    }
    float4 metrics;
    d = all(isfinite(o)) ? controlled(o,d,metrics) : 0;
    if (!all(isfinite(o))) metrics = float4(0,1,1,1);
    U0[id.xy] = float4(d,1);
    U1[id.xy] = metrics;
}
float2 sparse(int2 p) {
    return float2(SparseFlow.Load(int3(clamp(p,0,int2(flowExtent)-1),0))) / flowDivisor;
}
[numthreads(8,8,1)]
void Dense(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= inferenceExtent)) return;
    float2 pos = (float2(id.xy)+.5)*float2(flowColorExtent)/float2(inferenceExtent)/flowGrid-.5;
    int2 p = int2(floor(pos)); float2 f = frac(pos);
    float2 m = lerp(lerp(sparse(p),sparse(p+int2(1,0)),f.x),
                    lerp(sparse(p+int2(0,1)),sparse(p+int2(1,1)),f.x),f.y);
    U2[id.xy] = validMotion != 0 ? m*float2(inferenceExtent)/float2(flowColorExtent) : 0;
}
[numthreads(8,8,1)]
void FlowColor(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= flowColorExtent)) return;
    U0[id.xy] = T0.SampleLevel(linearClamp,(id.xy+.5)/float2(flowColorExtent),0);
}
[numthreads(8,8,1)]
void FlowLuma(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= flowColorExtent)) return;
    float3 color=T0.SampleLevel(linearClamp,(id.xy+.5)/float2(flowColorExtent),0).rgb;
    U3[id.xy] = saturate(dot(color,float3(.2126,.7152,.0722)));
}
[numthreads(8,8,1)]
void Zero(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= inferenceExtent)) return;
    U2[id.xy] = 0; U3[id.xy] = 0;
}
static const float LocalHistoryExactError = 2.0 / 255.0;
static const float LocalHistorySoftLimit = 8.0 / 255.0;
static const int2 LocalHistoryTaps[4] = {int2(2,2), int2(5,2), int2(2,5), int2(5,5)};
groupshared uint LocalHistoryRequests;
groupshared uint LocalHistoryInvalid;
groupshared uint LocalHistoryBestOffset;
groupshared uint LocalHistoryBestRefinement;
groupshared float4 LocalHistoryCurrent[4];

bool IsLocalHistoryAnchor(uint lane) {
    uint2 local = uint2(lane % 8, lane / 8);
    return all((local == 2) | (local == 5));
}
float LocalHistoryColorError(float3 a, float3 b) {
    float3 difference = abs(a - b);
    return max(difference.r, max(difference.g, difference.b));
}
float LocalHistoryTrust(float error) {
    return 1 - smoothstep(LocalHistoryExactError, LocalHistorySoftLimit, error);
}
float4 GuideFromPremul(float4 c, out bool ok) {
    ok = all(isfinite(c)) && c.a > 1e-6;
    return ok ? float4(saturate(encode(straight(c))), c.a) : float4(0, 0, 0, 0);
}
float4 GuideSample(Texture2D<float4> tex, float2 uv, out bool ok) {
    return GuideFromPremul(tex.SampleLevel(linearClamp, uv, 0), ok);
}
float LocalHistoryPatchError(Texture2D<float4> history, int2 tile, uint2 extent, float2 offset) {
    float2 first = tile + LocalHistoryTaps[0] + offset;
    float2 last = tile + LocalHistoryTaps[3] + offset;
    if (any(first < 0) || any(last > float2(extent) - 1)) return 1;
    float error = 0;
    [unroll] for (uint tap = 0; tap < 4; ++tap) {
        float2 pos = tile + LocalHistoryTaps[tap] + offset;
        bool ok;
        float4 previous = GuideSample(history, (pos + .5) / float2(extent), ok);
        if (!ok || abs(LocalHistoryCurrent[tap].a - previous.a) > .02) return 1;
        error = max(error, LocalHistoryColorError(LocalHistoryCurrent[tap].rgb, previous.rgb));
    }
    return error;
}
uint LocalHistoryScore(float error, uint distance, uint index) {
    return (uint(round(saturate(error) * 4095)) << 16) | (min(distance, 255u) << 8) | (index & 255u);
}
float LocalHistoryScoreError(uint score) {
    return float(score >> 16) / 4095.0;
}
bool FindLocalHistoryOffset(Texture2D<float4> history, int2 tile, uint2 extent, float2 scale, int radius,
                            uint lane, float4 currentGuide, bool currentOk, bool needsRecovery,
                            out float2 offset, out float patchError) {
    offset = 0;
    patchError = 1;
    if (any(tile + LocalHistoryTaps[0] < 0) || any(tile + LocalHistoryTaps[3] >= int2(extent))) return false;
    if (lane == 0) {
        LocalHistoryRequests = 0;
        LocalHistoryInvalid = 0;
        LocalHistoryBestOffset = 0xffffffff;
        LocalHistoryBestRefinement = 0xffffffff;
    }
    uint2 local = uint2(lane % 8, lane / 8);
    bool anchor = IsLocalHistoryAnchor(lane);
    if (anchor) LocalHistoryCurrent[uint(local.x == 5) + 2 * uint(local.y == 5)] = currentGuide;
    GroupMemoryBarrierWithGroupSync();
    if (anchor && !currentOk) InterlockedAdd(LocalHistoryInvalid, 1);
    if (anchor && needsRecovery) InterlockedAdd(LocalHistoryRequests, 1);
    GroupMemoryBarrierWithGroupSync();
    if (LocalHistoryInvalid > 0 || LocalHistoryRequests < 2) return false;
    float spread = max(LocalHistoryColorError(LocalHistoryCurrent[0].rgb, LocalHistoryCurrent[1].rgb),
                   max(LocalHistoryColorError(LocalHistoryCurrent[0].rgb, LocalHistoryCurrent[2].rgb),
                       LocalHistoryColorError(LocalHistoryCurrent[0].rgb, LocalHistoryCurrent[3].rgb)));
    if (!isfinite(spread) || spread < 3 * LocalHistoryExactError) return false;

    const uint side = uint(2 * radius + 1);
    [loop] for (uint candidate = lane; candidate < side * side; candidate += 64) {
        int2 delta = int2(candidate % side, candidate / side) - radius;
        float error = LocalHistoryPatchError(history, tile, extent, delta * scale);
        InterlockedMin(LocalHistoryBestOffset, LocalHistoryScore(error, uint(dot(float2(delta), float2(delta))), candidate));
    }
    GroupMemoryBarrierWithGroupSync();
    uint chosen = LocalHistoryBestOffset & 255;
    int2 coarse = int2(chosen % side, chosen / side) - radius;
    offset = coarse * scale;
    patchError = LocalHistoryScoreError(LocalHistoryBestOffset);
    if (patchError <= LocalHistoryExactError) return true;

    if (lane < 9) {
        int2 subpixel = int2(lane % 3, lane / 3) - 1;
        float2 delta = coarse + .5 * subpixel;
        if (all(abs(delta) <= radius)) {
            float error = LocalHistoryPatchError(history, tile, extent, delta * scale);
            uint distance = uint(round(dot(delta, delta) * 4));
            InterlockedMin(LocalHistoryBestRefinement, LocalHistoryScore(error, distance, lane));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (LocalHistoryBestRefinement != 0xffffffff) {
        float refinedError = LocalHistoryScoreError(LocalHistoryBestRefinement);
        if (refinedError < patchError) {
            uint sub = LocalHistoryBestRefinement & 255;
            offset = (coarse + .5 * (int2(sub % 3, sub / 3) - 1)) * scale;
            patchError = refinedError;
        }
    }
    return patchError < LocalHistorySoftLimit;
}
bool StationaryGuideMatchReuse(int2 pixel) {
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 q = clamp(pixel + int2(x, y), 0, int2(inferenceExtent) - 1);
            float2 quv = (q + .5) / float2(inferenceExtent);
            bool cOk, rOk;
            float4 cg = GuideSample(T0, quv, cOk);
            float4 rg = GuideSample(T2, quv, rOk);
            if (!cOk || !rOk || abs(cg.a - rg.a) > .02 ||
                LocalHistoryColorError(cg.rgb, rg.rgb) > LocalHistoryExactError) return false;
        }
    }
    return true;
}
// Reuse reads an immutable full-NR residual and its actual source reference.
// T3..T5 contain successive backward motions, newest first, in inference pixels.
// Alpha is validity, not opacity; the source/reference FP16 textures retain alpha.
[numthreads(8,8,1)]
void Reuse(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    bool active = all(id.xy < inferenceExtent);
    float2 uv = active ? (id.xy + .5) / float2(inferenceExtent) : 0;
    float2 referenceUv = uv;
    float confidence = active && residualAge >= 1 && residualAge <= 3 && residualReuse != 0 ? 1 : 0;
    float2 border = .5 / float2(inferenceExtent);
    if (active && residualReuse == 2) {
        if (validMotion == 0) confidence = 0;
        [unroll] for (uint i=0;i<3;++i) {
            if (i >= residualAge || confidence == 0) break;
            float2 mv = i == 0 ? T3.SampleLevel(linearClamp,referenceUv,0).xy :
                        i == 1 ? T4.SampleLevel(linearClamp,referenceUv,0).xy :
                                 T5.SampleLevel(linearClamp,referenceUv,0).xy;
            if (!all(isfinite(mv))) { confidence = 0; break; }
            referenceUv += mv/float2(inferenceExtent);
            if (any(referenceUv < border) || any(referenceUv > 1-border)) confidence = 0;
        }
    }
    float4 current = active ? T0.SampleLevel(linearClamp,uv,0) : 0;
    float4 reference = active ? T2.SampleLevel(linearClamp,referenceUv,0) : 0;
    float4 residual = active ? T1.SampleLevel(linearClamp,referenceUv,0) : 0;
    if (!active || !all(isfinite(current)) || !all(isfinite(reference)) || !all(isfinite(residual)) ||
        current.a <= 1e-6 || reference.a <= 1e-6) confidence = 0;
    if (active) {
        float difference = length(straight(current)-straight(reference));
        confidence *= (1-smoothstep(.015,.12,difference)) *
                      (1-smoothstep(.02,.15,abs(current.a-reference.a))) * saturate(residual.a);
        if (!isfinite(confidence)) confidence = 0;
    }
    bool allowRecovery = (recoveryFlags & 1u) != 0 && residualReuse == 2 &&
        residualAge >= 1 && residualAge <= 3 && validMotion != 0;
    if (allowRecovery) {
        bool curOk = false;
        float4 curGuide = active ? GuideFromPremul(current, curOk) : 0;
        bool needsPatch = active && curOk && confidence < .75;
        if (needsPatch && StationaryGuideMatchReuse(int2(id.xy))) {
            float4 sameResidual = T1.SampleLevel(linearClamp, uv, 0);
            if (all(isfinite(sameResidual)) && saturate(sameResidual.a) > confidence) {
                residual = sameResidual;
                confidence = saturate(sameResidual.a);
            }
        }
        bool rejected = active && curOk && confidence < 1.0;
        float2 offset;
        float patchError;
        bool found = FindLocalHistoryOffset(T2, int2(group.xy * 8), inferenceExtent,
            float2(inferenceExtent) / float2(sourceExtent), int(min(6u, 3u * residualAge)),
            lane, curGuide, curOk, needsPatch, offset, patchError);
        if (found && rejected) {
            float2 previous = float2(id.xy) + offset;
            if (all(previous >= 0) && all(previous <= float2(inferenceExtent) - 1)) {
                float2 candUv = (previous + .5) / float2(inferenceExtent);
                bool refOk;
                float4 refGuide = GuideSample(T2, candUv, refOk);
                float4 candResidual = T1.SampleLevel(linearClamp, candUv, 0);
                if (refOk && all(isfinite(candResidual))) {
                    float pixelError = LocalHistoryColorError(curGuide.rgb, refGuide.rgb);
                    float alphaTrust = 1 - smoothstep(.02, .15, abs(curGuide.a - refGuide.a));
                    float recovered = LocalHistoryTrust(max(patchError, pixelError)) * alphaTrust * saturate(candResidual.a);
                    if (isfinite(recovered) && recovered > confidence) {
                        residual = candResidual;
                        confidence = recovered;
                    }
                }
            }
        }
    }
    if (!active) return;
    U0[id.xy] = float4(confidence > 0 ? residual.rgb * confidence : 0, confidence);
}
float4 rawDelta(float2 uv) { return T0.SampleLevel(linearClamp,uv,0); }
bool InsideSource(int2 p) { return all(p >= 0) && all(p < int2(sourceExtent)); }
bool TemporalPreviousPos(int2 p, bool localMatch, float2 offset, out float2 previousPos, out float2 mv) {
    mv = 0;
    if (!InsideSource(p)) { previousPos = 0; return false; }
    if (localMatch) {
        previousPos = float2(p) + offset;
    } else {
        if (afMode >= 2 && validMotion != 0)
            mv = T4.SampleLevel(linearClamp, (p + .5) / float2(sourceExtent), 0).xy * float2(sourceExtent) / float2(inferenceExtent);
        previousPos = float2(p) + mv;
    }
    return all(previousPos >= 0) && all(previousPos <= float2(sourceExtent) - 1);
}
struct TemporalPatch {
    float2 previousUv;
    float3 lo, hi;
    float confidence;
};
TemporalPatch MatchTemporalPatch(int2 p, float3 current, bool localMatch, float2 offset, float errorLimit) {
    TemporalPatch patch;
    patch.previousUv = (p + .5) / float2(sourceExtent);
    patch.lo = patch.hi = current;
    patch.confidence = 0;
    float2 centerPrev, centerMv;
    if (!TemporalPreviousPos(p, localMatch, offset, centerPrev, centerMv)) return patch;
    patch.previousUv = (centerPrev + .5) / float2(sourceExtent);
    float error = 0, maximumError = 0, maxAlphaDiff = 0;
    bool valid = true;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 n = p + int2(x, y);
            float2 prevN, mvN;
            if (!TemporalPreviousPos(n, localMatch, offset, prevN, mvN)) { valid = false; continue; }
            if (!localMatch && afMode >= 2 && validMotion != 0 && any(abs(mvN - centerMv) > 2)) {
                valid = false; continue;
            }
            float4 curPx = T2.Load(int3(n, 0));
            bool curOk, oldOk;
            float4 curGuide = GuideFromPremul(curPx, curOk);
            float4 oldGuide = GuideSample(T3, (prevN + .5) / float2(sourceExtent), oldOk);
            float3 resN = rawDelta((n + .5) / float2(sourceExtent)).rgb;
            if (!curOk || !oldOk || !all(isfinite(resN))) { valid = false; continue; }
            float e = LocalHistoryColorError(curGuide.rgb, oldGuide.rgb);
            error += e / 9;
            maximumError = max(maximumError, e);
            maxAlphaDiff = max(maxAlphaDiff, abs(curGuide.a - oldGuide.a));
            patch.lo = min(patch.lo, resN);
            patch.hi = max(patch.hi, resN);
        }
    }
    if (valid && maximumError <= errorLimit)
        patch.confidence = (1 - smoothstep(.008, .04, error)) *
                           (1 - smoothstep(.025, .10, maximumError)) *
                           (1 - smoothstep(.02, .15, maxAlphaDiff));
    return patch;
}
[numthreads(8,8,1)]
void Temporal(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    bool active = all(id.xy < sourceExtent);
    bool allowRecovery = (recoveryFlags & 1u) != 0 && validHistory != 0 && afMode >= 2 &&
        debugView == 0 && view == 0 && validMotion != 0;
    float2 recoveredOffset = 0;
    float recoveredPatchError = 1;
    bool recoveredFound = false;
    if (allowRecovery) {
        bool curOk = false;
        float4 curGuide = active ? GuideFromPremul(T2.Load(int3(id.xy, 0)), curOk) : 0;
        bool needsRecovery = false;
        if (active && curOk) {
            float2 prevPos, mv;
            needsRecovery = !TemporalPreviousPos(int2(id.xy), false, 0, prevPos, mv);
            if (!needsRecovery) {
                bool oldOk;
                float4 oldGuide = GuideSample(T3, (prevPos + .5) / float2(sourceExtent), oldOk);
                needsRecovery = !oldOk || abs(curGuide.a - oldGuide.a) > .02 ||
                    LocalHistoryColorError(curGuide.rgb, oldGuide.rgb) > .02;
            }
        }
        recoveredFound = FindLocalHistoryOffset(T3, int2(group.xy * 8), sourceExtent, float2(1, 1), 3,
            lane, curGuide, curOk, needsRecovery, recoveredOffset, recoveredPatchError);
    }
    if (!active) return;
    float2 uv = (id.xy+.5)/float2(sourceExtent);
    float4 observation = rawDelta(uv);
    float3 current = observation.rgb;
    float validity = saturate(observation.a);
    if (!all(isfinite(current)) || !isfinite(validity) || validity == 0) {
        U0[id.xy] = 0; return;
    }
    float3 stable = current;
    // Diagnostics inspect this observation without adding temporal history.
    if (validHistory != 0 && afMode != 0 && debugView == 0 && view == 0 && any(current != 0)) {
        TemporalPatch patch = MatchTemporalPatch(int2(id.xy), current, false, 0, 1.0);
        if (allowRecovery) {
            if (recoveredFound && patch.confidence < .5) {
                TemporalPatch recovered = MatchTemporalPatch(int2(id.xy), current, true, recoveredOffset, LocalHistorySoftLimit);
                recovered.confidence *= LocalHistoryTrust(recoveredPatchError);
                if (recovered.confidence > patch.confidence) patch = recovered;
            }
            if (patch.confidence < .5) {
                TemporalPatch stationary = MatchTemporalPatch(int2(id.xy), current, true, 0, LocalHistoryExactError);
                if (stationary.confidence > patch.confidence) patch = stationary;
            }
        }
        float2 referenceUv = patch.previousUv;
        float confidence = patch.confidence;
        float4 historySample = T1.SampleLevel(linearClamp,referenceUv,0);
        confidence *= validity*saturate(historySample.a);
        float3 history = historySample.rgb;
        float3 low = 0;
        [unroll] for (int y=-1;y<=1;++y) [unroll] for (int x=-1;x<=1;++x) {
            float3 d = rawDelta(uv+float2(x,y)/float2(sourceExtent)).rgb;
            low+=d/9;
        }
        history = clamp(history,patch.lo-.015,patch.hi+.015);
        float weight = afWeight;
        if (afMode == 3) {
            // Appearing correction builds slowly, disappearing correction releases fast.
            weight = length(current) > length(history) ? appearWeight : min(afWeight,.45);
            weight *= 1-smoothstep(.015,.10,length(current-history));
        }
        if (afMode == 4) {
            float3 oldLow = 0;
            [unroll] for (int y=-1;y<=1;++y) [unroll] for (int x=-1;x<=1;++x)
                oldLow += T1.SampleLevel(linearClamp,referenceUv+
                                float2(x,y)/float2(sourceExtent),0).rgb/9;
            stable = current-low+lerp(low,clamp(oldLow,low-.015,low+.015),weight*confidence);
        } else stable = lerp(current,history,weight*confidence);
    }
    // A disappearing component clears its old enhancement immediately, even
    // in the low-frequency AF mode. Rejected reuse cannot resurrect history.
    stable = float3(current.r == 0 ? 0 : stable.r,current.g == 0 ? 0 : stable.g,
                    current.b == 0 ? 0 : stable.b);
    U0[id.xy] = float4(all(isfinite(stable)) ? stable : 0,validity);
}
bool inGamut(float3 color) { return all(color >= -1e-6) && all(color <= 1.000001); }
// The single output boundary, after residual upsampling, AF and output mixing.
// In-gamut values keep their exact RGB; out-of-gamut colors follow a constant
// Oklab lightness/hue ray, with only the final numerical guard clipping channels.
float3 finalizeColor(float3 color, out float scale) {
    scale = 1;
    if (all(color >= 0) && all(color <= 1)) return color;
    float3 lab = toLab(color);
    lab.x = saturate(lab.x);
    float3 rgb = fromLab(lab);
    if (!inGamut(rgb)) {
        float lo = 0, hi = 1;
        [loop] for (int i=0;i<24;++i) {
            float mid = (lo+hi)*.5;
            if (inGamut(fromLab(float3(lab.x,lab.yz*mid)))) lo = mid; else hi = mid;
        }
        scale = lo;
        rgb = fromLab(float3(lab.x,lab.yz*lo));
    }
    return saturate(rgb);
}
[numthreads(8,8,1)]
void Compose(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= sourceExtent)) return;
    float2 uv = (id.xy+.5)/float2(sourceExtent);
    float4 original = T0.Load(int3(id.xy,0));
    float4 observation = T1.Load(int3(id.xy,0));
    float3 delta = observation.rgb;
    float validity = saturate(observation.a);
    // Direct composition may read the inference residual when AF is disabled.
    // Preserve Temporal's observation guard, including diagnostic/alpha paths.
    if (!all(isfinite(delta)) || !isfinite(validity) || validity == 0) delta = 0;
    delta = float3(delta.r == 0 ? 0 : delta.r,delta.g == 0 ? 0 : delta.g,
                   delta.b == 0 ? 0 : delta.b);
    if (view == 1 || (view == 2 && uv.x < .5)) { U0[id.xy]=original; return; }
    // Preserve the real premultiplied original, including FP16 quantization.
    if (debugView == 0 && view != 3 && (mix == 0 || all(delta == 0))) {
        U0[id.xy]=original; return;
    }
    if (original.a <= 1e-6) { U0[id.xy]=float4(0,0,0,original.a); return; }
    float3 color = straight(original)+delta*mix;
    if (!all(isfinite(color))) { U0[id.xy]=original; return; }
    float gamutScale;
    color = finalizeColor(color,gamutScale);
    if (debugView != 0) {
        float4 m = T2.SampleLevel(linearClamp,uv,0);
        if (debugView == 1) color = abs(delta)*8;
        else if (debugView == 2) color = max(delta,0)*8;
        else if (debugView == 3) color = max(-delta,0)*8;
        else if (debugView == 4) color = m.y;
        else if (debugView == 5) color = m.z;
        else if (debugView == 6) color = gamutScale;
        else color = abs(toLab(color)-toLab(straight(original)))*8;
    }
    if (view == 3) color = .18+delta*8;
    if (!all(isfinite(color))) { U0[id.xy]=original; return; }
    float ignored;
    color = finalizeColor(color,ignored);
    U0[id.xy] = float4(color*original.a,original.a);
}
#endif
