#include "SurveyWriter.h"
#include "Survey.h"
#include "DustLog.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <set>
#include <map>
#include <cstring>

// ==================== Format helpers ====================

static const char* DXGIFormatStr(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_UNKNOWN:                    return "UNKNOWN";
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:      return "R32G32B32A32_TYPELESS";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:         return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_UINT:          return "R32G32B32A32_UINT";
    case DXGI_FORMAT_R32G32B32A32_SINT:          return "R32G32B32A32_SINT";
    case DXGI_FORMAT_R32G32B32_TYPELESS:         return "R32G32B32_TYPELESS";
    case DXGI_FORMAT_R32G32B32_FLOAT:            return "R32G32B32_FLOAT";
    case DXGI_FORMAT_R32G32B32_UINT:             return "R32G32B32_UINT";
    case DXGI_FORMAT_R32G32B32_SINT:             return "R32G32B32_SINT";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:      return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:         return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UNORM:         return "R16G16B16A16_UNORM";
    case DXGI_FORMAT_R16G16B16A16_UINT:          return "R16G16B16A16_UINT";
    case DXGI_FORMAT_R16G16B16A16_SNORM:         return "R16G16B16A16_SNORM";
    case DXGI_FORMAT_R16G16B16A16_SINT:          return "R16G16B16A16_SINT";
    case DXGI_FORMAT_R32G32_TYPELESS:            return "R32G32_TYPELESS";
    case DXGI_FORMAT_R32G32_FLOAT:               return "R32G32_FLOAT";
    case DXGI_FORMAT_R32G32_UINT:                return "R32G32_UINT";
    case DXGI_FORMAT_R32G32_SINT:                return "R32G32_SINT";
    case DXGI_FORMAT_R32G8X24_TYPELESS:          return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:       return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:   return "R32_FLOAT_X8X24_TYPELESS";
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:    return "X32_TYPELESS_G8X24_UINT";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:       return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R10G10B10A2_UNORM:          return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UINT:           return "R10G10B10A2_UINT";
    case DXGI_FORMAT_R11G11B10_FLOAT:            return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:          return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM:             return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:        return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_UINT:              return "R8G8B8A8_UINT";
    case DXGI_FORMAT_R8G8B8A8_SNORM:             return "R8G8B8A8_SNORM";
    case DXGI_FORMAT_R8G8B8A8_SINT:              return "R8G8B8A8_SINT";
    case DXGI_FORMAT_R16G16_TYPELESS:            return "R16G16_TYPELESS";
    case DXGI_FORMAT_R16G16_FLOAT:               return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16_UNORM:               return "R16G16_UNORM";
    case DXGI_FORMAT_R16G16_UINT:                return "R16G16_UINT";
    case DXGI_FORMAT_R16G16_SNORM:               return "R16G16_SNORM";
    case DXGI_FORMAT_R16G16_SINT:                return "R16G16_SINT";
    case DXGI_FORMAT_R32_TYPELESS:               return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT:                  return "D32_FLOAT";
    case DXGI_FORMAT_R32_FLOAT:                  return "R32_FLOAT";
    case DXGI_FORMAT_R32_UINT:                   return "R32_UINT";
    case DXGI_FORMAT_R32_SINT:                   return "R32_SINT";
    case DXGI_FORMAT_R24G8_TYPELESS:             return "R24G8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:          return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:      return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:       return "X24_TYPELESS_G8_UINT";
    case DXGI_FORMAT_R8G8_TYPELESS:              return "R8G8_TYPELESS";
    case DXGI_FORMAT_R8G8_UNORM:                 return "R8G8_UNORM";
    case DXGI_FORMAT_R8G8_UINT:                  return "R8G8_UINT";
    case DXGI_FORMAT_R8G8_SNORM:                 return "R8G8_SNORM";
    case DXGI_FORMAT_R8G8_SINT:                  return "R8G8_SINT";
    case DXGI_FORMAT_R16_TYPELESS:               return "R16_TYPELESS";
    case DXGI_FORMAT_R16_FLOAT:                  return "R16_FLOAT";
    case DXGI_FORMAT_D16_UNORM:                  return "D16_UNORM";
    case DXGI_FORMAT_R16_UNORM:                  return "R16_UNORM";
    case DXGI_FORMAT_R16_UINT:                   return "R16_UINT";
    case DXGI_FORMAT_R16_SNORM:                  return "R16_SNORM";
    case DXGI_FORMAT_R16_SINT:                   return "R16_SINT";
    case DXGI_FORMAT_R8_TYPELESS:                return "R8_TYPELESS";
    case DXGI_FORMAT_R8_UNORM:                   return "R8_UNORM";
    case DXGI_FORMAT_R8_UINT:                    return "R8_UINT";
    case DXGI_FORMAT_R8_SNORM:                   return "R8_SNORM";
    case DXGI_FORMAT_R8_SINT:                    return "R8_SINT";
    case DXGI_FORMAT_A8_UNORM:                   return "A8_UNORM";
    case DXGI_FORMAT_R1_UNORM:                   return "R1_UNORM";
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:         return "R9G9B9E5_SHAREDEXP";
    case DXGI_FORMAT_R8G8_B8G8_UNORM:            return "R8G8_B8G8_UNORM";
    case DXGI_FORMAT_G8R8_G8B8_UNORM:            return "G8R8_G8B8_UNORM";
    case DXGI_FORMAT_BC1_TYPELESS:               return "BC1_TYPELESS";
    case DXGI_FORMAT_BC1_UNORM:                  return "BC1_UNORM";
    case DXGI_FORMAT_BC1_UNORM_SRGB:             return "BC1_UNORM_SRGB";
    case DXGI_FORMAT_BC2_TYPELESS:               return "BC2_TYPELESS";
    case DXGI_FORMAT_BC2_UNORM:                  return "BC2_UNORM";
    case DXGI_FORMAT_BC2_UNORM_SRGB:             return "BC2_UNORM_SRGB";
    case DXGI_FORMAT_BC3_TYPELESS:               return "BC3_TYPELESS";
    case DXGI_FORMAT_BC3_UNORM:                  return "BC3_UNORM";
    case DXGI_FORMAT_BC3_UNORM_SRGB:             return "BC3_UNORM_SRGB";
    case DXGI_FORMAT_BC4_TYPELESS:               return "BC4_TYPELESS";
    case DXGI_FORMAT_BC4_UNORM:                  return "BC4_UNORM";
    case DXGI_FORMAT_BC4_SNORM:                  return "BC4_SNORM";
    case DXGI_FORMAT_BC5_TYPELESS:               return "BC5_TYPELESS";
    case DXGI_FORMAT_BC5_UNORM:                  return "BC5_UNORM";
    case DXGI_FORMAT_BC5_SNORM:                  return "BC5_SNORM";
    case DXGI_FORMAT_B5G6R5_UNORM:               return "B5G6R5_UNORM";
    case DXGI_FORMAT_B5G5R5A1_UNORM:             return "B5G5R5A1_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM:             return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8X8_UNORM:             return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM: return "R10G10B10_XR_BIAS_A2_UNORM";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:          return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:        return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:          return "B8G8R8X8_TYPELESS";
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:        return "B8G8R8X8_UNORM_SRGB";
    case DXGI_FORMAT_BC6H_TYPELESS:              return "BC6H_TYPELESS";
    case DXGI_FORMAT_BC6H_UF16:                  return "BC6H_UF16";
    case DXGI_FORMAT_BC6H_SF16:                  return "BC6H_SF16";
    case DXGI_FORMAT_BC7_TYPELESS:               return "BC7_TYPELESS";
    case DXGI_FORMAT_BC7_UNORM:                  return "BC7_UNORM";
    case DXGI_FORMAT_BC7_UNORM_SRGB:             return "BC7_UNORM_SRGB";
    case DXGI_FORMAT_B4G4R4A4_UNORM:             return "B4G4R4A4_UNORM";
    default:
    {
        static char buf[32];
        snprintf(buf, sizeof(buf), "FORMAT_%u", (unsigned)f);
        return buf;
    }
    }
}

