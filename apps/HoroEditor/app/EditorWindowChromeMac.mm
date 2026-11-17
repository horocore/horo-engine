#include "EditorWindowChrome.h"

#import <AppKit/AppKit.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <algorithm>

/** @brief Owned title presentation; AppKit still owns window layout, dragging and system controls. */
@interface HoroEditorTitleAccessory : NSTitlebarAccessoryViewController
@end
@implementation HoroEditorTitleAccessory
@end

namespace Horo::Editor {
    /** @copydoc ApplyNativeEditorWindowChrome */
    void ApplyNativeEditorWindowChrome(SDL_Window &window, const EditorWindowColors &colors) {
        NSWindow *native =
            (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(&window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        if (!native)
            return;
        const auto &surface = colors.surface;
        const bool dark = surface[0] * 0.2126F + surface[1] * 0.7152F + surface[2] * 0.0722F < 0.5F;
        NSAppearance *appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
        NSApp.appearance = appearance;
        native.appearance = appearance;
        native.titlebarAppearsTransparent = YES;
        native.backgroundColor = [NSColor colorWithSRGBRed:surface[0] green:surface[1] blue:surface[2] alpha:1];
        if (@available(macOS 11.0, *))
            native.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
        HoroEditorTitleAccessory *title = nil;
        for (NSTitlebarAccessoryViewController *controller in native.titlebarAccessoryViewControllers)
            if ([controller isKindOfClass:[HoroEditorTitleAccessory class]])
                title = (HoroEditorTitleAccessory *)controller;
        if (!title) {
            title = [[HoroEditorTitleAccessory alloc] init];
            title.layoutAttribute = NSLayoutAttributeLeft;
            NSTextField *label = [NSTextField labelWithString:native.title];
            label.font = [NSFont systemFontOfSize:[NSFont systemFontSize] weight:NSFontWeightSemibold];
            NSView *container = [[NSView alloc] initWithFrame:label.frame];
            [container addSubview:label];
            title.view = container;
            [container release];
            [native addTitlebarAccessoryViewController:title];
            [title release];
        }
        NSTextField *label = (NSTextField *)title.view.subviews.firstObject;
        label.stringValue = native.title;
        label.textColor = [NSColor colorWithSRGBRed:colors.text[0] green:colors.text[1] blue:colors.text[2] alpha:1];
        [label sizeToFit];
        const CGFloat height = std::max(CGFloat{24}, native.frame.size.height - native.contentLayoutRect.size.height);
        const NSSize size = label.frame.size;
        title.view.frame = NSMakeRect(0, 0, size.width, height);
        label.frame = NSMakeRect(0, (height - size.height) * 0.5, size.width, size.height);
        native.titleVisibility = NSWindowTitleHidden;
    }
}  // namespace Horo::Editor
