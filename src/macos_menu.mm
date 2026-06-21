#import <Cocoa/Cocoa.h>
#include "keyboard.h"
#include "menu_actions.h"
#include "menu_bridge.h"
#include "imgui_state.h"  // FileDialogAction
#ifdef KONCPC_MODERN_UI
#include "imgui.h"
#endif
#include <atomic>
#include <memory>
#include <string>
#include "SDL3/SDL.h"

// NOTE: a native NSMenu runs a modal event-tracking loop ON THE MAIN THREAD,
// which suspends our hand-rolled main-thread render loop while a menu is open —
// so emulation stalls at ~0 FPS for the duration and, crucially, NOTHING can be
// drawn during tracking.  Pausing on begin-tracking was tried and reverted: the
// pause overlay could only render AFTER the menu closed, so it looked like
// "closing the menu paused it."  The real fix is to keep presenting during
// tracking via a CVDisplayLink / common-mode run-loop tick (filed); until then
// the native menu simply freezes the frame while open, same as before.

extern "C" void koncpc_menu_action(int action);

// ── Bridge-item dispatch ───────────────────────────────────────────────────
//
// The KONCPC_* command items route through menuAction:/[item tag]==action.
// The NON-KONCPC items (Settings deep-links, Window toggles, View ▸ Scale /
// Renderer, file dialogs, About, Command Palette) are single-sourced via
// menu_bridge.h.  Each such item carries a packed tag: high nibble = kind,
// low bits = payload — so one selector (bridgeAction:) routes them all and
// validateMenuItem: can compute the live checkmark from the same bridge fns.
enum BridgeKind {
  BK_ABOUT = 1,
  BK_SETTINGS,   // payload = OptionsTab int
  BK_PALETTE,
  BK_FILEDLG,    // payload = FileDialogAction int
  BK_WINDOW,     // payload = index into koncpc_window_menu_items()
  BK_SCALE,      // payload = scale index
  BK_RENDERER,   // payload = video_plugin index
};
static inline NSInteger pack_bridge_tag(BridgeKind kind, int payload) {
  return (static_cast<NSInteger>(kind) << 24) | (payload & 0x00FFFFFF);
}
static inline BridgeKind bridge_kind(NSInteger tag) {
  return static_cast<BridgeKind>((tag >> 24) & 0xFF);
}
static inline int bridge_payload(NSInteger tag) {
  return static_cast<int>(tag & 0x00FFFFFF);
}

@interface KoncepcjaMenuTarget : NSObject
@end

@implementation KoncepcjaMenuTarget
- (void)menuAction:(id)sender {
  NSInteger action = [sender tag];
  koncpc_menu_action(static_cast<int>(action));
}

// Bridge items: route to the single-source entry points in menu_bridge.h.
- (void)bridgeAction:(id)sender {
  NSInteger tag = [sender tag];
  int payload = bridge_payload(tag);
  switch (bridge_kind(tag)) {
    case BK_ABOUT:
      koncpc_show_about_dialog();
      break;
    case BK_SETTINGS:
      koncpc_open_settings_tab(payload);
      break;
    case BK_PALETTE:
      koncpc_open_command_palette();
      break;
    case BK_FILEDLG:
      koncpc_request_file_dialog(payload);
      break;
    case BK_WINDOW: {
      const auto& items = koncpc_window_menu_items();
      if (payload >= 0 && payload < static_cast<int>(items.size()))
        koncpc_window_toggle(items[payload].key);
      break;
    }
    case BK_SCALE:
      koncpc_set_scale(payload);
      break;
    case BK_RENDERER:
      koncpc_set_renderer(payload);
      break;
  }
}

// Queried by AppKit each time the menu opens, so toggle items show a live
// checkmark from the single source of truth instead of no state at all.
- (BOOL)validateMenuItem:(NSMenuItem*)item {
  SEL act = [item action];
  if (act == @selector(menuAction:)) {
    const MenuAction* entry = koncpc_find_action(static_cast<KONCPC_KEYS>([item tag]));
    if (entry != nullptr && entry->toggle) {
      [item setState:koncpc_action_is_active(entry->action) ? NSControlStateValueOn
                                                            : NSControlStateValueOff];
    }
  } else if (act == @selector(bridgeAction:)) {
    NSInteger tag = [item tag];
    int payload = bridge_payload(tag);
    bool on = false;
    switch (bridge_kind(tag)) {
      case BK_WINDOW: {
        const auto& items = koncpc_window_menu_items();
        if (payload >= 0 && payload < static_cast<int>(items.size()))
          on = koncpc_window_is_open(items[payload].key);
        break;
      }
      case BK_SCALE:
        on = (koncpc_current_scale() == payload);
        break;
      case BK_RENDERER:
        on = (koncpc_current_renderer() == payload);
        break;
      default:
        break;
    }
    [item setState:on ? NSControlStateValueOn : NSControlStateValueOff];
  }
  return YES;
}
@end

