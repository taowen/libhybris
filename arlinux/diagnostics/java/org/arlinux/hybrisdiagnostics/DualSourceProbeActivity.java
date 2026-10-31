/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.widget.LinearLayout;
import java.io.File;

public final class DualSourceProbeActivity extends ProbeActivity {
    @Override protected String probeId() { return "dual_source"; }
    @Override protected String probeTitle() { return "Dual-source blend compatibility"; }
    @Override protected String description() { return "MaliBridge approximation: SRC1 factors use SRC0 and the second shader output is discarded. Compares actual blended pixels and rejects output interfaces that cannot be safely rewritten. This is not true dual-source blending."; }
    @Override protected void addControls(LinearLayout content) {
        String[][] modes = {{"dual-native", "Native driver"}, {"dual-normal", "Automatic compatibility"},
            {"dual-force", "Force blend approximation"}, {"dual-off", "Compatibility disabled"}, {"dual-unrequested", "Feature not enabled"}};
        for (String[] mode : modes) addMode(content, mode[1], () -> runProbe(mode[0], 64));
    }
    @Override protected boolean accepts(String mode, int size) {
        return java.util.Arrays.asList("dual-native", "dual-normal", "dual-force", "dual-off", "dual-unrequested").contains(mode);
    }
    @Override protected String[] arguments(String directory, String mode, int size, File output) {
        return new String[]{directory + "/libhybris_dual_source_probe.so", directory, mode, output.getPath()};
    }
}
