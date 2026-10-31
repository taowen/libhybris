/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.app.Activity;
import android.content.Intent;
import android.graphics.BitmapFactory;
import android.os.Bundle;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;

public final class MainActivity extends Activity {
    private TextView report;
    private ImageView preview;
    private CheckBox large;

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
        title.setText("libhybris · Vulkan diagnostics");
        title.setTextSize(22);
        content.addView(title);
        TextView description = new TextView(this);
        description.setText("Runs on this phone's GPU. No Linux, Wine or root required. Each run starts a fresh probe process. Robustness compatibility is approximate; deviations from strict null/OOB behavior are reported separately. Results cover offscreen BC upload, sampling and compressed readback; they do not certify games or window presentation.\n\nBC kernels: MaliBridge by ASEbrahim · https://github.com/ASEbrahim/MaliBridge\nThis is an independent libhybris integration, not an official MaliBridge build. GPL-3.0-or-later and component notices are included in the APK assets.");
        android.text.util.Linkify.addLinks(description, android.text.util.Linkify.WEB_URLS);
        content.addView(description);
        large = new CheckBox(this);
        large.setText("1024 × 1024 textures (auto fast-quality path)");
        content.addView(large);
        String[][] modes = {{"native", "Native driver"}, {"normal", "Compatibility layer · normal policy"},
            {"decode", "Force BC decoding"}, {"auto", "Force BC transcoding · auto"},
            {"fast", "Force BC transcoding · fast"}, {"default", "Force BC transcoding · default"},
            {"high", "Force BC transcoding · high"}, {"robust-native", "Robustness2 · native driver"},
            {"robust", "Robustness2 · normal policy"}, {"robust-force", "Robustness2 · force compatibility"},
            {"robust-off", "Robustness2 · compatibility disabled"}};
        for (String[] mode : modes) {
            Button button = new Button(this);
            button.setText(mode[1]);
            button.setOnClickListener(v -> startProbe(mode[0], large.isChecked() ? 1024 : 64));
            content.addView(button);
        }
        Button share = new Button(this);
        share.setText("Share report");
        share.setOnClickListener(v -> startActivity(Intent.createChooser(new Intent(Intent.ACTION_SEND)
            .setType("text/plain").putExtra(Intent.EXTRA_TEXT, report.getText().toString()), "Share report")));
        content.addView(share);
        preview = new ImageView(this);
        preview.setAdjustViewBounds(true);
        content.addView(preview);
        report = new TextView(this);
        report.setTextIsSelectable(true);
        report.setTextSize(13);
        content.addView(report);
        loadReport();
        if (state == null && getIntent().hasExtra("mode"))
            startProbe(getIntent().getStringExtra("mode"), getIntent().getIntExtra("size", 64));
    }

    private void startProbe(String mode, int size) {
        if (!java.util.Arrays.asList("native", "normal", "decode", "auto", "fast", "default", "high", "robust-native", "robust", "robust-force", "robust-off").contains(mode)
                || (size != 64 && size != 1024)) throw new IllegalArgumentException("Invalid probe mode or size");
        startActivityForResult(new Intent(this, ProbeActivity.class).putExtra("mode", mode).putExtra("size", size), 1);
    }

    private void loadReport() {
        try {
            report.setText(new String(java.nio.file.Files.readAllBytes(new File(getFilesDir(), "report.txt").toPath()), StandardCharsets.UTF_8));
            preview.setImageBitmap(BitmapFactory.decodeFile(new File(getFilesDir(), "preview.png").getPath()));
        } catch (IOException missing) { report.setText("Choose an experiment above."); }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        loadReport();
    }
}
