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
    float padding;
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
float3 controlled(int2 p, out float4 metrics) {
    float3 o = decode(T0.Load(int3(p,0)).rgb);
    float3 n = decode(T1.Load(int3(p,0)).rgb);
    float3 delta = (n-o)*strength;
    metrics = float4(0,1,1,1);
    if (!all(isfinite(delta))) return 0;
    if (strength == 0 || all(delta == 0)) return 0;
    if (chroma == 1 && lightness == 1 && darken == 1 && brighten == 1 &&
        hue == 0 && darkProtect == 0 && brightProtect == 0 && compression == 0) return delta;
    float3 originalLab = toLab(o);
    float3 d = toLab(o+delta)-originalLab;
    d.x = (min(d.x,0)*darken + max(d.x,0)*brighten)*lightness;
    d.yz *= chroma;
    float c = length(originalLab.yz);
    float2 direction = originalLab.yz / max(c,1e-12);
    float2 candidate = originalLab.yz+d.yz;
    d.yz = lerp(candidate, direction*max(dot(candidate,direction),0),
                hue*smoothstep(0,.02,c))-originalLab.yz;
    float dark = darkProtect*(1-smoothstep(.08,.35,originalLab.x));
    float bright = brightProtect*smoothstep(.72,.98,originalLab.x);
    bright *= lerp(.5,1,smoothstep(0,.02,d.x));
    metrics.y = (1-dark)*(1-bright);
    d *= metrics.y;
    float magnitude = length(d), excess = max(magnitude-.04,0);
    float compressed = .04+excess/(1+excess/.12);
    metrics.z = magnitude > .04 ? lerp(1,compressed/max(magnitude,1e-12),compression) : 1;
    d *= metrics.z; metrics.x = d.x;
    // Preserve a continuous, constant-L/hue gamut map for SDR proxy controls.
    float3 lab = originalLab+d;
    lab.x = saturate(lab.x);
    float3 rgb = fromLab(lab);
    if (any(rgb < 0) || any(rgb > 1)) {
        float lo = 0, hi = 1;
        [loop] for (int i=0;i<16;++i) {
            float mid = (lo+hi)*.5;
            float3 trial = fromLab(float3(lab.x, lab.yz*mid));
            if (all(trial >= 0) && all(trial <= 1)) lo=mid; else hi=mid;
        }
        metrics.w = lo;
        rgb = fromLab(float3(lab.x,lab.yz*lo));
    }
    return rgb-o;
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
    float4 metrics;
    float3 d = controlled(id.xy,metrics);
    if (lowGain != 1 || highGain != 1) {
        float3 low = 0;
        float mass = 0;
        float3 guide = decode(T0.Load(int3(id.xy,0)).rgb);
        [unroll] for (int y=-1;y<=1;++y) [unroll] for (int x=-1;x<=1;++x) {
            int2 p = clamp(int2(id.xy)+int2(x,y),0,int2(inferenceExtent)-1);
            float3 g = decode(T0.Load(int3(p,0)).rgb);
            float w = exp(-length(g-guide)*24);
            float4 ignored;
            low += controlled(p,ignored)*w; mass += w;
        }
        low /= max(mass,1e-6);
        d = low*lowGain + (d-low)*highGain;
    }
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
float3 rawDelta(float2 uv) { return T0.SampleLevel(linearClamp,uv,0).rgb; }
[numthreads(8,8,1)]
void Temporal(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= sourceExtent)) return;
    float2 uv = (id.xy+.5)/float2(sourceExtent);
    float3 current = rawDelta(uv);
    float3 stable = current;
    if (validHistory != 0 && afMode != 0) {
        float2 mv = 0;
        if (afMode == 2 || afMode == 3 || afMode == 4)
            mv = T4.SampleLevel(linearClamp,uv,0).xy/float2(inferenceExtent);
        float2 referenceUv = uv+mv;
        float4 original = T2.Load(int3(id.xy,0));
        float4 previous = T3.SampleLevel(linearClamp,referenceUv,0);
        float3 nowColor = straight(original), previousColor = straight(previous);
        float difference = length(nowColor-previousColor);
        float alphaDifference = abs(original.a-previous.a);
        float confidence = (1-smoothstep(.015,.12,difference)) *
                           (1-smoothstep(.02,.15,alphaDifference));
        if (any(referenceUv <= 0) || any(referenceUv >= 1)) confidence = 0;
        if ((afMode == 2 || afMode == 3 || afMode == 4) && validMotion == 0) confidence=0;
        float3 history = T1.SampleLevel(linearClamp,referenceUv,0).rgb;
        float3 minimum = current, maximum = current, low = 0;
        [unroll] for (int y=-1;y<=1;++y) [unroll] for (int x=-1;x<=1;++x) {
            float3 d = rawDelta(uv+float2(x,y)/float2(sourceExtent));
            minimum=min(minimum,d); maximum=max(maximum,d); low+=d/9;
        }
        history = clamp(history,minimum-.015,maximum+.015);
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
    U0[id.xy] = float4(all(isfinite(stable)) ? stable : 0,1);
}
[numthreads(8,8,1)]
void Compose(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= sourceExtent)) return;
    float2 uv = (id.xy+.5)/float2(sourceExtent);
    float4 original = T0.Load(int3(id.xy,0));
    float3 delta = T1.Load(int3(id.xy,0)).rgb;
    float3 color = straight(original)+delta*mix;
    if (debugView != 0) {
        float4 m = T2.SampleLevel(linearClamp,uv,0);
        if (debugView == 1) color = abs(delta)*8;
        else if (debugView == 2) color = max(delta,0)*8;
        else if (debugView == 3) color = max(-delta,0)*8;
        else if (debugView == 4) color = m.y;
        else if (debugView == 5) color = m.z;
        else if (debugView == 6) color = m.w;
        else color = abs(toLab(color)-toLab(straight(original)))*8;
    }
    if (view == 1 || (view == 2 && uv.x < .5)) { U0[id.xy]=original; return; }
    if (view == 3) color = .18+delta*8;
    if (original.a <= 1e-6) { U0[id.xy]=float4(0,0,0,original.a); return; }
    // Zero-delta and mix-zero paths preserve the actual original, including quantization.
    if (debugView == 0 && view == 0 && (mix == 0 || all(delta == 0))) {
        U0[id.xy]=original; return;
    }
    if (!all(isfinite(color))) { U0[id.xy]=original; return; }
    color=clamp(color,min(straight(original),0),max(straight(original),1));
    U0[id.xy] = float4(color*original.a,original.a);
}
#endif
