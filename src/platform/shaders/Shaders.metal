#include <metal_stdlib>

using namespace metal;

struct VertexIn {
    float2 position;
    float2 textureCoordinate;
};

struct VertexOut {
    float4 position [[ position ]];
    float2 textureCoordinate;
};

// 简单的顶点着色器
vertex VertexOut vertexShader(uint vertexID [[ vertex_id ]],
                              constant VertexIn *vertices [[ buffer(0) ]]) {
    VertexOut out;
    out.position = float4(vertices[vertexID].position, 0.0, 1.0);
    out.textureCoordinate = vertices[vertexID].textureCoordinate;
    return out;
}

// 颜色解包：15 位色（5-5-5）映射至归一化 rgba
inline float4 expandColor5(uint16_t color15) {
    float r = float(color15 & 0x1F) / 31.0f;
    float g = float((color15 >> 5) & 0x1F) / 31.0f;
    float b = float((color15 >> 10) & 0x1F) / 31.0f;
    return float4(r, g, b, 1.0f);
}

// 片段着色器：采样 + CAS 锐化（inlined，无中间 texture）
fragment float4 fragmentShader(VertexOut in [[ stage_in ]],
                              texture2d<float> colorTexture [[ texture(0) ]],
                              constant uint &casScale [[buffer(1)]]) {
    constexpr sampler textureSampler (mag_filter::nearest,
                                      min_filter::nearest,
                                      address::clamp_to_edge);

    // 3x3 邻域 UV 偏移（单位：纹理坐标）
    float2 texSize = float2(colorTexture.get_width(),
                           colorTexture.get_height());
    float2 stepUV = 1.0 / texSize;

    float2 uv = in.textureCoordinate;

    // 邻域 9 点采样（center + 4-directional）
    float3 b = colorTexture.sample(textureSampler, uv + float2( 0.0, -stepUV.y)).rgb;
    float3 d = colorTexture.sample(textureSampler, uv + float2(-stepUV.x,  0.0)).rgb;
    float3 e = colorTexture.sample(textureSampler, uv + float2( 0.0,  0.0)).rgb;
    float3 f = colorTexture.sample(textureSampler, uv + float2( stepUV.x,  0.0)).rgb;
    float3 h = colorTexture.sample(textureSampler, uv + float2( 0.0,  stepUV.y)).rgb;

    // CAS off：直接返回原色
    if (casScale >= 16) return float4(e, 1.0);

    // Luma 用绿色通道近似（GBA 调色盘图像绿色通道对比度最佳）
    float bL = b.g, dL = d.g, eL = e.g, fL = f.g, hL = h.g;

    // 对比度检测（噪声/细节感知）
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float con = abs(nz) * rsqrt(
        max(max(max(bL, dL), max(eL, fL)), max(hL, 1e-10f)) -
        min(min(min(bL, dL), min(eL, fL)), hL));
    nz = 1.0 - 0.5 * saturate(con);

    // Ring min/max（用于 lobe 限制）
    half3 mn4 = min(min(half3(b), half3(d)), min(half3(f), half3(h)));
    half3 mx4 = max(max(half3(b), half3(d)), max(half3(f), half3(h)));

    // CAS sharpness：0 = 最大锐化，N = 降低 N 档
    half sharp = 0.0h;
    if (casScale > 0) sharp = exp2(-half(casScale));

    // lobe 限制计算
    half hitMin = min(mn4.g, half(e.g)) / max(mx4.g * 4.0h, 1e-10h);
    half hitMax = (1.0h - max(mx4.g, half(e.g))) / max(mn4.g * 4.0h, 1e-10h);
    half lobe = max(-hitMin, hitMax);
    lobe = clamp(lobe, -1.0h, 0.0h) * sharp;
    lobe *= half(nz);

    // 最终输出
    half3 outRGB = (lobe * (half3(b) + half3(d) + half3(f) + half3(h)) + half3(e))
                   / (1.0h + 4.0h * lobe);
    outRGB = clamp(outRGB, 0.0h, 1.0h);

    return float4(float3(outRGB), 1.0);
}

