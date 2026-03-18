#import "MetalView.h"
#include "../core/APU.h"
#include "../core/GBA.h"
#include "../core/IORegisters.h"
#import <AudioToolbox/AudioToolbox.h>
#import <MetalKit/MetalKit.h>
#include <chrono>
#import <simd/simd.h>
#include <thread>
#include <vector>

#include "RingBuffer.h"

// 简单顶点结构
typedef struct {
  vector_float2 position;
  vector_float2 textureCoordinate;
} Vertex;

@implementation MetalView {
  id<MTLCommandQueue> _commandQueue;
  id<MTLRenderPipelineState> _pipelineState;

  // Decoupled Buffering
  std::vector<uint32_t> _cpuBuffer;
  std::vector<uint32_t> _readyBuffer;

  id<MTLTexture> _displayTexture; // Single GPU Texture

  // Threading
  dispatch_queue_t _emulationQueue;
  bool _running;

  // Audio
  AudioQueueRef _audioQueue;
  AudioQueueBufferRef _audioBuffers[3];
  NSLock *_audioLock;
  Core::RingBuffer<int16_t> *_audioRingBuffer;

  id<MTLBuffer> _vertexBuffer;

  // GBA Core
  Core::GBA *_gba;
  BOOL _loadedRealROM; // YES = ROM from file (skip test scene), NO = dummy (run
                       // test scene)

  NSLock *_displayLock; // Protects _readyBuffer access
  NSLock *_coreLock;    // Protects _gba access
  NSCondition *_audioCondition; // Replaces sleep for fine-grain sync
  id<MTLComputePipelineState> _computePipelineState;
  id<MTLBuffer> _vramBuffer;
  id<MTLBuffer> _palBuffer;
  id<MTLBuffer> _oamBuffer;
  id<MTLBuffer> _ioRegsBuffer;
  bool _newFrameReady;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.device = MTLCreateSystemDefaultDevice();
    if (!self.device) {
      NSLog(@"Metal is not supported on this device");
      return nil;
    }

    self.colorPixelFormat = MTLPixelFormatRGBA8Unorm;
    self.preferredFramesPerSecond = 60;
    self.delegate = self;

    _commandQueue = [self.device newCommandQueue];

    _coreLock = [[NSLock alloc] init];
    _audioCondition = [[NSCondition alloc] init];

    _gba = new Core::GBA();

    [self buildResources];
    [self buildPipeline];

    // --- 从 rom 目录加载 BIOS 与 ROM ---

    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *execPath = [[NSProcessInfo processInfo] arguments][0];
    NSString *projectRoot = [[execPath stringByDeletingLastPathComponent]
        stringByDeletingLastPathComponent];
    NSString *romDir = [projectRoot stringByAppendingPathComponent:@"rom"];
    // 若按可执行文件相对路径找不到 rom，则尝试当前工作目录下的 rom
    if (![fm fileExistsAtPath:romDir]) {
      NSString *cwd = [fm currentDirectoryPath];
      if (cwd.length)
        romDir = [cwd stringByAppendingPathComponent:@"rom"];
    }
    NSString *romPath = [romDir stringByAppendingPathComponent:
                                    @"Castlevania - Aria of Sorrow (USA).gba"];
    NSData *romFileData = [NSData dataWithContentsOfFile:romPath];
    if (!romFileData) {
      NSError *err = nil;
      NSArray<NSString *> *contents = [fm contentsOfDirectoryAtPath:romDir
                                                              error:&err];
      for (NSString *name in contents) {
        if ([name.lowercaseString hasSuffix:@".gba"]) {
          romPath = [romDir stringByAppendingPathComponent:name];
          romFileData = [NSData dataWithContentsOfFile:romPath];
          break;
        }
      }
    }
    if (romFileData) {
      NSLog(@"Loaded ROM from: %@ (%lu bytes)", romPath,
            (unsigned long)romFileData.length);
      // 优先加载 BIOS（16KB）：gba_bios.bin 或 bios.bin
      NSData *biosData = nil;
      NSString *biosPath =
          [romDir stringByAppendingPathComponent:@"gba_bios.bin"];
      biosData = [NSData dataWithContentsOfFile:biosPath];
      if (!biosData || biosData.length != 16384) {
        biosPath = [romDir stringByAppendingPathComponent:@"bios.bin"];
        biosData = [NSData dataWithContentsOfFile:biosPath];
      }
      if (biosData && biosData.length == 16384) {
        std::vector<uint8_t> biosVec(biosData.length);
        memcpy(biosVec.data(), biosData.bytes, biosData.length);
        _gba->loadBIOS(biosVec);
        NSLog(@"Loaded BIOS from: %@ (%lu bytes)", biosPath,
              (unsigned long)biosData.length);
      } else if (biosData.length && biosData.length != 16384) {
        NSLog(@"Skip BIOS: wrong size %lu (expected 16384)",
              (unsigned long)biosData.length);
      }
      std::vector<uint8_t> romVec(romFileData.length);
      memcpy(romVec.data(), romFileData.bytes, romFileData.length);
      _gba->loadROM(romVec);
      // 传递 ROM 路径用于推导 .sav 存档路径
      _gba->setROMPath([romPath UTF8String]);
      _gba->loadSave();
      _loadedRealROM = YES;
    } else {
      NSLog(@"No ROM found in rom folder, using dummy ROM");
      std::vector<uint8_t> romData = {0x2A, 0x00, 0xA0, 0xE3, 0x0A, 0x10,
                                      0xA0, 0xE3, 0x01, 0x20, 0x80, 0xE0,
                                      0xFE, 0xFF, 0xFF, 0xEA};
      _gba->loadROM(romData);
      _loadedRealROM = NO;
    }

    // --- PPU TEST SCENE SETUP (only when using dummy ROM; skip for real game
    // ROM) ---
    if (!_loadedRealROM) {
      auto &bus = _gba->getBus();
      bus.write16(0x04000000, 0x0100);
      bus.write16(0x04000008, 0x0800);
      bus.write16(0x05000002, 0x001F);
      bus.write16(0x05000004, 0x03E0);
      for (int i = 0; i < 32; i++) {
        bus.write8(0x06000000 + 32 + i, (i % 2 == 0) ? 0x12 : 0x21);
      }
      bus.write16(0x06004000, 0x0001);
      bus.write16(0x06004002, 0x0001);
      bus.write16(0x06004000 + 64, 0x0001);
      for (int i = 0; i < 32; i++) {
        bus.write8(0x02000000 + i, 0x33);
      }
      bus.write32(0x040000D4, 0x02000000);
      bus.write32(0x040000D8, 0x06010040);
      bus.write16(0x040000DC, 0x0008);
      bus.write16(0x040000DE, 0x8400);
      bus.write16(0x05000206, 0x03E0);
      bus.write16(0x04000000, 0x1100);
      bus.write16(0x07000000, 0x001E);
      bus.write16(0x07000002, 0x001E);
      bus.write16(0x07000004, 0x0002);
      bus.write16(0x05000202, 0x7C00);
    }

    _gba->reset();

    // Setup MTLBuffers for GPU PPU rendering.
    // NOTE: newBufferWithBytesNoCopy requires page-aligned memory which
    // std::vector does NOT guarantee. Use regular newBufferWithBytes instead.
    // On Apple Silicon UMA the copy overhead is negligible.
    auto &bus = _gba->getBus();
    _vramBuffer =
        [self.device newBufferWithBytes:(void *)bus.getVRAMPointer()
                                 length:96 * 1024
                                options:MTLResourceStorageModeShared];
    _palBuffer =
        [self.device newBufferWithBytes:(void *)bus.getPalettePointer()
                                 length:1024
                                options:MTLResourceStorageModeShared];
    _oamBuffer =
        [self.device newBufferWithBytes:(void *)bus.getOAMPointer()
                                 length:1024
                                options:MTLResourceStorageModeShared];
    _ioRegsBuffer =
        [self.device newBufferWithBytes:(void *)bus.getIORegsPointer()
                                 length:4096 // 0x1000 IO region
                                options:MTLResourceStorageModeShared];

    // 所有初始化完成后才启动仿真循环 (避免数据竞争)
    _running = true;
    [self startEmulationLoop];
  }
  return self;
}

