// Standalone filesystem coverage; includes the private resolver in its translation unit.
// See verification command in the change report; no Notification Center or audio device is used.
#include "../../src/plat/src/cocoa/cocoa_notify.mm"
#include <cassert>

int main() {
    @autoreleasepool {
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSString *user = [root stringByAppendingPathComponent:@"user/Library/Sounds"];
        NSString *system = [root stringByAppendingPathComponent:@"system"];
        assert([fm createDirectoryAtPath:system withIntermediateDirectories:YES attributes:nil error:nil]);
        NSData *data = [@"fixture" dataUsingEncoding:NSUTF8StringEncoding];
        assert([data writeToFile:[system stringByAppendingPathComponent:@"Glass.aiff"] atomically:YES]);
        assert([resolvedNotificationSound(@"missing", user, @[system]) isEqualToString:@"msga-notify.wav"]);
        assert(![fm fileExistsAtPath:user]); // Missing sounds must not create directories.
        assert([resolvedNotificationSound(@"../Glass", user, @[system]) isEqualToString:@"msga-notify.wav"]);
        NSString *copied = resolvedNotificationSound(@"Glass", user, @[system]);
        assert([copied hasPrefix:@"msga-system-"] && [copied hasSuffix:@".aiff"]);
        NSString *copyPath = [user stringByAppendingPathComponent:copied];
        assert([[NSData dataWithContentsOfFile:copyPath] isEqualToData:data]);
        assert([resolvedNotificationSound(@"Glass", user, @[system]) isEqualToString:copied]);
        assert([data writeToFile:[user stringByAppendingPathComponent:@"Custom.wav"] atomically:YES]);
        assert([resolvedNotificationSound(@"Custom", user, @[system]) isEqualToString:@"Custom.wav"]);
        assert([resolvedNotificationSound(@"Custom.wav", user, @[system]) isEqualToString:@"Custom.wav"]);
        NSString *blocked = [root stringByAppendingPathComponent:@"blocked"];
        assert([data writeToFile:blocked atomically:YES]);
        assert([resolvedNotificationSound(@"Glass", [blocked stringByAppendingPathComponent:@"Sounds"], @[system]) isEqualToString:@"msga-notify.wav"]);
        // Never replace a conflicting file, including a symlink, in the user's sound folder.
        assert([fm removeItemAtPath:copyPath error:nil]);
        assert([fm createSymbolicLinkAtPath:copyPath withDestinationPath:[system stringByAppendingPathComponent:@"Glass.aiff"] error:nil]);
        assert([resolvedNotificationSound(@"Glass", user, @[system]) isEqualToString:@"msga-notify.wav"]);
        assert([fm removeItemAtPath:root error:nil]);
    }
}
