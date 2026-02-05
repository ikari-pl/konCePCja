#import <Cocoa/Cocoa.h>
#include "menu_actions.h"
#include "keyboard.h"

@interface Cap32MenuTarget : NSObject
@end

extern "C" void cap32_menu_action(int action);

@implementation Cap32MenuTarget
- (void)menuAction:(id)sender {
  NSInteger action = [sender tag];
  cap32_menu_action((int)action);
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

static const MenuAction* find_menu_action(CAP32_KEYS action) {
  for (const auto &entry : cap32_menu_actions()) {
    if (entry.action == action) return &entry;
  }
  return nullptr;
}

static void add_menu_group(NSMenu *mainMenu, Cap32MenuTarget *target, NSString *title, std::initializer_list<CAP32_KEYS> actions) {
  for (NSMenuItem *item in [mainMenu itemArray]) {
    if ([[item title] isEqualToString:title]) return;
  }

  NSMenuItem *menuItem = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
  NSMenu *submenu = [[NSMenu alloc] initWithTitle:title];

  for (CAP32_KEYS action : actions) {
    const MenuAction *entry = find_menu_action(action);
    if (!entry) continue;
    NSString *itemTitle = [NSString stringWithUTF8String:entry->title];
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:itemTitle action:@selector(menuAction:) keyEquivalent:@""];
    [item setTarget:target];
    [item setTag:(NSInteger)entry->action];
    applyShortcut(item, entry->shortcut);
    [submenu addItem:item];
  }

  [menuItem setSubmenu:submenu];
  [mainMenu addItem:menuItem];
}

static void cap32_install_emulator_menu(NSMenu *mainMenu) {
  if (!mainMenu) return;

  Cap32MenuTarget *target = [[Cap32MenuTarget alloc] init];

  add_menu_group(mainMenu, target, @"Emulator", {
    CAP32_GUI,
    CAP32_FULLSCRN,
    CAP32_RESET,
    CAP32_EXIT,
  });

  add_menu_group(mainMenu, target, @"Media", {
    CAP32_TAPEPLAY,
    CAP32_MF2STOP,
    CAP32_NEXTDISKA,
  });

  add_menu_group(mainMenu, target, @"Tools", {
    CAP32_VKBD,
    CAP32_DEVTOOLS,
    CAP32_SCRNSHOT,
    CAP32_SNAPSHOT,
    CAP32_LD_SNAP,
    CAP32_PASTE,
  });

  add_menu_group(mainMenu, target, @"Options", {
    CAP32_JOY,
    CAP32_PHAZER,
    CAP32_FPS,
    CAP32_SPEED,
    CAP32_DEBUG,
    CAP32_DELAY,
    CAP32_WAITBREAK,
  });
}

extern "C" __attribute__((visibility("default"))) void SDL_CocoaAddMenuItems(NSMenu *mainMenu) {
  @autoreleasepool {
    cap32_install_emulator_menu(mainMenu);
  }
}

void cap32_setup_macos_menu() {
  @autoreleasepool {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    cap32_install_emulator_menu([NSApp mainMenu]);
  }
}
