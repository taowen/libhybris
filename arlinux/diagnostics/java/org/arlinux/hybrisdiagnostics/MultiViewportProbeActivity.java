/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.widget.LinearLayout;
import java.io.File;

public final class MultiViewportProbeActivity extends ProbeActivity {
    @Override protected String probeId() { return "multiviewport"; }
    @Override protected String probeTitle() { return "Multi-viewport compatibility"; }
    @Override protected String description() { return "MaliBridge first-viewport approximation: keeps only viewport/scissor zero. Renders and reads back static, dynamic and with-count state. Does not emulate shader ViewportIndex."; }
    @Override protected void addControls(LinearLayout content) {
        String[][] modes = {{"viewport-native", "Native driver"}, {"viewport-normal", "Automatic compatibility"},
            {"viewport-force", "Force first-viewport fallback"}, {"viewport-off", "Compatibility disabled"}};
        for (String[] mode : modes) addMode(content, mode[1], () -> runProbe(mode[0], 64));
    }
    @Override protected boolean accepts(String mode, int size) {
        return java.util.Arrays.asList("viewport-native", "viewport-normal", "viewport-force", "viewport-off").contains(mode);
    }
    @Override protected String[] arguments(String directory, String mode, int size, File output) {
        return new String[]{directory + "/libhybris_multiviewport_probe.so", directory, mode, output.getPath()};
    }
}