static const char* TopologyStr(D3D11_PRIMITIVE_TOPOLOGY t)
{
    switch (t)
    {
    case D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED:         return "UNDEFINED";
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:          return "POINTLIST";
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:           return "LINELIST";
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:          return "LINESTRIP";
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:       return "TRIANGLELIST";
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:      return "TRIANGLESTRIP";
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:       return "LINELIST_ADJ";
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:      return "LINESTRIP_ADJ";
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:   return "TRIANGLELIST_ADJ";
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:  return "TRIANGLESTRIP_ADJ";
    default:
        if (t >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST &&
            t <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST)
        {
            static char buf[32];
            snprintf(buf, sizeof(buf), "PATCHLIST_%d_CP",
                     t - D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + 1);
            return buf;
        }
        return "UNKNOWN";
    }
}

static const char* SRVDimensionStr(D3D11_SRV_DIMENSION d)
{
    switch (d)
    {
    case D3D11_SRV_DIMENSION_UNKNOWN:          return "UNKNOWN";
    case D3D11_SRV_DIMENSION_BUFFER:           return "BUFFER";
    case D3D11_SRV_DIMENSION_TEXTURE1D:        return "TEXTURE1D";
    case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:   return "TEXTURE1DARRAY";
    case D3D11_SRV_DIMENSION_TEXTURE2D:        return "TEXTURE2D";
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:   return "TEXTURE2DARRAY";
    case D3D11_SRV_DIMENSION_TEXTURE2DMS:      return "TEXTURE2DMS";
    case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY: return "TEXTURE2DMSARRAY";
    case D3D11_SRV_DIMENSION_TEXTURE3D:        return "TEXTURE3D";
    case D3D11_SRV_DIMENSION_TEXTURECUBE:      return "TEXTURECUBE";
    case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY: return "TEXTURECUBEARRAY";
    case D3D11_SRV_DIMENSION_BUFFEREX:         return "BUFFEREX";
    default: return "UNKNOWN";
    }
}

