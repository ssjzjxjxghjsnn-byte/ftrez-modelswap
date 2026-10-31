package com.ftrez.modelswap;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Installs the packaged configuration without using the target application's AssetManager. */
final class DefaultConfigInstaller {
    private static final String ASSET_ENTRY = "assets/config.json";
    private static final int MAX_CONFIG_BYTES = 1024 * 1024;

    private DefaultConfigInstaller() {}

    @FunctionalInterface
    interface Publisher {
        boolean publish(File temporary, File destination) throws IOException;
    }

    static boolean installIfAbsent(String moduleApkPath, File modelDirectory, Publisher publisher)
            throws IOException {
        Path destination = new File(modelDirectory, "config.json").toPath();
        if (Files.exists(destination, LinkOption.NOFOLLOW_LINKS)) {
            requireReadableFile(destination);
            return false;
        }
        if (moduleApkPath == null || moduleApkPath.isEmpty()) {
            throw new IOException("Cannot install config.json: module APK path is unavailable");
        }
        // initZygote supplies this module's APK. context.getAssets() belongs to the game.
        try (ZipFile moduleApk = new ZipFile(moduleApkPath)) {
            ZipEntry entry = moduleApk.getEntry(ASSET_ENTRY);
            if (entry == null || entry.isDirectory()) {
                throw new IOException("Module APK is missing " + ASSET_ENTRY);
            }
            if (entry.getSize() <= 0 || entry.getSize() > MAX_CONFIG_BYTES) {
                throw new IOException("Invalid packaged config.json size: " + entry.getSize());
            }
            Path temporary = Files.createTempFile(modelDirectory.toPath(), ".config-", ".tmp");
            try {
                CRC32 checksum = new CRC32();
                long count = 0;
                try (InputStream input = moduleApk.getInputStream(entry);
                     FileOutputStream output = new FileOutputStream(temporary.toFile())) {
                    byte[] buffer = new byte[8192];
                    int bytes;
                    while ((bytes = input.read(buffer)) != -1) {
                        count += bytes;
                        if (count > MAX_CONFIG_BYTES) {
                            throw new IOException("Packaged config.json exceeds the size limit");
                        }
                        output.write(buffer, 0, bytes);
                        checksum.update(buffer, 0, bytes);
                    }
                    if (count != entry.getSize() || checksum.getValue() != entry.getCrc()) {
                        throw new IOException("Packaged config.json size or CRC check failed");
                    }
                    output.getFD().sync();
                }
                // Native renameat2(RENAME_NOREPLACE) is atomic and cannot replace a race winner.
                // Files.move(ATOMIC_MOVE) is not sufficient: Android may replace its destination.
                boolean installed = publisher.publish(temporary.toFile(), destination.toFile());
                requireReadableFile(destination);
                return installed;
            } finally {
                Files.deleteIfExists(temporary);
            }
        }
    }

    private static void requireReadableFile(Path destination) throws IOException {
        if (!Files.isRegularFile(destination) || !Files.isReadable(destination)) {
            throw new IOException("Existing config.json is not a readable regular file; preserved: "
                    + destination);
        }
    }
}
