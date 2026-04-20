#pragma once

#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

// ==================== Data structures ====================

struct SurveyRTInfo
{
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t    width  = 0;
    uint32_t    height = 0;
    uint64_t    rtvPtr = 0;
};

struct SurveySRVInfo
{
    uint64_t              srvPtr    = 0;
    DXGI_FORMAT           format    = DXGI_FORMAT_UNKNOWN;
    uint32_t              width     = 0;
    uint32_t              height    = 0;
    D3D11_SRV_DIMENSION   dimension = D3D11_SRV_DIMENSION_UNKNOWN;
    uint32_t              mipLevels = 0;   // detail 2+
    uint32_t              arraySize = 0;   // detail 2+
};

struct SurveyCBInfo
{
    uint64_t cbPtr   = 0;
    uint32_t size    = 0;
    uint8_t  data[512] = {};
    bool     hasData = false;
};

struct SurveyDrawEvent
{
    uint32_t drawIndex = 0;

    enum DrawType { DRAW, DRAW_INDEXED, DRAW_INDEXED_INSTANCED, DRAW_INSTANCED };
    DrawType type = DRAW;

    uint32_t vertexOrIndexCount = 0;
    uint32_t instanceCount      = 1;
    uint32_t startIndex         = 0;
    int32_t  baseVertex         = 0;
    uint32_t startInstanceLocation = 0;

    // Shaders
    uint64_t vsPtr = 0;
    uint64_t psPtr = 0;
    uint64_t gsPtr = 0;
    uint64_t hsPtr = 0;
    uint64_t dsPtr = 0;

    // Render targets
    SurveyRTInfo renderTargets[8];
    uint32_t     numRenderTargets = 0;
    uint64_t     dsvPtr    = 0;
    DXGI_FORMAT  dsvFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t     dsvWidth  = 0;
    uint32_t     dsvHeight = 0;

    // Pixel shader SRVs (slots 0-15)
    SurveySRVInfo psSRVs[16];
    uint32_t      numPSSRVs = 0;

    // Vertex shader SRVs (slots 0-3)
    SurveySRVInfo vsSRVs[4];
    uint32_t      numVSSRVs = 0;

    // Constant buffers
    SurveyCBInfo psCBs[8];
    SurveyCBInfo vsCBs[4];

    // IA state
    D3D11_PRIMITIVE_TOPOLOGY topology       = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    uint64_t                 inputLayoutPtr  = 0;
    uint32_t                 vertexStride    = 0;   // detail 3
    DXGI_FORMAT              indexFormat     = DXGI_FORMAT_UNKNOWN; // detail 3
    uint32_t                 indexBufferOffset = 0;  // detail 3

    // OM state
    uint64_t blendStatePtr        = 0;
    uint64_t depthStencilStatePtr = 0;
    uint64_t rasterizerStatePtr   = 0;

    // Viewport (detail 3)
    float vpX = 0, vpY = 0, vpW = 0, vpH = 0;
    float vpMinDepth = 0, vpMaxDepth = 0;

    // Crash-proofing: which queries failed
    uint32_t failedQueries = 0; // bitmask
};

struct SurveyFrameData
{
    uint64_t frameIndex = 0;
    std::vector<SurveyDrawEvent> draws;
    uint32_t totalDrawCalls          = 0;
    uint32_t totalDrawIndexedCalls   = 0;
    uint32_t totalDrawInstCalls      = 0;
    float    captureTimeMs           = 0.0f;
};

// ==================== Shader source tracking ====================

struct ShaderSourceInfo
{
    std::string source;
    std::string entryPoint;
    std::string target;
    std::string sourceName;
};

// ==================== Recorder API ====================

namespace SurveyRecorder
{
    // Called from draw hooks when Survey::IsActive()
    void OnDraw(ID3D11DeviceContext* ctx, UINT VertexCount, UINT StartVertexLocation);
    void OnDrawIndexed(ID3D11DeviceContext* ctx, UINT IndexCount,
                       UINT StartIndexLocation, INT BaseVertexLocation);
    void OnDrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT IndexCountPerInstance,
                                UINT InstanceCount, UINT StartIndexLocation,
                                INT BaseVertexLocation, UINT StartInstanceLocation);

    // Called from Present hook — finalizes the frame, writes output.
    // Returns the completed frame data (moved out).
    SurveyFrameData OnEndFrame();

    // Called at survey start/stop
    void Reset();
    void Shutdown();

    // Shader source capture — called from D3DCompile and CreateShader hooks
    void OnShaderCompiled(const void* pSrcData, SIZE_T srcSize,
                          const char* entryPoint, const char* target,
                          const char* sourceName, const void* bytecode, SIZE_T bytecodeSize);
    void OnPixelShaderCreated(const void* bytecode, SIZE_T bytecodeSize,
                              ID3D11PixelShader* shader);
    void OnVertexShaderCreated(const void* bytecode, SIZE_T bytecodeSize,
                               ID3D11VertexShader* shader);

    // Look up shader source by pointer (for writer)
    const ShaderSourceInfo* GetShaderSource(uint64_t shaderPtr);

    // Get all tracked shader sources
    const std::unordered_map<uint64_t, ShaderSourceInfo>& GetShaderMap();
}
