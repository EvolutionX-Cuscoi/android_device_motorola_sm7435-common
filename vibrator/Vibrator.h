/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/vibrator/BnVibrator.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

// Vibrator HAL for LDO driven motors (leds-qpnp-vibrator-ldo). The motor
// has no waveform engine, so strength is done with the LDO voltage and
// every effect is played as timed pulses.
class Vibrator : public BnVibrator {
  public:
    Vibrator();
    ~Vibrator() override;

    ndk::ScopedAStatus getCapabilities(int32_t* _aidl_return) override;
    ndk::ScopedAStatus off() override;
    ndk::ScopedAStatus on(int32_t timeoutMs,
                          const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus perform(Effect effect, EffectStrength strength,
                               const std::shared_ptr<IVibratorCallback>& callback,
                               int32_t* _aidl_return) override;
    ndk::ScopedAStatus getSupportedEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus setAmplitude(float amplitude) override;
    ndk::ScopedAStatus setExternalControl(bool enabled) override;
    ndk::ScopedAStatus getCompositionDelayMax(int32_t* maxDelayMs) override;
    ndk::ScopedAStatus getCompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus getSupportedPrimitives(std::vector<CompositePrimitive>* supported) override;
    ndk::ScopedAStatus getPrimitiveDuration(CompositePrimitive primitive,
                                            int32_t* durationMs) override;
    ndk::ScopedAStatus compose(const std::vector<CompositeEffect>& composite,
                               const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus alwaysOnEnable(int32_t id, Effect effect, EffectStrength strength) override;
    ndk::ScopedAStatus alwaysOnDisable(int32_t id) override;
    ndk::ScopedAStatus getResonantFrequency(float* resonantFreqHz) override;
    ndk::ScopedAStatus getQFactor(float* qFactor) override;
    ndk::ScopedAStatus getFrequencyResolution(float* freqResolutionHz) override;
    ndk::ScopedAStatus getFrequencyMinimum(float* freqMinimumHz) override;
    ndk::ScopedAStatus getBandwidthAmplitudeMap(std::vector<float>* _aidl_return) override;
    ndk::ScopedAStatus getPwlePrimitiveDurationMax(int32_t* durationMs) override;
    ndk::ScopedAStatus getPwleCompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus getSupportedBraking(std::vector<Braking>* supported) override;
    ndk::ScopedAStatus composePwle(const std::vector<PrimitivePwle>& composite,
                                   const std::shared_ptr<IVibratorCallback>& callback) override;

  private:
    struct Pulse {
        int32_t delayMs;     // pause before this pulse starts
        int32_t durationMs;  // requested on time, the LDO plays at least kMinPlayMs
        float amplitude;     // 0..1, mapped to the LDO voltage
    };

    void play(std::vector<Pulse> pulses, const std::shared_ptr<IVibratorCallback>& callback);
    void workerLoop();
    bool waitFor(int32_t ms, uint64_t generation, std::unique_lock<std::mutex>& lock);
    void setVoltage(float amplitude);
    void motorOn(int32_t durationMs);
    void motorOff();
    static int32_t effectiveMs(int32_t durationMs);

    int32_t mVminMv;
    int32_t mVmaxMv;
    float mAmplitude = 1.0f;

    std::mutex mLock;
    std::condition_variable mCv;
    std::thread mThread;
    bool mExit = false;
    uint64_t mGeneration = 0;
    bool mPending = false;
    std::vector<Pulse> mPulses;
    std::shared_ptr<IVibratorCallback> mCallback;
};

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