static const char* DrawTypeStr(SurveyDrawEvent::DrawType t)
{
    switch (t)
    {
    case SurveyDrawEvent::DRAW:                    return "Draw";
    case SurveyDrawEvent::DRAW_INDEXED:            return "DrawIndexed";
    case SurveyDrawEvent::DRAW_INDEXED_INSTANCED:  return "DrawIndexedInstanced";
    case SurveyDrawEvent::DRAW_INSTANCED:          return "DrawInstanced";
    default: return "Unknown";
    }
}

// ==================== Base64 encoder (for CB data) ====================

static const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64Encode(const uint8_t* data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3)
    {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < len) n |= ((uint32_t)data[i + 2]);

        out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kBase64Chars[(n >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kBase64Chars[n & 0x3F] : '=');
    }
    return out;
}

// ==================== JSON string escaping ====================

static std::string JsonEscape(const char* s)
{
    std::string out;
    for (; *s; s++)
    {
        switch (*s)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)*s < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*s);
                out += buf;
            }
            else
                out.push_back(*s);
        }
    }
    return out;
}

// ==================== JSON writer helpers ====================

static void WritePtr(FILE* f, const char* key, uint64_t ptr)
{
    if (ptr)
        fprintf(f, "\"%s\": \"0x%llX\"", key, (unsigned long long)ptr);
    else
        fprintf(f, "\"%s\": null", key);
}

// ==================== Frame writer ====================

