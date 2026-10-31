/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.widget.CheckBox;
import android.widget.LinearLayout;
import java.io.File;

public final class BcProbeActivity extends ProbeActivity {
    private CheckBox large;
    @Override protected String probeId() { return "bc"; }
    @Override protected String probeTitle() { return "BC textures"; }
    @Override protected String description() { return "Compare native BC with GPU decoding and transcoding. Checks sampling, compressed readback and compute-state restoration; not window presentation or game acceptance."; }
    @Override protected void addControls(LinearLayout content) {
        large = new CheckBox(this); large.setText("1024 × 1024 textures (auto fast-quality path)"); content.addView(large);
        String[][] modes = {{"native", "Native driver"}, {"normal", "Compatibility layer · normal policy"},
            {"decode", "Force BC decoding"}, {"auto", "Force BC transcoding · auto"},
            {"fast", "Force BC transcoding · fast"}, {"default", "Force BC transcoding · default"}, {"high", "Force BC transcoding · high"}};
        for (String[] mode : modes) addMode(content, mode[1], () -> runProbe(mode[0], large.isChecked() ? 1024 : 64));
    }
    @Override protected boolean accepts(String mode, int size) {
        return java.util.Arrays.asList("native", "normal", "decode", "auto", "fast", "default", "high").contains(mode) && (size == 64 || size == 1024);
    }
    @Override protected String[] arguments(String directory, String mode, int size, File output) {
        return new String[]{directory + "/libhybris_bc_probe.so", directory, mode, Integer.toString(size), output.getPath()};
    }
}
