/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vibrator.moto_ldo"

#include "Vibrator.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

namespace {

constexpr char kLedPath[] = "/sys/class/leds/vibrator/";

// Limits from leds-qpnp-vibrator-ldo.c
constexpr int32_t kMinPlayMs = 50;     // QPNP_VIB_MIN_PLAY_MS
constexpr int32_t kMaxPlayMs = 15000;  // QPNP_VIB_MAX_PLAY_MS
constexpr int32_t kLdoMinMv = 1504;    // QPNP_VIB_LDO_VMIN_UV
constexpr int32_t kLdoMaxMv = 3544;    // QPNP_VIB_LDO_VMAX_UV

// The framework calls on() and then setAmplitude(). The LDO only takes a new
// voltage when a pulse starts, so wait a moment for the amplitude to arrive.
constexpr int32_t kAmplitudeSettleMs = 4;

constexpr int32_t kComposeDelayMaxMs = 1000;
constexpr int32_t kComposeSizeMax = 256;

// Negative amplitude means "use the current setAmplitude() value".
constexpr float kFollowAmplitude = -1.0f;

bool writeNode(const char* node, const std::string& value) {
    const std::string path = std::string(kLedPath) + node;
    if (!::android::base::WriteStringToFile(value, path)) {
        PLOG(ERROR) << "Failed to write " << value << " to " << path;
        return false;
    }
    return true;
}

int32_t readNodeInt(const char* node, int32_t fallback) {
    std::string value;
    int32_t parsed;
    if (!::android::base::ReadFileToString(std::string(kLedPath) + node, &value) ||
        !::android::base::ParseInt(::android::base::Trim(value), &parsed)) {
        return fallback;
    }
    return parsed;
}

float strengthScale(EffectStrength strength) {
    switch (strength) {
        case EffectStrength::LIGHT:
            return 0.5f;
        case EffectStrength::MEDIUM:
            return 0.75f;
        case EffectStrength::STRONG:
            return 1.0f;
    }
    return 0.75f;
}

struct PrimitiveInfo {
    int32_t durationMs;
    float gain;
};

bool primitiveInfo(CompositePrimitive primitive, PrimitiveInfo* info) {
    switch (primitive) {
        case CompositePrimitive::NOOP:
            *info = {0, 0.0f};
            return true;
        case CompositePrimitive::CLICK:
            *info = {kMinPlayMs, 0.8f};
            return true;
        case CompositePrimitive::THUD:
            *info = {70, 1.0f};
            return true;
        case CompositePrimitive::SPIN:
            *info = {80, 0.7f};
            return true;
        case CompositePrimitive::QUICK_RISE:
            *info = {60, 0.6f};
            return true;
        case CompositePrimitive::SLOW_RISE:
            *info = {80, 0.5f};
            return true;
        case CompositePrimitive::QUICK_FALL:
            *info = {60, 0.6f};
            return true;
        case CompositePrimitive::LIGHT_TICK:
            *info = {kMinPlayMs, 0.45f};
            return true;
        case CompositePrimitive::LOW_TICK:
            *info = {kMinPlayMs, 0.35f};
            return true;
    }
    return false;
}

}  // namespace

Vibrator::Vibrator() {
    const int32_t bootVmax = readNodeInt("vmax_mv", 2900);
    mVmaxMv = ::android::base::GetIntProperty("ro.vendor.vibrator.ldo.vmax_mv", bootVmax);
    mVmaxMv = std::clamp(mVmaxMv, kLdoMinMv, kLdoMaxMv);
    mVminMv = ::android::base::GetIntProperty("ro.vendor.vibrator.ldo.vmin_mv", 1600);
    mVminMv = std::clamp(mVminMv, kLdoMinMv, mVmaxMv);
    LOG(INFO) << "LDO vibrator, " << mVminMv << " to " << mVmaxMv << " mV";

    writeNode("state", "1");
    mThread = std::thread(&Vibrator::workerLoop, this);
}

Vibrator::~Vibrator() {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mExit = true;
        mGeneration++;
    }
    mCv.notify_all();
    if (mThread.joinable()) {
        mThread.join();
    }
    motorOff();
}

int32_t Vibrator::effectiveMs(int32_t durationMs) {
    return std::clamp(durationMs, kMinPlayMs, kMaxPlayMs);
}

void Vibrator::setVoltage(float amplitude) {
    amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    const int32_t mv =
            mVminMv + static_cast<int32_t>(std::lround((mVmaxMv - mVminMv) * amplitude));
    writeNode("vmax_mv", std::to_string(mv));
}

void Vibrator::motorOn(int32_t durationMs) {
    writeNode("duration", std::to_string(effectiveMs(durationMs)));
    writeNode("activate", "1");
}

void Vibrator::motorOff() {
    writeNode("activate", "0");
}

bool Vibrator::waitFor(int32_t ms, uint64_t generation, std::unique_lock<std::mutex>& lock) {
    if (ms <= 0) {
        return !mExit && mGeneration == generation;
    }
    return !mCv.wait_for(lock, std::chrono::milliseconds(ms),
                         [&] { return mExit || mGeneration != generation; });
}