// Add a KONCPC_* command item to a submenu, label/placement/shortcut all from
// the action registry.  Shortcut is shown as TEXT only (no keyEquivalent):
// SDL owns every key, so an AppKit accelerator here would double-fire.
static void add_action_item(NSMenu* submenu, KoncepcjaMenuTarget* target,
                            const MenuAction* entry) {
  NSString* itemTitle = [NSString stringWithUTF8String:entry->title];
  std::string sc = koncpc_action_shortcut(entry->action);
  if (!sc.empty()) {
    itemTitle = [itemTitle stringByAppendingFormat:@"  (%s)", sc.c_str()];
  }
  NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:itemTitle
                                                action:@selector(menuAction:)
                                         keyEquivalent:@""];
  [item setTarget:target];
  [item setTag:static_cast<NSInteger>(entry->action)];
  [submenu addItem:item];
}

// Add a bridge item (non-KONCPC).  No keyEquivalent — bridge items carry no
// SDL shortcut.
static NSMenuItem* add_bridge_item(NSMenu* submenu, KoncepcjaMenuTarget* target,
                                   NSString* title, BridgeKind kind, int payload) {
  NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                action:@selector(bridgeAction:)
                                         keyEquivalent:@""];
  [item setTarget:target];
  [item setTag:pack_bridge_tag(kind, payload)];
  [submenu addItem:item];
  return item;
}

// Append every registry action whose MenuGroup matches `group` to `submenu`,
// so labels + placement come from menu_actions.cpp.
static void add_group_actions(NSMenu* submenu, KoncepcjaMenuTarget* target,
                              MenuGroup group) {
  for (const MenuAction& entry : koncpc_menu_actions()) {
    if (entry.group != group) continue;
    add_action_item(submenu, target, &entry);
  }
}

static NSMenu* make_submenu(NSMenu* mainMenu, NSString* title) {
  NSMenuItem* menuItem = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
  NSMenu* submenu = [[NSMenu alloc] initWithTitle:title];
  [menuItem setSubmenu:submenu];
  [mainMenu addItem:menuItem];
  return submenu;
}

// Submenu of an existing top-level menu matched by exact title, or nil.  Used
// to REUSE AppKit's auto-created menus (the app menu + "Window") instead of
// adding parallel duplicates.
static NSMenu* find_top_menu(NSMenu* mainMenu, NSString* title) {
  for (NSMenuItem* item in [mainMenu itemArray]) {
    if ([[item title] isEqualToString:title]) return [item submenu];
  }
  return nil;
}

// Create a top-level submenu INSERTED at a given index, so our custom menus
// land between the app menu and the (system) Window menu rather than after it.
static NSMenu* insert_submenu(NSMenu* mainMenu, NSString* title, NSInteger idx) {
  NSMenuItem* menuItem = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
  NSMenu* submenu = [[NSMenu alloc] initWithTitle:title];
  [menuItem setSubmenu:submenu];
  [mainMenu insertItem:menuItem atIndex:idx];
  return submenu;
}

// Re-target AppKit's existing application menu (the bold one named after the
// executable) so its About / Settings… / Quit items drive OUR dialogs and the
// unsaved-changes quit guard, instead of adding a redundant parallel app menu.
// Leaves their Cmd+, / Cmd+Q accelerators intact (host keys SDL doesn't own).
static void wire_app_menu(NSMenu* appMenu, KoncepcjaMenuTarget* target) {
  if (!appMenu) return;
  int settings_tab = koncpc_settings_tab_items().front().tab;
  for (NSMenuItem* it in [appMenu itemArray]) {
    NSString* t = [it title];
    if ([t hasPrefix:@"About"]) {
      [it setTarget:target];
      [it setAction:@selector(bridgeAction:)];
      [it setTag:pack_bridge_tag(BK_ABOUT, 0)];
    } else if ([t hasPrefix:@"Settings"] || [t hasPrefix:@"Preferences"]) {
      [it setTarget:target];
      [it setAction:@selector(bridgeAction:)];
      [it setTag:pack_bridge_tag(BK_SETTINGS, settings_tab)];
    } else if ([t hasPrefix:@"Quit"]) {
      // Route Quit through KONCPC_EXIT so the unsaved-disk guard runs (F13).
      [it setTarget:target];
      [it setAction:@selector(menuAction:)];
      [it setTag:static_cast<NSInteger>(KONCPC_EXIT)];
    }
  }
}