namespace SurveyWriter
{

void WriteFrame(const SurveyFrameData& frame, const char* outputDir)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sframe_%llu.json", outputDir,
             (unsigned long long)frame.frameIndex);

    FILE* f = fopen(path, "w");
    if (!f)
    {
        Log("SURVEY: Failed to open %s for writing", path);
        return;
    }

    int detail = Survey::GetDetailLevel();

    fprintf(f, "{\n");
    fprintf(f, "  \"frameIndex\": %llu,\n", (unsigned long long)frame.frameIndex);
    fprintf(f, "  \"captureTimeMs\": %.2f,\n", frame.captureTimeMs);
    fprintf(f, "  \"detailLevel\": %d,\n", detail);
    fprintf(f, "  \"totalDrawCalls\": %u,\n", frame.totalDrawCalls);
    fprintf(f, "  \"totalDrawIndexedCalls\": %u,\n", frame.totalDrawIndexedCalls);
    fprintf(f, "  \"totalDrawInstancedCalls\": %u,\n", frame.totalDrawInstCalls);
    fprintf(f, "  \"totalEvents\": %zu,\n", frame.draws.size());
    fprintf(f, "  \"draws\": [\n");

    for (size_t di = 0; di < frame.draws.size(); di++)
    {
        const SurveyDrawEvent& ev = frame.draws[di];

        fprintf(f, "    {\n");
        fprintf(f, "      \"index\": %u,\n", ev.drawIndex);
        fprintf(f, "      \"type\": \"%s\",\n", DrawTypeStr(ev.type));

        if (ev.type == SurveyDrawEvent::DRAW)
            fprintf(f, "      \"vertexCount\": %u,\n", ev.vertexOrIndexCount);
        else
            fprintf(f, "      \"indexCount\": %u,\n", ev.vertexOrIndexCount);

        if (ev.instanceCount > 1)
            fprintf(f, "      \"instanceCount\": %u,\n", ev.instanceCount);

        if (ev.failedQueries)
            fprintf(f, "      \"failedQueries\": %u,\n", ev.failedQueries);

        // Render targets
        fprintf(f, "      \"renderTargets\": [");
        bool firstRT = true;
        for (uint32_t ri = 0; ri < ev.numRenderTargets; ri++)
        {
            const SurveyRTInfo& rt = ev.renderTargets[ri];
            if (rt.rtvPtr == 0) continue;
            if (!firstRT) fprintf(f, ",");
            firstRT = false;
            fprintf(f, "\n        { \"slot\": %u, \"format\": \"%s\", \"width\": %u, \"height\": %u }",
                    ri, DXGIFormatStr(rt.format), rt.width, rt.height);
        }
        fprintf(f, "\n      ],\n");

        // Depth-stencil
        if (ev.dsvPtr)
        {
            fprintf(f, "      \"depthStencil\": { \"format\": \"%s\", \"width\": %u, \"height\": %u },\n",
                    DXGIFormatStr(ev.dsvFormat), ev.dsvWidth, ev.dsvHeight);
        }
        else
        {
            fprintf(f, "      \"depthStencil\": null,\n");
        }

        // Topology
        if (detail >= Survey::DETAIL_STANDARD)
            fprintf(f, "      \"topology\": \"%s\",\n", TopologyStr(ev.topology));
        else
            fprintf(f, "      \"topology\": \"%s\"\n", TopologyStr(ev.topology));

        if (detail >= Survey::DETAIL_STANDARD)
        {
            // Shaders
            fprintf(f, "      "); WritePtr(f, "vs", ev.vsPtr); fprintf(f, ",\n");
            fprintf(f, "      "); WritePtr(f, "ps", ev.psPtr); fprintf(f, ",\n");
            fprintf(f, "      "); WritePtr(f, "gs", ev.gsPtr); fprintf(f, ",\n");
            fprintf(f, "      "); WritePtr(f, "hs", ev.hsPtr); fprintf(f, ",\n");
            fprintf(f, "      "); WritePtr(f, "ds", ev.dsPtr); fprintf(f, ",\n");

            // Input layout
            fprintf(f, "      "); WritePtr(f, "inputLayout", ev.inputLayoutPtr); fprintf(f, ",\n");

            // OM states
            fprintf(f, "      "); WritePtr(f, "blendState", ev.blendStatePtr); fprintf(f, ",\n");
            fprintf(f, "      "); WritePtr(f, "depthStencilState", ev.depthStencilStatePtr); fprintf(f, ",\n");
            fprintf(f, "      "); WritePtr(f, "rasterizerState", ev.rasterizerStatePtr); fprintf(f, ",\n");

            // PS SRVs
            fprintf(f, "      \"psSRVs\": [");
            bool firstSRV = true;
            for (uint32_t si = 0; si < ev.numPSSRVs; si++)
            {
                const SurveySRVInfo& srv = ev.psSRVs[si];
                if (srv.srvPtr == 0) continue;
                if (!firstSRV) fprintf(f, ",");
                firstSRV = false;
                fprintf(f, "\n        { \"slot\": %u, \"format\": \"%s\", \"width\": %u, \"height\": %u, \"dimension\": \"%s\"",
                        si, DXGIFormatStr(srv.format), srv.width, srv.height, SRVDimensionStr(srv.dimension));
                if (detail >= Survey::DETAIL_DEEP && (srv.mipLevels || srv.arraySize))
                    fprintf(f, ", \"mipLevels\": %u, \"arraySize\": %u", srv.mipLevels, srv.arraySize);
                fprintf(f, " }");
            }
            fprintf(f, "\n      ],\n");

            // VS SRVs
            fprintf(f, "      \"vsSRVs\": [");
            firstSRV = true;
            for (uint32_t si = 0; si < ev.numVSSRVs; si++)
            {
                const SurveySRVInfo& srv = ev.vsSRVs[si];
                if (srv.srvPtr == 0) continue;
                if (!firstSRV) fprintf(f, ",");
                firstSRV = false;
                fprintf(f, "\n        { \"slot\": %u, \"format\": \"%s\", \"width\": %u, \"height\": %u, \"dimension\": \"%s\" }",
                        si, DXGIFormatStr(srv.format), srv.width, srv.height, SRVDimensionStr(srv.dimension));
            }
            fprintf(f, "\n      ],\n");

            // PS CBs
            fprintf(f, "      \"psCBs\": [");
            bool firstCB = true;
            for (int ci = 0; ci < 8; ci++)
            {
                const SurveyCBInfo& cb = ev.psCBs[ci];
                if (cb.cbPtr == 0) continue;
                if (!firstCB) fprintf(f, ",");
                firstCB = false;
                fprintf(f, "\n        { \"slot\": %d, \"size\": %u", ci, cb.size);
                if (cb.hasData)
                {
                    uint32_t encLen = (cb.size > 512) ? 512 : cb.size;
                    std::string b64 = Base64Encode(cb.data, encLen);
                    fprintf(f, ", \"data\": \"%s\"", b64.c_str());
                }
                fprintf(f, " }");
            }
            fprintf(f, "\n      ],\n");

            // VS CBs
            fprintf(f, "      \"vsCBs\": [");
            firstCB = true;
            for (int ci = 0; ci < 4; ci++)
            {
                const SurveyCBInfo& cb = ev.vsCBs[ci];
                if (cb.cbPtr == 0) continue;
                if (!firstCB) fprintf(f, ",");
                firstCB = false;
                fprintf(f, "\n        { \"slot\": %d, \"size\": %u", ci, cb.size);
                if (cb.hasData)
                {
                    uint32_t encLen = (cb.size > 512) ? 512 : cb.size;
                    std::string b64 = Base64Encode(cb.data, encLen);
                    fprintf(f, ", \"data\": \"%s\"", b64.c_str());
                }
                fprintf(f, " }");
            }
            fprintf(f, "\n      ]");
        }

        if (detail >= Survey::DETAIL_FULL)
        {
            fprintf(f, ",\n");
            fprintf(f, "      \"viewport\": { \"x\": %.1f, \"y\": %.1f, \"w\": %.1f, \"h\": %.1f, \"minDepth\": %.3f, \"maxDepth\": %.3f },\n",
                    ev.vpX, ev.vpY, ev.vpW, ev.vpH, ev.vpMinDepth, ev.vpMaxDepth);
            fprintf(f, "      \"vertexStride\": %u,\n", ev.vertexStride);
            fprintf(f, "      \"indexFormat\": \"%s\",\n", DXGIFormatStr(ev.indexFormat));
            fprintf(f, "      \"indexBufferOffset\": %u", ev.indexBufferOffset);
        }

        fprintf(f, "\n    }%s\n", (di + 1 < frame.draws.size()) ? "," : "");
    }

    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    fclose(f);
    Log("SURVEY: Wrote %s (%zu draw events)", path, frame.draws.size());
}

