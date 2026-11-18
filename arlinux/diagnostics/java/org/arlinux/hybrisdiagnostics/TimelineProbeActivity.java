/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.arlinux.hybrisdiagnostics;

import android.widget.LinearLayout;
import java.io.File;

public final class TimelineProbeActivity extends ProbeActivity {
    @Override protected String probeId() { return "timeline"; }
    @Override protected String probeTitle() { return "Timeline semaphores"; }
    @Override protected String description() {
        return "Check host waits, mixed semaphore submissions and GPU readback.";
    }
    @Override protected void addControls(LinearLayout content) {
        addMode(content, "Native driver", () -> runProbe("timeline-native", 64));
        addMode(content, "Compatibility layer", () -> runProbe("timeline-compat", 64));
    }
    @Override protected boolean accepts(String mode, int size) {
        return "timeline-native".equals(mode) || "timeline-compat".equals(mode);
    }
    @Override protected String[] arguments(String directory, String mode, int size, File output) {
        return new String[]{directory + "/libhybris_timeline_probe.so", directory, mode, output.getPath()};
    }
}
