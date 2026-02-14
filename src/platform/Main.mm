#import "MetalView.h"
#import <Cocoa/Cocoa.h>

// AppDelegate interface
@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong, nonatomic) NSWindow *window;
@property(strong, nonatomic) MetalView *metalView;
@property(strong, nonatomic) NSTimer *renderTimer;
@end

// AppDelegate implementation
@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification {
  // 让命令行启动的应用显示为前台 GUI 应用
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp activateIgnoringOtherApps:YES];

  // Create the main window
  NSRect frame = NSMakeRect(0, 0, 240 * 3, 160 * 3); // 3x GBA resolution
  NSUInteger styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                         NSWindowStyleMaskMiniaturizable |
                         NSWindowStyleMaskResizable;

  self.window = [[NSWindow alloc] initWithContentRect:frame
                                            styleMask:styleMask
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
  [self.window setTitle:@"M1 GBA Emulator"];
  [self.window makeKeyAndOrderFront:nil];
  [self.window center];
  [self.window setDelegate:self];

  // Create Metal View
  self.metalView = [[MetalView alloc] initWithFrame:frame];
  [self.window setContentView:self.metalView];

  // Set up application menu
  [self setupMenu];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:
    (NSApplication *)sender {
  return YES;
}

- (void)setupMenu {
  NSMenu *mainMenu = [[NSMenu alloc] init];
  NSApplication.sharedApplication.mainMenu = mainMenu;

  NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
  [mainMenu addItem:appMenuItem];

  NSMenu *appMenu = [[NSMenu alloc] init];
  [appMenuItem setSubmenu:appMenu];

  [appMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Quit"
                                              action:@selector(terminate:)
                                       keyEquivalent:@"q"]];

  // File Menu
  NSMenuItem *fileMenuItem = [[NSMenuItem alloc] init];
  [mainMenu addItem:fileMenuItem];
  NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
  [fileMenuItem setSubmenu:fileMenu];
  [fileMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Open ROM..."
                                               action:@selector(openDocument:)
                                        keyEquivalent:@"o"]];
}

- (void)openDocument:(id)sender {
  NSOpenPanel *panel = [NSOpenPanel openPanel];
  [panel setAllowedFileTypes:@[ @"gba", @"bin" ]];
  [panel beginWithCompletionHandler:^(NSInteger result) {
    if (result == NSModalResponseOK) {
      NSURL *url = [panel URL];
      NSData *data = [NSData dataWithContentsOfURL:url];
      if (data) {
        [self.metalView loadROM:data];
        [self.window
            setTitle:[NSString stringWithFormat:@"M1 GBA Emulator - %@",
                                                [url lastPathComponent]]];
      }
    }
  }];
}

@end

// Main entry point
int main(int argc, const char *argv[]) {
  @autoreleasepool {
    NSApplication *app = [NSApplication sharedApplication];
    AppDelegate *delegate = [[AppDelegate alloc] init];
    [app setDelegate:delegate];

    // Check for command line arguments
    if (argc > 1) {
      NSString *path = [NSString stringWithUTF8String:argv[1]];
      // We need to wait for app to launch to load ROM?
      // Actually, we can just defer it or set a property on delegate.
      // Let's use a delayed invocation or notification.
      // Or better, just queue it on main loop.
      dispatch_async(dispatch_get_main_queue(), ^{
        NSData *data = [NSData dataWithContentsOfFile:path];
        if (data) {
          [delegate.metalView loadROM:data];
          [delegate.window
              setTitle:[NSString stringWithFormat:@"M1 GBA Emulator - %@",
                                                  [path lastPathComponent]]];
          NSLog(@"Loaded ROM from command line: %@", path);
        } else {
          NSLog(@"Failed to load ROM from command line: %@", path);
        }
      });
    }

    [app run];
  }
  return 0;
}
