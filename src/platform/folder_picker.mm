#import <Cocoa/Cocoa.h>

#include "folder_picker.h"

bool pickFolderDialog(const char* title, std::string* out) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = NO;
        panel.message = [NSString stringWithUTF8String:title];
        panel.prompt = @"Add Folder";
        panel.directoryURL = [NSURL fileURLWithPath:NSHomeDirectory()];
        if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0) return false;
        *out = panel.URLs[0].path.UTF8String;
        return !out->empty();
    }
}
