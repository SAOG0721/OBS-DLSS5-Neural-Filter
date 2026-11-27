// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>

namespace neural {
struct PassSettings {
    int style = 0;
    float intensity = 1, tone = 1, structure = 1, skin = 0;
    bool mask = false, ui = false;
    bool operator==(const PassSettings&) const = default;
};
struct Settings {
    static constexpr int schemaVersion = 2;
    std::array<PassSettings, 3> pass{};
    int passes = 1, view = 0, debug = 0;
    float mix = 1, strength = 1, chroma = 1, lightness = 1, darken = 1, brighten = 1;
    float midtone = 1, chromaProtection = 0;
    float hue = 0, darkProtect = 0, brightProtect = 0, compression = 0, low = 1, high = 1;
    bool advanced = false, scaling = false;
    int percent = 100, flow = 0, amdQuality = 1, nvQuality = 2, af = 0;
    int modelPreset = 1, resolutionControl = 1, processingMode = 0, updateInterval = 2, residualTransfer = 0;
    // Supplied by the OBS render context; never persisted as a source setting.
    float targetFps = 60;
    float afStrength = 1, responseMs = 80;
    std::string runtime;
    bool operator==(const Settings&) const = default;
    void sanitize() noexcept {
        auto finiteClamp = [](float& v, float fallback, float hi = 2.f) {
            v = std::isfinite(v) ? std::clamp(v, 0.f, hi) : fallback;
        };
        passes = std::clamp(passes, 1, 3);
        modelPreset = std::clamp(modelPreset, 0, 2);
        resolutionControl = std::clamp(resolutionControl, 0, 1);
        processingMode = std::clamp(processingMode, 0, 1);
        updateInterval = std::clamp(updateInterval, 2, 4);
        residualTransfer = std::clamp(residualTransfer, 0, 1);
        if (!std::isfinite(targetFps) || targetFps <= 0) targetFps = 60;
        view = std::clamp(view, 0, 3); debug = std::clamp(debug, 0, 7);
        percent = std::clamp(percent, 25, 100); flow = std::clamp(flow, 0, 2);
        amdQuality = std::clamp(amdQuality, 0, 1); nvQuality = std::clamp(nvQuality, 1, 5);
        af = std::clamp(af, 0, 4);
        for (auto& p : pass) {
            p.style = std::clamp(p.style, 0, 2);
            finiteClamp(p.intensity, 1); finiteClamp(p.tone, 1);
            finiteClamp(p.structure, 1); finiteClamp(p.skin, 0);
        }
        finiteClamp(mix, 1, 1); finiteClamp(strength, 1); finiteClamp(chroma, 1);
        finiteClamp(lightness, 1); finiteClamp(darken, 1); finiteClamp(brighten, 1);
        finiteClamp(midtone, 1); finiteClamp(chromaProtection, 0, 1);
        finiteClamp(hue, 0, 1); finiteClamp(darkProtect, 0, 1);
        finiteClamp(brightProtect, 0, 1); finiteClamp(compression, 0, 1);
        finiteClamp(low, 1); finiteClamp(high, 1); finiteClamp(afStrength, 1, 1);
        responseMs = std::isfinite(responseMs) ? std::clamp(responseMs, 10.f, 500.f) : 80.f;
    }
    bool bypass() const noexcept { return view == 1 || (view == 0 && (mix == 0 || strength == 0)); }
    bool adaptiveResolution() const noexcept { return scaling && resolutionControl == 0; }
    auto resourceKey() const {
        return std::tie(modelPreset, scaling, resolutionControl, percent, processingMode, updateInterval,
                        residualTransfer, flow, amdQuality, nvQuality, runtime);
    }
    auto outletKey() const {
        return std::tie(mix, strength, chroma, lightness, darken, brighten, midtone, chromaProtection, hue, darkProtect,
                        brightProtect, compression, low, high, view, debug, af, afStrength, responseMs);
    }
    int firstDirty(const Settings& previous) const noexcept {
        if (modelPreset != previous.modelPreset) return 0;
        int first = std::min(passes, previous.passes);
        for (int i = 0; i < first; ++i) if (pass[i] != previous.pass[i]) return i;
        return first;
    }
};
struct Extent {
    std::uint32_t width = 0, height = 0;
    bool operator==(const Extent&) const = default;
};
inline Extent inferenceExtent(Extent source, const Settings& settings) noexcept {
    const auto percent = settings.scaling && !settings.adaptiveResolution() ? static_cast<unsigned>(settings.percent) : 100u;
    return {static_cast<unsigned>(static_cast<std::uint64_t>(source.width) * percent / 100),
            static_cast<unsigned>(static_cast<std::uint64_t>(source.height) * percent / 100)};
}
// Conservative initial safety envelope, tightened independently of OBS versions.
// 256x144 is an implementation guard, not a claim about the model's intrinsic minimum.
inline bool safeExtent(Extent e) noexcept {
    return e.width >= 256 && e.height >= 144 && e.width <= 3840 && e.height <= 2160;
}
struct FrameKey {
    std::uint64_t cycle = 0, inputRevision = 0, generation = 0;
    Extent extent;
    bool operator==(const FrameKey&) const = default;
};
inline bool temporalPair(std::uint64_t previous, std::uint64_t current, bool reset) noexcept {
    return !reset && previous && current > previous && current - previous <= 250000000ull;
}
inline float historyWeight(std::uint64_t previous, std::uint64_t current, float responseMs,
                           float strength) noexcept {
    if (!temporalPair(previous, current, false)) return 0;
    return std::exp(-static_cast<float>(current - previous) / (responseMs * 1000000.f)) * strength;
}
} // namespace neural
