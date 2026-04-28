#pragma once

// ShaderSourceCatalog
// -------------------
// Parses Kenshi's HLSL sources at startup (from vanilla_shaders/) and builds
// a per-file metadata catalog: entry points, uniforms (function-parameter
// style, OGRE convention), texture/sampler bindings, vertex inputs/outputs.
// Each declaration carries the #ifdef gate stack that controls it, so we can
// resolve any specific compile-time variant by name+defines at runtime.
//
// This is the source-of-truth half of Priority 1 in the renderer architecture
// vision: replace runtime D3DReflect with proactive source-level parsing.
//
// Phase 1.A: parse + catalog (this file).
// Phase 1.B: variant resolution (ResolveVariant).
// Phase 1.C: cross-validation against D3DReflect (in D3DCompile hook path).

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace ShaderSourceCatalog
{

// ----- Type system (minimal — only what we need to classify) -----

enum class HLSLType : uint8_t
{
    Unknown = 0,

    // Scalars
    Void,
    Float, Float2, Float3, Float4,
    Int, Int2, Int3, Int4,
    Uint, Uint2, Uint3, Uint4,
    Bool,

    // Matrices
    Float2x2, Float3x3, Float4x4, Float3x4, Float4x3,

    // Resource / sampler types (OGRE-era + DX11)
    Sampler,        // bare 'sampler' (legacy)
    Sampler2D,      // sampler2D
    Sampler3D,      // sampler3D
    SamplerCUBE,    // samplerCUBE
    SamplerState,   // SamplerState (DX11)
    Texture2D,
    Texture3D,
    TextureCUBE,

    // Anything else (struct types like GBuffer, etc.) — typeRaw holds the name
    Struct,
};

enum class ShaderStage : uint8_t
{
    Unknown = 0,
    Vertex,
    Pixel,
    Geometry,
    Hull,
    Domain,
    Compute,
};

// Boolean expression over preprocessor defines. Used to gate a declaration.
// Built from #ifdef / #ifndef / #if defined() / #elif / #else / #endif and
// the &&, ||, !, defined() operators that appear inside #if expressions.
struct GateExpr
{
    enum Kind : uint8_t { Always, Cond, And, Or, Not };
    Kind kind = Always;
    // For Kind::Cond
    std::string name;
    bool        defined = true;   // true: defined(name); false: !defined(name)
    // For Kind::And / Or / Not
    std::vector<GateExpr> children;
};

// Wraps the gate expression that controls a declaration. A declaration with
// an empty (Always) gate is unconditional.
struct DefineGate
{
    GateExpr expr;
    bool empty() const { return expr.kind == GateExpr::Always; }
};

// A 'uniform' function parameter — becomes a CB variable after compilation.
struct ShaderUniform
{
    std::string name;
    HLSLType    type = HLSLType::Unknown;
    std::string typeRaw;        // exact source string of the type token
    uint32_t    arraySize = 0;  // 0 = scalar/non-array; otherwise N
    DefineGate  gate;
};

// A texture/sampler parameter with optional 'register(sN)' annotation.
struct ShaderResource
{
    std::string name;
    HLSLType    type = HLSLType::Unknown;
    std::string typeRaw;
    int         registerSlot = -1;  // -1 = unspecified
    char        registerKind = 's'; // 's', 't', 'b', 'u' — OGRE always emits 's'
    DefineGate  gate;
};

// A non-uniform parameter (vertex input, vertex output, PS input).
// 'isOutput' distinguishes 'out' params from 'in'/(unqualified) inputs.
struct ShaderInputAttr
{
    std::string name;
    std::string semantic;       // POSITION, NORMAL, TEXCOORD0, SV_Position, ...
    HLSLType    type = HLSLType::Unknown;
    std::string typeRaw;
    bool        isOutput = false;
    DefineGate  gate;
};

struct ShaderEntryPoint
{
    std::string                  name;
    ShaderStage                  stage = ShaderStage::Unknown;
    std::vector<ShaderInputAttr> inputs;
    std::vector<ShaderInputAttr> outputs;
    std::vector<ShaderUniform>   uniforms;
    std::vector<ShaderResource>  resources;
    // Defines mentioned anywhere inside the function body (for variant enumeration).
    std::vector<std::string>     bodyReferencedDefines;
};

struct ShaderFile
{
    std::string                                  path;          // relative to vanilla_shaders/
    std::string                                  category;      // first folder ("deferred", "post", "common", ...)
    std::vector<std::string>                     includes;      // resolved include paths (relative)
    std::vector<ShaderEntryPoint>                entryPoints;
    std::unordered_map<std::string, std::string> defines;       // top-level #define NAME VALUE
    // File-scope uniforms and resources (top-level 'uniform' decls and
    // 'Texture2D ... : register(sN);' / 'SamplerState X { ... };' / 'cbuffer { ... };'
    // bodies). These apply to every entry point in the file and are merged
    // into each ResolvedVariant alongside the entry point's own parameters.
    std::vector<ShaderUniform>                   globalUniforms;
    std::vector<ShaderResource>                  globalResources;
};

// Result of resolving a (file, entry, defines[]) tuple — keeps pointers into the
// owning ShaderFile so values are stable and cheap to compare.
struct ResolvedVariant
{
    const ShaderFile*                       file  = nullptr;
    const ShaderEntryPoint*                 entry = nullptr;
    std::vector<const ShaderInputAttr*>     activeInputs;
    std::vector<const ShaderInputAttr*>     activeOutputs;
    std::vector<const ShaderUniform*>       activeUniforms;
    std::vector<const ShaderResource*>      activeResources;
    bool                                    valid = false;
};

// ----- API -----

// Parses every .hlsl under rootDir. Idempotent: a second call clears and re-parses.
// Returns the number of files parsed (0 on failure).
size_t Init(const std::string& rootDir);

void Shutdown();

// Lookups
const ShaderFile* GetFileByPath(const std::string& relativePath);
const ShaderFile* GetFileByBasename(const std::string& basenameNoExt);   // e.g., "objects"
const ShaderEntryPoint* FindEntryPoint(const ShaderFile* file, const std::string& entryName);

size_t GetFileCount();
const std::vector<std::string>& GetAllRelativePaths();

// Variant resolution (Phase 1.B). Filters declarations so only those whose gate
// holds under the given defines remain. defines[] is the set of macros the
// game/D3DCompile would see as defined; missing names are treated as undefined.
ResolvedVariant ResolveVariant(const ShaderFile*                file,
                               const std::string&               entryName,
                               const std::vector<std::string>&  defines);

// Diagnostic helpers
const char* TypeToString(HLSLType t);
const char* StageToString(ShaderStage s);

// One-shot dump of catalog summary to the log (file/entry/uniform counts).
void LogSummary();

// ----- Phase 1.C: cross-validation against D3DReflect -----

// Cross-validation runs D3DReflect on every shader compile, which adds heap
// pressure and runs alongside (older) D3DCompiler_43 used by Kenshi. Off by
// default; enable from Dust.ini:
//   [Dust]
//   ValidateCatalog=1
// Or flip the static once at startup before any compile fires.
void SetValidationEnabled(bool enabled);
bool IsValidationEnabled();


struct ValidationResult
{
    bool          ranReflection         = false;  // false = no catalog entry / reflection failed
    int           catalogUniforms       = 0;
    int           reflectionUniforms    = 0;
    int           uniformsMatched       = 0;
    int           uniformsCatalogOnly   = 0;     // catalog-only — likely unused / compiler-stripped
    int           uniformsReflectionOnly= 0;     // reflection-only — likely a parser bug

    int           catalogResources      = 0;
    int           reflectionBindings    = 0;
    int           resourcesMatched      = 0;
    int           resourcesCatalogOnly  = 0;
    int           resourcesReflectionOnly = 0;

    // Specific issues (max ~16 entries — beyond that, summary counts only)
    std::vector<std::string> issues;
};

// Compare the catalog's view of (sourceName, entryName, defines) against
// what D3DReflect produces from the compiled bytecode. Logs a one-line
// summary; on mismatch, logs the first few specific differences.
//
// Identifies the catalog file by basename of pSourceName (e.g.,
// "deferred/objects.hlsl" -> "objects"). Skips silently if no matching
// entry point is found in the catalog.
ValidationResult ValidateAgainstBytecode(const char*              sourceName,
                                         const char*              entryName,
                                         const std::vector<std::string>& defines,
                                         const void*              bytecode,
                                         size_t                   bytecodeSize);

} // namespace ShaderSourceCatalog
