/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.widget.LinearLayout;
import java.io.File;

public final class DistanceProbeActivity extends ProbeActivity {
    @Override protected String probeId() { return "distance"; }
    @Override protected String probeTitle() { return "MaliBridge shader interfaces"; }
    @Override protected String description() { return "MaliBridge Clip/Cull lowering and fragment ClipDistance reads. CullDistance is demoted without culling when native support is absent; this limitation is reported."; }
    @Override protected void addControls(LinearLayout content) {
        String[][] modes = {{"distance-native", "Native driver"}, {"distance-normal", "Automatic compatibility"}};
        for (String[] mode : modes) addMode(content, mode[1], () -> runProbe(mode[0], 64));
    }
    @Override protected boolean accepts(String mode, int size) {
        return java.util.Arrays.asList("distance-native", "distance-normal").contains(mode);
    }
    @Override protected String[] arguments(String directory, String mode, int size, File output) {
        return new String[]{directory + "/libhybris_distance_probe.so", directory, mode, output.getPath()};
    }
}
