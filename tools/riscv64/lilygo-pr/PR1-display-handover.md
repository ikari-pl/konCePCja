# Let the launcher hand the display to an external application

## Problem

The T-Display K230 cannot run any external full-screen application.

`k230_phone_ui` creates its display once (`main.c:12802`, `lv_linux_drm_create()`)
and holds DRM master for its entire lifetime. DRM permits exactly one master per
device, so nothing else can drive the panel. Measured on a stock v0.2.4 image
with the launcher running:

```
drmSetMaster    -> EBUSY   (Device or resource busy)
drmModeSetPlane -> EACCES  (Permission denied)   # even for an idle overlay plane
```

`strings` on the shipped binary finds no `drmDropMaster`, `drmSetMaster` or
`drmModeCreateLease`, so there is no handover path of any kind.

This is invisible today because nothing has ever needed one. Every app in the UI
is either an LVGL screen inside `k230_phone_ui` itself, or headless:

| binary | links libdrm | what it does |
| --- | --- | --- |
| `face_detect.elf` | yes | writes `face_detection_result.jpg` |
| `ai2d_kpu.elf` | no | writes `result.bin` |
| `camera_rtsp_demo` | no | streams over the network |
| `lvglsim` | yes | a real display client -- the launcher never invokes it |

Confirmed by running `face_detect_image.sh` alongside the launcher: it completes
normally and never touches the panel.

## Change

Two calls in the LVGL DRM driver, and a helper in the launcher that uses them.

`lv_linux_drm_release()` / `lv_linux_drm_acquire()` are added in
`0005-lvgl-drm-release-reacquire-master.patch`, which sits alongside the four
LVGL patches already carried in the Buildroot overlay -- one of which
(`0002-...-add-k230-plane-rotation`) modifies this same file.

Launcher side, modelled on the existing `system()` call sites (2642 / 2926 / 3163):

```c
static int launch_external_app(const char * cmd)
{
    if(lv_linux_drm_release(disp) != LV_RESULT_OK) {
        LV_LOG_WARN("could not release the display; not launching %s", cmd);
        return -1;
    }

    int const rc = system(cmd);

    if(lv_linux_drm_acquire(disp) != LV_RESULT_OK) {
        LV_LOG_ERROR("could not reacquire the display after %s", cmd);
        return -2;
    }

    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    return rc;
}
```

## Dropping master is not enough on its own

This is the part worth reviewing closely, because the obvious implementation
hangs the UI.

While not master, `drmModePageFlip` fails with `EACCES`. `drm_dev->req` stays
set, and `drm_flush_wait()` sits in `poll(&pfd, 1, -1)` -- an infinite wait --
for a completion event that can never arrive. The UI freezes, and **stays frozen
after the panel is handed back**, because the stuck state is inside LVGL, not in
the kernel. Restoring the CRTC does not recover it.

So `release()` also gates flushing: while paused, `drm_flush()` calls
`lv_display_flush_ready()` and returns without submitting anything, and
`drm_flush_wait()` returns immediately. LVGL's state machine stays consistent
and no ioctls are issued. `release()` first waits out any in-flight flip, so its
completion is not lost.

We reproduced the freeze twice before understanding it; the fix is small but it
is not optional.

## Verification

The mechanism was proven on hardware before this patch was written, using an
`LD_PRELOAD` shim that performs exactly the same two operations on the
launcher's own fd (DRM master belongs to the open file description, so the fd
can be found by scanning `/proc/self/fd`). With the launcher running throughout:

```
[drm_yield] drmDropMaster(fd=14) rc=0
konCePCja   drm_present: 1232x568 landscape on connector 54
            22 fps, flips ok=22 fail=0
[drm_yield] drmSetMaster(fd=14) rc=0
card0 holder: k230_phone_ui        # launcher never restarted, stayed responsive
```

The external application was an Amstrad CPC emulator presenting via DRM dumb
buffers at RGB565.

Caveat: the patch itself is **not compile-tested** against the Buildroot tree --
we had no SDK checkout. The logic mirrors the verified shim, and field names are
checked against LVGL `59dc7e43`, the revision pinned in
`package/lvgl/59dc7e436ae97a25e32656739ea6a943f9f11b6a/`.

## Not in this PR

Adding third-party apps to the launcher's grid (icons, a runtime manifest) is a
separate design question and belongs in its own change. This PR only adds the
capability to hand the display over; it does not decide who may use it.

`drmModeCreateLease()` would be the richer alternative -- the panel exposes
seven planes and the launcher drives only the primary, so an app could composite
above a live UI. Only the master can create a lease, so it would also have to
come from the launcher side. Worth considering later; release/acquire is the
minimal version and matches how a handheld actually behaves, one full-screen app
at a time.
