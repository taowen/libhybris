/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.widget.LinearLayout;
import java.io.File;

public final class SpecConstantsProbeActivity extends ProbeActivity {
    @Override protected String probeId() { return "spec_constants"; }
    @Override protected String probeTitle() { return "Mali specialization constants"; }
    @Override protected String description() { return "Compare validator-checked reference shaders with the historical composite form after repair. Samples a real texture using default, overridden and nested specialization constants. Native and disabled routes use only valid reference shaders."; }
    @Override protected void addControls(LinearLayout content) {
        String[][] modes = {{"spec-native", "Native reference"}, {"spec-normal", "Automatic Mali repair"},
            {"spec-force", "Force repair"}, {"spec-off", "Repair disabled · reference"}};
        for (String[] mode : modes) addMode(content, mode[1], () -> runProbe(mode[0], 64));
    }
    @Override protected boolean accepts(String mode, int size) {
        return java.util.Arrays.asList("spec-native", "spec-normal", "spec-force", "spec-off").contains(mode);
    }
    @Override protected String[] arguments(String directory, String mode, int size, File output) {
        return new String[]{directory + "/libhybris_spec_constants_probe.so", directory, mode, output.getPath()};
    }
}