// Audio Constants
const int kSampleRate = 44100;
const int kNumChannels = 2;
const int kBitsPerChannel = 16;

static void HandleOutputBuffer(void *inUserData, AudioQueueRef inAQ,
                               AudioQueueBufferRef inBuffer) {
  MetalView *self = (__bridge MetalView *)inUserData;
  [self fillAudioBuffer:inBuffer];
}

- (void)fillAudioBuffer:(AudioQueueBufferRef)buffer {
  [self->_audioLock lock];

  size_t bytesToCopy = buffer->mAudioDataBytesCapacity;
  size_t samplesToCopy = bytesToCopy / sizeof(int16_t);
  size_t availableSamples = _audioRingBuffer->size();

  if (availableSamples < samplesToCopy) {
    // 设置全零以防出现爆音，并将剩余数据全部 pop 出来
    memset(buffer->mAudioData, 0, bytesToCopy);
    if (availableSamples > 0) {
      _audioRingBuffer->pop((int16_t *)buffer->mAudioData, availableSamples);
    }
    buffer->mAudioDataByteSize = (UInt32)bytesToCopy;
    
    // We drained the buffer, wake up the emulation thread!
    [_audioCondition broadcast];

    static int starvLog = 0;
    if (starvLog++ % 60 == 0) {
      NSLog(@"AUDIO STARVATION: Needed %zu but had %zu", bytesToCopy,
            availableSamples * sizeof(int16_t));
    }
  } else {
    _audioRingBuffer->pop((int16_t *)buffer->mAudioData, samplesToCopy);
    buffer->mAudioDataByteSize = (UInt32)bytesToCopy;
    
    // Signal that space might be freed
    [_audioCondition broadcast];
  }

  [self->_audioLock unlock];
  AudioQueueEnqueueBuffer(_audioQueue, buffer, 0, NULL);
}