static void koncpc_install_emulator_menu(NSMenu* mainMenu) {
  if (!mainMenu) return;

  // Idempotency guard: our first custom top-level menu is "Machine".
  if (find_top_menu(mainMenu, @"Machine")) return;

  KoncepcjaMenuTarget* target = [[KoncepcjaMenuTarget alloc] init];

  // ── App menu ── re-target AppKit's existing application menu (item 0) for
  // About / Settings / Quit instead of adding a parallel "konCePCja" menu
  // (which produced a duplicate app menu + wrong placement).
  NSMenu* appMenu =
      ([mainMenu numberOfItems] > 0) ? [[mainMenu itemAtIndex:0] submenu] : nil;
  wire_app_menu(appMenu, target);

  // Insert our custom menus BEFORE the system "Window" menu so it stays last
  // and we don't add a second one.
  NSInteger insertIdx = [mainMenu numberOfItems];
  {
    NSArray* items = [mainMenu itemArray];
    for (NSInteger i = 0; i < static_cast<NSInteger>([items count]); i++) {
      if ([[[items objectAtIndex:i] title] isEqualToString:@"Window"]) {
        insertIdx = i;
        break;
      }
    }
  }

  // ── Machine ── 7 Settings deep-links + Reset.
  {
    NSMenu* m = insert_submenu(mainMenu, @"Machine", insertIdx++);
    for (const SettingsTabItem& it : koncpc_settings_tab_items()) {
      if (it.separator_before) [m addItem:[NSMenuItem separatorItem]];
      add_bridge_item(m, target, [NSString stringWithUTF8String:it.label],
                      BK_SETTINGS, it.tab);
    }
    [m addItem:[NSMenuItem separatorItem]];
    if (const MenuAction* e = koncpc_find_action(KONCPC_RESET))
      add_action_item(m, target, e);
  }

  // ── Edit ──
  {
    NSMenu* m = insert_submenu(mainMenu, @"Edit", insertIdx++);
    add_group_actions(m, target, MenuGroup::Edit);
  }

  // ── Media ── loaders/savers (bridge file dialogs) + KONCPC actions.
  {
    NSMenu* m = insert_submenu(mainMenu, @"Media", insertIdx++);
    add_bridge_item(m, target, @"Load Disk A...", BK_FILEDLG,
                    static_cast<int>(FileDialogAction::LoadDiskA));
    add_bridge_item(m, target, @"Load Disk B...", BK_FILEDLG,
                    static_cast<int>(FileDialogAction::LoadDiskB));
    add_bridge_item(m, target, @"Save Disk A...", BK_FILEDLG,
                    static_cast<int>(FileDialogAction::SaveDiskA));
    add_bridge_item(m, target, @"Save Disk B...", BK_FILEDLG,
                    static_cast<int>(FileDialogAction::SaveDiskB));
    [m addItem:[NSMenuItem separatorItem]];
    add_bridge_item(m, target, @"Load Tape...", BK_FILEDLG,
                    static_cast<int>(FileDialogAction::LoadTape));
    if (const MenuAction* e = koncpc_find_action(KONCPC_TAPEPLAY))
      add_action_item(m, target, e);
    [m addItem:[NSMenuItem separatorItem]];
    add_bridge_item(m, target, @"Load Cartridge...", BK_FILEDLG,
                    static_cast<int>(FileDialogAction::LoadCartridge));
    [m addItem:[NSMenuItem separatorItem]];
    add_bridge_item(m, target, @"Load Snapshot...", BK_FILEDLG,
                    static_cast<int>(FileDialogAction::LoadSnapshot));
    add_bridge_item(m, target, @"Save Snapshot...", BK_FILEDLG,
                    static_cast<int>(FileDialogAction::SaveSnapshot));
    if (const MenuAction* e = koncpc_find_action(KONCPC_SNAPSHOT))
      add_action_item(m, target, e);
    if (const MenuAction* e = koncpc_find_action(KONCPC_LD_SNAP))
      add_action_item(m, target, e);
    [m addItem:[NSMenuItem separatorItem]];
    if (const MenuAction* e = koncpc_find_action(KONCPC_NEXTDISKA))
      add_action_item(m, target, e);
    // Open Recent (MRU) is in-window-only; intentionally skipped natively.
  }

  // ── View ── Fullscreen, Scale, Renderer, Screenshot, Show FPS.
  {
    NSMenu* m = insert_submenu(mainMenu, @"View", insertIdx++);
    if (const MenuAction* e = koncpc_find_action(KONCPC_FULLSCRN))
      add_action_item(m, target, e);

    // Scale ▸
    NSMenuItem* scaleItem = [[NSMenuItem alloc] initWithTitle:@"Scale"
                                                       action:nil
                                                keyEquivalent:@""];
    NSMenu* scaleMenu = [[NSMenu alloc] initWithTitle:@"Scale"];
    {
      const auto& labels = koncpc_scale_labels();
      for (int i = 0; i < static_cast<int>(labels.size()); i++) {
        add_bridge_item(scaleMenu, target,
                        [NSString stringWithUTF8String:labels[i]], BK_SCALE, i);
      }
    }
    [scaleItem setSubmenu:scaleMenu];
    [m addItem:scaleItem];

    // Renderer ▸ (grouped GPU/CPU via the bridge's group string)
    NSMenuItem* rendItem = [[NSMenuItem alloc] initWithTitle:@"Renderer"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* rendMenu = [[NSMenu alloc] initWithTitle:@"Renderer"];
    {
      const char* prev_group = nullptr;
      for (int i = 0; i < koncpc_renderer_count(); i++) {
        if (koncpc_renderer_hidden(i)) continue;
        const char* group = koncpc_renderer_group(i);
        if (!prev_group || strcmp(prev_group, group) != 0) {
          NSMenuItem* hdr = [[NSMenuItem alloc]
              initWithTitle:[NSString stringWithUTF8String:group]
                     action:nil
              keyEquivalent:@""];
          [hdr setEnabled:NO];
          [rendMenu addItem:hdr];
          prev_group = group;
        }
        add_bridge_item(rendMenu, target,
                        [NSString stringWithUTF8String:koncpc_renderer_name(i)],
                        BK_RENDERER, i);
      }
    }
    [rendItem setSubmenu:rendMenu];
    [m addItem:rendItem];

    [m addItem:[NSMenuItem separatorItem]];
    if (const MenuAction* e = koncpc_find_action(KONCPC_SCRNSHOT))
      add_action_item(m, target, e);
    if (const MenuAction* e = koncpc_find_action(KONCPC_FPS))
      add_action_item(m, target, e);
  }

  // ── Input ── Joystick, Light Gun, Limit Speed (all MenuGroup::Input).
  {
    NSMenu* m = insert_submenu(mainMenu, @"Input", insertIdx++);
    add_group_actions(m, target, MenuGroup::Input);
  }

  // ── Tools ── DevTools, Command Palette, Multiface II, Diagnostics ▸.
  {
    NSMenu* m = insert_submenu(mainMenu, @"Tools", insertIdx++);
    if (const MenuAction* e = koncpc_find_action(KONCPC_DEVTOOLS))
      add_action_item(m, target, e);
    add_bridge_item(m, target, @"Command Palette", BK_PALETTE, 0);
    if (const MenuAction* e = koncpc_find_action(KONCPC_MF2STOP))
      add_action_item(m, target, e);
    [m addItem:[NSMenuItem separatorItem]];
    NSMenuItem* diagItem = [[NSMenuItem alloc] initWithTitle:@"Diagnostics"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* diagMenu = [[NSMenu alloc] initWithTitle:@"Diagnostics"];
    if (const MenuAction* e = koncpc_find_action(KONCPC_DEBUG))
      add_action_item(diagMenu, target, e);
    [diagItem setSubmenu:diagMenu];
    [m addItem:diagItem];
  }

  // ── Window ── REUSE AppKit's existing Window menu (Minimize/Zoom/…) and
  // append our 3 specials + 17 devtools windows (single-sourced list) after a
  // separator, rather than adding a second "Window" menu.
  {
    NSMenu* m = find_top_menu(mainMenu, @"Window");
    if (!m) m = make_submenu(mainMenu, @"Window");  // fallback if none yet
    [m addItem:[NSMenuItem separatorItem]];
    const auto& items = koncpc_window_menu_items();
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
      if (items[i].separator_before && i != 0)
        [m addItem:[NSMenuItem separatorItem]];
      add_bridge_item(m, target,
                      [NSString stringWithUTF8String:items[i].label], BK_WINDOW,
                      i);
    }
  }
}

extern "C" __attribute__((visibility("default"))) void SDL_CocoaAddMenuItems(NSMenu* mainMenu) {
  @autoreleasepool {
    koncpc_install_emulator_menu(mainMenu);
  }
}

// ── Live display while a native menu is open (Phase 2) ──────────────────────
// A native NSMenu runs a modal tracking loop in NSEventTrackingRunLoopMode on
// the main thread, suspending our main render loop — so the display would freeze
// while a menu is held open.  The Z80 keeps running (decoupled from render), so
// we register a repeating CFRunLoopTimer IN THAT MODE whose callback presents
// the latest emulated frame.  Started on NSMenuDidBeginTracking, stopped on
// NSMenuDidEndTracking; a depth counter handles nested submenus.
void koncpc_render_tracking_tick();  // defined in kon_cpc_ja.cpp

static int g_menu_track_depth = 0;             // nested begin/end (submenus)
static CFRunLoopTimerRef g_track_timer = NULL;  // live only during tracking

static void koncpc_track_timer_cb(CFRunLoopTimerRef, void*) {
  koncpc_render_tracking_tick();
}

static void koncpc_menu_track_begin() {
  if (g_menu_track_depth++ != 0) return;  // already inside a tracking session
  if (g_track_timer) return;
  // ~60 Hz tick; the ring present is cheap and idempotent if no new frame.
  g_track_timer = CFRunLoopTimerCreate(
      kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(), 1.0 / 60.0, 0, 0,
      koncpc_track_timer_cb, NULL);
  CFRunLoopAddTimer(CFRunLoopGetCurrent(), g_track_timer,
                    (__bridge CFStringRef)NSEventTrackingRunLoopMode);
}

static void koncpc_menu_track_end() {
  if (g_menu_track_depth > 0) g_menu_track_depth--;
  if (g_menu_track_depth != 0) return;  // still inside a nested submenu
  if (g_track_timer) {
    CFRunLoopTimerInvalidate(g_track_timer);
    CFRelease(g_track_timer);
    g_track_timer = NULL;
  }
}

static void koncpc_register_menu_tracking_observers() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:NSMenuDidBeginTrackingNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) { koncpc_menu_track_begin(); }];
    [nc addObserverForName:NSMenuDidEndTrackingNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) { koncpc_menu_track_end(); }];
  });
}

