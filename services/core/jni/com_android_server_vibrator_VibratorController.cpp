/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "VibratorController"

#include <android/hardware/vibrator/1.3/IVibrator.h>
#include <android/hardware/vibrator/IVibrator.h>

#include <nativehelper/JNIHelp.h>
#include "android_runtime/AndroidRuntime.h"
#include "core_jni_helpers.h"
#include "jni.h"

#include <utils/Log.h>
#include <utils/misc.h>

#include <vibratorservice/VibratorHalController.h>

#include "com_android_server_vibrator_VibratorManagerService.h"

namespace V1_0 = android::hardware::vibrator::V1_0;
namespace V1_3 = android::hardware::vibrator::V1_3;
namespace aidl = android::hardware::vibrator;

namespace android {

static JavaVM* sJvm = nullptr;
static jmethodID sMethodIdOnComplete;
static jclass sFrequencyMappingClass;
static jmethodID sFrequencyMappingCtor;
static jclass sVibratorInfoClass;
static jmethodID sVibratorInfoCtor;
static struct {
    jfieldID id;
    jfieldID scale;
    jfieldID delay;
} sPrimitiveClassInfo;
static struct {
    jfieldID startAmplitude;
    jfieldID endAmplitude;
    jfieldID startFrequency;
    jfieldID endFrequency;
    jfieldID duration;
} sRampClassInfo;

static_assert(static_cast<uint8_t>(V1_0::EffectStrength::LIGHT) ==
              static_cast<uint8_t>(aidl::EffectStrength::LIGHT));
static_assert(static_cast<uint8_t>(V1_0::EffectStrength::MEDIUM) ==
              static_cast<uint8_t>(aidl::EffectStrength::MEDIUM));
static_assert(static_cast<uint8_t>(V1_0::EffectStrength::STRONG) ==
              static_cast<uint8_t>(aidl::EffectStrength::STRONG));

static_assert(static_cast<uint8_t>(V1_3::Effect::CLICK) ==
              static_cast<uint8_t>(aidl::Effect::CLICK));
static_assert(static_cast<uint8_t>(V1_3::Effect::DOUBLE_CLICK) ==
              static_cast<uint8_t>(aidl::Effect::DOUBLE_CLICK));
static_assert(static_cast<uint8_t>(V1_3::Effect::TICK) == static_cast<uint8_t>(aidl::Effect::TICK));
static_assert(static_cast<uint8_t>(V1_3::Effect::THUD) == static_cast<uint8_t>(aidl::Effect::THUD));
static_assert(static_cast<uint8_t>(V1_3::Effect::POP) == static_cast<uint8_t>(aidl::Effect::POP));
static_assert(static_cast<uint8_t>(V1_3::Effect::HEAVY_CLICK) ==
              static_cast<uint8_t>(aidl::Effect::HEAVY_CLICK));
static_assert(static_cast<uint8_t>(V1_3::Effect::RINGTONE_1) ==
              static_cast<uint8_t>(aidl::Effect::RINGTONE_1));
static_assert(static_cast<uint8_t>(V1_3::Effect::RINGTONE_2) ==
              static_cast<uint8_t>(aidl::Effect::RINGTONE_2));
static_assert(static_cast<uint8_t>(V1_3::Effect::RINGTONE_15) ==
              static_cast<uint8_t>(aidl::Effect::RINGTONE_15));
static_assert(static_cast<uint8_t>(V1_3::Effect::TEXTURE_TICK) ==
              static_cast<uint8_t>(aidl::Effect::TEXTURE_TICK));

static std::shared_ptr<vibrator::HalController> findVibrator(int32_t vibratorId) {
    vibrator::ManagerHalController* manager =
            android_server_vibrator_VibratorManagerService_getManager();
    if (manager == nullptr) {
        return nullptr;
    }
    auto result = manager->getVibrator(vibratorId);
    return result.isOk() ? std::move(result.value()) : nullptr;
}

class VibratorControllerWrapper {
public:
    VibratorControllerWrapper(JNIEnv* env, int32_t vibratorId, jobject callbackListener)
          : mHal(findVibrator(vibratorId)),
            mVibratorId(vibratorId),
            mCallbackListener(env->NewGlobalRef(callbackListener)) {
        LOG_ALWAYS_FATAL_IF(mHal == nullptr,
                            "Failed to connect to vibrator HAL, or vibratorId is invalid");
        LOG_ALWAYS_FATAL_IF(mCallbackListener == nullptr,
                            "Unable to create global reference to vibration callback handler");
    }

