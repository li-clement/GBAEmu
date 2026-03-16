#import <MetalKit/MetalKit.h>

@interface MetalView : MTKView <MTKViewDelegate>
- (void)loadROM:(NSData *)romData;
- (void)loadROM:(NSData *)romData fromPath:(NSString *)path;
- (void)saveSave; // 将存档写入 .sav 文件
@end
