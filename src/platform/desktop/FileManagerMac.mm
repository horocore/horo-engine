#include "FileManagerNative.h"

#import <AppKit/AppKit.h>

namespace Horo::FileManagerInternal {
    /** @copydoc Dispatch */
    Result<void> Dispatch(const Target &target) {
        @autoreleasepool {
            NSURL *url = [NSURL fileURLWithFileSystemRepresentation:target.path.c_str() isDirectory:target.directory relativeToURL:nil];
            if (url == nil)
                return Result<void>::Failure(MakeError(FileManagerErrors::DispatchFailed, "The native path could not form a file URL."));
            NSWorkspace *workspace = [NSWorkspace sharedWorkspace];
            if (target.directory) {
                if (![workspace openURL:url])
                    return Result<void>::Failure(MakeError(FileManagerErrors::DispatchFailed));
            } else {
                [workspace activateFileViewerSelectingURLs:@[ url ]];
            }
            return Result<void>::Success();
        }
    }
}  // namespace Horo::FileManagerInternal