    ~VibratorControllerWrapper() {
        auto jniEnv = GetOrAttachJNIEnvironment(sJvm);
        jniEnv->DeleteGlobalRef(mCallbackListener);
    }

    int32_t getVibratorId() const { return mVibratorId; }

    vibrator::Info getVibratorInfo() { return mHal->getInfo(); }

    void initHal() { mHal->init(); }

    template <typename T>
    vibrator::HalResult<T> halCall(const vibrator::HalFunction<vibrator::HalResult<T>>& fn,
                                   const char* functionName) {
        return mHal->doWithRetry(fn, functionName);
    }

    std::function<void()> createCallback(jlong vibrationId) {
        return [vibrationId, this]() {
            auto jniEnv = GetOrAttachJNIEnvironment(sJvm);
            jniEnv->CallVoidMethod(mCallbackListener, sMethodIdOnComplete, mVibratorId,
                                   vibrationId);
        };
    }

private:
    const std::shared_ptr<vibrator::HalController> mHal;
    const int32_t mVibratorId;
    const jobject mCallbackListener;
};

static aidl::BrakingPwle brakingPwle(aidl::Braking braking, int32_t duration) {
    aidl::BrakingPwle pwle;
    pwle.braking = braking;
    pwle.duration = duration;
    return pwle;
}

static aidl::ActivePwle activePwleFromJavaPrimitive(JNIEnv* env, jobject ramp) {
    aidl::ActivePwle pwle;
    pwle.startAmplitude =
            static_cast<float>(env->GetFloatField(ramp, sRampClassInfo.startAmplitude));
    pwle.endAmplitude = static_cast<float>(env->GetFloatField(ramp, sRampClassInfo.endAmplitude));
    pwle.startFrequency =
            static_cast<float>(env->GetFloatField(ramp, sRampClassInfo.startFrequency));
    pwle.endFrequency = static_cast<float>(env->GetFloatField(ramp, sRampClassInfo.endFrequency));
    pwle.duration = static_cast<int32_t>(env->GetIntField(ramp, sRampClassInfo.duration));
    return pwle;
}

/* Return true if braking is not NONE and the active PWLE starts and ends with zero amplitude. */
static bool shouldBeReplacedWithBraking(aidl::ActivePwle activePwle, aidl::Braking braking) {
    return (braking != aidl::Braking::NONE) && (activePwle.startAmplitude == 0) &&
            (activePwle.endAmplitude == 0);
}

/* Return true if braking is not NONE and the active PWLE only ends with zero amplitude. */
static bool shouldAddLastBraking(aidl::ActivePwle lastActivePwle, aidl::Braking braking) {
    return (braking != aidl::Braking::NONE) && (lastActivePwle.startAmplitude > 0) &&
            (lastActivePwle.endAmplitude == 0);
}

static aidl::CompositeEffect effectFromJavaPrimitive(JNIEnv* env, jobject primitive) {
    aidl::CompositeEffect effect;
    effect.primitive = static_cast<aidl::CompositePrimitive>(
            env->GetIntField(primitive, sPrimitiveClassInfo.id));
    effect.scale = static_cast<float>(env->GetFloatField(primitive, sPrimitiveClassInfo.scale));
    effect.delayMs = static_cast<int32_t>(env->GetIntField(primitive, sPrimitiveClassInfo.delay));
    return effect;
}

static void destroyNativeWrapper(void* ptr) {
    VibratorControllerWrapper* wrapper = reinterpret_cast<VibratorControllerWrapper*>(ptr);
    if (wrapper) {
        delete wrapper;
    }
}

static jlong vibratorNativeInit(JNIEnv* env, jclass /* clazz */, jint vibratorId,
                                jobject callbackListener) {
    std::unique_ptr<VibratorControllerWrapper> wrapper =
            std::make_unique<VibratorControllerWrapper>(env, vibratorId, callbackListener);
    wrapper->initHal();
    return reinterpret_cast<jlong>(wrapper.release());
}

static jlong vibratorGetNativeFinalizer(JNIEnv* /* env */, jclass /* clazz */) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(&destroyNativeWrapper));
}

