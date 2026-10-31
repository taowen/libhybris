/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.widget.LinearLayout;
import java.io.File;

public final class DynamicRenderingProbeActivity extends ProbeActivity {
    @Override protected String probeId() { return "dynamic_rendering"; }
    @Override protected String probeTitle() { return "Dynamic rendering"; }
    @Override protected String description() { return "Compare native render passes with dynamic-rendering translation using actual GPU pixels and unchanged caller data."; }
    @Override protected void addControls(LinearLayout content) {
        String[][] modes = {{"rendering-native", "Native reference"}, {"rendering-normal", "Automatic Mali repair"},
            {"rendering-force", "Force repair"}};
        for (String[] mode : modes) addMode(content, mode[1], () -> runProbe(mode[0], 64));
    }
    @Override protected boolean accepts(String mode, int size) {
        return java.util.Arrays.asList("rendering-native", "rendering-normal", "rendering-force").contains(mode);
    }
    @Override protected String[] arguments(String directory, String mode, int size, File output) {
        return new String[]{directory + "/libhybris_dynamic_rendering_probe.so", directory, mode, output.getPath()};
    }
}
