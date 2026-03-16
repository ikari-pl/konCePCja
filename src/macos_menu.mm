#import <Cocoa/Cocoa.h>
#include "menu_actions.h"
#include "keyboard.h"
#include "imgui.h"
#include "SDL3/SDL.h"
#include <memory>
#include <mach/mach_time.h>

@interface KoncepcjaMenuTarget : NSObject
@end

extern "C" void koncpc_menu_action(int action);

@implementation KoncepcjaMenuTarget
- (void)menuAction:(id)sender {
  NSInteger action = [sender tag];
  koncpc_menu_action(static_cast<int>(action));
}
@end

static void applyShortcut(NSMenuItem *item, const char *shortcut) {
  if (!shortcut || !shortcut[0]) return;
  NSString *s = [NSString stringWithUTF8String:shortcut];
  NSString *upper = [s uppercaseString];
  NSEventModifierFlags mods = 0;
  if ([upper containsString:@"SHIFT+"]) mods |= NSEventModifierFlagShift;
  if ([upper containsString:@"CMD+"] || [upper containsString:@"COMMAND+"]) mods |= NSEventModifierFlagCommand;
  if ([upper containsString:@"ALT+"] || [upper containsString:@"OPTION+"]) mods |= NSEventModifierFlagOption;
  if ([upper containsString:@"CTRL+"] || [upper containsString:@"CONTROL+"]) mods |= NSEventModifierFlagControl;

  // Extract the key part after all modifiers (everything after the last '+')
  NSRange lastPlus = [upper rangeOfString:@"+" options:NSBackwardsSearch];
  NSString *keyPart = (lastPlus.location != NSNotFound)
    ? [upper substringFromIndex:lastPlus.location + 1]
    : upper;

  unichar key = 0;
  if ([keyPart hasPrefix:@"F"] && [keyPart length] >= 2) {
    NSInteger fn = [[keyPart substringFromIndex:1] integerValue];
    switch (fn) {
      case 1: key = NSF1FunctionKey; break;
      case 2: key = NSF2FunctionKey; break;
      case 3: key = NSF3FunctionKey; break;
      case 4: key = NSF4FunctionKey; break;
      case 5: key = NSF5FunctionKey; break;
      case 6: key = NSF6FunctionKey; break;
      case 7: key = NSF7FunctionKey; break;
      case 8: key = NSF8FunctionKey; break;
      case 9: key = NSF9FunctionKey; break;
      case 10: key = NSF10FunctionKey; break;
      case 11: key = NSF11FunctionKey; break;
      case 12: key = NSF12FunctionKey; break;
      default: break;
    }
  } else if ([keyPart isEqualToString:@"PAUSE"]) {
    key = NSPauseFunctionKey;
  }

  if (key) {
    NSString *ke = [NSString stringWithCharacters:&key length:1];
    [item setKeyEquivalent:ke];
    [item setKeyEquivalentModifierMask:mods];
  }
}

static const MenuAction* find_menu_action(KONCPC_KEYS action) {
  for (const auto &entry : koncpc_menu_actions()) {
    if (entry.action == action) return &entry;
  }
  return nullptr;
}

static void add_menu_group(NSMenu *mainMenu, KoncepcjaMenuTarget *target, NSString *title, std::initializer_list<KONCPC_KEYS> actions) {
  for (NSMenuItem *item in [mainMenu itemArray]) {
    if ([[item title] isEqualToString:title]) return;
  }

  NSMenuItem *menuItem = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
  NSMenu *submenu = [[NSMenu alloc] initWithTitle:title];

  for (KONCPC_KEYS action : actions) {
    const MenuAction *entry = find_menu_action(action);
    if (!entry) continue;
    NSString *itemTitle = [NSString stringWithUTF8String:entry->title];
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:itemTitle action:@selector(menuAction:) keyEquivalent:@""];
    [item setTarget:target];
    [item setTag:static_cast<NSInteger>(entry->action)];
    applyShortcut(item, entry->shortcut);
    [submenu addItem:item];
  }

  [menuItem setSubmenu:submenu];
  [mainMenu addItem:menuItem];
}

