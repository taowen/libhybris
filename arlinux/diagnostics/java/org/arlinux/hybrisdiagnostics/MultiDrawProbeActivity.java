/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.widget.LinearLayout;
import java.io.File;

public final class MultiDrawProbeActivity extends ProbeActivity {
    @Override protected String probeId() { return "multidraw"; }
    @Override protected String probeTitle() { return "Indirect multi-draw compatibility"; }
    @Override protected String description() { return "MaliBridge indirect-command splitting. Compares GPU-produced indirect commands, indexed draws, nonzero offsets and padded strides. Shader DrawIndex is not emulated."; }
    @Override protected void addControls(LinearLayout content) {
        String[][] modes = {{"indirect-native", "Native driver"}, {"indirect-normal", "Automatic compatibility"},
            {"indirect-force", "Force indirect splitting"}, {"indirect-off", "Compatibility disabled"}, {"indirect-unrequested", "Feature not enabled"}};
        for (String[] mode : modes) addMode(content, mode[1], () -> runProbe(mode[0], 64));
    }
    @Override protected boolean accepts(String mode, int size) {
        return java.util.Arrays.asList("indirect-native", "indirect-normal", "indirect-force", "indirect-off", "indirect-unrequested").contains(mode);
    }
    @Override protected String[] arguments(String directory, String mode, int size, File output) {
        return new String[]{directory + "/libhybris_multidraw_probe.so", directory, mode, output.getPath()};
    }
}
