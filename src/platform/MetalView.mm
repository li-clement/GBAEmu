#import "MetalView.h"
#include "APU.h"
#include "GBA.h"
#include "IORegisters.h"
#import <AudioToolbox/AudioToolbox.h>
#import <MetalKit/MetalKit.h>
#include <chrono>
#import <simd/simd.h>
#include <thread>
#include <vector>

// 简单顶点结构
typedef struct {
  vector_float2 position;
  vector_float2 textureCoordinate;
} Vertex;

// Shader source code embedded as string
static NSString *const kShaderSource =
    @""
     "#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "\n"
     "struct VertexIn {\n"
     "    float2 position;\n"
     "    float2 textureCoordinate;\n"
     "};\n"
     "\n"
     "struct VertexOut {\n"
     "    float4 position [[ position ]];\n"
     "    float2 textureCoordinate;\n"
     "};\n"
     "\n"
     "vertex VertexOut vertexShader(uint vertexID [[ vertex_id ]],\n"
     "                              constant VertexIn *vertices [[ buffer(0) "
     "]]) {\n"
     "    VertexOut out;\n"
     "    out.position = float4(vertices[vertexID].position, 0.0, 1.0);\n"
     "    out.textureCoordinate = vertices[vertexID].textureCoordinate;\n"
     "    return out;\n"
     "}\n"
     "\n"
     "fragment float4 fragmentShader(VertexOut in [[ stage_in ]],\n"
     "                               texture2d<float> colorTexture [[ "
     "texture(0) ]]) {\n"
     "    constexpr sampler textureSampler (mag_filter::nearest,\n"
     "                                      min_filter::nearest);\n"
     "    return colorTexture.sample(textureSampler, in.textureCoordinate);\n"
     "}\n";

// Triple Buffering
static const int kMaxInflightBuffers = 3;

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
  std::vector<int16_t> _audioRingBuffer;

  id<MTLBuffer> _vertexBuffer;

  // GBA Core
  Core::GBA *_gba;
  BOOL _loadedRealROM; // YES = ROM from file (skip test scene), NO = dummy (run
                       // test scene)

  NSLock *_displayLock; // Protects _readyBuffer access
  NSLock *_coreLock;    // Protects _gba access
  bool _newFrameReady;  // Signals drawInMTKView to replaceRegion
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

    [self buildResources];
    [self buildPipeline];

    _gba = new Core::GBA();

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
  size_t availableBytes = _audioRingBuffer.size() * sizeof(int16_t);

  if (availableBytes < bytesToCopy) {
    // Not enough data, pad with silence or just copy what we have?
    // Let's pad with silence for now to avoid stalling
    memset(buffer->mAudioData, 0, bytesToCopy);
    // Copy what we have
    memcpy(buffer->mAudioData, _audioRingBuffer.data(), availableBytes);
    _audioRingBuffer.clear();
    buffer->mAudioDataByteSize = (UInt32)bytesToCopy;

    static int starvLog = 0;
    if (starvLog++ % 60 == 0) {
      NSLog(@"AUDIO STARVATION: Needed %zu but had %zu", bytesToCopy,
            availableBytes);
    }
  } else {
    memcpy(buffer->mAudioData, _audioRingBuffer.data(), bytesToCopy);
    _audioRingBuffer.erase(_audioRingBuffer.begin(),
                           _audioRingBuffer.begin() +
                               (bytesToCopy / sizeof(int16_t)));
    buffer->mAudioDataByteSize = (UInt32)bytesToCopy;
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

  // 2. 创建 Triple Buffering 资源
  NSUInteger bytesPerRow = 1024;
  NSUInteger bufferSize = bytesPerRow * 160;

  // Create the single GPU texture
  MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:240
                                  height:160
                               mipmapped:NO];
  textureDescriptor.usage = MTLTextureUsageShaderRead;
  // Managed storage mode ensures CPU -> GPU copy works synchronously via
  // replaceRegion
  textureDescriptor.storageMode = MTLStorageModeManaged;

  _displayTexture = [self.device newTextureWithDescriptor:textureDescriptor];
  _cpuBuffer.resize(240 * 160, 0);
  _readyBuffer.resize(240 * 160, 0);
  _newFrameReady = false;

  _emulationQueue =
      dispatch_queue_create("com.gbaemu.core", DISPATCH_QUEUE_SERIAL);
  _displayLock = [[NSLock alloc] init];

  [self setupAudio];
  // _running 和 startEmulationLoop 已移到 initWithFrame 末尾
}

- (void)loadROM:(NSData *)romData {
  if (!romData || romData.length == 0)
    return;
  if (_gba == nullptr)
    return;

  std::vector<uint8_t> data(romData.length);
  memcpy(data.data(), romData.bytes, romData.length);

  [_coreLock lock];
  _gba->loadROM(data);
  _gba->reset();
  [_coreLock unlock];
}