static void koncpc_install_emulator_menu(NSMenu *mainMenu) {
  if (!mainMenu) return;

  KoncepcjaMenuTarget *target = [[KoncepcjaMenuTarget alloc] init];

  add_menu_group(mainMenu, target, @"Emulator", {
    KONCPC_GUI,
    KONCPC_FULLSCRN,
    KONCPC_RESET,
    KONCPC_EXIT,
  });

  add_menu_group(mainMenu, target, @"Media", {
    KONCPC_TAPEPLAY,
    KONCPC_MF2STOP,
    KONCPC_NEXTDISKA,
  });

  add_menu_group(mainMenu, target, @"Tools", {
    KONCPC_VKBD,
    KONCPC_DEVTOOLS,
    KONCPC_SCRNSHOT,
    KONCPC_SNAPSHOT,
    KONCPC_LD_SNAP,
    KONCPC_PASTE,
  });

  add_menu_group(mainMenu, target, @"Options", {
    KONCPC_JOY,
    KONCPC_PHAZER,
    KONCPC_FPS,
    KONCPC_SPEED,
    KONCPC_DEBUG,
    KONCPC_DELAY,
    KONCPC_WAITBREAK,
  });
}

extern "C" __attribute__((visibility("default"))) void SDL_CocoaAddMenuItems(NSMenu *mainMenu) {
  @autoreleasepool {
    koncpc_install_emulator_menu(mainMenu);
  }
}

void koncpc_setup_macos_menu() {
  @autoreleasepool {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    koncpc_install_emulator_menu([NSApp mainMenu]);
  }
}

// App Nap control: used by screenshot capture to ensure rendering continues
// when the emulator window is in the background.
static id<NSObject> g_app_nap_activity = nil;

void koncpc_disable_app_nap() {
  @autoreleasepool {
    if (!g_app_nap_activity) {
      g_app_nap_activity = [[NSProcessInfo processInfo]
          beginActivityWithOptions:NSActivityUserInitiated
          reason:@"Screenshot capture in progress"];
    }
  }
}

void koncpc_enable_app_nap() {
  @autoreleasepool {
    if (g_app_nap_activity) {
      [[NSProcessInfo processInfo] endActivity:g_app_nap_activity];
      g_app_nap_activity = nil;
    }
  }
}

void koncpc_activate_app() {
  // Must dispatch to main thread for NSApp activation
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      [NSApp activateIgnoringOtherApps:YES];
    }
  });
}

extern SDL_Window* mainSDLWindow;

