#import "LiteMetalView.h"

@implementation LiteMetalView

+ (Class)layerClass
{
    return [CAMetalLayer class];
}

- (CAMetalLayer*)metalLayer
{
    return (CAMetalLayer*)self.layer;
}

@end
