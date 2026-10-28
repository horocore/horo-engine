#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/menu/EditorMenuPlatform.h"

#import <AppKit/AppKit.h>
#include <optional>
#include <vector>

namespace {
    std::optional<Horo::Editor::EditorMenuInvocation> g_pendingInvocation;
    std::vector<Horo::Editor::EditorMenuInvocation> g_invocations;

    NSString *ToNSString(const std::string_view value) {
        return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
    }

    NSString *LocalizedTitle(const Horo::Editor::ILocalizationService &localization, const std::string_view key) {
        return ToNSString(localization.Get("editor", key));
    }
}  // namespace

@interface HoroEditorMenuTarget : NSObject
+ (void)performEditorMenuAction:(id)sender;
@end

@implementation HoroEditorMenuTarget

+ (void)performEditorMenuAction:(id)sender {
    const NSInteger tag = [(NSMenuItem *)sender tag];
    if (tag > 0 && static_cast<std::size_t>(tag) <= g_invocations.size()) {
        const auto &inv = g_invocations[static_cast<std::size_t>(tag) - 1];
        // Skip auto-fire: only accept explicit user clicks, not system-initiated menu validation.
        // NSMenu sends performAction during first responder setup; we detect this by checking
        // whether the event is a real mouse click (NSEventTypeLeftMouseDown) or key equivalent.
        NSEvent *currentEvent = [NSApp currentEvent];
        if (currentEvent && (currentEvent.type == NSEventTypeLeftMouseDown || currentEvent.type == NSEventTypeKeyDown)) {
            g_pendingInvocation = inv;
        }
    }
}

@end

namespace Horo::Editor {
    namespace {
        NSInteger RegisterInvocation(const EditorMenuInvocation &invocation) {
            g_invocations.push_back(invocation);
            return static_cast<NSInteger>(g_invocations.size());
        }

        void AppendModelItem(NSMenu *menu, const EditorMenuItem &modelItem, const ILocalizationService &localization) {
            if (modelItem.kind == EditorMenuItemKind::Separator) {
                [menu addItem:[NSMenuItem separatorItem]];
                return;
            }

            NSString *title = LocalizedTitle(localization, modelItem.labelKey);
            NSString *keyEquivalent = ToNSString(modelItem.macKeyEquivalent);
            const bool actionable =
                modelItem.kind == EditorMenuItemKind::Command && modelItem.action != EditorMenuAction::None && modelItem.enabledByDefault;
            SEL selector = actionable ? @selector(performEditorMenuAction:) : nil;
            NSMenuItem *nativeItem = [[NSMenuItem alloc] initWithTitle:title action:selector keyEquivalent:keyEquivalent];
            [title release];
            [keyEquivalent release];

            if (modelItem.kind == EditorMenuItemKind::Submenu) {
                NSString *submenuTitle = LocalizedTitle(localization, modelItem.labelKey);
                NSMenu *submenu = [[NSMenu alloc] initWithTitle:submenuTitle];
                [submenuTitle release];
                [submenu setAutoenablesItems:NO];
                for (const EditorMenuItem &child : modelItem.children) {
                    AppendModelItem(submenu, child, localization);
                }
                [nativeItem setSubmenu:submenu];
                [submenu release];
                [nativeItem setEnabled:YES];
            } else {
                [nativeItem setTarget:actionable ? [HoroEditorMenuTarget class] : nil];
                [nativeItem setTag:RegisterInvocation(EditorMenuInvocation{modelItem.action, modelItem.primitive})];
                [nativeItem setEnabled:actionable];
            }

            [menu addItem:nativeItem];
            [nativeItem release];
        }

        void AppendApplicationMenu(NSMenu *mainMenu) {
            NSMenuItem *appRoot = [[NSMenuItem alloc] initWithTitle:@"Horo Editor" action:nil keyEquivalent:@""];
            NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"Horo Editor"];
            [appMenu setAutoenablesItems:NO];

            NSMenuItem *about = [[NSMenuItem alloc] initWithTitle:@"About Horo Editor" action:nil keyEquivalent:@""];
            [about setEnabled:NO];
            [appMenu addItem:about];
            [about release];
            [appMenu addItem:[NSMenuItem separatorItem]];

            NSMenuItem *services = [[NSMenuItem alloc] initWithTitle:@"Services" action:nil keyEquivalent:@""];
            NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
            [services setSubmenu:servicesMenu];
            [NSApp setServicesMenu:servicesMenu];
            [appMenu addItem:services];
            [servicesMenu release];
            [services release];
            [appMenu addItem:[NSMenuItem separatorItem]];

            NSMenuItem *hide = [[NSMenuItem alloc] initWithTitle:@"Hide Horo Editor" action:@selector(hide:) keyEquivalent:@"h"];
            [appMenu addItem:hide];
            [hide release];
            NSMenuItem *hideOthers = [[NSMenuItem alloc] initWithTitle:@"Hide Others"
                                                                action:@selector(hideOtherApplications:)
                                                         keyEquivalent:@"h"];
            [hideOthers setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
            [appMenu addItem:hideOthers];
            [hideOthers release];
            NSMenuItem *showAll = [[NSMenuItem alloc] initWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
            [appMenu addItem:showAll];
            [showAll release];
            [appMenu addItem:[NSMenuItem separatorItem]];

            NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"Quit Horo Editor"
                                                          action:@selector(performEditorMenuAction:)
                                                   keyEquivalent:@"q"];
            [quit setTarget:[HoroEditorMenuTarget class]];
            [quit setTag:RegisterInvocation(EditorMenuInvocation{EditorMenuAction::ExitApplication, std::nullopt})];
            [appMenu addItem:quit];
            [quit release];

            [appRoot setSubmenu:appMenu];
            [mainMenu addItem:appRoot];
            [appMenu release];
            [appRoot release];
        }
    }  // namespace

    /** @copydoc UsesNativeEditorMenuBar */
    bool UsesNativeEditorMenuBar() noexcept {
        return true;
    }

    /** @copydoc InstallNativeEditorMenuBar */
    void InstallNativeEditorMenuBar(const EditorMenuModel &model, const ILocalizationService &localization) {
        g_invocations.clear();
        g_pendingInvocation.reset();
        [NSApplication sharedApplication];
        NSMenu *mainMenu = [[NSMenu alloc] initWithTitle:@"Horo Editor"];
        [mainMenu setAutoenablesItems:NO];
        AppendApplicationMenu(mainMenu);
        for (const EditorMenuItem &menu : model.menus) {
            AppendModelItem(mainMenu, menu, localization);
        }
        [NSApp setMainMenu:mainMenu];
        [mainMenu release];
    }

    /** @copydoc PollNativeEditorMenuAction */
    std::optional<EditorMenuInvocation> PollNativeEditorMenuAction() noexcept {
        std::optional<EditorMenuInvocation> invocation = g_pendingInvocation;
        g_pendingInvocation.reset();
        return invocation;
    }

    /** @copydoc OpenInExternalEditor */
    bool OpenInExternalEditor(const std::filesystem::path &absolutePath) noexcept {
        if (!absolutePath.is_absolute())
            return false;
        NSString *path = ToNSString(absolutePath.string());
        if (path == nil)
            return false;
        NSURL *url = [NSURL fileURLWithPath:path];
        [path release];
        return url != nil && [[NSWorkspace sharedWorkspace] openURL:url];
    }
}  // namespace Horo::Editor