- (void)setupAudio {
  AudioStreamBasicDescription format = {0};
  format.mSampleRate = kSampleRate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags =
      kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  format.mFramesPerPacket = 1;
  format.mChannelsPerFrame = kNumChannels;
  format.mBitsPerChannel = kBitsPerChannel;
  format.mBytesPerFrame = (kBitsPerChannel / 8) * kNumChannels;
  format.mBytesPerPacket = format.mBytesPerFrame * format.mFramesPerPacket;

  _audioLock = [[NSLock alloc] init];

  AudioQueueNewOutput(&format, HandleOutputBuffer, (__bridge void *)self, NULL,
                      NULL, 0, &_audioQueue);

  // Create 3 buffers of ~20ms each (44100 * 0.02 * 4 bytes = ~3528 bytes)
  int bufferSize = 4096;
  for (int i = 0; i < 3; i++) {
    AudioQueueAllocateBuffer(_audioQueue, bufferSize, &_audioBuffers[i]);
    // Prime with silence
    memset(_audioBuffers[i]->mAudioData, 0, bufferSize);
    _audioBuffers[i]->mAudioDataByteSize = bufferSize;
    AudioQueueEnqueueBuffer(_audioQueue, _audioBuffers[i], 0, NULL);
  }

  AudioQueueStart(_audioQueue, NULL);
}

