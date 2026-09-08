
SUBDIRS = common null fbdev
if HAS_ANDROID_4_2_0
SUBDIRS += hwcomposer
endif
if HAS_ANDROID_5_0_0
SUBDIRS += hwcomposer
endif

# Desktop windows use Mesa with the standard loader and ICD/Turnip WSI.
# Frontend Wayland/X11 plugins have been removed.
