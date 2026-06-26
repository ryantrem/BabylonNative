// A UIView whose backing layer is a CAMetalLayer — the surface Dawn's Metal backend renders
// into. Exposing the layer via +layerClass is the standard way to get a Metal-presentable
// view without a MTKView (we don't use MetalKit's draw loop; Lite drives presentation through
// Dawn on the JS thread).
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>

@interface LiteMetalView : UIView
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
@end
