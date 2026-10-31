/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.app.Activity;
import android.content.Intent;
import android.graphics.BitmapFactory;
import android.os.Bundle;
import android.view.View;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;

/** Shared presentation and process lifetime; each probe owns its controls and executable. */
public abstract class ProbeActivity extends Activity {
    private LinearLayout controls;
    private TextView report;
    private ImageView preview;
    private Button cancel;
    private ProbeRun active;
    private boolean destroyed;
    protected abstract String probeId();
    protected abstract String probeTitle();
    protected abstract String description();
    protected abstract void addControls(LinearLayout content);
    protected abstract boolean accepts(String mode, int size);
    protected abstract String[] arguments(String directory, String mode, int size, File output);

    protected final void addMode(LinearLayout content, String title, Runnable action) {
        Button button = new Button(this);
        button.setText(title);
        button.setOnClickListener(v -> action.run());
        content.addView(button);
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        int padding = (int)(16 * getResources().getDisplayMetrics().density);
        content.setPadding(padding, padding, padding, padding);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        setContentView(scroll);
        TextView title = new TextView(this);
        title.setText(probeTitle()); title.setTextSize(22); content.addView(title);
        TextView explanation = new TextView(this);
        explanation.setText(description()); content.addView(explanation);
        controls = new LinearLayout(this); controls.setOrientation(LinearLayout.VERTICAL);
        content.addView(controls); addControls(controls);
        cancel = new Button(this); cancel.setText("Cancel probe"); cancel.setVisibility(View.GONE);
        cancel.setOnClickListener(v -> { if (active != null) active.cancel(); }); content.addView(cancel);
        addMode(content, "Share report", () -> startActivity(Intent.createChooser(new Intent(Intent.ACTION_SEND)
            .setType("text/plain").putExtra(Intent.EXTRA_TEXT, report.getText().toString()), "Share report")));
        preview = new ImageView(this); preview.setAdjustViewBounds(true); content.addView(preview);
        report = new TextView(this); report.setTextIsSelectable(true); report.setTextSize(13); content.addView(report);
        showReport();
        if (state == null && getIntent().hasExtra("mode"))
            runProbe(getIntent().getStringExtra("mode"), getIntent().getIntExtra("size", 64));
    }

    protected final void runProbe(String mode, int size) {
        if (active != null) return;
        if (!accepts(mode, size)) { report.setText("FAIL: invalid mode or size for this probe."); return; }
        controls.setVisibility(View.GONE); cancel.setVisibility(View.VISIBLE); preview.setVisibility(View.GONE);
        report.setText("Running on the GPU… Back cancels this probe process.");
        active = new ProbeRun(this, probeId(), (directory, output) -> arguments(directory, mode, size, output), () -> {
            if (destroyed) return;
            active = null; controls.setVisibility(View.VISIBLE); cancel.setVisibility(View.GONE); showReport();
        });
        new Thread(active, "hybris-" + probeId() + "-probe").start();
    }

    private void showReport() {
        File output = new File(getFilesDir(), probeId());
        try {
            report.setText(new String(java.nio.file.Files.readAllBytes(new File(output, "report.txt").toPath()), StandardCharsets.UTF_8));
            android.graphics.Bitmap image = BitmapFactory.decodeFile(new File(output, "preview.png").getPath());
            preview.setImageBitmap(image); preview.setVisibility(image == null ? View.GONE : View.VISIBLE);
        } catch (IOException missing) { report.setText("Choose an experiment above."); preview.setVisibility(View.GONE); }
    }

    @Override public void onBackPressed() {
        if (active != null) active.cancel(); else super.onBackPressed();
    }
    @Override protected void onDestroy() {
        destroyed = true;
        if (active != null) active.cancel();
        super.onDestroy();
    }
}