// ======================================
// 【核心 GPU PPU 模块 (Compute shader)】
// 从 zero-copy buffers 中解析寄存器、VRAM、调色板和 OAM
// ======================================
kernel void ppg_compute_shader(texture2d<float, access::write> outTexture [[texture(0)]],
                               constant uint8_t *vram [[buffer(0)]],
                               constant uint8_t *pal [[buffer(1)]],
                               constant uint8_t *oam [[buffer(2)]],
                               constant uint8_t *io_regs [[buffer(3)]],
                               uint2 gridPos [[thread_position_in_grid]]) {
    
    // Bounds check
    if (gridPos.x >= 240 || gridPos.y >= 160) return;
    
    // 获取全局显示控制寄存器
    uint16_t dispcnt = *(constant uint16_t*)(&io_regs[0x000]);
    int mode = dispcnt & 0x7;
    
    // 第一图层打底色：调色板 [0]
    uint16_t backdrop15 = *(constant uint16_t*)(&pal[0]);
    float4 finalColor = expandColor5(backdrop15);
    int bestPrio = 4;
    
    // ----- 【图层 1：Tile Background (Mode 0, 1, 2)】 -----
    if (mode == 0 || mode == 1 || mode == 2) {
        // Mode 0: BG0, BG1, BG2, BG3 (All Text)
        // Mode 1: BG0, BG1 (Text), BG2 (Affine)
        // Mode 2: BG2, BG3 (All Affine)
        for (int bg = 3; bg >= 0; bg--) {
            bool enable = false;
            bool isAffine = false;
            
            if (bg == 0 && mode < 2) enable = dispcnt & (1 << 8);
            if (bg == 1 && mode < 2) enable = dispcnt & (1 << 9);
            if (bg == 2) { enable = dispcnt & (1 << 10); isAffine = (mode > 0); }
            if (bg == 3 && mode != 1) { enable = dispcnt & (1 << 11); isAffine = (mode == 2); }
            
            if (!enable) continue;
            
            uint16_t bgcnt = *(constant uint16_t*)(&io_regs[0x008 + bg * 2]);
            int prio = bgcnt & 3;
            if (prio > bestPrio) continue;
            
            int charBaseBlock = (bgcnt >> 2) & 0x3;
            int screenBaseBlock = (bgcnt >> 8) & 0x1F;
            int screenSize = (bgcnt >> 14) & 0x3;
            
            if (!isAffine) {
                // =============== TEXT BACKGROUND ===============
                uint16_t hofs = *(constant uint16_t*)(&io_regs[0x010 + bg * 4]) & 0x1FF;
                uint16_t vofs = *(constant uint16_t*)(&io_regs[0x012 + bg * 4]) & 0x1FF;
                int colorMode = (bgcnt >> 7) & 1; // 0 = 4BPP, 1 = 8BPP
                
                int width = (screenSize & 1) ? 512 : 256;
                int height = (screenSize & 2) ? 512 : 256;
                
                int effectiveX = (gridPos.x + hofs) & (width - 1);
                int effectiveY = (gridPos.y + vofs) & (height - 1);
                int tileX = effectiveX / 8;
                int tileY = effectiveY / 8;
                
                int blockX = tileX / 32;
                int blockY = tileY / 32;
                int sbbOffset = 0;
                if (screenSize == 1) sbbOffset = blockX;
                else if (screenSize == 2) sbbOffset = blockY;
                else if (screenSize == 3) sbbOffset = blockY * 2 + blockX;
                
                int localTileX = tileX % 32;
                int localTileY = tileY % 32;
                int mapIndex = localTileY * 32 + localTileX;
                
                uint32_t mapOffsetBase = (screenBaseBlock * 2048);
                uint32_t currentMapOffset = mapOffsetBase + (sbbOffset * 2048);
                uint16_t tileInfo = *(constant uint16_t*)(&vram[currentMapOffset + (mapIndex * 2)]);
                
                int tileIndex = tileInfo & 0x3FF;
                int hFlip = (tileInfo >> 10) & 1;
                int vFlip = (tileInfo >> 11) & 1;
                int paletteBank = (tileInfo >> 12) & 0xF;
                
                int localX = effectiveX % 8;
                int localY = effectiveY % 8;
                if (hFlip) localX = 7 - localX;
                if (vFlip) localY = 7 - localY;
                
                uint32_t tileOffsetBase = (charBaseBlock * 16384);
                if (colorMode == 0) { // 4 BPP
                    uint32_t tileDataOffset = tileOffsetBase + (tileIndex * 32);
                    uint8_t byteChunk = vram[tileDataOffset + (localY * 4) + (localX / 2)];
                    uint8_t colorIndex = (localX & 1) ? ((byteChunk >> 4) & 0xF) : (byteChunk & 0xF);
                    if (colorIndex != 0 && prio <= bestPrio) {
                        bestPrio = prio;
                        uint16_t bgC15 = *(constant uint16_t*)(&pal[paletteBank * 32 + colorIndex * 2]);
                        finalColor = expandColor5(bgC15);
                    }
                } else { // 8 BPP
                    uint32_t tileDataOffset = tileOffsetBase + (tileIndex * 64);
                    uint8_t colorIndex = vram[tileDataOffset + (localY * 8) + localX];
                    if (colorIndex != 0 && prio <= bestPrio) {
                        bestPrio = prio;
                        uint16_t bgC15 = *(constant uint16_t*)(&pal[colorIndex * 2]);
                        finalColor = expandColor5(bgC15);
                    }
                }
            } else {
                // =============== AFFINE BACKGROUND ===============
                int pa_reg = (bg == 2) ? 0x20 : 0x30;
                int16_t PA = *(constant int16_t*)(&io_regs[pa_reg + 0]);
                int16_t PB = *(constant int16_t*)(&io_regs[pa_reg + 2]);
                int16_t PC = *(constant int16_t*)(&io_regs[pa_reg + 4]);
                int16_t PD = *(constant int16_t*)(&io_regs[pa_reg + 6]);
                
                uint32_t refX_L = *(constant uint16_t*)(&io_regs[pa_reg + 8]);
                uint32_t refX_H = *(constant uint16_t*)(&io_regs[pa_reg + 10]);
                int32_t refX = (refX_H << 16) | refX_L;
                refX = (refX << 4) >> 4; // Sign extend 28-bit
                
                uint32_t refY_L = *(constant uint16_t*)(&io_regs[pa_reg + 12]);
                uint32_t refY_H = *(constant uint16_t*)(&io_regs[pa_reg + 14]);
                int32_t refY = (refY_H << 16) | refY_L;
                refY = (refY << 4) >> 4;
                
                int32_t texX = refX + gridPos.x * PA + gridPos.y * PB;
                int32_t texY = refY + gridPos.x * PC + gridPos.y * PD;
                
                texX >>= 8;
                texY >>= 8;
                
                int width = 128 << screenSize; 
                int height = width;
                
                bool wraparound = bgcnt & 0x2000;
                if (wraparound) {
                    texX &= (width - 1);
                    texY &= (height - 1);
                } else {
                    if (texX < 0 || texX >= width || texY < 0 || texY >= height) continue;
                }
                
                int tileX = texX / 8;
                int tileY = texY / 8;
                int localX = texX % 8;
                int localY = texY % 8;
                
                int mapIndex = tileY * (width / 8) + tileX;
                uint32_t mapOffsetBase = (screenBaseBlock * 2048);
                uint8_t tileIndex = vram[mapOffsetBase + mapIndex]; // 8-bit tile index for affine
                
                uint32_t tileOffsetBase = (charBaseBlock * 16384);
                uint32_t tileDataOffset = tileOffsetBase + (tileIndex * 64);
                uint8_t colorIndex = vram[tileDataOffset + (localY * 8) + localX];
                
                if (colorIndex != 0 && prio <= bestPrio) {
                    bestPrio = prio;
                    uint16_t bgC15 = *(constant uint16_t*)(&pal[colorIndex * 2]);
                    finalColor = expandColor5(bgC15);
                }
            }
        }
    } 
    // ----- 【图层 2：Bitmap FrameBuffer Mode 3/4/5】 -----
    else if (mode == 3) {
        if (dispcnt & (1 << 10)) { 
            uint16_t bgcnt = *(constant uint16_t*)(&io_regs[0x00C]);
            int prio = bgcnt & 3;
            if (prio <= bestPrio) {
                bestPrio = prio;
                uint32_t lineOffset = gridPos.y * 240 * 2;
                uint16_t color15 = *(constant uint16_t*)(&vram[lineOffset + gridPos.x * 2]);
                finalColor = expandColor5(color15);
            }
        }
    } else if (mode == 4) {
        if (dispcnt & (1 << 10)) { 
            uint16_t bgcnt = *(constant uint16_t*)(&io_regs[0x00C]);
            int prio = bgcnt & 3;
            if (prio <= bestPrio) {
                uint32_t frameOffset = (dispcnt & 0x10) ? 0xA000 : 0x0000;
                uint32_t lineOffset = frameOffset + (gridPos.y * 240);
                uint8_t index = vram[lineOffset + gridPos.x];
                if (index != 0) {
                    bestPrio = prio;
                    uint16_t color15 = *(constant uint16_t*)(&pal[index * 2]);
                    finalColor = expandColor5(color15);
                }
            }
        }
    } else if (mode == 5) {
        if (dispcnt & (1 << 10)) {
            uint16_t bgcnt = *(constant uint16_t*)(&io_regs[0x00C]);
            int prio = bgcnt & 3;
            if (prio <= bestPrio) {
                if (gridPos.x < 160 && gridPos.y < 128) {
                    bestPrio = prio;
                    uint32_t frameOffset = (dispcnt & 0x10) ? 0xA000 : 0x0000;
                    uint32_t lineOffset = frameOffset + (gridPos.y * 160 * 2);
                    uint16_t color15 = *(constant uint16_t*)(&vram[lineOffset + gridPos.x * 2]);
                    finalColor = expandColor5(color15);
                }
            }
        }
    }
    
    // ----- 【图层 3：Sprites (OAM) 遮盖】 -----
    if (dispcnt & (1 << 12)) { 
        for (int i = 127; i >= 0; i--) {
            uint16_t attr0 = *(constant uint16_t*)(&oam[i * 8 + 0]);
            uint16_t attr1 = *(constant uint16_t*)(&oam[i * 8 + 2]);
            uint16_t attr2 = *(constant uint16_t*)(&oam[i * 8 + 4]);
            
            int prio = (attr2 >> 10) & 3;
            if (prio > bestPrio) continue;
            
            int doubleSizeOrDisable = (attr0 >> 9) & 1;
            int rotScaleMode = (attr0 >> 8) & 1;
            if (!rotScaleMode && doubleSizeOrDisable) continue;
            
            int y = attr0 & 0xFF;
            if (y >= 160) y -= 256;
            
            int shape = (attr0 >> 14) & 3;
            int colorMode = (attr0 >> 13) & 1;
            
            int x = attr1 & 0x1FF;
            if (x >= 256) x -= 512;
            
            int sizeType = (attr1 >> 14) & 3;
            int hFlip = (attr1 >> 12) & 1;
            int vFlip = (attr1 >> 13) & 1;
            
            int width = 8, height = 8;
            if (shape == 0) {
                if (sizeType == 1) { width = 16; height = 16; }
                else if (sizeType == 2) { width = 32; height = 32; }
                else if (sizeType == 3) { width = 64; height = 64; }
            } else if (shape == 1) { 
                if (sizeType == 0) { width = 16; height = 8; }
                else if (sizeType == 1) { width = 32; height = 8; }
                else if (sizeType == 2) { width = 32; height = 16; }
                else if (sizeType == 3) { width = 64; height = 32; }
            } else if (shape == 2) { 
                if (sizeType == 0) { width = 8; height = 16; }
                else if (sizeType == 1) { width = 8; height = 32; }
                else if (sizeType == 2) { width = 16; height = 32; }
                else if (sizeType == 3) { width = 32; height = 64; }
            }
            
            if (gridPos.x >= x && gridPos.x < x + width &&
                gridPos.y >= y && gridPos.y < y + height) {
                
                int localX = gridPos.x - x;
                int localY = gridPos.y - y;
                if (hFlip) localX = width - 1 - localX;
                if (vFlip) localY = height - 1 - localY;
                
                int tileX = localX / 8;
                int tileY = localY / 8;
                int pitch = width / 8; 
                
                bool mapping1D = dispcnt & (1 << 6);
                int tileOffset = 0;
                if (mapping1D) {
                    int tileStride = colorMode ? 2 : 1;
                    tileOffset = (tileY * pitch + tileX) * tileStride;
                } else {
                    tileOffset = tileY * 32 + tileX * (colorMode ? 2 : 1);
                }
                
                int baseTileIndex = attr2 & 0x3FF;
                int tileIndex = baseTileIndex + tileOffset;
                int paletteNum = (attr2 >> 12) & 0xF;
                uint32_t objVramBase = 0x10000;
                
                int localTileX = localX % 8;
                int localTileY = localY % 8;
                uint8_t colorIndex = 0;
                
                if (colorMode == 0) {
                    uint32_t dataOffset = objVramBase + (tileIndex * 32);
                    uint8_t byteChunk = vram[dataOffset + (localTileY * 4) + (localTileX / 2)];
                    colorIndex = (localTileX & 1) ? ((byteChunk >> 4) & 0xF) : (byteChunk & 0xF);
                    if (colorIndex != 0 && prio <= bestPrio) {
                        bestPrio = prio;
                        uint32_t objPalBase = 0x200;
                        uint16_t c15 = *(constant uint16_t*)(&pal[objPalBase + paletteNum * 32 + colorIndex * 2]);
                        finalColor = expandColor5(c15);
                    }
                } else {
                    uint32_t dataOffset = objVramBase + (tileIndex * 64); 
                    colorIndex = vram[dataOffset + (localTileY * 8) + localTileX];
                    if (colorIndex != 0 && prio <= bestPrio) {
                        bestPrio = prio;
                        uint32_t objPalBase = 0x200;
                        uint16_t c15 = *(constant uint16_t*)(&pal[objPalBase + colorIndex * 2]);
                        finalColor = expandColor5(c15);
                    }
                }
            }
        }
    }
    
    outTexture.write(finalColor, gridPos);
}
