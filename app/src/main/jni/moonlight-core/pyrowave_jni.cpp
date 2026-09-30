// JNI entry points for MoonBridge's PyroWave methods. Frames themselves never
// cross JNI: callbacks.c hands PyroWave decode units straight to the renderer.

#include <jni.h>
#include <android/native_window_jni.h>

#include "pyrowave_renderer.h"

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_isPyroWaveAvailable(JNIEnv* env, jclass clazz) {
    return PwIsAvailable(nullptr, 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_getPyroWaveStatus(JNIEnv* env, jclass clazz) {
    char reason[256];
    PwIsAvailable(reason, sizeof(reason));
    return env->NewStringUTF(reason);
}

JNIEXPORT jint JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_pyroWaveSetup(JNIEnv* env, jclass clazz, jint videoFormat,
                                                          jint width, jint height, jint frameRate,
                                                          jboolean fullRange) {
    return PwRendererSetup(videoFormat, width, height, frameRate, fullRange == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_pyroWaveSetSurface(JNIEnv* env, jclass clazz, jobject surface) {
    ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    // The renderer takes its own reference
    PwRendererSetWindow(window);
    if (window != nullptr) {
        ANativeWindow_release(window);
    }
}

JNIEXPORT void JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_pyroWaveStart(JNIEnv* env, jclass clazz) {
    PwRendererStart();
}

JNIEXPORT void JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_pyroWaveStop(JNIEnv* env, jclass clazz) {
    PwRendererStop();
}

JNIEXPORT void JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_pyroWaveCleanup(JNIEnv* env, jclass clazz) {
    PwRendererCleanup();
}

// Fills stats (length >= 13) in the order of MoonBridge.PYROWAVE_STAT_*
JNIEXPORT void JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_pyroWaveGetStats(JNIEnv* env, jclass clazz, jlongArray stats) {
    PW_RENDERER_STATS s;
    PwRendererGetStats(&s);
    const jlong values[] = {
        s.receivedFrames, s.replacedFrames, s.rejectedFrames, s.partialFrames,
        s.decodedFrames, s.presentedFrames, s.noWindowFrames,
        (jlong)s.totalDecodeUs, (jlong)s.totalPresentUs,
        s.outputWidth, s.outputHeight, s.fragmentPath ? 1 : 0, s.mailbox ? 1 : 0,
    };
    const jsize count = sizeof(values) / sizeof(values[0]);
    if (env->GetArrayLength(stats) >= count) {
        env->SetLongArrayRegion(stats, 0, count, values);
    }
}

}
