/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.app.Activity;
import android.graphics.Bitmap;
import android.os.*;
import android.widget.TextView;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.concurrent.atomic.AtomicBoolean;

public final class ProbeActivity extends Activity {
    static { System.loadLibrary("hybris_diagnostics"); }
    private static native String run(String directory, String mode, int size, String trace);
    private static native int[] pixels();
    private final AtomicBoolean complete = new AtomicBoolean();

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
                result = source + "\nLoaded compatibility layer SHA-256: " + hash + "\n"
                    + run(directory, getIntent().getStringExtra("mode"), getIntent().getIntExtra("size", 64),
                          new File(getFilesDir(), "trace.txt").getPath());
                image = pixels();
            } catch (Throwable failure) { result = "FAIL: " + failure + "\n"; }
            finishWith(result, image);
        }, "hybris-gpu-probe").start();
    }

    private void finishWith(String result, int[] image) {
        if (!complete.compareAndSet(false, true)) return;
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
        // Policy initialization is process-wide. A finished or cancelled probe
        // must never leave it resident for the next experiment.
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override public void onBackPressed() { finishWith("CANCELLED: no pass result.\n", null); }
}