static jboolean vibratorIsAvailable(JNIEnv* env, jclass /* clazz */, jlong ptr) {
    VibratorControllerWrapper* wrapper = reinterpret_cast<VibratorControllerWrapper*>(ptr);
    if (wrapper == nullptr) {
        ALOGE("vibratorIsAvailable failed because native wrapper was not initialized");
        return JNI_FALSE;
    }
    auto pingFn = [](vibrator::HalWrapper* hal) { return hal->ping(); };
    return wrapper->halCall<void>(pingFn, "ping").isOk() ? JNI_TRUE : JNI_FALSE;
}

static jlong vibratorOn(JNIEnv* env, jclass /* clazz */, jlong ptr, jlong timeoutMs,
                        jlong vibrationId) {
    VibratorControllerWrapper* wrapper = reinterpret_cast<VibratorControllerWrapper*>(ptr);
    if (wrapper == nullptr) {
        ALOGE("vibratorOn failed because native wrapper was not initialized");
        return -1;
    }
    auto callback = wrapper->createCallback(vibrationId);
    auto onFn = [timeoutMs, &callback](vibrator::HalWrapper* hal) {
        return hal->on(std::chrono::milliseconds(timeoutMs), callback);
    };
    auto result = wrapper->halCall<void>(onFn, "on");
    return result.isOk() ? timeoutMs : (result.isUnsupported() ? 0 : -1);
}

static void vibratorOff(JNIEnv* env, jclass /* clazz */, jlong ptr) {
    VibratorControllerWrapper* wrapper = reinterpret_cast<VibratorControllerWrapper*>(ptr);
    if (wrapper == nullptr) {
        ALOGE("vibratorOff failed because native wrapper was not initialized");
        return;
    }
    auto offFn = [](vibrator::HalWrapper* hal) { return hal->off(); };
    wrapper->halCall<void>(offFn, "off");
}

static void vibratorSetAmplitude(JNIEnv* env, jclass /* clazz */, jlong ptr, jfloat amplitude) {
    VibratorControllerWrapper* wrapper = reinterpret_cast<VibratorControllerWrapper*>(ptr);
    if (wrapper == nullptr) {
        ALOGE("vibratorSetAmplitude failed because native wrapper was not initialized");
        return;
    }
    auto setAmplitudeFn = [amplitude](vibrator::HalWrapper* hal) {
        return hal->setAmplitude(static_cast<float>(amplitude));
    };
    wrapper->halCall<void>(setAmplitudeFn, "setAmplitude");
}

static void vibratorSetExternalControl(JNIEnv* env, jclass /* clazz */, jlong ptr,
                                       jboolean enabled) {
    VibratorControllerWrapper* wrapper = reinterpret_cast<VibratorControllerWrapper*>(ptr);
    if (wrapper == nullptr) {
        ALOGE("vibratorSetExternalControl failed because native wrapper was not initialized");
        return;
    }
    auto setExternalControlFn = [enabled](vibrator::HalWrapper* hal) {
        return hal->setExternalControl(enabled);
    };
    wrapper->halCall<void>(setExternalControlFn, "setExternalControl");
}

