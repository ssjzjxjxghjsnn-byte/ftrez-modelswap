#include <jni.h>

#include <android/log.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
void ioException(JNIEnv* env, const char* message) {
    __android_log_print(ANDROID_LOG_ERROR, "FTREZ-ModelSwap", "%s", message);
    jclass exception = env->FindClass("java/io/IOException");
    if (exception) {
        env->ThrowNew(exception, message);
        env->DeleteLocalRef(exception);
    }
}
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ftrez_modelswap_NativeBridge_publishConfig(
        JNIEnv* env, jclass, jstring temporaryPath, jstring destinationPath) {
    if (!temporaryPath || !destinationPath) {
        ioException(env, "Cannot install config.json: a publication path is null");
        return JNI_FALSE;
    }
    const char* temporary = env->GetStringUTFChars(temporaryPath, nullptr);
    if (!temporary) return JNI_FALSE;
    const char* destination = env->GetStringUTFChars(destinationPath, nullptr);
    if (!destination) {
        env->ReleaseStringUTFChars(temporaryPath, temporary);
        return JNI_FALSE;
    }

    int error = 0;
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
    long result;
    do {
        result = syscall(SYS_renameat2, AT_FDCWD, temporary, AT_FDCWD,
                         destination, RENAME_NOREPLACE);
    } while (result == -1 && errno == EINTR);
    if (result == -1) error = errno;
#else
    error = ENOSYS;
#endif
    env->ReleaseStringUTFChars(destinationPath, destination);
    env->ReleaseStringUTFChars(temporaryPath, temporary);

    if (!error) return JNI_TRUE;
    if (error == EEXIST) return JNI_FALSE;
    char message[512];
    const bool unsupported = error == ENOSYS || error == EINVAL || error == EOPNOTSUPP;
    std::snprintf(message, sizeof(message),
                  "Cannot install config.json: atomic no-replace publication %s (errno %d: %s). "
                  "No existing file was overwritten. Copy resources/config.json manually "
                  "into the model directory if config.json is absent, then restart the game.",
                  unsupported ? "is unsupported on this filesystem/kernel" : "failed",
                  error, std::strerror(error));
    ioException(env, message);
    return JNI_FALSE;
}
