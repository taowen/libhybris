package io.taowen.hybriswsitest;

import android.app.Activity;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

/** One compositor per process. Restart this test package for a fresh run. */
public final class CompositorActivity extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("wsihost"); }
    private static native int run(Surface surface, int width, int height, String runtime, String library);
    private boolean started;
    private void copyAssets(String source, File target) throws Exception {
        String[] children = getAssets().list(source);
        if (children.length > 0) {
            target.mkdirs();
            for (String name : children) copyAssets(source + "/" + name, new File(target, name));
        } else {
            try (InputStream input = getAssets().open(source); FileOutputStream output = new FileOutputStream(target)) {
                byte[] bytes = new byte[16384]; int count;
                while ((count = input.read(bytes)) != -1) output.write(bytes, 0, count);
            }
        }
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        setContentView(view);
    }
    @Override public void surfaceCreated(SurfaceHolder holder) {}
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (started) return;
        started = true;
        new Thread(new Runnable() { public void run() {
            try {
                copyAssets("xkb", new File(getFilesDir(), "xkb"));
                File runtime = new File(getFilesDir(), "runtime");
                runtime.mkdirs();
                int result = CompositorActivity.run(holder.getSurface(), width, height, runtime.getAbsolutePath(),
                    getApplicationInfo().nativeLibraryDir + "/libanlabwc.so");
                android.util.Log.e("HybrisWSITest", "compositor returned " + result);
            } catch (Exception error) { android.util.Log.e("HybrisWSITest", "startup failed", error); }
        }}, "test-compositor").start();
    }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        // This package contains only disposable test state. Process exit avoids
        // reusing native globals or a Surface after Android destroys it.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