void koncpc_setup_macos_menu() {
  @autoreleasepool {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    koncpc_install_emulator_menu([NSApp mainMenu]);
    koncpc_register_menu_tracking_observers();
  }
}

// App Nap control: used by screenshot capture to ensure rendering continues
// when the emulator window is in the background.
static id<NSObject> g_app_nap_activity = nil;

void koncpc_disable_app_nap() {
  @autoreleasepool {
    if (!g_app_nap_activity) {
      g_app_nap_activity =
          [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated
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

// Restore mainSDLWindow's content view as firstResponder so AppKit routes
// key events to [SDLContentView keyDown:] (and from there into SDL's
// event queue) instead of the NSApplication menu bar.  Used after native
// sheet dialogs (NSOpenPanel, NSSavePanel) where the sheet steals
// firstResponder and AppKit doesn't auto-restore it on dismiss — bug
// reproduces every time the user goes Menu → Media → Load Disk A.
extern SDL_Window* mainSDLWindow;
void koncpc_restore_keyboard_focus() {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      if (!mainSDLWindow) return;
      NSWindow* nswin = (__bridge NSWindow*)SDL_GetPointerProperty(
          SDL_GetWindowProperties(mainSDLWindow), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
      if (!nswin) return;
      [nswin makeKeyAndOrderFront:nil];
      NSView* contentView = [nswin contentView];
      if (contentView) {
        [nswin makeFirstResponder:contentView];
      }
    }
  });
}

extern SDL_Window* mainSDLWindow;

#ifdef KONCPC_MODERN_UI
static NSWindow* nswindow_from_viewport(ImGuiViewport* vp) {
  SDL_WindowID wid = (SDL_WindowID)(uintptr_t)vp->PlatformHandle;
  SDL_Window* sdlWin = SDL_GetWindowFromID(wid);
  if (!sdlWin) return nil;
  return (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWin),
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
      SDL_GetWindowProperties(mainSDLWindow), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
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
#else   // !KONCPC_MODERN_UI
// Headless build has no ImGui viewports — function is a stub.  Header
// declaration (macos_menu.h) stays unchanged so callers don't need a
// build-flag guard at every callsite.
void koncpc_order_viewports_above_main() {}
#endif  // KONCPC_MODERN_UI

// ── Dock icon ──────────────────────────────────────────────

static NSImage* g_icon_overlay = nil;  // CRT overlay (translucent screen area)
static NSImage* g_static_icon = nil;   // static logo shown at startup / fallback
static std::atomic<bool> g_icon_update_in_flight{false};

// Screen region in 850x759 icon (proportional coordinates)
static constexpr CGFloat kScreenX = 0.2141;
static constexpr CGFloat kScreenY = 0.4800;
static constexpr CGFloat kScreenW = 0.4918;
static constexpr CGFloat kScreenH = 0.4350;

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
      [initial
          drawInRect:NSMakeRect((side - sz.width) / 2, (side - sz.height) / 2, sz.width, sz.height)
            fromRect:NSZeroRect
           operation:NSCompositingOperationSourceOver
            fraction:1.0];
      [sq unlockFocus];
      [NSApp setApplicationIconImage:sq];
      [sq release];
    }
  }
}

