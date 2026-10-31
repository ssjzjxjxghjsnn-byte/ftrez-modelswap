package com.ftrez.modelswap;

import android.app.Application;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import java.io.File;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicBoolean;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.IXposedHookZygoteInit;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

public final class HookEntry implements IXposedHookLoadPackage, IXposedHookZygoteInit {
    private static final String TAG = "[FTREZ-ModelSwap] ";
    private static final long START_DELAY_MS = 15_000L;
    private static final AtomicBoolean HOOK_INSTALLED = new AtomicBoolean();
    private static final AtomicBoolean START_SCHEDULED = new AtomicBoolean();
    private String moduleApkPath;
    private XC_MethodHook.Unhook attachHook;

    @Override
    public void initZygote(StartupParam param) {
        moduleApkPath = param.modulePath;
    }

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam param) {
        if (!BuildConfig.TARGET_PACKAGE.equals(param.packageName)
                || !BuildConfig.TARGET_PACKAGE.equals(param.processName)
                || !HOOK_INSTALLED.compareAndSet(false, true)) {
            return;
        }
        try {
            attachHook = XposedHelpers.findAndHookMethod(Application.class, "attach",
                    Context.class, new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam call) {
                            if (call.hasThrowable()
                                    || !START_SCHEDULED.compareAndSet(false, true)) {
                                return;
                            }
                            if (attachHook != null) {
                                attachHook.unhook();
                                attachHook = null;
                            }
                            Context context = (Context) call.thisObject;
                            Handler handler = new Handler(Looper.getMainLooper());
                            if (handler.postDelayed(() -> bootstrap(context), START_DELAY_MS)) {
                                XposedBridge.log(TAG + "Native startup scheduled after 15 seconds");
                            } else {
                                XposedBridge.log(TAG + "Native startup failed: main looper rejected task");
                            }
                        }
                    });
        } catch (Throwable error) {
            logFailure("Cannot install application attach hook", error);
        }
    }

    private void bootstrap(Context context) {
        try {
            File filesDir = ensureDirectory(context.getFilesDir());
            File modelDir = resolveModelDirectory(context, filesDir);
            XposedBridge.log(TAG + "Model resource directory: " + modelDir);
            NativeBridge.load(moduleApkPath);
            boolean installed = DefaultConfigInstaller.installIfAbsent(moduleApkPath, modelDir,
                    (temporary, destination) -> NativeBridge.publishConfig(
                            temporary.getAbsolutePath(), destination.getAbsolutePath()));
            XposedBridge.log(TAG + (installed ? "Default config.json installed: "
                    : "Existing config.json preserved: ") + new File(modelDir, "config.json"));
            NativeBridge.start(filesDir.getAbsolutePath(), modelDir.getAbsolutePath());
            XposedBridge.log(TAG + "Native startup requested; model directory: " + modelDir);
        } catch (Throwable error) {
            logFailure("Native startup failed", error);
        }
    }

    private static File resolveModelDirectory(Context context, File filesDir) throws IOException {
        try {
            File externalDir = context.getExternalFilesDir(null);
            if (externalDir != null) {
                return ensureDirectory(new File(externalDir, "ftrez-models"));
            }
            XposedBridge.log(TAG + "External files directory unavailable; using internal model directory");
        } catch (IOException | RuntimeException error) {
            logFailure("External model directory unavailable; using internal model directory", error);
        }
        return ensureDirectory(new File(filesDir, "ftrez-models"));
    }

    private static File ensureDirectory(File directory) throws IOException {
        if (directory == null) {
            throw new IOException("Application files directory is unavailable");
        }
        if (!directory.isDirectory() && !directory.mkdirs() && !directory.isDirectory()) {
            throw new IOException("Cannot create directory: " + directory);
        }
        if (!directory.canWrite()) {
            throw new IOException("Directory is not writable: " + directory);
        }
        return directory;
    }

    private static void logFailure(String message, Throwable error) {
        XposedBridge.log(TAG + message + ": " + error);
        XposedBridge.log(error);
    }
}