- (void)buildResources {
  // 1. 创建全屏四边形顶点
  static const Vertex quadVertices[] = {
      // Pixel positions, Texture coordinates
      {{1.0, -1.0}, {1.0, 1.0}}, {{-1.0, -1.0}, {0.0, 1.0}},
      {{-1.0, 1.0}, {0.0, 0.0}},

      {{1.0, -1.0}, {1.0, 1.0}}, {{-1.0, 1.0}, {0.0, 0.0}},
      {{1.0, 1.0}, {1.0, 0.0}},
  };

  _vertexBuffer = [self.device newBufferWithBytes:quadVertices
                                           length:sizeof(quadVertices)
                                          options:MTLResourceStorageModeShared];

  // Create the single GPU texture
  MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:240
                                  height:160
                               mipmapped:NO];
  textureDescriptor.usage =
      MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  // Use Shared so Compute Pipeline can write to it on UMA, and we skip CPU
  // copies completely
  textureDescriptor.storageMode = MTLStorageModeShared;

  _displayTexture = [self.device newTextureWithDescriptor:textureDescriptor];

  _emulationQueue =
      dispatch_queue_create("com.gbaemu.core", DISPATCH_QUEUE_SERIAL);
  _displayLock = [[NSLock alloc] init];

  _audioRingBuffer = new Core::RingBuffer<int16_t>(32768); // 32K samples buffer
  [self setupAudio];
}

- (void)loadROM:(NSData *)romData {
  if (!romData || romData.length == 0)
    return;
  if (_gba == nullptr)
    return;

  std::vector<uint8_t> data(romData.length);
  memcpy(data.data(), romData.bytes, romData.length);

  [_coreLock lock];
  // 切换 ROM 前保存当前存档
  _gba->saveSave();
  _gba->loadROM(data);
  _gba->reset();
  [_coreLock unlock];
}

- (void)loadROM:(NSData *)romData fromPath:(NSString *)path {
  if (!romData || romData.length == 0)
    return;
  if (_gba == nullptr)
    return;

  std::vector<uint8_t> data(romData.length);
  memcpy(data.data(), romData.bytes, romData.length);

  [_coreLock lock];
  // 切换 ROM 前保存当前存档
  _gba->saveSave();
  _gba->setROMPath([path UTF8String]);
  _gba->loadROM(data);
  // loadROM 内部会自动检测存档类型，但需要在 setROMPath 后调用 loadSave
  _gba->loadSave();
  _gba->reset();
  [_coreLock unlock];
}

- (void)saveSave {
  if (_gba == nullptr)
    return;
  [_coreLock lock];
  _gba->saveSave();
  [_coreLock unlock];
}

- (void)buildPipeline {
  NSError *error = nil;

  // 使用当前可执行文件同目录下或源码目录下的 Shaders.metal
  NSString *execPath = [[NSProcessInfo processInfo] arguments][0];
  NSString *projectRoot = [[execPath stringByDeletingLastPathComponent]
      stringByDeletingLastPathComponent];
  NSString *srcDir =
      [projectRoot stringByAppendingPathComponent:@"src/platform/shaders"];
  NSString *shaderPath =
      [srcDir stringByAppendingPathComponent:@"Shaders.metal"];

  NSFileManager *fm = [NSFileManager defaultManager];
  if (![fm fileExistsAtPath:shaderPath]) {
    NSString *cwd = [fm currentDirectoryPath];
    shaderPath = [cwd
        stringByAppendingPathComponent:@"src/platform/shaders/Shaders.metal"];
  }

  NSString *shaderSource =
      [NSString stringWithContentsOfFile:shaderPath
                                encoding:NSUTF8StringEncoding
                                   error:&error];
  if (!shaderSource) {
    NSLog(@"Could not load Shaders.metal from %@: %@", shaderPath, error);
    return;
  }

  MTLCompileOptions *options = [[MTLCompileOptions alloc] init];
  options.fastMathEnabled = YES;
  id<MTLLibrary> library = [self.device newLibraryWithSource:shaderSource
                                                     options:options
                                                       error:&error];
  if (!library) {
    NSLog(@"Error compiling shaders: %@", error);
    return;
  }

  // View pipeline
  id<MTLFunction> vertexFunction =
      [library newFunctionWithName:@"vertexShader"];
  id<MTLFunction> fragmentFunction =
      [library newFunctionWithName:@"fragmentShader"];

  MTLRenderPipelineDescriptor *pipelineDescriptor =
      [[MTLRenderPipelineDescriptor alloc] init];
  pipelineDescriptor.label = @"GBA Render Pipeline";
  pipelineDescriptor.vertexFunction = vertexFunction;
  pipelineDescriptor.fragmentFunction = fragmentFunction;
  pipelineDescriptor.colorAttachments[0].pixelFormat = self.colorPixelFormat;

  _pipelineState =
      [self.device newRenderPipelineStateWithDescriptor:pipelineDescriptor
                                                  error:&error];
  if (!_pipelineState) {
    NSLog(@"Failed to create pipeline state: %@", error);
  }

  // Compute Pipeline for PPU
  id<MTLFunction> computeFunction =
      [library newFunctionWithName:@"ppg_compute_shader"];
  _computePipelineState =
      [self.device newComputePipelineStateWithFunction:computeFunction
                                                 error:&error];
  if (!_computePipelineState) {
    NSLog(@"Failed to create compute pipeline state: %@", error);
  }
}