void koncpc_update_dock_icon_preview(const void* pixels, int surface_w, int surface_h, int pitch,
                                     int vis_x, int vis_y, int vis_w, int vis_h) {
  (void)surface_w;
  (void)surface_h;
  if (!pixels || vis_w <= 0 || vis_h <= 0 || !g_icon_overlay) return;

  // Skip if a previous update is still in flight (don't queue up work)
  if (g_icon_update_in_flight.exchange(true)) return;

  // Copy visible pixels for async use
  size_t row_bytes = static_cast<size_t>(vis_w) * 4;
  size_t buf_size = static_cast<size_t>(vis_h) * row_bytes;
  std::shared_ptr<uint8_t> px(new uint8_t[buf_size], std::default_delete<uint8_t[]>());
  const uint8_t* src = static_cast<const uint8_t*>(pixels);
  for (int y = 0; y < vis_h; y++) {
    memcpy(px.get() + y * row_bytes, src + (vis_y + y) * pitch + vis_x * 4, row_bytes);
  }

  int w = vis_w, h = vis_h;

  // Compositing (lockFocus/drawInRect) is expensive — do it on a background queue.
  // Only the final setApplicationIconImage must run on the main thread.
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
    @autoreleasepool {
      CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
      CGContextRef ctx =
          CGBitmapContextCreate(px.get(), (size_t)w, (size_t)h, 8, row_bytes, cs,
                                kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
      CGColorSpaceRelease(cs);
      if (!ctx) {
        g_icon_update_in_flight.store(false);
        return;
      }

      CGImageRef cgScreen = CGBitmapContextCreateImage(ctx);
      CGContextRelease(ctx);
      if (!cgScreen) {
        g_icon_update_in_flight.store(false);
        return;
      }

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
          ox + iconSize.width * kScreenX - pad_x, oy + iconSize.height * kScreenY - pad_y,
          iconSize.width * kScreenW + pad_x * 2, iconSize.height * kScreenH + pad_y * 2);
      NSImage* screenImg = [[NSImage alloc] initWithCGImage:cgScreen size:NSMakeSize(w, h)];
      [screenImg drawInRect:screenRect
                   fromRect:NSZeroRect
                  operation:NSCompositingOperationSourceOver
                   fraction:1.0];

      // 2. CRT overlay on top
      NSRect iconRect = NSMakeRect(ox, oy, iconSize.width, iconSize.height);
      [g_icon_overlay drawInRect:iconRect
                        fromRect:NSZeroRect
                       operation:NSCompositingOperationSourceOver
                        fraction:1.0];

      [composite unlockFocus];
      CGImageRelease(cgScreen);
      [screenImg release];

      // Only this part needs the main thread
      dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp setApplicationIconImage:composite];
        [composite release];
        g_icon_update_in_flight.store(false);
      });
    }
  });
}
