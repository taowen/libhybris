/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.app.Activity;
import android.graphics.Bitmap;
import android.os.*;
import android.widget.TextView;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.MessageDigest;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.TimeUnit;

public final class ProbeActivity extends Activity {
    private final AtomicBoolean complete = new AtomicBoolean();
    private volatile Process probe;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("Running on the GPU…\n\nResults are saved when this experiment finishes. Back cancels this probe process.");
        text.setTextSize(20);
        text.setPadding(24, 48, 24, 24);
        setContentView(text);
        new Handler(Looper.getMainLooper()).postDelayed(() -> finishWith("TIMEOUT: probe exceeded 180 seconds. No pass result.\n", null), 180000);
        new Thread(() -> {
            String result;
            int[] image = null;
            try {
                String directory = getApplicationInfo().nativeLibraryDir;
                String source;
                try (InputStream input = getAssets().open("build.json")) {
                    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                    byte[] buffer = new byte[4096];
                    int count;
                    while ((count = input.read(buffer)) != -1) bytes.write(buffer, 0, count);
                    source = new String(bytes.toByteArray(), StandardCharsets.UTF_8);
                }
                File layer = new File(directory, "libVkLayer_hybris_compat.so");
                MessageDigest digest = MessageDigest.getInstance("SHA-256");
                try (InputStream input = new FileInputStream(layer)) {
                    byte[] buffer = new byte[65536];
                    int count;
                    while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
                }
                StringBuilder hash = new StringBuilder();
                for (byte value : digest.digest()) hash.append(String.format("%02x", value & 255));
                if (!hash.toString().equals(new org.json.JSONObject(source).getString("compatibilityLayerSha256")))
                    throw new IOException("Loaded layer differs from its build identity");
                android.util.Log.i("HybrisDiagnostics", source + "\nLoaded layer: " + hash);
                File nativeReport = new File(getFilesDir(), "native-report.txt");
                File pixels = new File(getFilesDir(), "preview.argb");
                if (pixels.exists() && !pixels.delete()) throw new IOException("Cannot clear previous preview");
                // exec resets the zygote's AT_SECURE flag without weakening the
                // compatibility layer's secure-execution environment policy.
                probe = new ProcessBuilder(directory + "/libhybris_diagnostics_exec.so", directory,
                    getIntent().getStringExtra("mode"), Integer.toString(getIntent().getIntExtra("size", 64)),
                    getFilesDir().getPath())
                    .redirectOutput(nativeReport).redirectError(new File(getFilesDir(), "trace.txt")).start();
                if (complete.get()) { probe.destroyForcibly(); return; }
                if (!probe.waitFor(180, TimeUnit.SECONDS)) throw new IOException("GPU probe timed out");
                String nativeText = new String(java.nio.file.Files.readAllBytes(nativeReport.toPath()), StandardCharsets.UTF_8);
                result = source + "\nLoaded compatibility layer SHA-256: " + hash + "\n" + nativeText;
                if (probe.exitValue() != 0 && !nativeText.contains("FAIL"))
                    result += "\nFAIL: native process exited " + probe.exitValue() + "\n";
                if (pixels.isFile()) {
                    byte[] bytes = java.nio.file.Files.readAllBytes(pixels.toPath());
                    if (bytes.length != 256 * 256 * 4) throw new IOException("Invalid preview dimensions");
                    image = new int[256 * 256];
                    ByteBuffer.wrap(bytes).order(ByteOrder.nativeOrder()).asIntBuffer().get(image);
                }
            } catch (Throwable failure) { result = "FAIL: " + failure + "\n"; }
            finishWith(result, image);
        }, "hybris-gpu-probe").start();
    }

    private void finishWith(String result, int[] image) {
        if (!complete.compareAndSet(false, true)) return;
        if (probe != null && probe.isAlive()) probe.destroyForcibly();
        try {
            File preview = new File(getFilesDir(), "preview.png");
            if (image != null && image.length == 256 * 256) {
                Bitmap bitmap = Bitmap.createBitmap(image, 256, 256, Bitmap.Config.ARGB_8888);
                try (OutputStream output = new FileOutputStream(preview)) { bitmap.compress(Bitmap.CompressFormat.PNG, 100, output); }
                bitmap.recycle();
            } else preview.delete();
            try (OutputStream output = new FileOutputStream(new File(getFilesDir(), "report.txt"))) {
                output.write(result.getBytes(StandardCharsets.UTF_8));
            }
        } catch (IOException failure) { android.util.Log.e("HybrisDiagnostics", "Cannot save report", failure); }
        android.util.Log.i("HybrisDiagnostics", result);
        runOnUiThread(() -> { setResult(RESULT_OK); finish(); });
    }

    @Override protected void onDestroy() {
        super.onDestroy();
        if (probe != null && probe.isAlive()) probe.destroyForcibly();
    }

    @Override public void onBackPressed() { finishWith("CANCELLED: no pass result.\n", null); }
}