- (void)startEmulationLoop {
  __unsafe_unretained MetalView *weakSelf = self;
  dispatch_async(_emulationQueue, ^{
    while (true) {
      MetalView *strongSelf = weakSelf;
      if (!strongSelf || !strongSelf->_running) {
        // 仿真线程退出时自动保存存档
        if (strongSelf && strongSelf->_gba) {
          strongSelf->_gba->saveSave();
        }
        break;
      }

      // Run Emulation (Produces 1 frame)
      // 只在 stepFrame 期间持锁，之后立即释放让渲染线程可以 memcpy VRAM
      [strongSelf->_coreLock lock];
      strongSelf->_gba->stepFrame(nullptr, 1024);

      // 快速拷贝 audio 数据出来，然后立刻释放 coreLock
      auto &audioOut = strongSelf->_gba->getAPU().getBuffer();
      std::vector<int16_t> audioCopy(audioOut.begin(), audioOut.end());
      strongSelf->_gba->getAPU().clearBuffer();
      [strongSelf->_coreLock unlock];

      // 音频同步在 coreLock 之外进行，不阻塞渲染线程
      {
        [strongSelf->_audioCondition lock];
        [strongSelf->_audioLock lock];
        
        // 我们等待直到 RingBuffer 有足够空间 (需要最多保留 audioCopy 的容量)
        while (strongSelf->_running && strongSelf->_audioRingBuffer->size() > 8192) {
          [strongSelf->_audioLock unlock];
          [strongSelf->_audioCondition wait];
          [strongSelf->_audioLock lock];
        }
        
        strongSelf->_audioRingBuffer->push(audioCopy.data(), audioCopy.size());
        
        [strongSelf->_audioLock unlock];
        [strongSelf->_audioCondition unlock];
      }

      // Mark that logic has passed VBLANK, safe to encode GPU pass
      [strongSelf->_displayLock lock];
      strongSelf->_newFrameReady = true;
      [strongSelf->_displayLock unlock];
    }
  });
}

// Logic is now in background thread
- (void)updateGameLogic {
  static int scrollX = 0;
  static int scrollY = 0;
  static int scrollFrame = 0;
  scrollFrame++;
  if (scrollFrame % 2 == 0) { // Every 2 frames
    scrollX = (scrollX + 1) & 0x1FF;
    scrollY = (scrollY + 1) & 0x1FF;
    _gba->getBus().write16(0x04000000 + 0x10, scrollX); // BG0HOFS
    _gba->getBus().write16(0x04000000 + 0x12, scrollY); // BG0VOFS
  }
}

