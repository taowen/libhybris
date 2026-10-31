/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.widget.LinearLayout;
import java.io.File;

public final class Maintenance5ProbeActivity extends ProbeActivity {
    @Override protected String probeId() { return "maintenance5"; }
    @Override protected String probeTitle() { return "Maintenance5"; }
    @Override protected String description() { return "Compare native render passes with maintenance5 flags, inline stages, indexed draws and remaining image layers using actual GPU pixels and unchanged caller data."; }
    @Override protected void addControls(LinearLayout content) {
        String[][] modes = {{"maintenance-native", "Native reference"}, {"maintenance-normal", "Automatic compatibility"},
            {"maintenance-force", "Force repair"}};
        for (String[] mode : modes) addMode(content, mode[1], () -> runProbe(mode[0], 64));
    }
    @Override protected boolean accepts(String mode, int size) {
        return java.util.Arrays.asList("maintenance-native", "maintenance-normal", "maintenance-force").contains(mode);
    }
    @Override protected String[] arguments(String directory, String mode, int size, File output) {
        return new String[]{directory + "/libhybris_maintenance5_probe.so", directory, mode, output.getPath()};
    }
}
