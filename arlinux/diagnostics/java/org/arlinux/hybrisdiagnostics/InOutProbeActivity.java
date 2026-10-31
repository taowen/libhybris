/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.widget.LinearLayout;
import java.io.File;

public final class InOutProbeActivity extends ProbeActivity {
    @Override protected String probeId() { return "inout"; }
    @Override protected String probeTitle() { return "Shader interface compatibility"; }
    @Override protected String description() { return "VS vec4 to FS vec2 linkage, with render-pass, pipeline feedback and dynamic rendering. Reads back actual GPU pixels; records whether normalization ran."; }
    @Override protected void addControls(LinearLayout content) {
        String[][] modes = {{"inout-native", "Native driver"}, {"inout-normal", "Automatic compatibility"},
            {"inout-off", "Compatibility disabled"}};
        for (String[] mode : modes) addMode(content, mode[1], () -> runProbe(mode[0], 64));
    }
    @Override protected boolean accepts(String mode, int size) {
        return java.util.Arrays.asList("inout-native", "inout-normal", "inout-off").contains(mode);
    }
    @Override protected String[] arguments(String directory, String mode, int size, File output) {
        return new String[]{directory + "/libhybris_inout_probe.so", directory, mode, output.getPath()};
    }
}