void Vibrator::workerLoop() {
    std::unique_lock<std::mutex> lock(mLock);
    while (true) {
        mCv.wait(lock, [&] { return mExit || mPending; });
        if (mExit) {
            break;
        }
        mPending = false;
        const uint64_t generation = mGeneration;
        std::vector<Pulse> pulses = std::move(mPulses);
        std::shared_ptr<IVibratorCallback> callback = std::move(mCallback);

        bool done = true;
        for (const Pulse& pulse : pulses) {
            if (!waitFor(pulse.delayMs, generation, lock)) {
                done = false;
                break;
            }
            float amplitude = pulse.amplitude;
            if (amplitude < 0.0f) {
                if (!waitFor(kAmplitudeSettleMs, generation, lock)) {
                    done = false;
                    break;
                }
                amplitude = mAmplitude;
            }
            if (amplitude > 0.0f && pulse.durationMs > 0) {
                lock.unlock();
                setVoltage(amplitude);
                motorOn(pulse.durationMs);
                lock.lock();
                if (!waitFor(effectiveMs(pulse.durationMs), generation, lock)) {
                    done = false;
                    break;
                }
            }
        }

        // Cancelled pulses are stopped too, a new request reprograms the motor
        // right after on this same thread.
        lock.unlock();
        motorOff();
        if (done && callback) {
            callback->onComplete();
        }
        lock.lock();
    }
}

void Vibrator::play(std::vector<Pulse> pulses, const std::shared_ptr<IVibratorCallback>& callback) {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mGeneration++;
        mPulses = std::move(pulses);
        mCallback = callback;
        mPending = true;
    }
    mCv.notify_all();
}

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    *_aidl_return = IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                    IVibrator::CAP_AMPLITUDE_CONTROL | IVibrator::CAP_COMPOSE_EFFECTS;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mGeneration++;
        mPending = false;
        mPulses.clear();
        mCallback = nullptr;
    }
    mCv.notify_all();
    motorOff();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    if (timeoutMs <= 0) {
        return off();
    }
    play({{0, timeoutMs, kFollowAmplitude}}, callback);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength strength,
                                     const std::shared_ptr<IVibratorCallback>& callback,
                                     int32_t* _aidl_return) {
    const float scale = strengthScale(strength);
    std::vector<Pulse> pulses;

    switch (effect) {
        case Effect::TEXTURE_TICK:
            pulses = {{0, kMinPlayMs, 0.3f * scale}};
            break;
        case Effect::TICK:
            pulses = {{0, kMinPlayMs, 0.45f * scale}};
            break;
        case Effect::CLICK:
            pulses = {{0, kMinPlayMs, 0.8f * scale}};
            break;
        case Effect::HEAVY_CLICK:
            pulses = {{0, 60, 1.0f * scale}};
            break;
        case Effect::DOUBLE_CLICK:
            pulses = {{0, kMinPlayMs, 0.8f * scale}, {70, kMinPlayMs, 0.8f * scale}};
            break;
        default:
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    int32_t totalMs = 0;
    for (const Pulse& pulse : pulses) {
        totalMs += pulse.delayMs + effectiveMs(pulse.durationMs);
    }
    play(std::move(pulses), callback);
    *_aidl_return = totalMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    *_aidl_return = {Effect::CLICK, Effect::DOUBLE_CLICK, Effect::TICK, Effect::HEAVY_CLICK,
                     Effect::TEXTURE_TICK};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
    if (amplitude <= 0.0f || amplitude > 1.0f) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard<std::mutex> lock(mLock);
    mAmplitude = amplitude;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool /* enabled */) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* maxDelayMs) {
    *maxDelayMs = kComposeDelayMaxMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* maxSize) {
    *maxSize = kComposeSizeMax;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(std::vector<CompositePrimitive>* supported) {
    *supported = {CompositePrimitive::NOOP,       CompositePrimitive::CLICK,
                  CompositePrimitive::THUD,       CompositePrimitive::SPIN,
                  CompositePrimitive::QUICK_RISE, CompositePrimitive::SLOW_RISE,
                  CompositePrimitive::QUICK_FALL, CompositePrimitive::LIGHT_TICK,
                  CompositePrimitive::LOW_TICK};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive,
                                                  int32_t* durationMs) {
    PrimitiveInfo info;
    if (!primitiveInfo(primitive, &info)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    *durationMs = info.durationMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) {
    if (composite.empty() || composite.size() > kComposeSizeMax) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    std::vector<Pulse> pulses;
    for (const CompositeEffect& e : composite) {
        PrimitiveInfo info;
        if (!primitiveInfo(e.primitive, &info)) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
        if (e.delayMs < 0 || e.delayMs > kComposeDelayMaxMs || e.scale < 0.0f ||
            e.scale > 1.0f) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }

        const float amplitude = e.scale * info.gain;
        // The motor can not play anything shorter than kMinPlayMs, so back to
        // back primitives are merged into one pulse instead of a long buzz.
        if (e.delayMs == 0 && !pulses.empty()) {
            Pulse& last = pulses.back();
            last.durationMs = std::max(last.durationMs, info.durationMs);
            last.amplitude = std::max(last.amplitude, amplitude);
            continue;
        }
        pulses.push_back({e.delayMs, info.durationMs, amplitude});
    }

    play(std::move(pulses), callback);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return) {
    _aidl_return->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t /* id */, Effect /* effect */,
                                            EffectStrength /* strength */) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t /* id */) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getResonantFrequency(float* /* resonantFreqHz */) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getQFactor(float* /* qFactor */) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getFrequencyResolution(float* /* freqResolutionHz */) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float* /* freqMinimumHz */) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float>* /* _aidl_return */) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t* /* durationMs */) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t* /* maxSize */) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking>* /* supported */) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle>& /* composite */,
                                         const std::shared_ptr<IVibratorCallback>& /* callback */) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