- (void)buildPipeline {
  NSError *error;

  // 使用运行时编译
  id<MTLLibrary> library = [self.device newLibraryWithSource:kShaderSource
                                                     options:nil
                                                       error:&error];
  if (!library) {
    NSLog(@"Error compiling shaders: %@", error);
    return;
  }

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
}

- (void)startEmulationLoop {
  __weak MetalView *weakSelf = self;
  dispatch_async(_emulationQueue, ^{
    while (true) {
      MetalView *strongSelf = weakSelf;
      if (!strongSelf || !strongSelf->_running)
        break;

      // Run Emulation (Produces 1 frame)
      [strongSelf->_coreLock lock];
      strongSelf->_gba->stepFrame(strongSelf->_cpuBuffer.data(), 1024);

      // Push Audio and Sync Speed strictly to 44100Hz playback rate!
      {
        auto &audioOut = strongSelf->_gba->getAPU().getBuffer();

        // Audio Sync Throttle: Block if buffer is > 4096 samples (approx. 90ms
        // of lag!)
        while (true) {
          if (!strongSelf || !strongSelf->_running)
            break;
          bool full = false;
          [strongSelf->_audioLock lock];
          if (strongSelf->_audioRingBuffer.size() > 4096) {
            full = true;
          }
          [strongSelf->_audioLock unlock];

          if (!full)
            break;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        [strongSelf->_audioLock lock];
        strongSelf->_audioRingBuffer.insert(strongSelf->_audioRingBuffer.end(),
                                            audioOut.begin(), audioOut.end());
        [strongSelf->_audioLock unlock];

        static int mixLog = 0;
        if (mixLog++ % 60 == 0) {
          int nonZeroCount = 0;
          for (auto s : audioOut)
            if (s != 0)
              nonZeroCount++;
          NSLog(@"AUDIO MIX: Pushed %zu samples. Non-zero: %d. Ring Buffer "
                @"size: %zu",
                audioOut.size(), nonZeroCount,
                strongSelf->_audioRingBuffer.size());
        }

        strongSelf->_gba->getAPU().clearBuffer();
      }
      [strongSelf->_coreLock unlock];

      // Publish new frame to readyBuffer
      [strongSelf->_displayLock lock];
      memcpy(strongSelf->_readyBuffer.data(), strongSelf->_cpuBuffer.data(),
             240 * 160 * 4);
      strongSelf->_newFrameReady = true;
      [strongSelf->_displayLock unlock];
    }
  });
}

// Logic is now in background thread
- (void)updateGameLogic {
  // Scrolling logic moved to GBA core or input handling?
  // Left empty for now as stepFrame handles the core.
  // We could handle other main-thread logic here if needed.

  // Test Scrolling: Scroll diagonally
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

  // Visual Debug: Show R0 value as red intensity
  // MOV R0, #42 -> R0 should be 42
  // ADD R2, R0, R1 -> 42+10 = 52
  // Let's print registers occasionally
  static int frameCount = 0;
  frameCount++;
  if (frameCount < 600 || frameCount % 60 == 0) {
    uint32_t r0 = _gba->getCPU().getRegister(0);
    uint32_t r1 = _gba->getCPU().getRegister(1);
    uint32_t r2 = _gba->getCPU().getRegister(2);
    uint32_t pc = _gba->getCPU().getRegister(15);

    FILE *f = fopen("log.txt", "a");
    if (f) {
      uint32_t keyinput = _gba->getBus().read16(0x04000130);
      static uint32_t lastKeyInput = 0xFFFF;
      if (keyinput != lastKeyInput) {
        fprintf(f, "[Input] KEYINPUT Changed: %04X\n", keyinput);
        lastKeyInput = keyinput;
      }

      // Only log CPU state occasionally to reduce spam
      if (frameCount % 60 == 0) {
        fprintf(f, "[Frame %d] CPU PC=%08X R0=%d R1=%d R2=%d\n", frameCount, pc,
                r0, r1, r2);
      }
      fclose(f);
    }
  }
}

- (void)drawInMTKView:(MTKView *)view {
  if (!_displayTexture)
    return;

  bool uploadNeeded = false;
  [self->_displayLock lock];
  if (self->_newFrameReady) {
    [self->_displayTexture replaceRegion:MTLRegionMake2D(0, 0, 240, 160)
                             mipmapLevel:0
                               withBytes:self->_readyBuffer.data()
                             bytesPerRow:1024];
    self->_newFrameReady = false;
  }
  [self->_displayLock unlock];

  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  commandBuffer.label = @"MyCommand";

  MTLRenderPassDescriptor *renderPassDescriptor =
      view.currentRenderPassDescriptor;
  if (renderPassDescriptor != nil) {
    id<MTLRenderCommandEncoder> renderEncoder =
        [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
    renderEncoder.label = @"MyRenderEncoder";

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

    static int logCount = 0;
    if (logCount++ % 60 == 0) {
      NSLog(@"Frame Presented");
    }
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
