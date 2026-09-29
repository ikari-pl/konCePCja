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

`release()` records the CRTC in `drm_dev->saved_crtc`. That field is declared in
`drm_dev_t` and restored by `drm_del_event_cb()`, but nothing has ever assigned
it, so the restore on display delete was dead code; it now does something.

Launcher side (`launcher-lvglsim.patch`), modelled on the existing `system()`
call sites (2642 / 2926 / 3163):

```c
static int launch_external_app(const char *cmd)
{
    int rc;

    if(!main_display) {
        return -1;
    }

    if(lv_linux_drm_release(main_display) != LV_RESULT_OK) {
        touch_trace_log("EXTERNAL_APP_RELEASE_FAIL cmd=%s", cmd);
        return -1;
    }

    rc = system(cmd);

    if(lv_linux_drm_acquire(main_display) != LV_RESULT_OK) {
        touch_trace_log("EXTERNAL_APP_ACQUIRE_FAIL cmd=%s", cmd);
        return -2;
    }

    /* The panel is showing whatever the child last drew on it */
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(main_display);

    return rc;
}
```

### The caller is lvglsim

`lvglsim` already ships in this image at `/root/app/lvglsim` and nothing has ever
been able to start it -- it is an LVGL DRM client, so it needs the panel to
itself. It is reachable now through a `PAGE_LVGL_DEMO` tile, which is the same
shape as the Reboot page: a `command_button` that arms a one-shot
`lv_timer_create(..., 250, NULL)`.

The timer matters. `system()` blocks the LVGL loop for as long as the child runs,
so the button handler only sets the "Running..." label and arms the timer; the
label reaches the panel on the next refresh, and the blocking call happens after
that. `reboot_confirm_event_cb()` / `reboot_timer_cb()` do exactly this.

`render_page()`'s switch has no `default:`, so the new enumerator must be handled
there or the build warns -- which is the right place for it anyway. The other
seven switches over `page_id_t` all have a `default:`; `page_name()` gets a case
regardless so the touch traces name the page like every other one.

## Dropping master is not enough on its own

This is the part worth reviewing closely, because the obvious implementation
hangs the UI.

While not master, `drmModePageFlip` fails with `EACCES`. `drm_dev->req` stays
set, and `drm_flush_wait()` sits in `poll(&pfd, 1, -1)` -- an infinite wait --
for a completion event that can never arrive. The UI freezes, and **stays frozen
after the panel is handed back**, because the stuck state is inside LVGL, not in
the kernel. Restoring the CRTC does not recover it.

So `release()` also gates flushing: while paused, `drm_flush()` clears
`act_buf` and returns without submitting anything, and `drm_flush_wait()`
returns immediately. No ioctls are issued and LVGL's state machine stays
consistent -- this driver signals completion through `flush_wait_cb` and never
calls `lv_display_flush_ready()`, so there is nothing else to unwind.

`release()` first waits out the flip already in flight, while it is still
master, so the completion event is consumed rather than left queued on the fd
to be delivered spuriously after the next `acquire()`. That wait is bounded
(`DRM_RELEASE_FLIP_TIMEOUT_MS`), so `release()` cannot itself become the thing
that blocks forever.

The poll loop is factored out into `drm_wait_flip(drm_dev, timeout_ms)`, which
`drm_flush_wait()` calls with `-1`; its behaviour is unchanged.

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

The patch itself applies and compiles. Base is LVGL `59dc7e43` with this
overlay's `0001`--`0004` applied, the same tree Buildroot builds; `0005` applies
with no fuzz and no rejects, and the driver translation unit was compiled with
`-Wall -Wextra` both for the host and for riscv64 with the SDK's own
`Xuantie-900-gcc-linux-6.6.0-glibc` 14.1.1:

| build | host | riscv64 | `release`+`acquire` symbols |
| --- | --- | --- | --- |
| `0001`--`0004` | exit 0, 18680 B | exit 0, 42856 B | 0 |
| `0001`--`0005` | exit 0, 19768 B | exit 0, 45792 B | 2 |

No warnings from the changed code. The symbol counts are there because the file
sits inside `#if LV_USE_LINUX_DRM`, so a misconfigured `lv_conf.h` compiles to
an empty object and exits 0 -- the object is checked with `nm`, not just the
exit status. `compile-check.sh` alongside this patch reproduces the table.

## Not in this PR

Adding third-party apps to the launcher's grid (icons, a runtime manifest) is a
separate design question and belongs in its own change. This PR wires exactly
one caller, and that caller is a binary this image already ships; it does not
add a way for anything else to ask for the panel.

`drmModeCreateLease()` would be the richer alternative -- the panel exposes
seven planes and the launcher drives only the primary, so an app could composite
above a live UI. Only the master can create a lease, so it would also have to
come from the launcher side. Worth considering later; release/acquire is the
minimal version and matches how a handheld actually behaves, one full-screen app
at a time.