static jlong vibratorPerformEffect(JNIEnv* env, jclass /* clazz */, jlong ptr, jlong effect,
                                   jlong strength, jlong vibrationId) {
    VibratorControllerWrapper* wrapper = reinterpret_cast<VibratorControllerWrapper*>(ptr);
    if (wrapper == nullptr) {
        ALOGE("vibratorPerformEffect failed because native wrapper was not initialized");
        return -1;
    }
    aidl::Effect effectType = static_cast<aidl::Effect>(effect);
    aidl::EffectStrength effectStrength = static_cast<aidl::EffectStrength>(strength);
    auto callback = wrapper->createCallback(vibrationId);
    auto performEffectFn = [effectType, effectStrength, &callback](vibrator::HalWrapper* hal) {
        return hal->performEffect(effectType, effectStrength, callback);
    };
    auto result = wrapper->halCall<std::chrono::milliseconds>(performEffectFn, "performEffect");
    return result.isOk() ? result.value().count() : (result.isUnsupported() ? 0 : -1);
}

static jlong vibratorPerformComposedEffect(JNIEnv* env, jclass /* clazz */, jlong ptr,
                                           jobjectArray composition, jlong vibrationId) {
    VibratorControllerWrapper* wrapper = reinterpret_cast<VibratorControllerWrapper*>(ptr);
    if (wrapper == nullptr) {
        ALOGE("vibratorPerformComposedEffect failed because native wrapper was not initialized");
        return -1;
    }
    size_t size = env->GetArrayLength(composition);
    std::vector<aidl::CompositeEffect> effects;
    for (size_t i = 0; i < size; i++) {
        jobject element = env->GetObjectArrayElement(composition, i);
        effects.push_back(effectFromJavaPrimitive(env, element));
    }
    auto callback = wrapper->createCallback(vibrationId);
    auto performComposedEffectFn = [&effects, &callback](vibrator::HalWrapper* hal) {
        return hal->performComposedEffect(effects, callback);
    };
    auto result = wrapper->halCall<std::chrono::milliseconds>(performComposedEffectFn,
                                                              "performComposedEffect");
    return result.isOk() ? result.value().count() : (result.isUnsupported() ? 0 : -1);
}

static jlong vibratorPerformPwleEffect(JNIEnv* env, jclass /* clazz */, jlong ptr,
                                       jobjectArray waveform, jint brakingId, jlong vibrationId) {
    VibratorControllerWrapper* wrapper = reinterpret_cast<VibratorControllerWrapper*>(ptr);
    if (wrapper == nullptr) {
        ALOGE("vibratorPerformPwleEffect failed because native wrapper was not initialized");
        return -1;
    }
    aidl::Braking braking = static_cast<aidl::Braking>(brakingId);
    size_t size = env->GetArrayLength(waveform);
    std::vector<aidl::PrimitivePwle> primitives;
    std::chrono::milliseconds totalDuration(0);
    for (size_t i = 0; i < size; i++) {
        jobject element = env->GetObjectArrayElement(waveform, i);
        aidl::ActivePwle activePwle = activePwleFromJavaPrimitive(env, element);
        if ((i > 0) && shouldBeReplacedWithBraking(activePwle, braking)) {
            primitives.push_back(brakingPwle(braking, activePwle.duration));
        } else {
            primitives.push_back(activePwle);
        }
        totalDuration += std::chrono::milliseconds(activePwle.duration);

        if ((i == (size - 1)) && shouldAddLastBraking(activePwle, braking)) {
            primitives.push_back(brakingPwle(braking, 0 /* duration */));
        }
    }

    auto callback = wrapper->createCallback(vibrationId);
    auto performPwleEffectFn = [&primitives, &callback](vibrator::HalWrapper* hal) {
        return hal->performPwleEffect(primitives, callback);
    };
    auto result = wrapper->halCall<void>(performPwleEffectFn, "performPwleEffect");
    return result.isOk() ? totalDuration.count() : (result.isUnsupported() ? 0 : -1);
}

static void vibratorAlwaysOnEnable(JNIEnv* env, jclass /* clazz */, jlong ptr, jlong id,
                                   jlong effect, jlong strength) {
    VibratorControllerWrapper* wrapper = reinterpret_cast<VibratorControllerWrapper*>(ptr);
    if (wrapper == nullptr) {
        ALOGE("vibratorAlwaysOnEnable failed because native wrapper was not initialized");
        return;
    }
    auto alwaysOnEnableFn = [id, effect, strength](vibrator::HalWrapper* hal) {
        return hal->alwaysOnEnable(static_cast<int32_t>(id), static_cast<aidl::Effect>(effect),
                                   static_cast<aidl::EffectStrength>(strength));
    };
    wrapper->halCall<void>(alwaysOnEnableFn, "alwaysOnEnable");
}