static NSWindow* nswindow_from_viewport(ImGuiViewport* vp) {
  SDL_WindowID wid = (SDL_WindowID)(uintptr_t)vp->PlatformHandle;
  SDL_Window* sdlWin = SDL_GetWindowFromID(wid);
  if (!sdlWin) return nil;
  return (__bridge NSWindow*)SDL_GetPointerProperty(
      SDL_GetWindowProperties(sdlWin),
      SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
}

void koncpc_order_viewports_above_main() {
  // On macOS, ImGui's SDL3 backend skips SDL_SetWindowParent() because it
  // breaks multi-monitor support.  Instead we use Cocoa's orderWindow: to
  // keep ImGui viewport windows above the main emulator window without
  // making them system-level always-on-top.
  //
  // Two tiers:
  //   1. Tool windows (with title bar) → above main emulator window
  //   2. Popups/menus/dropdowns (NoTaskBarIcon) → above all tool windows
  if (!mainSDLWindow) return;

  NSWindow* mainNS = (__bridge NSWindow*)SDL_GetPointerProperty(
      SDL_GetWindowProperties(mainSDLWindow),
      SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
  if (!mainNS) return;
  NSInteger mainNum = [mainNS windowNumber];

  ImGuiPlatformIO& pio = ImGui::GetPlatformIO();

  // Pass 1: tool windows above main emulator
  for (int i = 1; i < pio.Viewports.Size; i++) {
    ImGuiViewport* vp = pio.Viewports[i];
    if (vp->Flags & ImGuiViewportFlags_NoTaskBarIcon) continue;  // popup
    NSWindow* ns = nswindow_from_viewport(vp);
    if (ns && [ns windowNumber] != mainNum) {
      [ns orderWindow:NSWindowAbove relativeTo:mainNum];
    }
  }

  // Pass 2: popups/menus above everything (orderFront without stealing focus)
  for (int i = 1; i < pio.Viewports.Size; i++) {
    ImGuiViewport* vp = pio.Viewports[i];
    if (!(vp->Flags & ImGuiViewportFlags_NoTaskBarIcon)) continue;  // tool
    NSWindow* ns = nswindow_from_viewport(vp);
    if (ns) {
      [ns orderFront:nil];
    }
  }
}

// ── Dock icon ──────────────────────────────────────────────

static NSImage* g_icon_overlay = nil;  // CRT overlay (translucent screen area)
static NSImage* g_static_icon = nil;   // static logo shown at startup / fallback
static std::atomic<bool> g_icon_update_in_flight{false};
static std::atomic<bool> g_icon_preview_disabled{false}; // auto-disabled if too slow
static uint64_t g_icon_last_time_ns = 0;       // last update duration
static int g_icon_slow_count = 0;              // consecutive slow updates

// Screen region in 850x759 icon (proportional coordinates)
static constexpr CGFloat kScreenX = 0.2141;
static constexpr CGFloat kScreenY = 0.4800;
static constexpr CGFloat kScreenW = 0.4918;
static constexpr CGFloat kScreenH = 0.4350;

static uint64_t nanos_now() {
  static mach_timebase_info_data_t tb = {0, 0};
  if (tb.denom == 0) mach_timebase_info(&tb);
  return mach_absolute_time() * tb.numer / tb.denom;
}

void koncpc_set_dock_icon(const char* png_path) {
  @autoreleasepool {
    if (!png_path) return;
    NSString* path = [NSString stringWithUTF8String:png_path];

    // Load the CRT overlay (koncepcja-icon.png — translucent screen area)
    g_icon_overlay = [[NSImage alloc] initWithContentsOfFile:path];

    // Load the static logo (koncepcja-logo.png — shown before live preview starts)
    NSString* logoPath = [[path stringByDeletingLastPathComponent]
      stringByAppendingPathComponent:@"koncepcja-logo.png"];
    g_static_icon = [[NSImage alloc] initWithContentsOfFile:logoPath];

    // Set the static logo as the initial Dock icon
    NSImage* initial = g_static_icon ? g_static_icon : g_icon_overlay;
    if (initial) {
      // Center in a square canvas
      NSSize sz = [initial size];
      CGFloat side = fmax(sz.width, sz.height);
      NSImage* sq = [[NSImage alloc] initWithSize:NSMakeSize(side, side)];
      [sq lockFocus];
      [initial drawInRect:NSMakeRect((side - sz.width)/2, (side - sz.height)/2, sz.width, sz.height)
                 fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];
      [sq unlockFocus];
      [NSApp setApplicationIconImage:sq];
    }
  }
}

void koncpc_update_dock_icon_preview(const void* pixels, int surface_w, int surface_h,
                                     int pitch, int vis_x, int vis_y, int vis_w, int vis_h) {
  (void)surface_w; (void)surface_h;
  if (!pixels || vis_w <= 0 || vis_h <= 0 || !g_icon_overlay) return;

  // Skip if preview was auto-disabled due to being too slow
  if (g_icon_preview_disabled.load(std::memory_order_relaxed)) return;

  // Skip if a previous update is still in flight (don't queue up work)
  if (g_icon_update_in_flight.exchange(true)) return;

  // Copy visible pixels for async use
  size_t row_bytes = static_cast<size_t>(vis_w) * 4;
  size_t buf_size = static_cast<size_t>(vis_h) * row_bytes;
  std::shared_ptr<uint8_t> px(new uint8_t[buf_size], std::default_delete<uint8_t[]>());
  const uint8_t* src = static_cast<const uint8_t*>(pixels);
  for (int y = 0; y < vis_h; y++) {
    memcpy(px.get() + y * row_bytes,
           src + (vis_y + y) * pitch + vis_x * 4,
           row_bytes);
  }

  int w = vis_w, h = vis_h;

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    uint64_t t0 = nanos_now();
    @autoreleasepool {
      CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
      CGContextRef ctx = CGBitmapContextCreate(
        px.get(), (size_t)w, (size_t)h, 8, row_bytes,
        cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
      CGColorSpaceRelease(cs);
      if (!ctx) { g_icon_update_in_flight.store(false); return; }

      CGImageRef cgScreen = CGBitmapContextCreateImage(ctx);
      CGContextRelease(ctx);
      if (!cgScreen) { g_icon_update_in_flight.store(false); return; }

      // Square canvas with centered icon
      NSSize iconSize = [g_icon_overlay size];
      CGFloat side = fmax(iconSize.width, iconSize.height);
      CGFloat ox = (side - iconSize.width) / 2;
      CGFloat oy = (side - iconSize.height) / 2;

      NSImage* composite = [[NSImage alloc] initWithSize:NSMakeSize(side, side)];
      [composite lockFocus];

      // 1. Live CPC screen
      CGFloat pad_x = iconSize.width * 0.005;
      CGFloat pad_y = iconSize.height * 0.005;
      NSRect screenRect = NSMakeRect(
        ox + iconSize.width * kScreenX - pad_x,
        oy + iconSize.height * kScreenY - pad_y,
        iconSize.width * kScreenW + pad_x * 2,
        iconSize.height * kScreenH + pad_y * 2);
      NSImage* screenImg = [[NSImage alloc] initWithCGImage:cgScreen size:NSMakeSize(w, h)];
      [screenImg drawInRect:screenRect fromRect:NSZeroRect
                  operation:NSCompositingOperationSourceOver fraction:1.0];

      // 2. CRT overlay on top
      NSRect iconRect = NSMakeRect(ox, oy, iconSize.width, iconSize.height);
      [g_icon_overlay drawInRect:iconRect fromRect:NSZeroRect
                       operation:NSCompositingOperationSourceOver fraction:1.0];

      [composite unlockFocus];
      CGImageRelease(cgScreen);

      dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp setApplicationIconImage:composite];
        g_icon_update_in_flight.store(false);

        // Measure time and auto-disable if consistently too slow (>8ms)
        uint64_t elapsed_ns = nanos_now() - t0;
        g_icon_last_time_ns = elapsed_ns;
        if (elapsed_ns > 8000000) { // >8ms
          g_icon_slow_count++;
          if (g_icon_slow_count >= 5) {
            g_icon_preview_disabled.store(true);
            // Fall back to static logo
            if (g_static_icon) {
              NSSize sz = [g_static_icon size];
              CGFloat s = fmax(sz.width, sz.height);
              NSImage* sq = [[NSImage alloc] initWithSize:NSMakeSize(s, s)];
              [sq lockFocus];
              [g_static_icon drawInRect:NSMakeRect((s-sz.width)/2,(s-sz.height)/2,sz.width,sz.height)
                               fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];
              [sq unlockFocus];
              [NSApp setApplicationIconImage:sq];
            }
          }
        } else {
          g_icon_slow_count = 0; // reset on a fast update
        }
      });
    }
  });
}
