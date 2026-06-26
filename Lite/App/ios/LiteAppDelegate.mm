#import "LiteAppDelegate.h"
#import "LiteViewController.h"

@implementation LiteAppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [[LiteViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

@end