static void vibratorAlwaysOnDisable(JNIEnv* env, jclass /* clazz */, jlong ptr, jlong id) {
    VibratorControllerWrapper* wrapper = reinterpret_cast<VibratorControllerWrapper*>(ptr);
    if (wrapper == nullptr) {
        ALOGE("vibratorAlwaysOnDisable failed because native wrapper was not initialized");
        return;
    }
    auto alwaysOnDisableFn = [id](vibrator::HalWrapper* hal) {
        return hal->alwaysOnDisable(static_cast<int32_t>(id));
    };
    wrapper->halCall<void>(alwaysOnDisableFn, "alwaysOnDisable");
}

static jobject vibratorGetInfo(JNIEnv* env, jclass /* clazz */, jlong ptr,
                               jfloat suggestedSafeRange) {
    VibratorControllerWrapper* wrapper = reinterpret_cast<VibratorControllerWrapper*>(ptr);
    if (wrapper == nullptr) {
        ALOGE("vibratorGetInfo failed because native wrapper was not initialized");
        return nullptr;
    }
    vibrator::Info info = wrapper->getVibratorInfo();

    jlong capabilities =
            static_cast<jlong>(info.capabilities.valueOr(vibrator::Capabilities::NONE));
    jfloat minFrequency = static_cast<jfloat>(info.minFrequency.valueOr(NAN));
    jfloat resonantFrequency = static_cast<jfloat>(info.resonantFrequency.valueOr(NAN));
    jfloat frequencyResolution = static_cast<jfloat>(info.frequencyResolution.valueOr(NAN));
    jfloat qFactor = static_cast<jfloat>(info.qFactor.valueOr(NAN));
    jintArray supportedEffects = nullptr;
    jintArray supportedBraking = nullptr;
    jintArray supportedPrimitives = nullptr;
    jfloatArray maxAmplitudes = nullptr;

    if (info.supportedEffects.isOk()) {
        std::vector<aidl::Effect> effects = info.supportedEffects.value();
        supportedEffects = env->NewIntArray(effects.size());
        env->SetIntArrayRegion(supportedEffects, 0, effects.size(),
                               reinterpret_cast<jint*>(effects.data()));
    }
    if (info.supportedBraking.isOk()) {
        std::vector<aidl::Braking> braking = info.supportedBraking.value();
        supportedBraking = env->NewIntArray(braking.size());
        env->SetIntArrayRegion(supportedBraking, 0, braking.size(),
                               reinterpret_cast<jint*>(braking.data()));
    }
    if (info.supportedPrimitives.isOk()) {
        std::vector<aidl::CompositePrimitive> primitives = info.supportedPrimitives.value();
        supportedPrimitives = env->NewIntArray(primitives.size());
        env->SetIntArrayRegion(supportedPrimitives, 0, primitives.size(),
                               reinterpret_cast<jint*>(primitives.data()));
    }
    if (info.maxAmplitudes.isOk()) {
        std::vector<float> amplitudes = info.maxAmplitudes.value();
        maxAmplitudes = env->NewFloatArray(amplitudes.size());
        env->SetFloatArrayRegion(maxAmplitudes, 0, amplitudes.size(),
                                 reinterpret_cast<jfloat*>(amplitudes.data()));
    }

    jobject frequencyMapping = env->NewObject(sFrequencyMappingClass, sFrequencyMappingCtor,
                                              minFrequency, resonantFrequency, frequencyResolution,
                                              suggestedSafeRange, maxAmplitudes);

    return env->NewObject(sVibratorInfoClass, sVibratorInfoCtor, wrapper->getVibratorId(),
                          capabilities, supportedEffects, supportedBraking, supportedPrimitives,
                          qFactor, frequencyMapping);
}

