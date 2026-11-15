// JNI surface for the harness. Every entry point takes ownership of any file
// descriptors it is given, including when validation fails.

#include <android/log.h>
#include <jni.h>

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>

#include "engine.hpp"
#include "sha256.hpp"

namespace {

JavaVM* g_vm = nullptr;
jclass g_log_class = nullptr;
jmethodID g_log_method = nullptr;

constexpr const char* kLogTag = "ReaHarness";

int androidPriority(int level) {
    switch (level) {
        case 0: return ANDROID_LOG_DEBUG;
        case 1: return ANDROID_LOG_WARN;
        case 2: return ANDROID_LOG_ERROR;
        default: return ANDROID_LOG_INFO;
    }
}

// Called by the engine on the boot thread (and potentially future worker
// threads). Uses the cached JavaVM so the callback does not depend on the thread
// already being attached.
extern "C" void reaLog(int level, const char* tag, const char* message) {
    __android_log_print(androidPriority(level), kLogTag, "%s: %s", tag, message);

    if (g_vm == nullptr || g_log_class == nullptr || g_log_method == nullptr) {
        return;
    }

    JNIEnv* env = nullptr;
    bool attached = false;
    const jint state = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (state == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThreadAsDaemon(&env, nullptr) != JNI_OK) {
            return;
        }
        attached = true;
    } else if (state != JNI_OK) {
        return;
    }
    if (env == nullptr) {
        return;
    }

    jstring jtag = env->NewStringUTF(tag);
    jstring jmessage = env->NewStringUTF(message);
    if (jtag != nullptr && jmessage != nullptr) {
        env->CallStaticVoidMethod(g_log_class, g_log_method, static_cast<jint>(level), jtag, jmessage);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (jtag != nullptr) {
        env->DeleteLocalRef(jtag);
    }
    if (jmessage != nullptr) {
        env->DeleteLocalRef(jmessage);
    }
    if (attached) {
        g_vm->DetachCurrentThread();
    }
}

std::string fromJavaString(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        throw std::runtime_error("null Java string");
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        throw std::runtime_error("GetStringUTFChars failed");
    }
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

void reportFailure(const char* entry, const char* what) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s failed: %s", entry, what);
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env != nullptr) {
        jclass local = env->FindClass("com/rea/harness/LogBus");
        if (local != nullptr) {
            g_log_class = static_cast<jclass>(env->NewGlobalRef(local));
            env->DeleteLocalRef(local);
            if (g_log_class != nullptr) {
                g_log_method = env->GetStaticMethodID(
                    g_log_class, "append", "(ILjava/lang/String;Ljava/lang/String;)V");
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    g_log_method = nullptr;
                }
            }
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }

    rea::Engine::instance().setLogger([](int level, const std::string& message) {
        reaLog(level, "engine", message.c_str());
    });

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "rea_harness JNI loaded");
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM*, void*) {
    if (g_vm != nullptr && g_log_class != nullptr) {
        JNIEnv* env = nullptr;
        if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env != nullptr) {
            env->DeleteGlobalRef(g_log_class);
        }
    }
    g_log_class = nullptr;
    g_log_method = nullptr;
    g_vm = nullptr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rea_harness_NativeBridge_boot(JNIEnv* env, jobject /*thiz*/, jint elf_fd,
                                       jint pak_fd, jstring sandbox_directory,
                                       jstring elf_sha256, jstring pak_sha256) {
    try {
        const std::string sandbox = fromJavaString(env, sandbox_directory);
        const std::string elf_digest = fromJavaString(env, elf_sha256);
        const std::string pak_digest = fromJavaString(env, pak_sha256);
        const bool ok = rea::Engine::instance().boot(
            static_cast<int>(elf_fd), static_cast<int>(pak_fd), sandbox, elf_digest, pak_digest);
        return ok ? JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& error) {
        reportFailure("boot", error.what());
        return JNI_FALSE;
    } catch (...) {
        reportFailure("boot", "unknown native exception");
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_rea_harness_NativeBridge_configure(JNIEnv*, jobject /*thiz*/, jint width,
                                            jint height, jint shadow_size, jint anisotropy,
                                            jint crowd_percent, jint upscaler, jint frame_gen,
                                            jboolean low_latency) {
    try {
        rea::EngineConfig config;
        config.width = static_cast<int>(width);
        config.height = static_cast<int>(height);
        config.shadow_size = static_cast<int>(shadow_size);
        config.anisotropy = static_cast<int>(anisotropy);
        config.crowd_percent = static_cast<int>(crowd_percent);
        config.upscaler = static_cast<int>(upscaler);
        config.frame_gen = static_cast<int>(frame_gen);
        config.low_latency = low_latency == JNI_TRUE;
        rea::Engine::instance().configure(config);
    } catch (const std::exception& error) {
        reportFailure("configure", error.what());
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_rea_harness_NativeBridge_stats(JNIEnv* env, jobject /*thiz*/) {
    try {
        const std::string json = rea::Engine::instance().statsJson();
        return env->NewStringUTF(json.c_str());
    } catch (const std::exception& error) {
        reportFailure("stats", error.what());
        return env->NewStringUTF("{\"booted\":false,\"error\":\"stats failed\"}");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_rea_harness_NativeBridge_shutdown(JNIEnv*, jobject /*thiz*/) {
    try {
        rea::Engine::instance().shutdown();
    } catch (const std::exception& error) {
        reportFailure("shutdown", error.what());
    }
}
