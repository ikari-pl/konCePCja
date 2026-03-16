#import <Cocoa/Cocoa.h>
#include "menu_actions.h"
#include "keyboard.h"
#include "imgui.h"
#include "SDL3/SDL.h"

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

static NSImage* g_base_icon = nil;  // the single icon file — also serves as CRT overlay

void koncpc_set_dock_icon(const char* png_path) {
  @autoreleasepool {
    if (!png_path) return;
    NSString* path = [NSString stringWithUTF8String:png_path];
    g_base_icon = [[NSImage alloc] initWithContentsOfFile:path];
    if (g_base_icon) {
      [NSApp setApplicationIconImage:g_base_icon];
    }
  }
}

void koncpc_update_dock_icon_preview(const void* pixels, int surface_w, int surface_h,
                                     int pitch, int vis_x, int vis_y, int vis_w, int vis_h) {
  @autoreleasepool {
    if (!pixels || vis_w <= 0 || vis_h <= 0 || !g_base_icon) return;

    // Create CGImage from full RGBA surface, then crop to visible area
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
      const_cast<void*>(pixels), (size_t)surface_w, (size_t)surface_h, 8, (size_t)pitch,
      cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) return;

    CGImageRef cgFull = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!cgFull) return;

    // Crop to visible CPC screen area (CGImage y=0 is bottom, but
    // CGBitmapContext flips it, so vis_y from top works directly)
    CGRect cropRect = CGRectMake(vis_x, vis_y, vis_w, vis_h);
    CGImageRef cgScreen = CGImageCreateWithImageInRect(cgFull, cropRect);
    CGImageRelease(cgFull);
    if (!cgScreen) return;

    int w = vis_w;
    int h = vis_h;

    // Composite: live CPC screen + icon overlay.
    // koncepcja-icon.png (850x759) has a translucent screen area (~alpha 40)
    // and opaque CPC body. We draw the live screen FIRST, then the icon ON TOP.
    // The opaque body frames the screen; the translucent area adds CRT shine.
    //
    // Screen region in 850x759 (proportional coordinates):
    //   Source coords (y from bottom): (182,676) to (600,383)
    //   Image coords: (182,83) to (600,376)
    //   Cocoa: x=0.214, y=0.505, w=0.492, h=0.386
    static constexpr CGFloat kScreenX = 0.2141;
    static constexpr CGFloat kScreenY = 0.5046; // Cocoa y (from bottom)
    static constexpr CGFloat kScreenW = 0.4918;
    static constexpr CGFloat kScreenH = 0.3860;

    NSSize iconSize = [g_base_icon size];
    NSImage* composite = [[NSImage alloc] initWithSize:iconSize];
    [composite lockFocus];

    // 1. Draw live CPC screen into the monitor area.
    //    Slightly oversized (+2% each edge) to ensure the CPC output
    //    fills the entire monitor with no transparent gaps at edges.
    CGFloat pad_x = iconSize.width * 0.005;
    CGFloat pad_y = iconSize.height * 0.005;
    NSRect screenRect = NSMakeRect(
      iconSize.width * kScreenX - pad_x,
      iconSize.height * kScreenY - pad_y,
      iconSize.width * kScreenW + pad_x * 2,
      iconSize.height * kScreenH + pad_y * 2);

    NSImage* screenImg = [[NSImage alloc] initWithCGImage:cgScreen size:NSMakeSize(w, h)];
    [screenImg drawInRect:screenRect
                 fromRect:NSZeroRect
                operation:NSCompositingOperationSourceOver
                 fraction:1.0];

    // 2. Draw icon on top — opaque body frames the screen,
    //    translucent screen area adds CRT glass shine
    [g_base_icon drawInRect:NSMakeRect(0, 0, iconSize.width, iconSize.height)
                   fromRect:NSZeroRect
                  operation:NSCompositingOperationSourceOver
                   fraction:1.0];

    [composite unlockFocus];
    CGImageRelease(cgScreen);

    [NSApp setApplicationIconImage:composite];
  }
}