// ==================== Shader dump ====================

void WriteShaders(const char* outputDir)
{
    const auto& shaderMap = SurveyRecorder::GetShaderMap();
    int written = 0;

    for (const auto& pair : shaderMap)
    {
        const ShaderSourceInfo& info = pair.second;
        if (info.source.empty())
            continue;

        // Determine prefix from target profile
        const char* prefix = "shader";
        if (!info.target.empty())
        {
            if (info.target[0] == 'p') prefix = "ps";
            else if (info.target[0] == 'v') prefix = "vs";
            else if (info.target[0] == 'g') prefix = "gs";
            else if (info.target[0] == 'h') prefix = "hs";
            else if (info.target[0] == 'd') prefix = "ds";
            else if (info.target[0] == 'c') prefix = "cs";
        }

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%sshaders\\%s_0x%llX.hlsl",
                 outputDir, prefix, (unsigned long long)pair.first);

        FILE* f = fopen(path, "w");
        if (!f) continue;

        fprintf(f, "// Shader pointer: 0x%llX\n", (unsigned long long)pair.first);
        fprintf(f, "// Entry point: %s\n", info.entryPoint.c_str());
        fprintf(f, "// Target: %s\n", info.target.c_str());
        if (!info.sourceName.empty())
            fprintf(f, "// Source name: %s\n", info.sourceName.c_str());
        fprintf(f, "// Captured by Dust Pipeline Survey\n\n");
        fwrite(info.source.c_str(), 1, info.source.size(), f);
        fclose(f);
        written++;
    }

    Log("SURVEY: Wrote %d shader source files to %sshaders\\", written, outputDir);
}

