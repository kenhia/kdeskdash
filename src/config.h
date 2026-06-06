/**
 * @file config.h
 * kdeskdash runtime configuration (environment overrides).
 */
#ifndef KDESKDASH_CONFIG_H
#define KDESKDASH_CONFIG_H

typedef struct {
    const char *drm_dev;   /* KDESKDASH_DRM_DEV   — default /dev/dri/card1 (vc4 GPU) */
    const char *touch_dev; /* KDESKDASH_TOUCH_DEV — default /dev/input/event1 (ILITEK) */
} kdeskdash_config_t;

/* Populate cfg from the environment, falling back to defaults. */
void config_load(kdeskdash_config_t *cfg);

#endif /* KDESKDASH_CONFIG_H */
