/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.widget.LinearLayout;
import java.io.File;

public final class RobustnessProbeActivity extends ProbeActivity {
    @Override protected String probeId() { return "robustness"; }
    @Override protected String probeTitle() { return "Robustness2"; }
    @Override protected String description() { return "GPU reads, writes and atomics using null and out-of-bounds descriptors. Compatibility approximations are reported separately from failures. These checks do not establish Vulkan conformance."; }
    @Override protected void addControls(LinearLayout content) {
        String[][] modes = {{"robust-native", "Native driver"}, {"robust", "Compatibility layer · normal policy"},
            {"robust-force", "Force compatibility"}, {"robust-off", "Compatibility disabled"}};
        for (String[] mode : modes) addMode(content, mode[1], () -> runProbe(mode[0], 64));
    }
    @Override protected boolean accepts(String mode, int size) {
        return java.util.Arrays.asList("robust-native", "robust", "robust-force", "robust-off").contains(mode);
    }
    @Override protected String[] arguments(String directory, String mode, int size, File output) {
        return new String[]{directory + "/libhybris_robustness_probe.so", directory, mode, output.getPath()};
    }
}
