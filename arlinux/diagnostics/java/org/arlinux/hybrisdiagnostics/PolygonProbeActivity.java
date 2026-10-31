/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.widget.LinearLayout;
import java.io.File;

public final class PolygonProbeActivity extends ProbeActivity {
    @Override protected String probeId() { return "polygon"; }
    @Override protected String probeTitle() { return "Polygon line and point modes"; }
    @Override protected String description() { return "Compares LINE and POINT pixels with explicit native line/point primitives in ordinary render passes, including indexed/secondary draws and GPU uploads. Culling and depth bias are not emulated."; }
    @Override protected void addControls(LinearLayout content) {
        String[][] modes = {{"polygon-native", "Native primitive references"}, {"polygon-normal", "Automatic polygon fallback"},
            {"polygon-force", "Force polygon fallback"}, {"polygon-off", "Fallback disabled · references"}};
        for (String[] mode : modes) addMode(content, mode[1], () -> runProbe(mode[0], 64));
    }
    @Override protected boolean accepts(String mode, int size) {
        return java.util.Arrays.asList("polygon-native", "polygon-normal", "polygon-force", "polygon-off").contains(mode);
    }
    @Override protected String[] arguments(String directory, String mode, int size, File output) {
        return new String[]{directory + "/libhybris_polygon_probe.so", directory, mode, output.getPath()};
    }
}
