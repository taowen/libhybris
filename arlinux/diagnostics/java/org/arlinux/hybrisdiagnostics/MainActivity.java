/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.*;

public final class MainActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout content = new LinearLayout(this); content.setOrientation(LinearLayout.VERTICAL);
        int padding = (int)(16 * getResources().getDisplayMetrics().density); content.setPadding(padding, padding, padding, padding);
        ScrollView scroll = new ScrollView(this); scroll.addView(content); setContentView(scroll);
        TextView title = new TextView(this); title.setText("libhybris · Vulkan diagnostics"); title.setTextSize(22); content.addView(title);
        TextView description = new TextView(this);
        description.setText("Runs directly on this phone's GPU. No Linux, Wine or root required. Each probe has its own native executable, controls, report and trace.\n\nMaliBridge by ASEbrahim · https://github.com/ASEbrahim/MaliBridge\nThis is an independent libhybris integration. GPL-3.0-or-later and component notices are included in the APK assets.");
        android.text.util.Linkify.addLinks(description, android.text.util.Linkify.WEB_URLS); content.addView(description);
        addProbe(content, "Clip/Cull interface allocation", DistanceProbeActivity.class);
        addProbe(content, "Shader interface compatibility", InOutProbeActivity.class);
        addProbe(content, "Polygon line and point modes", PolygonProbeActivity.class);
        addProbe(content, "Dual-source blend compatibility", DualSourceProbeActivity.class);
        addProbe(content, "Indirect multi-draw compatibility", MultiDrawProbeActivity.class);
        addProbe(content, "Multi-viewport compatibility", MultiViewportProbeActivity.class);
        addProbe(content, "BC textures", BcProbeActivity.class);
        addProbe(content, "Robustness2", RobustnessProbeActivity.class);
        addProbe(content, "Mali specialization constants", SpecConstantsProbeActivity.class);
    }
    private void addProbe(LinearLayout content, String label, Class<? extends Activity> activity) {
        Button button = new Button(this); button.setText(label);
        button.setOnClickListener(v -> startActivity(new Intent(this, activity))); content.addView(button);
    }
}
