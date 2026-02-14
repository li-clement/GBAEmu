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

// 简单的片段着色器，采样纹理
fragment float4 fragmentShader(VertexOut in [[ stage_in ]],
                               texture2d<float> colorTexture [[ texture(0) ]]) {
    constexpr sampler textureSampler (mag_filter::nearest,
                                      min_filter::nearest);
    
    // Sample the texture
    return colorTexture.sample(textureSampler, in.textureCoordinate);
}
