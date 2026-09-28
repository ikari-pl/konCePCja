# Launcher change: run an external full-screen application

Companion to `0005-lvgl-drm-release-reacquire-master.patch`.

## Where

`k230_launcher/k230_phone_ui/src/main.c`. The display is created once at
**main.c:12802** (`disp = lv_linux_drm_create()`) and never released. The
existing `system()` call sites — **2642, 2926, 3163** — are the pattern to
follow for shelling out.

## What

```c
/* Run an external application that needs the panel.
 *
 * DRM allows one master per device, so the display has to be handed over for
 * the duration: the child cannot modeset while we hold it (drmSetMaster
 * returns EBUSY, drmModeSetPlane EACCES), and two clients flipping the same
 * CRTC tears. Releasing and reacquiring keeps the LVGL display object alive,
 * so the UI is still there when the app exits.
 */
static int launch_external_app(const char * cmd)
{
    if(lv_linux_drm_release(disp) != LV_RESULT_OK) {
        LV_LOG_WARN("could not release the display; not launching %s", cmd);
        return -1;
    }

    int const rc = system(cmd);

    if(lv_linux_drm_acquire(disp) != LV_RESULT_OK) {
        /* Without the display back the UI is invisible and unrecoverable, so
         * make the failure loud rather than leaving a black screen. */
        LV_LOG_ERROR("could not reacquire the display after %s", cmd);
        return -2;
    }

    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    return rc;
}
```

Then an app entry, e.g.:

```c
launch_external_app("/root/koncepcja/koncepcja-app.sh");
```

## Notes

- `system()` blocks, which is what we want: one full-screen app at a time, and
  the UI is not running while the panel belongs to someone else.
- The child must not be killed by the parent's signals; a wrapper script that
  `exec`s the real binary keeps process management simple.
- Apps that do **not** need the panel (`face_detect.elf` writes a JPEG,
  `ai2d_kpu.elf` writes a .bin, `camera_rtsp_demo` streams over the network)
  should keep using plain `system()` — they work fine alongside the UI today
  and releasing the display for them would blank the screen for nothing.

## A nicer variant, if it is worth the work

`drmModeCreateLease()` can hand a **single overlay plane** to the child while
the launcher keeps the primary. The panel exposes seven planes and the launcher
drives only the primary (id 34), so the hardware already supports compositing
an app above the live UI. Only the master can create a lease, which is exactly
why this has to come from the launcher side.

## Verified on hardware (T-Display K230, v0.2.4 image)

With `k230_phone_ui` running:

```
drmSetMaster    -> EBUSY  (Device or resource busy)
drmModeSetPlane -> EACCES (Permission denied)   # even for an idle overlay
```

and `strings` on the shipped binary shows no `drmDropMaster` / `drmSetMaster` /
`drmModeCreateLease`, so there is currently no path by which any external
application can drive the display.