// ==================== Summary ====================

void WriteSummary(const SurveyFrameData* frames, int numFrames, const char* outputDir)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%ssurvey_summary.json", outputDir);

    FILE* f = fopen(path, "w");
    if (!f)
    {
        Log("SURVEY: Failed to write summary to %s", path);
        return;
    }

    // Collect aggregates
    std::set<uint64_t> uniquePS, uniqueVS, uniqueGS;
    std::map<std::string, int> topologyCounts;
    std::set<std::string> rtFormats, srvFormats;
    uint32_t totalDraws = 0;
    float totalCaptureMs = 0;

    // Track RT configuration changes for pass sequence
    struct RTConfig
    {
        uint32_t numRTs;
        DXGI_FORMAT formats[8];
        uint32_t width, height;
        bool operator!=(const RTConfig& o) const
        {
            if (numRTs != o.numRTs || width != o.width || height != o.height) return true;
            for (uint32_t i = 0; i < numRTs; i++)
                if (formats[i] != o.formats[i]) return true;
            return false;
        }
    };

    // Use first frame for pass sequence
    std::vector<std::pair<int, RTConfig>> passSequence;

    for (int fi = 0; fi < numFrames; fi++)
    {
        const SurveyFrameData& frame = frames[fi];
        totalDraws += (uint32_t)frame.draws.size();
        totalCaptureMs += frame.captureTimeMs;

        RTConfig lastConfig = {};

        for (const auto& ev : frame.draws)
        {
            if (ev.psPtr) uniquePS.insert(ev.psPtr);
            if (ev.vsPtr) uniqueVS.insert(ev.vsPtr);
            if (ev.gsPtr) uniqueGS.insert(ev.gsPtr);

            topologyCounts[TopologyStr(ev.topology)]++;

            for (uint32_t ri = 0; ri < ev.numRenderTargets; ri++)
            {
                if (ev.renderTargets[ri].rtvPtr)
                    rtFormats.insert(DXGIFormatStr(ev.renderTargets[ri].format));
            }
            for (uint32_t si = 0; si < ev.numPSSRVs; si++)
            {
                if (ev.psSRVs[si].srvPtr)
                    srvFormats.insert(DXGIFormatStr(ev.psSRVs[si].format));
            }

            // Pass sequence (first frame only)
            if (fi == 0)
            {
                RTConfig cfg = {};
                cfg.numRTs = ev.numRenderTargets;
                for (uint32_t ri = 0; ri < ev.numRenderTargets && ri < 8; ri++)
                    cfg.formats[ri] = ev.renderTargets[ri].format;
                if (ev.numRenderTargets > 0)
                {
                    cfg.width = ev.renderTargets[0].width;
                    cfg.height = ev.renderTargets[0].height;
                }

                if (cfg != lastConfig)
                {
                    passSequence.push_back({(int)ev.drawIndex, cfg});
                    lastConfig = cfg;
                }
            }
        }
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"framesCapured\": %d,\n", numFrames);
    fprintf(f, "  \"totalDrawEvents\": %u,\n", totalDraws);
    fprintf(f, "  \"totalCaptureTimeMs\": %.2f,\n", totalCaptureMs);
    fprintf(f, "  \"uniquePixelShaders\": %zu,\n", uniquePS.size());
    fprintf(f, "  \"uniqueVertexShaders\": %zu,\n", uniqueVS.size());
    fprintf(f, "  \"uniqueGeometryShaders\": %zu,\n", uniqueGS.size());
    fprintf(f, "  \"detailLevel\": %d,\n", Survey::GetDetailLevel());

    // Topology histogram
    fprintf(f, "  \"topologyHistogram\": {\n");
    {
        bool first = true;
        for (const auto& pair : topologyCounts)
        {
            if (!first) fprintf(f, ",\n");
            first = false;
            fprintf(f, "    \"%s\": %d", pair.first.c_str(), pair.second);
        }
    }
    fprintf(f, "\n  },\n");

    // RT format inventory
    fprintf(f, "  \"renderTargetFormats\": [");
    {
        bool first = true;
        for (const auto& fmt : rtFormats)
        {
            if (!first) fprintf(f, ", ");
            first = false;
            fprintf(f, "\"%s\"", fmt.c_str());
        }
    }
    fprintf(f, "],\n");

    // SRV format inventory
    fprintf(f, "  \"srvFormats\": [");
    {
        bool first = true;
        for (const auto& fmt : srvFormats)
        {
            if (!first) fprintf(f, ", ");
            first = false;
            fprintf(f, "\"%s\"", fmt.c_str());
        }
    }
    fprintf(f, "],\n");

    // Shader source count
    fprintf(f, "  \"shaderSourcesCaptured\": %zu,\n", SurveyRecorder::GetShaderMap().size());

    // Pass sequence (RT configuration changes)
    fprintf(f, "  \"passSequence\": [\n");
    for (size_t pi = 0; pi < passSequence.size(); pi++)
    {
        const auto& p = passSequence[pi];
        fprintf(f, "    { \"drawIndex\": %d, \"numRTs\": %u, \"resolution\": \"%ux%u\", \"formats\": [",
                p.first, p.second.numRTs, p.second.width, p.second.height);
        for (uint32_t ri = 0; ri < p.second.numRTs; ri++)
        {
            if (ri > 0) fprintf(f, ", ");
            fprintf(f, "\"%s\"", DXGIFormatStr(p.second.formats[ri]));
        }
        fprintf(f, "] }%s\n", (pi + 1 < passSequence.size()) ? "," : "");
    }
    fprintf(f, "  ]\n");

    fprintf(f, "}\n");
    fclose(f);
    Log("SURVEY: Wrote summary to %s", path);
}

} // namespace SurveyWriter
