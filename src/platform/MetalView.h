#import <MetalKit/MetalKit.h>

@interface MetalView : MTKView <MTKViewDelegate>
- (void)loadROM:(NSData *)romData;
@end