- (void)drawInMTKView:(MTKView *)view {
  if (!_displayTexture || !_computePipelineState || !_vramBuffer)
    return;

  bool shouldDraw = false;
  [self->_displayLock lock];
  if (self->_newFrameReady) {
    shouldDraw = true;
    self->_newFrameReady = false;
  }
  [self->_displayLock unlock];

  if (!shouldDraw)
    return; // Wait for core emulation to finish a frame

  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  commandBuffer.label = @"GBA Render Command";

  // Dispatch Compute Shader to paint VRAM to _displayTexture
  id<MTLComputeCommandEncoder> computeEncoder =
      [commandBuffer computeCommandEncoder];
  [computeEncoder setComputePipelineState:_computePipelineState];
  [computeEncoder setTexture:_displayTexture atIndex:0];
  
  // 每帧同步最新的 VRAM/PAL/OAM/IO 数据到 Metal 缓冲区
  // (因为使用 newBufferWithBytes 而非零拷贝，GPU 不会自动看到 CPU 写入)
  [self->_coreLock lock];
  if (self->_gba) {
    auto &bus = self->_gba->getBus();
    // 同步 VRAM (96KB) 到 Metal Buffer
    memcpy(_vramBuffer.contents, bus.getVRAMPointer(), 96 * 1024);
    [computeEncoder setBuffer:_vramBuffer offset:0 atIndex:0];
    [computeEncoder setBytes:bus.getPalettePointer() length:1024 atIndex:1];
    [computeEncoder setBytes:bus.getOAMPointer()     length:1024 atIndex:2];
    [computeEncoder setBytes:bus.getIORegsPointer()  length:4096 atIndex:3];
  }
  [self->_coreLock unlock];

  MTLSize threadsPerGrid = MTLSizeMake(240, 160, 1);
  MTLSize threadsPerThreadgroup = MTLSizeMake(16, 16, 1);
  [computeEncoder dispatchThreads:threadsPerGrid
            threadsPerThreadgroup:threadsPerThreadgroup];
  [computeEncoder endEncoding];

  // Display Phase
  MTLRenderPassDescriptor *renderPassDescriptor =
      view.currentRenderPassDescriptor;
  if (renderPassDescriptor != nil) {
    id<MTLRenderCommandEncoder> renderEncoder =
        [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
    renderEncoder.label = @"Screen Rectangle Pass";

    [renderEncoder
        setViewport:(MTLViewport){0.0, 0.0, (double)self.drawableSize.width,
                                  (double)self.drawableSize.height, 0.0, 1.0}];

    [renderEncoder setRenderPipelineState:_pipelineState];
    [renderEncoder setVertexBuffer:_vertexBuffer offset:0 atIndex:0];
    [renderEncoder setFragmentTexture:_displayTexture atIndex:0];

    [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                      vertexStart:0
                      vertexCount:6];

    [renderEncoder endEncoding];
    [commandBuffer presentDrawable:view.currentDrawable];
  }
  [commandBuffer commit];
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
  // Handle resize
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (void)keyDown:(NSEvent *)event {
  [self handleKeyEvent:event pressed:YES];
}

- (void)keyUp:(NSEvent *)event {
  [self handleKeyEvent:event pressed:NO];
}

- (void)handleKeyEvent:(NSEvent *)event pressed:(BOOL)pressed {
  uint16_t mask = 0;
  switch (event.keyCode) {
  case 0x06:
    mask = Core::IO::KEY_A;
    break; // Z -> A
  case 0x07:
    mask = Core::IO::KEY_B;
    break; // X -> B
  case 0x7B:
    mask = Core::IO::KEY_LEFT;
    break; // Left Arrow
  case 0x7C:
    mask = Core::IO::KEY_RIGHT;
    break; // Right Arrow
  case 0x7D:
    mask = Core::IO::KEY_DOWN;
    break; // Down Arrow
  case 0x7E:
    mask = Core::IO::KEY_UP;
    break; // Up Arrow
  case 0x24:
    mask = Core::IO::KEY_START;
    break; // Enter -> Start
  case 0x33:
    mask = Core::IO::KEY_SELECT;
    break; // Backspace -> Select
  case 0x00:
    mask = Core::IO::KEY_L;
    break; // A -> L
  case 0x01:
    mask = Core::IO::KEY_R;
    break; // S -> R
  default:
    return;
  }

  if (_gba) {
    _gba->setKeyStatus(mask, pressed);
  }
}

@end
