#import "LiteViewController.h"
#import "LiteMetalView.h"

#include "LiteHost.h"

@interface LiteViewController ()
@property(nonatomic, strong) LiteMetalView* metalView;
@property(nonatomic, strong) UILabel* hudLabel;
@property(nonatomic, strong) CADisplayLink* hudLink;
@property(nonatomic, assign) BOOL liteStarted;
@end

@implementation LiteViewController

- (void)loadView
{
    self.metalView = [[LiteMetalView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.view = self.metalView;
}

- (void)viewDidLoad
{
    [super viewDidLoad];

    // Small FPS/frame-time overlay (parity with the Win32 HUD). Cosmetic; rendering is
    // driven independently on the JS thread.
    self.hudLabel = [[UILabel alloc] initWithFrame:CGRectMake(12, 48, 240, 60)];
    self.hudLabel.numberOfLines = 3;
    self.hudLabel.textColor = [UIColor greenColor];
    self.hudLabel.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightMedium];
    self.hudLabel.text = @"";
    [self.view addSubview:self.hudLabel];
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];

    CAMetalLayer* layer = self.metalView.metalLayer;
    const CGFloat scale = UIScreen.mainScreen.nativeScale;
    layer.contentsScale = scale;

    const CGSize bounds = self.metalView.bounds.size;
    const uint32_t wPx = (uint32_t)(bounds.width * scale);
    const uint32_t hPx = (uint32_t)(bounds.height * scale);
    if (wPx == 0 || hPx == 0)
    {
        return;
    }
    layer.drawableSize = CGSizeMake(wPx, hPx);

    if (!self.liteStarted)
    {
        self.liteStarted = lite::ios::StartLite((__bridge void*)layer, wPx, hPx);
        if (self.liteStarted)
        {
            self.hudLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(refreshHud)];
            [self.hudLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
        }
    }
    else
    {
        lite::ios::Resize(wPx, hPx);
    }
}

- (void)refreshHud
{
    double fps = 0, frameMs = 0, cpuMs = 0;
    lite::ios::GetHudStats(fps, frameMs, cpuMs);
    self.hudLabel.text = [NSString stringWithFormat:@"FPS:   %6.1f\nFrame: %6.2f ms\nCPU:   %6.2f ms",
                          fps, frameMs, cpuMs];
}

- (void)dealloc
{
    [self.hudLink invalidate];
    lite::ios::StopLite();
}

@end
