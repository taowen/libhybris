/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.content.Context;
import android.graphics.Bitmap;
import android.os.Handler;
import android.os.Looper;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** One cancellable native child. Reports for different probes never overwrite each other. */
final class ProbeRun implements Runnable {
    interface Arguments { String[] create(String directory, File output); }
    private final Context context;
    private final String id;
    private final Arguments arguments;
    private final Runnable finished;
    private volatile boolean cancelled;
    private volatile Process process;

    ProbeRun(Context context, String id, Arguments arguments, Runnable finished) {
        this.context = context.getApplicationContext(); this.id = id; this.arguments = arguments; this.finished = finished;
    }
    void cancel() { cancelled = true; Process child = process; if (child != null) child.destroyForcibly(); }

    private static String hash(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = new FileInputStream(file)) {
            byte[] bytes = new byte[65536]; int count;
            while ((count = input.read(bytes)) != -1) digest.update(bytes, 0, count);
        }
        StringBuilder text = new StringBuilder();
        for (byte value : digest.digest()) text.append(String.format("%02x", value & 255));
        return text.toString();
    }

    @Override public void run() {
        File output = new File(context.getFilesDir(), id);
        File work = null;
        String result;
        try {
            Files.createDirectories(output.toPath());
            work = Files.createTempDirectory(output.toPath(), "run-").toFile();
            String source;
            try (InputStream input = context.getAssets().open("build.json")) {
                ByteArrayOutputStream bytes = new ByteArrayOutputStream(); byte[] buffer = new byte[4096]; int count;
                while ((count = input.read(buffer)) != -1) bytes.write(buffer, 0, count);
                source = new String(bytes.toByteArray(), StandardCharsets.UTF_8);
            }
            JSONObject identity = new JSONObject(source), probe = identity.getJSONObject("probes").getJSONObject(id);
            String directory = context.getApplicationInfo().nativeLibraryDir;
            String[] command = arguments.create(directory, work);
            String layerHash = hash(new File(directory, "libVkLayer_hybris_compat.so"));
            String executableHash = hash(new File(command[0]));
            if (!layerHash.equals(identity.getString("compatibilityLayerSha256")) ||
                !new File(command[0]).getName().equals(probe.getString("executable")) ||
                !executableHash.equals(probe.getString("sha256")))
                throw new IOException("Loaded native files differ from their build identity");
            // exec supplies the child's actual AT_SECURE state; the layer guard remains unchanged.
            File nativeReport = new File(work, "native-report.txt");
            if (cancelled) throw new InterruptedException("Probe cancelled");
            process = new ProcessBuilder(command).redirectOutput(nativeReport).redirectError(new File(work, "trace.txt")).start();
            if (cancelled) process.destroyForcibly();
            boolean exited = process.waitFor(180, TimeUnit.SECONDS);
            if (cancelled) result = "CANCELLED: no pass result.\n";
            else if (!exited) result = "TIMEOUT: probe exceeded 180 seconds. No pass result.\n";
            else {
                String nativeText = new String(Files.readAllBytes(nativeReport.toPath()), StandardCharsets.UTF_8);
                result = source + "\nProbe: " + id + "\nLoaded compatibility layer SHA-256: " + layerHash +
                    "\nLoaded probe SHA-256: " + executableHash + "\n" + nativeText;
                boolean unsupported = process.exitValue() == 2 && nativeText.contains("UNSUPPORTED:");
                if (process.exitValue() != 0 && !unsupported && !nativeText.contains("FAIL"))
                    result += "\nFAIL: native process exited " + process.exitValue() + "\n";
                if (process.exitValue() == 0 && !nativeText.contains("PASS: "))
                    result += "\nFAIL: native process returned no completed check summary.\n";
            }
            File pixels = new File(work, "preview.argb");
            if (exited && !cancelled && pixels.isFile()) {
                byte[] bytes = Files.readAllBytes(pixels.toPath());
                if (bytes.length != 256 * 256 * 4) throw new IOException("Invalid preview dimensions");
                int[] image = new int[256 * 256]; ByteBuffer.wrap(bytes).order(ByteOrder.nativeOrder()).asIntBuffer().get(image);
                Bitmap bitmap = Bitmap.createBitmap(image, 256, 256, Bitmap.Config.ARGB_8888);
                try (OutputStream stream = new FileOutputStream(new File(work, "preview.png"))) { bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream); }
                bitmap.recycle();
            }
        } catch (Exception failure) { result = cancelled ? "CANCELLED: no pass result.\n" : "FAIL: " + failure + "\n"; }
        finally {
            Process child = process;
            if (child != null && child.isAlive()) {
                child.destroyForcibly();
                try { child.waitFor(2, TimeUnit.SECONDS); }
                catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); }
            }
        }
        try {
            Files.createDirectories(output.toPath());
            if (work != null) {
                for (String name : new String[]{"trace.txt", "preview.png"}) {
                    File source = new File(work, name), target = new File(output, name);
                    if (source.isFile()) Files.move(source.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
                    else Files.deleteIfExists(target.toPath());
                }
                File report = new File(work, "report.txt");
                Files.write(report.toPath(), result.getBytes(StandardCharsets.UTF_8));
                Files.move(report.toPath(), new File(output, "report.txt").toPath(), StandardCopyOption.REPLACE_EXISTING);
            } else {
                Files.deleteIfExists(new File(output, "trace.txt").toPath());
                Files.deleteIfExists(new File(output, "preview.png").toPath());
                Files.write(new File(output, "report.txt").toPath(), result.getBytes(StandardCharsets.UTF_8));
            }
        } catch (IOException failure) { android.util.Log.e("HybrisDiagnostics", "Cannot save report", failure); }
        finally {
            if (work != null) {
                File[] files = work.listFiles(); if (files != null) for (File file : files) file.delete();
                work.delete();
            }
        }
        android.util.Log.i("HybrisDiagnostics", result);
        new Handler(Looper.getMainLooper()).post(finished);
    }
}
