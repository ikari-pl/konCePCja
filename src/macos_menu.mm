#import <Cocoa/Cocoa.h>
#include "menu_actions.h"
#include "keyboard.h"

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