static const JNINativeMethod method_table[] = {
        {"nativeInit",
         "(ILcom/android/server/vibrator/VibratorController$OnVibrationCompleteListener;)J",
         (void*)vibratorNativeInit},
        {"getNativeFinalizer", "()J", (void*)vibratorGetNativeFinalizer},
        {"isAvailable", "(J)Z", (void*)vibratorIsAvailable},
        {"on", "(JJJ)J", (void*)vibratorOn},
        {"off", "(J)V", (void*)vibratorOff},
        {"setAmplitude", "(JF)V", (void*)vibratorSetAmplitude},
        {"performEffect", "(JJJJ)J", (void*)vibratorPerformEffect},
        {"performComposedEffect", "(J[Landroid/os/vibrator/PrimitiveSegment;J)J",
         (void*)vibratorPerformComposedEffect},
        {"performPwleEffect", "(J[Landroid/os/vibrator/RampSegment;IJ)J",
         (void*)vibratorPerformPwleEffect},
        {"setExternalControl", "(JZ)V", (void*)vibratorSetExternalControl},
        {"alwaysOnEnable", "(JJJJ)V", (void*)vibratorAlwaysOnEnable},
        {"alwaysOnDisable", "(JJ)V", (void*)vibratorAlwaysOnDisable},
        {"getInfo", "(JF)Landroid/os/VibratorInfo;", (void*)vibratorGetInfo},
};

int register_android_server_vibrator_VibratorController(JavaVM* jvm, JNIEnv* env) {
    sJvm = jvm;
    auto listenerClassName =
            "com/android/server/vibrator/VibratorController$OnVibrationCompleteListener";
    jclass listenerClass = FindClassOrDie(env, listenerClassName);
    sMethodIdOnComplete = GetMethodIDOrDie(env, listenerClass, "onComplete", "(IJ)V");

    jclass primitiveClass = FindClassOrDie(env, "android/os/vibrator/PrimitiveSegment");
    sPrimitiveClassInfo.id = GetFieldIDOrDie(env, primitiveClass, "mPrimitiveId", "I");
    sPrimitiveClassInfo.scale = GetFieldIDOrDie(env, primitiveClass, "mScale", "F");
    sPrimitiveClassInfo.delay = GetFieldIDOrDie(env, primitiveClass, "mDelay", "I");

    jclass rampClass = FindClassOrDie(env, "android/os/vibrator/RampSegment");
    sRampClassInfo.startAmplitude = GetFieldIDOrDie(env, rampClass, "mStartAmplitude", "F");
    sRampClassInfo.endAmplitude = GetFieldIDOrDie(env, rampClass, "mEndAmplitude", "F");
    sRampClassInfo.startFrequency = GetFieldIDOrDie(env, rampClass, "mStartFrequency", "F");
    sRampClassInfo.endFrequency = GetFieldIDOrDie(env, rampClass, "mEndFrequency", "F");
    sRampClassInfo.duration = GetFieldIDOrDie(env, rampClass, "mDuration", "I");

    jclass frequencyMappingClass = FindClassOrDie(env, "android/os/VibratorInfo$FrequencyMapping");
    sFrequencyMappingClass = static_cast<jclass>(env->NewGlobalRef(frequencyMappingClass));
    sFrequencyMappingCtor = GetMethodIDOrDie(env, sFrequencyMappingClass, "<init>", "(FFFF[F)V");

    jclass vibratorInfoClass = FindClassOrDie(env, "android/os/VibratorInfo");
    sVibratorInfoClass = static_cast<jclass>(env->NewGlobalRef(vibratorInfoClass));
    sVibratorInfoCtor = GetMethodIDOrDie(env, sVibratorInfoClass, "<init>",
                                         "(IJ[I[I[IFLandroid/os/VibratorInfo$FrequencyMapping;)V");

    return jniRegisterNativeMethods(env,
                                    "com/android/server/vibrator/VibratorController$NativeWrapper",
                                    method_table, NELEM(method_table));
}

}; // namespace android
