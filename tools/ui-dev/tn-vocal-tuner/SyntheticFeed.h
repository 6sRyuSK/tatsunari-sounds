#pragma once

#include "PfModels.h"

#include <atomic>
#include <cmath>

class SyntheticPfFeed
{
public:
    SyntheticPfFeed() { regenerate(); }

    pf_ui::PfUiFeed view()
    {
        return { &detected_, &target_, &shift_, &latency_, &sampleRate_ };
    }

    void setFrozen (bool frozen)
    {
        frozen_ = frozen;
        if (frozen_) setPhase (0.35);
    }

    void setPhase (double phase)
    {
        phase_ = phase;
        regenerate();
    }

    void setValues (float detected, float target, float shift, int latency, float sampleRate)
    {
        detected_.store (detected, std::memory_order_relaxed);
        target_.store (target, std::memory_order_relaxed);
        shift_.store (shift, std::memory_order_relaxed);
        latency_.store (latency, std::memory_order_relaxed);
        sampleRate_.store (sampleRate, std::memory_order_relaxed);
    }

    void advance()
    {
        if (frozen_) return;
        phase_ += 0.025;
        regenerate();
    }

    float detected() const { return detected_.load (std::memory_order_relaxed); }
    float target() const { return target_.load (std::memory_order_relaxed); }
    float shift() const { return shift_.load (std::memory_order_relaxed); }
    int latency() const { return latency_.load (std::memory_order_relaxed); }

private:
    void regenerate()
    {
        const float detected = 220.0f + 13.0f * (float) std::sin (phase_);
        const float target = phase_ < 3.141592653589793 ? 220.0f : 246.9417f;
        const float cents = 1200.0f * std::log2 (target / detected);
        setValues (detected, target, cents, 1024, 48000.0f);
    }

    std::atomic<float> detected_ { 220.0f };
    std::atomic<float> target_ { 220.0f };
    std::atomic<float> shift_ { 0.0f };
    std::atomic<int> latency_ { 1024 };
    std::atomic<float> sampleRate_ { 48000.0f };
    double phase_ = 0.35;
    bool frozen_ = false;
};
