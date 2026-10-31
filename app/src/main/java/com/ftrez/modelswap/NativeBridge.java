package com.ftrez.modelswap;

import java.io.IOException;

/** The complete Java/native interface; runtime startup retries belong to native code. */
public final class NativeBridge {
    private NativeBridge() {}

    static void load(String moduleApkPath) {
        if (moduleApkPath == null || moduleApkPath.isEmpty()) {
            throw new IllegalStateException("LSPosed did not provide the module APK path");
        }
        System.load(moduleApkPath + "!/lib/arm64-v8a/libModelSwap.so");
    }

    public static native void start(String filesDir, String modelDir);

    /** Atomically publishes a complete configuration; false means another file already exists. */
    static native boolean publishConfig(String temporaryPath, String destinationPath) throws IOException;
}
