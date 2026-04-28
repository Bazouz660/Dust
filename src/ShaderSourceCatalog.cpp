#include "ShaderSourceCatalog.h"

#ifdef SHADER_CATALOG_STANDALONE_TEST
  #include <cstdio>
  // Standalone test build — bypass DustLog so the parser can be exercised
  // without linking the rest of Dust.
  #define Log(...) (std::fprintf(stderr, "[Test] "), \
                    std::fprintf(stderr, __VA_ARGS__), \
                    std::fputc('\n', stderr))
#else
  #include "DustLog.h"
#endif

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <unordered_set>
#include <algorithm>

namespace ShaderSourceCatalog
{

// ============================================================================
// Catalog state
// ============================================================================

static std::unordered_map<std::string, ShaderFile> sFilesByPath;     // key = relative path with forward slashes
static std::unordered_map<std::string, std::string> sBasenameToPath; // "objects" -> "deferred/objects.hlsl"
static std::vector<std::string> sAllPaths;
static std::string sRootDir;

// ============================================================================
// Path utilities
// ============================================================================

static std::string NormalizeSlashes(std::string s)
{
    for (auto& c : s) if (c == '\\') c = '/';
    return s;
}

static std::string DirOf(const std::string& path)
{
    auto pos = path.find_last_of('/');
    return (pos == std::string::npos) ? std::string() : path.substr(0, pos);
}

static std::string Join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

static std::string BasenameNoExt(const std::string& path)
{
    auto slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

static std::string FirstFolderComponent(const std::string& relPath)
{
    auto slash = relPath.find_first_of('/');
    return (slash == std::string::npos) ? std::string() : relPath.substr(0, slash);
}

// Read entire file. Returns empty string on failure.
static std::string ReadFile(const std::string& fullPath)
{
    FILE* f = nullptr;
    fopen_s(&f, fullPath.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string out;
    if (size > 0)
    {
        out.resize((size_t)size);
        fread(&out[0], 1, (size_t)size, f);
    }
    fclose(f);
    return out;
}

// Walk rootDir recursively, collect *.hlsl as forward-slash relative paths.
static void EnumerateHLSLFiles(const std::string& rootDir,
                               const std::string& subdir,
                               std::vector<std::string>& out)
{
    std::string searchDir = rootDir;
    if (!subdir.empty()) searchDir = Join(searchDir, subdir);
    std::string pattern = searchDir + "/*";

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do
    {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == '.' && fd.cFileName[2] == 0)))
            continue;

        std::string rel = subdir.empty() ? std::string(fd.cFileName)
                                         : Join(subdir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            EnumerateHLSLFiles(rootDir, rel, out);
        }
        else
        {
            const char* dot = strrchr(fd.cFileName, '.');
            if (dot && _stricmp(dot, ".hlsl") == 0)
                out.push_back(rel);
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

// ============================================================================
// Comment stripping (preserve newlines for line tracking)
// ============================================================================

static std::string StripComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    size_t i = 0;
    const size_t n = src.size();

    while (i < n)
    {
        char c = src[i];

        // String literal — copy verbatim (HLSL has them only inside #include "...")
        if (c == '"')
        {
            out.push_back(c);
            ++i;
            while (i < n)
            {
                char d = src[i];
                out.push_back(d);
                ++i;
                if (d == '\\' && i < n) { out.push_back(src[i]); ++i; continue; }
                if (d == '"') break;
            }
            continue;
        }

        // Line comment — replace with single space, keep newline
        if (c == '/' && i + 1 < n && src[i + 1] == '/')
        {
            out.push_back(' ');
            i += 2;
            while (i < n && src[i] != '\n') ++i;
            continue;
        }

        // Block comment — replace with spaces preserving newlines
        if (c == '/' && i + 1 < n && src[i + 1] == '*')
        {
            i += 2;
            out.push_back(' ');
            while (i < n)
            {
                if (src[i] == '*' && i + 1 < n && src[i + 1] == '/') { i += 2; break; }
                if (src[i] == '\n') out.push_back('\n');
                ++i;
            }
            continue;
        }

        out.push_back(c);
        ++i;
    }

    return out;
}

// ============================================================================
// Include inlining (recursive, cycle-guarded)
// ============================================================================

// Forward declaration
static std::string ExpandIncludes(const std::string& src,
                                  const std::string& selfDir,
                                  std::unordered_set<std::string>& visited,
                                  std::vector<std::string>& topLevelIncludes,
                                  bool isTopLevel);

static bool TryParseIncludeLine(const std::string& src, size_t& pos,
                                std::string& outName)
{
    // Match: ^[ \t]*#[ \t]*include[ \t]*"NAME"
    size_t p = pos;
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) ++p;
    if (p >= src.size() || src[p] != '#') return false;
    ++p;
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) ++p;
    if (src.compare(p, 7, "include") != 0) return false;
    p += 7;
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) ++p;
    if (p >= src.size() || src[p] != '"') return false;
    ++p;
    size_t start = p;
    while (p < src.size() && src[p] != '"' && src[p] != '\n') ++p;
    if (p >= src.size() || src[p] != '"') return false;
    outName.assign(src, start, p - start);
    ++p;
    // skip rest of line
    while (p < src.size() && src[p] != '\n') ++p;
    if (p < src.size()) ++p;
    pos = p;
    return true;
}

// Resolve include relative to selfDir or to rootDir/common/ as fallback.
// Kenshi sources reference "gbuffer.hlsl" from deferred/skin.hlsl meaning
// deferred/gbuffer.hlsl, and "constants.hlsl" from deferred/deferred.hlsl
// meaning common/constants.hlsl.
static std::string ResolveIncludePath(const std::string& includeName,
                                      const std::string& selfDir)
{
    // 1) sibling in same dir
    {
        std::string full = Join(sRootDir, Join(selfDir, includeName));
        if (GetFileAttributesA(full.c_str()) != INVALID_FILE_ATTRIBUTES)
            return Join(selfDir, includeName);
    }
    // 2) common/
    {
        std::string rel = Join("common", includeName);
        std::string full = Join(sRootDir, rel);
        if (GetFileAttributesA(full.c_str()) != INVALID_FILE_ATTRIBUTES)
            return rel;
    }
    // 3) deferred/  (some forward shaders reference deferred/gbuffer indirectly)
    {
        std::string rel = Join("deferred", includeName);
        std::string full = Join(sRootDir, rel);
        if (GetFileAttributesA(full.c_str()) != INVALID_FILE_ATTRIBUTES)
            return rel;
    }
    // 4) bare (root)
    {
        std::string full = Join(sRootDir, includeName);
        if (GetFileAttributesA(full.c_str()) != INVALID_FILE_ATTRIBUTES)
            return includeName;
    }
    return {};
}

static std::string ExpandIncludes(const std::string& src,
                                  const std::string& selfDir,
                                  std::unordered_set<std::string>& visited,
                                  std::vector<std::string>& topLevelIncludes,
                                  bool isTopLevel)
{
    std::string out;
    out.reserve(src.size());

    size_t i = 0;
    const size_t n = src.size();

    while (i < n)
    {
        // Try to match include only at start-of-line (or start of buffer)
        bool atLineStart = (i == 0) || (src[i - 1] == '\n');
        if (atLineStart)
        {
            size_t saved = i;
            std::string name;
            if (TryParseIncludeLine(src, i, name))
            {
                std::string resolved = ResolveIncludePath(name, selfDir);
                if (resolved.empty())
                {
                    Log("ShaderSourceCatalog: warn — could not resolve #include \"%s\" from %s",
                        name.c_str(), selfDir.c_str());
                    out.append("\n");
                    continue;
                }
                if (isTopLevel)
                    topLevelIncludes.push_back(resolved);

                if (visited.count(resolved))
                {
                    // Already included once — skip silently (HLSL files don't have #pragma once)
                    out.append("\n");
                    continue;
                }
                visited.insert(resolved);

                std::string full = Join(sRootDir, resolved);
                std::string content = StripComments(ReadFile(full));
                std::string subDir = DirOf(resolved);
                std::string expanded = ExpandIncludes(content, subDir, visited,
                                                      topLevelIncludes, false);
                out.append(expanded);
                out.push_back('\n');
                continue;
            }
            i = saved;
        }
        out.push_back(src[i]);
        ++i;
    }

    return out;
}

// ============================================================================
// Lexer / token utilities
// ============================================================================

struct Source
{
    const std::string* text = nullptr;
    size_t pos = 0;

    bool eof() const { return !text || pos >= text->size(); }
    char peek(size_t off = 0) const
    {
        return (text && pos + off < text->size()) ? (*text)[pos + off] : '\0';
    }
    void advance() { if (!eof()) ++pos; }
    Source() = default;
    explicit Source(const std::string& s) : text(&s) {}
};

static bool IsIdentStart(char c) { return (c == '_') || isalpha((unsigned char)c); }
static bool IsIdentCont (char c) { return (c == '_') || isalnum((unsigned char)c); }

static void SkipSpace(Source& s)
{
    // Skips whitespace AND newlines (but stops on '#' at start-of-line — caller decides)
    while (!s.eof())
    {
        char c = s.peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            s.advance();
        }
        else break;
    }
}

static void SkipSpaceNoNewline(Source& s)
{
    while (!s.eof())
    {
        char c = s.peek();
        if (c == ' ' || c == '\t' || c == '\r') s.advance();
        else break;
    }
}

static std::string ReadIdent(Source& s)
{
    std::string out;
    if (s.eof() || !IsIdentStart(s.peek())) return out;
    while (!s.eof() && IsIdentCont(s.peek())) { out.push_back(s.peek()); s.advance(); }
    return out;
}

// Read until matching closer of the same kind (counts openCh-vs-closeCh only).
// Reads from current position assuming opener already consumed; returns inner text.
static std::string ReadBalancedAny(Source& s, char openCh, char closeCh)
{
    std::string out;
    int targetDepth = 1; // count of openCh-vs-closeCh
    while (!s.eof() && targetDepth > 0)
    {
        char c = s.peek();
        if (c == openCh) { ++targetDepth; out.push_back(c); s.advance(); continue; }
        if (c == closeCh)
        {
            --targetDepth;
            if (targetDepth == 0) { s.advance(); break; }
            out.push_back(c); s.advance(); continue;
        }
        out.push_back(c);
        s.advance();
    }
    return out;
}

// Skip until we find a matching '}' for '{' at current pos. Tracks nesting of {}.
static void SkipBracedBody(Source& s)
{
    // Expect current char is '{'
    if (s.peek() != '{') return;
    s.advance();
    int depth = 1;
    while (!s.eof() && depth > 0)
    {
        char c = s.peek();
        if (c == '{') ++depth;
        else if (c == '}') { --depth; if (depth == 0) { s.advance(); break; } }
        s.advance();
    }
}

// Scan inside a string range for #ifdef-like directives, recording referenced names.
static void CollectReferencedDefines(const std::string& body,
                                     std::vector<std::string>& out)
{
    auto pushUnique = [&](const std::string& n)
    {
        if (n.empty()) return;
        for (auto& e : out) if (e == n) return;
        out.push_back(n);
    };

    size_t i = 0;
    const size_t n = body.size();
    while (i < n)
    {
        // Find a '#' at start of line
        bool atLine = (i == 0 || body[i - 1] == '\n');
        if (atLine)
        {
            size_t p = i;
            while (p < n && (body[p] == ' ' || body[p] == '\t')) ++p;
            if (p < n && body[p] == '#')
            {
                ++p;
                while (p < n && (body[p] == ' ' || body[p] == '\t')) ++p;
                auto matchKw = [&](const char* kw) -> bool
                {
                    size_t k = strlen(kw);
                    if (p + k > n) return false;
                    if (body.compare(p, k, kw) != 0) return false;
                    char nc = (p + k < n) ? body[p + k] : '\0';
                    return !IsIdentCont(nc);
                };
                bool ifdef  = matchKw("ifdef");
                bool ifndef = matchKw("ifndef");
                bool ifkw   = (!ifdef && !ifndef) && matchKw("if");
                bool elifkw = matchKw("elif");
                if (ifdef)  { p += 5; }
                else if (ifndef) { p += 6; }
                else if (ifkw)   { p += 2; }
                else if (elifkw) { p += 4; }
                if (ifdef || ifndef || ifkw || elifkw)
                {
                    while (p < n && (body[p] == ' ' || body[p] == '\t')) ++p;
                    // Optional 'defined(' or '!defined('
                    if (body.compare(p, 1, "!") == 0) { ++p; while (p<n && (body[p]==' '||body[p]=='\t')) ++p; }
                    if (body.compare(p, 8, "defined(") == 0)
                    {
                        p += 8;
                        size_t s2 = p;
                        while (p < n && IsIdentCont(body[p])) ++p;
                        pushUnique(body.substr(s2, p - s2));
                    }
                    else if (body.compare(p, 7, "defined") == 0 &&
                             (p + 7 == n || (body[p + 7] == ' ' || body[p + 7] == '\t')))
                    {
                        p += 7;
                        while (p<n && (body[p]==' '||body[p]=='\t')) ++p;
                        size_t s2 = p;
                        while (p < n && IsIdentCont(body[p])) ++p;
                        pushUnique(body.substr(s2, p - s2));
                    }
                    else if (p < n && IsIdentStart(body[p]))
                    {
                        size_t s2 = p;
                        while (p < n && IsIdentCont(body[p])) ++p;
                        pushUnique(body.substr(s2, p - s2));
                    }
                }
            }
        }
        ++i;
    }
}

// ============================================================================
// Type classification
// ============================================================================

static const struct { const char* name; HLSLType t; } TYPE_TABLE[] = {
    { "void",           HLSLType::Void },
    { "float",          HLSLType::Float },
    { "float2",         HLSLType::Float2 },
    { "float3",         HLSLType::Float3 },
    { "float4",         HLSLType::Float4 },
    { "int",            HLSLType::Int },
    { "int2",           HLSLType::Int2 },
    { "int3",           HLSLType::Int3 },
    { "int4",           HLSLType::Int4 },
    { "uint",           HLSLType::Uint },
    { "uint2",          HLSLType::Uint2 },
    { "uint3",          HLSLType::Uint3 },
    { "uint4",          HLSLType::Uint4 },
    { "bool",           HLSLType::Bool },
    { "float2x2",       HLSLType::Float2x2 },
    { "float3x3",       HLSLType::Float3x3 },
    { "float4x4",       HLSLType::Float4x4 },
    { "float3x4",       HLSLType::Float3x4 },
    { "float4x3",       HLSLType::Float4x3 },
    // 'matrix' is an HLSL keyword aliasing float4x4 (also matrix<T,R,C> form)
    { "matrix",         HLSLType::Float4x4 },
    { "sampler",        HLSLType::Sampler },
    { "sampler2D",      HLSLType::Sampler2D },
    { "sampler3D",      HLSLType::Sampler3D },
    { "samplerCUBE",    HLSLType::SamplerCUBE },
    { "SamplerState",   HLSLType::SamplerState },
    { "Texture2D",      HLSLType::Texture2D },
    { "Texture2DArray", HLSLType::Texture2D },     // array variant — same resource family
    { "Texture3D",      HLSLType::Texture3D },
    { "TextureCUBE",    HLSLType::TextureCUBE },
    { "TextureCube",    HLSLType::TextureCUBE },
};

static HLSLType ClassifyType(const std::string& ident)
{
    for (auto& e : TYPE_TABLE)
        if (ident == e.name) return e.t;
    return HLSLType::Struct; // unknown but in type position — treat as struct
}

static bool IsResourceType(HLSLType t)
{
    switch (t)
    {
    case HLSLType::Sampler:
    case HLSLType::Sampler2D:
    case HLSLType::Sampler3D:
    case HLSLType::SamplerCUBE:
    case HLSLType::SamplerState:
    case HLSLType::Texture2D:
    case HLSLType::Texture3D:
    case HLSLType::TextureCUBE:
        return true;
    default:
        return false;
    }
}

// ============================================================================
// Top-level walk with gate stack
// ============================================================================

// ============================================================================
// #if expression parser (handles defined(X), !defined(X), &&, ||, parens)
// ============================================================================

namespace
{

struct ExprLex
{
    const std::string& s;
    size_t pos = 0;
    explicit ExprLex(const std::string& src) : s(src) {}

    void skipWs() { while (pos < s.size() && (s[pos]==' '||s[pos]=='\t')) ++pos; }
    bool eof() const { return pos >= s.size(); }
    char peek() const { return eof() ? '\0' : s[pos]; }
    bool match(const char* tok)
    {
        skipWs();
        size_t L = strlen(tok);
        if (pos + L > s.size()) return false;
        if (s.compare(pos, L, tok) != 0) return false;
        // For '&&', '||', '!', '(', ')', no boundary check needed
        // For 'defined', need following char to not be ident
        if (tok[0] >= 'a' && tok[0] <= 'z')
        {
            char c = (pos + L < s.size()) ? s[pos + L] : '\0';
            if (c == '_' || isalnum((unsigned char)c)) return false;
        }
        pos += L;
        return true;
    }
};

static GateExpr parseOr(ExprLex& lx);

static GateExpr parsePrimary(ExprLex& lx)
{
    lx.skipWs();
    if (lx.match("("))
    {
        GateExpr e = parseOr(lx);
        lx.match(")");
        return e;
    }
    if (lx.match("!"))
    {
        GateExpr e;
        e.kind = GateExpr::Not;
        e.children.push_back(parsePrimary(lx));
        return e;
    }
    if (lx.match("defined"))
    {
        lx.skipWs();
        bool paren = lx.match("(");
        lx.skipWs();
        std::string name;
        while (!lx.eof() && (IsIdentCont(lx.peek()))) { name.push_back(lx.peek()); ++lx.pos; }
        if (paren) { lx.skipWs(); lx.match(")"); }
        GateExpr e;
        if (name.empty()) { e.kind = GateExpr::Always; }
        else { e.kind = GateExpr::Cond; e.name = name; e.defined = true; }
        return e;
    }
    // Bare token: identifier (treated as defined(X)) or number literal
    if (!lx.eof() && IsIdentStart(lx.peek()))
    {
        std::string name;
        while (!lx.eof() && IsIdentCont(lx.peek())) { name.push_back(lx.peek()); ++lx.pos; }
        GateExpr e;
        e.kind = GateExpr::Cond;
        e.name = name;
        e.defined = true;
        return e;
    }
    if (!lx.eof() && isdigit((unsigned char)lx.peek()))
    {
        // Numeric literal — consume; treat 0 as Always-false, anything else as Always-true.
        // Always-false is uncommon; we represent with a sentinel Cond on a synthetic name.
        long val = 0;
        while (!lx.eof() && isdigit((unsigned char)lx.peek())) { val = val * 10 + (lx.peek() - '0'); ++lx.pos; }
        GateExpr e;
        if (val == 0)
        {
            e.kind = GateExpr::Cond;
            e.name = "__ALWAYS_FALSE__";
            e.defined = true;
        }
        else
        {
            e.kind = GateExpr::Always;
        }
        return e;
    }
    // Unknown — treat as always-false sentinel so we don't accidentally include
    // declarations under expressions we couldn't parse.
    GateExpr e;
    e.kind = GateExpr::Cond;
    e.name = "__UNKNOWN_EXPR__";
    e.defined = true;
    return e;
}

static GateExpr parseAnd(ExprLex& lx)
{
    GateExpr left = parsePrimary(lx);
    while (true)
    {
        lx.skipWs();
        if (!lx.match("&&")) break;
        GateExpr right = parsePrimary(lx);
        if (left.kind == GateExpr::And)
        {
            left.children.push_back(std::move(right));
        }
        else
        {
            GateExpr nd;
            nd.kind = GateExpr::And;
            nd.children.push_back(std::move(left));
            nd.children.push_back(std::move(right));
            left = std::move(nd);
        }
    }
    return left;
}

static GateExpr parseOr(ExprLex& lx)
{
    GateExpr left = parseAnd(lx);
    while (true)
    {
        lx.skipWs();
        if (!lx.match("||")) break;
        GateExpr right = parseAnd(lx);
        if (left.kind == GateExpr::Or)
        {
            left.children.push_back(std::move(right));
        }
        else
        {
            GateExpr nd;
            nd.kind = GateExpr::Or;
            nd.children.push_back(std::move(left));
            nd.children.push_back(std::move(right));
            left = std::move(nd);
        }
    }
    return left;
}

static GateExpr ParseIfExpression(const std::string& expr)
{
    ExprLex lx(expr);
    return parseOr(lx);
}

static GateExpr MakeDefinedCond(const std::string& name, bool defined)
{
    GateExpr e;
    e.kind = GateExpr::Cond;
    e.name = name;
    e.defined = defined;
    return e;
}

static GateExpr MakeNot(GateExpr inner)
{
    if (inner.kind == GateExpr::Not && !inner.children.empty())
        return std::move(inner.children[0]); // collapse double-negation
    GateExpr e;
    e.kind = GateExpr::Not;
    e.children.push_back(std::move(inner));
    return e;
}

static GateExpr MakeAnd(GateExpr a, GateExpr b)
{
    if (a.kind == GateExpr::Always) return b;
    if (b.kind == GateExpr::Always) return a;
    GateExpr e;
    e.kind = GateExpr::And;
    if (a.kind == GateExpr::And) e.children = std::move(a.children);
    else                         e.children.push_back(std::move(a));
    if (b.kind == GateExpr::And)
    {
        for (auto& c : b.children) e.children.push_back(std::move(c));
    }
    else
    {
        e.children.push_back(std::move(b));
    }
    return e;
}

static GateExpr MakeOr(GateExpr a, GateExpr b)
{
    if (a.kind == GateExpr::Always) return a;
    if (b.kind == GateExpr::Always) return b;
    GateExpr e;
    e.kind = GateExpr::Or;
    if (a.kind == GateExpr::Or) e.children = std::move(a.children);
    else                        e.children.push_back(std::move(a));
    if (b.kind == GateExpr::Or)
    {
        for (auto& c : b.children) e.children.push_back(std::move(c));
    }
    else
    {
        e.children.push_back(std::move(b));
    }
    return e;
}

// One #if/#elif/#else chain frame. We track the chain-wide "any prior branch
// matched" accumulator so that #elif and #else can express the proper
// "previous branches were all false AND current branch matches" semantics.
struct IfFrame
{
    GateExpr effective;     // the gate to apply to declarations under this branch
    GateExpr priorOr;       // disjunction of all branches' raw expressions seen so far
};

// Combine current stack of IfFrames into a single AND of their effectives.
static GateExpr GateFromStack(const std::vector<IfFrame>& stack)
{
    GateExpr e; e.kind = GateExpr::Always;
    for (const auto& f : stack)
        e = MakeAnd(std::move(e), f.effective);
    return e;
}

} // anonymous namespace

// ============================================================================
// Walker — top-level traversal with gate tracking
// ============================================================================

struct Walker
{
    Source src;
    std::vector<IfFrame> gateStack;     // one entry per active #if/#ifdef chain
    ShaderFile& file;
    // Resolves #define NAME VALUE pairs encountered in the file
    std::unordered_map<std::string, std::string> defines;

    Walker(const std::string& text, ShaderFile& f) : src(text), file(f) {}

    DefineGate currentGate() const
    {
        DefineGate g;
        g.expr = GateFromStack(gateStack);
        return g;
    }

    // Trim helper
    static std::string Trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
        return s.substr(a, b - a);
    }

    // Parse one preprocessor directive starting at '#' (assumed at current pos).
    void parseDirective()
    {
        // consume '#'
        src.advance();
        SkipSpaceNoNewline(src);
        std::string kw = ReadIdent(src);
        SkipSpaceNoNewline(src);
        // capture rest of line
        std::string rest;
        while (!src.eof() && src.peek() != '\n')
        {
            // Handle line-continuation: backslash + newline
            if (src.peek() == '\\' && src.pos + 1 < src.text->size() &&
                (*src.text)[src.pos + 1] == '\n')
            {
                src.advance(); src.advance();
                continue;
            }
            rest.push_back(src.peek());
            src.advance();
        }
        if (!src.eof()) src.advance(); // consume newline
        rest = Trim(rest);

        // Helper: open a new #if-chain with the given branch expression.
        auto openChain = [&](GateExpr branch)
        {
            IfFrame f;
            f.priorOr = branch;          // first branch's expression
            f.effective = std::move(branch);
            gateStack.push_back(std::move(f));
        };

        // Helper: switch to next branch (#elif EXPR or #else).
        // For #elif EXPR:  effective = !priorOr ∧ EXPR;  priorOr |= EXPR
        // For #else:       effective = !priorOr;        priorOr unchanged (closes the chain)
        auto nextBranch = [&](GateExpr branchExpr, bool isElse)
        {
            if (gateStack.empty())
            {
                // Stray #else / #elif — push synthetic always-false to be safe.
                IfFrame f;
                f.priorOr.kind = GateExpr::Always;
                f.effective = MakeDefinedCond("__STRAY_BRANCH__", true);
                gateStack.push_back(std::move(f));
                return;
            }
            IfFrame& top = gateStack.back();
            GateExpr negPrior = MakeNot(top.priorOr);
            if (isElse)
            {
                top.effective = std::move(negPrior);
                // priorOr stays the same; future #elif/#else are illegal but harmless
            }
            else
            {
                top.effective = MakeAnd(std::move(negPrior), GateExpr(branchExpr));
                top.priorOr = MakeOr(std::move(top.priorOr), std::move(branchExpr));
            }
        };

        if (kw == "ifdef")
        {
            std::string n = Trim(rest);
            size_t sp = n.find_first_of(" \t");
            if (sp != std::string::npos) n = n.substr(0, sp);
            openChain(MakeDefinedCond(n, true));
        }
        else if (kw == "ifndef")
        {
            std::string n = Trim(rest);
            size_t sp = n.find_first_of(" \t");
            if (sp != std::string::npos) n = n.substr(0, sp);
            openChain(MakeDefinedCond(n, false));
        }
        else if (kw == "if")
        {
            openChain(ParseIfExpression(Trim(rest)));
        }
        else if (kw == "elif")
        {
            nextBranch(ParseIfExpression(Trim(rest)), false);
        }
        else if (kw == "else")
        {
            nextBranch(GateExpr{}, true);
        }
        else if (kw == "endif")
        {
            if (!gateStack.empty()) gateStack.pop_back();
        }
        else if (kw == "define")
        {
            // #define NAME [VALUE...]
            std::string e = Trim(rest);
            size_t sp = e.find_first_of(" \t");
            std::string name = (sp != std::string::npos) ? e.substr(0, sp) : e;
            std::string value = (sp != std::string::npos) ? Trim(e.substr(sp + 1)) : std::string();
            if (!name.empty())
            {
                file.defines[name] = value;
                defines[name] = value;
            }
        }
        else if (kw == "undef")
        {
            std::string e = Trim(rest);
            size_t sp = e.find_first_of(" \t");
            std::string name = (sp != std::string::npos) ? e.substr(0, sp) : e;
            if (!name.empty())
            {
                file.defines.erase(name);
                defines.erase(name);
            }
        }
        // pragma / include (already inlined) / etc. — ignored
    }

    // True if identifier name should be treated as a shader entry point.
    static bool IsEntryPointName(const std::string& name)
    {
        // OGRE/DX conventions
        static const char* SUFFIXES[] = {
            "_vs", "_vp", "_ps", "_fs", "_fp",
            "_gs", "_gp", "_hs", "_ds", "_cs"
        };
        for (auto* sfx : SUFFIXES)
        {
            size_t L = strlen(sfx);
            if (name.size() > L && name.compare(name.size() - L, L, sfx) == 0)
                return true;
        }
        return false;
    }

    static ShaderStage StageFromName(const std::string& name)
    {
        if (name.size() < 3) return ShaderStage::Unknown;
        const char* s = name.c_str() + name.size() - 3;
        if (!strcmp(s, "_vs") || !strcmp(s, "_vp")) return ShaderStage::Vertex;
        if (!strcmp(s, "_ps") || !strcmp(s, "_fs") || !strcmp(s, "_fp")) return ShaderStage::Pixel;
        if (!strcmp(s, "_gs") || !strcmp(s, "_gp")) return ShaderStage::Geometry;
        if (!strcmp(s, "_hs")) return ShaderStage::Hull;
        if (!strcmp(s, "_ds")) return ShaderStage::Domain;
        if (!strcmp(s, "_cs")) return ShaderStage::Compute;
        return ShaderStage::Unknown;
    }

    // Try to parse a ': SEMANTIC' or ': register(sN)' clause from string starting at `s`.
    // Returns the consumed length and writes into outSemantic / outRegister.
    static size_t TryParseSemanticOrRegister(const std::string& s, size_t pos,
                                             std::string& outSemantic,
                                             int& outRegisterSlot,
                                             char& outRegisterKind)
    {
        size_t p = pos;
        // skip ws
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
        if (p >= s.size() || s[p] != ':') return 0;
        ++p;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
        // Either an identifier (semantic) or 'register' '(' ...
        if (p < s.size() && IsIdentStart(s[p]))
        {
            size_t s2 = p;
            while (p < s.size() && IsIdentCont(s[p])) ++p;
            std::string ident = s.substr(s2, p - s2);
            if (ident == "register")
            {
                while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
                if (p < s.size() && s[p] == '(')
                {
                    ++p;
                    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
                    if (p < s.size() &&
                        (s[p] == 's' || s[p] == 't' || s[p] == 'b' || s[p] == 'u'))
                    {
                        outRegisterKind = s[p];
                        ++p;
                        int num = 0; bool any = false;
                        while (p < s.size() && isdigit((unsigned char)s[p]))
                        { num = num * 10 + (s[p] - '0'); any = true; ++p; }
                        if (any) outRegisterSlot = num;
                    }
                    while (p < s.size() && s[p] != ')') ++p;
                    if (p < s.size() && s[p] == ')') ++p;
                }
                return p - pos;
            }
            else
            {
                outSemantic = ident;
                return p - pos;
            }
        }
        return 0;
    }

    // Parse a single parameter declaration (after splitting on top-level commas).
    // The gate is the gate active when this chunk's content STARTED (see
    // splitAndParseParams for why). Falls back to currentGate() if Always.
    void parseParameter(const std::string& raw, ShaderEntryPoint& ep,
                        const DefineGate& chunkGate)
    {
        std::string s = Trim(raw);
        if (s.empty()) return;

        // Capture per-parameter directives that may have leaked into the raw text.
        // (We've already processed directives during the gate walk, so this is
        // really about #ifdef wrapping that survived as text in the raw param.
        // In practice we split AFTER directive processing so this should be empty.)

        // Tokenize: walk identifiers / qualifiers / type / name / [ARRAY] / : SEMANTIC | : register(sN)
        bool isUniform = false;
        bool isOut = false;
        bool isIn = false;
        bool hasInOut = false;
        std::string typeRaw;
        std::string name;
        std::string semantic;
        int registerSlot = -1;
        char registerKind = 's';
        uint32_t arraySize = 0;
        bool seenType = false;

        size_t i = 0;
        const size_t N = s.size();

        // Read leading qualifiers (any combination of in/out/inout/uniform)
        while (i < N)
        {
            // skip ws
            while (i < N && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
            if (i >= N || !IsIdentStart(s[i])) break;
            size_t s2 = i;
            while (i < N && IsIdentCont(s[i])) ++i;
            std::string tok = s.substr(s2, i - s2);
            if (tok == "uniform") { isUniform = true; continue; }
            if (tok == "in")      { isIn = true; continue; }
            if (tok == "out")     { isOut = true; continue; }
            if (tok == "inout")   { hasInOut = true; continue; }
            if (tok == "const")   { /* ignore */ continue; }
            // First non-qualifier ident: this is the type.
            typeRaw = tok;
            seenType = true;
            break;
        }

        if (!seenType) return; // not a real parameter

        // Next ident (skipping ws) is the parameter name.
        while (i < N && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
        if (i < N && IsIdentStart(s[i]))
        {
            size_t s2 = i;
            while (i < N && IsIdentCont(s[i])) ++i;
            name = s.substr(s2, i - s2);
        }

        // Optional [ARRAY]
        while (i < N && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i < N && s[i] == '[')
        {
            ++i;
            // Read array dim — number, or identifier resolved via #define
            std::string dim;
            while (i < N && s[i] != ']')
            {
                dim.push_back(s[i]); ++i;
            }
            if (i < N && s[i] == ']') ++i;
            dim = Trim(dim);
            if (!dim.empty())
            {
                if (isdigit((unsigned char)dim[0]))
                {
                    arraySize = (uint32_t)atoi(dim.c_str());
                }
                else
                {
                    // Resolve via #define
                    auto it = defines.find(dim);
                    if (it != defines.end())
                        arraySize = (uint32_t)atoi(it->second.c_str());
                }
            }
        }

        // Optional : SEMANTIC or : register(sN)  (may appear once or both)
        for (int rep = 0; rep < 2; ++rep)
        {
            std::string sem; int rs = -1; char rk = 's';
            size_t consumed = TryParseSemanticOrRegister(s, i, sem, rs, rk);
            if (consumed == 0) break;
            i += consumed;
            if (!sem.empty()) semantic = sem;
            if (rs >= 0) { registerSlot = rs; registerKind = rk; }
        }

        // Trailing '= default' — ignore
        // (and anything else)

        HLSLType ht = ClassifyType(typeRaw);

        // Bucket the parameter:
        //  - Resource type (Texture/Sampler) → ShaderResource (uniform qualifier optional)
        //  - 'uniform' qualifier            → ShaderUniform
        //  - else                           → ShaderInputAttr (in or out)
        if (IsResourceType(ht))
        {
            ShaderResource r;
            r.name = name;
            r.type = ht;
            r.typeRaw = typeRaw;
            r.registerSlot = registerSlot;
            r.registerKind = registerKind;
            r.gate = chunkGate;
            ep.resources.push_back(std::move(r));
        }
        else if (isUniform)
        {
            ShaderUniform u;
            u.name = name;
            u.type = ht;
            u.typeRaw = typeRaw;
            u.arraySize = arraySize;
            u.gate = chunkGate;
            ep.uniforms.push_back(std::move(u));
        }
        else
        {
            ShaderInputAttr a;
            a.name = name;
            a.semantic = semantic;
            a.type = ht;
            a.typeRaw = typeRaw;
            a.isOutput = isOut || hasInOut;
            a.gate = chunkGate;
            if (a.isOutput) ep.outputs.push_back(std::move(a));
            else            ep.inputs.push_back(std::move(a));
        }
    }

    // Split a parameter list (raw text inside outer parens) by top-level commas,
    // honoring nesting of () [] {} and processing # directives inline so we can
    // gate per-parameter.
    //
    // Subtlety: the gate of a parameter is the one active when the parameter's
    // content STARTED accumulating, not when its trailing comma fires. Consider:
    //   ... uG2,
    //       uniform float uExposure
    //       #ifdef STARFIELD
    //           , uniform sampler2D uStarfield ...
    //       #endif
    //   )
    // The comma after uExposure is inside #ifdef STARFIELD. If we used the gate
    // at the comma to attribute uExposure, it would (wrongly) appear gated by
    // STARFIELD. Track a per-chunk start gate that's frozen once content begins.
    void splitAndParseParams(const std::string& paramText, ShaderEntryPoint& ep)
    {
        size_t startStackSize = gateStack.size();

        size_t i = 0;
        const size_t N = paramText.size();
        std::string current;
        int parenDepth = 0;
        int bracketDepth = 0;
        DefineGate chunkStartGate = currentGate();
        bool        chunkHasContent = false;

        auto flushParam = [&](const DefineGate& gateForChunk)
        {
            std::string t = Trim(current);
            current.clear();
            if (t.empty()) return;
            parseParameter(t, ep, gateForChunk);
        };

        while (i < N)
        {
            // Handle '#' directive at start-of-line (within the param list)
            bool atLine = (i == 0 || paramText[i - 1] == '\n');
            if (atLine)
            {
                size_t p = i;
                while (p < N && (paramText[p] == ' ' || paramText[p] == '\t')) ++p;
                if (p < N && paramText[p] == '#')
                {
                    Source dummy(paramText);
                    dummy.pos = p;
                    Source saved = src;
                    src = dummy;
                    parseDirective();
                    i = src.pos;
                    src = saved;
                    // Pre-content directives can refine the current chunk's
                    // start gate (e.g., #ifdef opens just before a parameter).
                    // Post-content directives are ignored — they belong to the
                    // NEXT chunk, which will pick up the gate after the comma.
                    if (!chunkHasContent)
                        chunkStartGate = currentGate();
                    continue;
                }
            }

            char c = paramText[i];
            if (c == '(') { ++parenDepth; current.push_back(c); chunkHasContent = true; ++i; continue; }
            if (c == ')') { --parenDepth; current.push_back(c); chunkHasContent = true; ++i; continue; }
            if (c == '[') { ++bracketDepth; current.push_back(c); chunkHasContent = true; ++i; continue; }
            if (c == ']') { --bracketDepth; current.push_back(c); chunkHasContent = true; ++i; continue; }
            if (c == ',' && parenDepth == 0 && bracketDepth == 0)
            {
                flushParam(chunkStartGate);
                ++i;
                chunkHasContent = false;
                chunkStartGate = currentGate();
                continue;
            }
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                chunkHasContent = true;
            current.push_back(c);
            ++i;
        }
        flushParam(chunkStartGate);

        if (gateStack.size() > startStackSize)
            gateStack.resize(startStackSize);
    }

    // Parse a cbuffer body — sequence of `[uniform] TYPE NAME[ARRAY];` statements.
    // Variables are added to file.globalUniforms. Honours nested #ifdef / #if /
    // #else / #endif using the walker's gate stack.
    void parseCBufferBody(const std::string& body)
    {
        size_t i = 0;
        const size_t N = body.size();
        size_t startStackSize = gateStack.size();

        auto skipWs = [&]() { while (i < N && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' || body[i] == '\r')) ++i; };
        auto readIdent = [&]() -> std::string
        {
            std::string out;
            while (i < N && IsIdentCont(body[i])) { out.push_back(body[i]); ++i; }
            return out;
        };

        while (i < N)
        {
            // Directive at start-of-line?
            bool atLine = (i == 0 || body[i - 1] == '\n');
            if (atLine)
            {
                size_t p = i;
                while (p < N && (body[p] == ' ' || body[p] == '\t')) ++p;
                if (p < N && body[p] == '#')
                {
                    Source dummy(body);
                    dummy.pos = p;
                    Source saved = src;
                    src = dummy;
                    parseDirective();
                    i = src.pos;
                    src = saved;
                    continue;
                }
            }

            skipWs();
            if (i >= N) break;
            if (body[i] == ';') { ++i; continue; }
            if (!IsIdentStart(body[i])) { ++i; continue; }

            // Read qualifier/type/name idents up to `;` or `[` or `=`
            std::vector<std::string> idents;
            while (i < N)
            {
                skipWs();
                if (i >= N) break;
                char c = body[i];
                if (c == ';' || c == '[' || c == '=' || c == ':') break;
                if (!IsIdentStart(c)) { ++i; continue; }
                idents.push_back(readIdent());
            }

            // Strip leading qualifiers
            size_t start = 0;
            while (start < idents.size())
            {
                const std::string& q = idents[start];
                if (q == "uniform" || q == "static" || q == "const" || q == "in") { ++start; continue; }
                break;
            }
            if (idents.size() - start < 2)
            {
                // skip to next ;
                while (i < N && body[i] != ';' && body[i] != '\n') ++i;
                if (i < N && body[i] == ';') ++i;
                continue;
            }
            std::string typeStr = idents[start];
            std::string nameStr = idents[start + 1];

            // Optional [ARRAY]
            uint32_t arraySize = 0;
            if (i < N && body[i] == '[')
            {
                ++i;
                std::string dim;
                while (i < N && body[i] != ']') { dim.push_back(body[i]); ++i; }
                if (i < N && body[i] == ']') ++i;
                // trim
                size_t a = 0, b = dim.size();
                while (a < b && (dim[a] == ' ' || dim[a] == '\t')) ++a;
                while (b > a && (dim[b - 1] == ' ' || dim[b - 1] == '\t')) --b;
                dim = dim.substr(a, b - a);
                if (!dim.empty())
                {
                    if (isdigit((unsigned char)dim[0])) arraySize = (uint32_t)atoi(dim.c_str());
                    else
                    {
                        auto it = defines.find(dim);
                        if (it != defines.end()) arraySize = (uint32_t)atoi(it->second.c_str());
                    }
                }
            }

            // skip to ';'
            while (i < N && body[i] != ';' && body[i] != '\n') ++i;
            if (i < N && body[i] == ';') ++i;

            HLSLType ht = ClassifyType(typeStr);
            if (ht != HLSLType::Unknown && ht != HLSLType::Struct)
            {
                ShaderUniform u;
                u.name = nameStr;
                u.type = ht;
                u.typeRaw = typeStr;
                u.arraySize = arraySize;
                u.gate = currentGate();
                file.globalUniforms.push_back(std::move(u));
            }
        }

        // Restore gate stack to what it was at body entry
        if (gateStack.size() > startStackSize)
            gateStack.resize(startStackSize);
    }

    // Top-level walk
    void run()
    {
        while (!src.eof())
        {
            // start of line?
            bool atLine = (src.pos == 0) || ((*src.text)[src.pos - 1] == '\n');
            char c = src.peek();

            if (atLine)
            {
                // Allow leading ws, then check for '#'
                size_t saved = src.pos;
                SkipSpaceNoNewline(src);
                if (!src.eof() && src.peek() == '#')
                {
                    parseDirective();
                    continue;
                }
                src.pos = saved;
            }

            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { src.advance(); continue; }
            if (c == ';') { src.advance(); continue; }

            // Try to read a top-level construct. Could be:
            //  - 'struct NAME { ... };'
            //  - 'cbuffer NAME ... { ... };'
            //  - 'static const TYPE NAME = VALUE;'
            //  - 'TYPE NAME : register(...)' top-level resource
            //  - function decl: '[qualifiers] TYPE NAME ( params ) [: SEMANTIC] { body }'
            //
            // Strategy: read identifiers/types until we see '(' (function), '{' (struct/cbuffer body),
            // or ';' (skip).

            size_t lineStart = src.pos;
            (void)lineStart;

            // Read a sequence of qualifier/type tokens
            std::string lastIdent;
            std::vector<std::string> idents;
            // Limit to a reasonable number of tokens (defensive)
            for (int guard = 0; guard < 32; ++guard)
            {
                SkipSpace(src);
                if (src.eof()) break;
                char p = src.peek();
                if (p == '(' || p == '{' || p == ';' || p == ':' || p == '=' || p == '[')
                    break;
                if (p == '#') break; // directive — let outer loop handle
                if (!IsIdentStart(p))
                {
                    // Unexpected — skip the char to make progress
                    src.advance();
                    continue;
                }
                std::string id = ReadIdent(src);
                idents.push_back(id);
                lastIdent = id;
            }

            if (src.eof()) break;

            char nxt = src.peek();

            // Helper: extract (type, name) from a tokenised top-level decl.
            // Strips leading qualifiers ('uniform', 'static', 'const'). Returns
            // false if the sequence doesn't look like a type+name pair.
            auto extractTypeName = [](const std::vector<std::string>& ids,
                                      std::string& outType, std::string& outName,
                                      bool& outHasUniform) -> bool
            {
                outHasUniform = false;
                size_t start = 0;
                while (start < ids.size())
                {
                    const std::string& q = ids[start];
                    if (q == "uniform") { outHasUniform = true; ++start; continue; }
                    if (q == "static" || q == "const" || q == "in") { ++start; continue; }
                    break;
                }
                if (ids.size() - start < 2) return false;
                outType = ids[start];
                outName = ids[start + 1];
                return true;
            };

            // Top-level array uniform: 'uniform float4 absorbance[2];'
            // The ident-collecting loop above stops at '[' — consume it now.
            uint32_t topArraySize = 0;
            if (nxt == '[')
            {
                src.advance();
                std::string dim;
                while (!src.eof() && src.peek() != ']') { dim.push_back(src.peek()); src.advance(); }
                if (!src.eof() && src.peek() == ']') src.advance();
                std::string trimmed;
                {
                    size_t a = 0, b = dim.size();
                    while (a < b && (dim[a] == ' ' || dim[a] == '\t')) ++a;
                    while (b > a && (dim[b - 1] == ' ' || dim[b - 1] == '\t')) --b;
                    trimmed = dim.substr(a, b - a);
                }
                if (!trimmed.empty())
                {
                    if (isdigit((unsigned char)trimmed[0]))
                        topArraySize = (uint32_t)atoi(trimmed.c_str());
                    else
                    {
                        auto it = defines.find(trimmed);
                        if (it != defines.end())
                            topArraySize = (uint32_t)atoi(it->second.c_str());
                    }
                }
                SkipSpace(src);
                nxt = src.peek();
            }

            // Bare ';' after a sequence of identifiers — a top-level uniform decl.
            //   uniform float4 viewport;
            //   uniform float4 absorbance[2];
            //   float farClip;          (legacy OGRE-fx, also valid)
            if (nxt == ';')
            {
                std::string typeStr, nameStr;
                bool hasUniform = false;
                if (extractTypeName(idents, typeStr, nameStr, hasUniform))
                {
                    HLSLType ht = ClassifyType(typeStr);
                    // Only treat as a uniform if it's a value type (not a struct
                    // we don't know about) — avoids picking up forward decls.
                    if (ht != HLSLType::Unknown && ht != HLSLType::Struct)
                    {
                        ShaderUniform u;
                        u.name = nameStr;
                        u.type = ht;
                        u.typeRaw = typeStr;
                        u.arraySize = topArraySize;
                        u.gate = currentGate();
                        file.globalUniforms.push_back(std::move(u));
                    }
                }
                src.advance();
                continue;
            }

            // struct — opaque, skip
            if (!idents.empty() && idents[0] == "struct")
            {
                while (!src.eof() && src.peek() != '{') src.advance();
                if (!src.eof() && src.peek() == '{') SkipBracedBody(src);
                while (!src.eof() && (src.peek() == ' ' || src.peek() == '\t')) src.advance();
                if (!src.eof() && src.peek() == ';') src.advance();
                continue;
            }

            // cbuffer NAME [: register(bN)] { TYPE var; ... };
            // Body variables are recorded as file globalUniforms.
            if (!idents.empty() && idents[0] == "cbuffer")
            {
                // skip past optional 'NAME : register(bN)' chunk to '{'
                while (!src.eof() && src.peek() != '{' && src.peek() != ';') src.advance();
                if (!src.eof() && src.peek() == '{')
                {
                    src.advance(); // consume '{'
                    int depth = 1;
                    std::string body;
                    while (!src.eof() && depth > 0)
                    {
                        char c = src.peek();
                        if (c == '{') ++depth;
                        else if (c == '}') { --depth; if (depth == 0) { src.advance(); break; } }
                        body.push_back(c);
                        src.advance();
                    }
                    // Parse body as a sequence of `[uniform] TYPE NAME[ARRAY];`
                    // statements. Use the existing parameter-style splitter, but
                    // semicolon-separated, gate-aware.
                    parseCBufferBody(body);
                }
                while (!src.eof() && (src.peek() == ' ' || src.peek() == '\t')) src.advance();
                if (!src.eof() && src.peek() == ';') src.advance();
                continue;
            }

            // SamplerState NAME { Filter = ...; ... };  (static sampler block)
            // Reflection sees a SamplerState binding with this name; record it.
            if (idents.size() == 2 && idents[0] == "SamplerState" && nxt == '{')
            {
                ShaderResource r;
                r.name = idents[1];
                r.type = HLSLType::SamplerState;
                r.typeRaw = "SamplerState";
                r.registerSlot = -1;
                r.gate = currentGate();
                file.globalResources.push_back(std::move(r));
                SkipBracedBody(src);
                while (!src.eof() && (src.peek() == ' ' || src.peek() == '\t')) src.advance();
                if (!src.eof() && src.peek() == ';') src.advance();
                continue;
            }

            if (nxt == '=')
            {
                // global initializer like 'static const int X = ...;'
                while (!src.eof() && src.peek() != ';') src.advance();
                if (!src.eof()) src.advance();
                continue;
            }

            // top-level resource: 'TYPE NAME : register(sN);'
            //   Texture2D overlayMap : register(s2);
            if (nxt == ':')
            {
                std::string buf;
                while (!src.eof() && src.peek() != ';' && src.peek() != '{')
                { buf.push_back(src.peek()); src.advance(); }
                if (!src.eof() && src.peek() == ';') src.advance();

                std::string typeStr, nameStr;
                bool hasUniform = false;
                if (extractTypeName(idents, typeStr, nameStr, hasUniform))
                {
                    HLSLType ht = ClassifyType(typeStr);
                    std::string sem; int rs = -1; char rk = 's';
                    size_t cursor = 0;
                    TryParseSemanticOrRegister(buf, cursor, sem, rs, rk);
                    if (IsResourceType(ht))
                    {
                        ShaderResource r;
                        r.name = nameStr;
                        r.type = ht;
                        r.typeRaw = typeStr;
                        r.registerSlot = rs;
                        r.registerKind = rk;
                        r.gate = currentGate();
                        file.globalResources.push_back(std::move(r));
                    }
                    else if (ht != HLSLType::Unknown && ht != HLSLType::Struct)
                    {
                        // Plain uniform with a register/semantic annotation
                        ShaderUniform u;
                        u.name = nameStr;
                        u.type = ht;
                        u.typeRaw = typeStr;
                        u.gate = currentGate();
                        file.globalUniforms.push_back(std::move(u));
                    }
                }
                continue;
            }

            if (nxt == '(')
            {
                // Function-like: '<rettype-ident...> NAME ( params )'
                if (idents.size() < 2)
                {
                    // Not a recognizable function decl — skip to ';' or '{'.
                    src.advance();
                    int d = 1;
                    while (!src.eof() && d > 0)
                    {
                        char cc = src.peek();
                        if (cc == '(') ++d;
                        else if (cc == ')') --d;
                        src.advance();
                    }
                    continue;
                }

                std::string fnName = idents.back();
                // type tokens are everything before fnName
                std::string typeStr;
                for (size_t k = 0; k + 1 < idents.size(); ++k)
                {
                    if (!typeStr.empty()) typeStr.push_back(' ');
                    typeStr += idents[k];
                }

                // Consume '(' and read balanced parameter text
                src.advance(); // '('
                std::string paramText = ReadBalancedAny(src, '(', ')');

                // Optional ': SEMANTIC' on function (some PS return: float4 fn(...) : COLOR)
                SkipSpace(src);
                if (!src.eof() && src.peek() == ':')
                {
                    // skip until '{' or ';'
                    while (!src.eof() && src.peek() != '{' && src.peek() != ';') src.advance();
                }

                // Now '{' (definition) or ';' (declaration)
                bool hasBody = (!src.eof() && src.peek() == '{');
                std::string bodyText;
                if (hasBody)
                {
                    // Capture body for define-reference scan
                    src.advance(); // consume '{'
                    int depth = 1;
                    while (!src.eof() && depth > 0)
                    {
                        char cc = src.peek();
                        if (cc == '{') ++depth;
                        else if (cc == '}') { --depth; if (depth == 0) { src.advance(); break; } }
                        bodyText.push_back(cc);
                        src.advance();
                    }
                }
                else if (!src.eof() && src.peek() == ';')
                {
                    src.advance();
                }

                // Only record as entry point if name suffix matches.
                if (IsEntryPointName(fnName))
                {
                    ShaderEntryPoint ep;
                    ep.name = fnName;
                    ep.stage = StageFromName(fnName);

                    splitAndParseParams(paramText, ep);
                    if (!bodyText.empty())
                        CollectReferencedDefines(bodyText, ep.bodyReferencedDefines);

                    file.entryPoints.push_back(std::move(ep));
                }
                continue;
            }

            // Anything else — advance to make progress.
            if (!src.eof()) src.advance();
        }
    }
};

// ============================================================================
// Per-file parse driver
// ============================================================================

static bool ParseOneFile(const std::string& relPath, ShaderFile& outFile)
{
    outFile.path = relPath;
    outFile.category = FirstFolderComponent(relPath);
    outFile.includes.clear();
    outFile.entryPoints.clear();
    outFile.defines.clear();

    std::string fullPath = Join(sRootDir, relPath);
    std::string raw = ReadFile(fullPath);
    if (raw.empty())
    {
        Log("ShaderSourceCatalog: empty/unreadable file %s", relPath.c_str());
        return false;
    }
    std::string stripped = StripComments(raw);

    std::unordered_set<std::string> visited;
    visited.insert(relPath);
    std::string expanded = ExpandIncludes(stripped, DirOf(relPath), visited,
                                          outFile.includes, true);

    Walker w(expanded, outFile);
    w.run();

    return true;
}

// ============================================================================
// Public API
// ============================================================================

size_t Init(const std::string& rootDir)
{
    sFilesByPath.clear();
    sBasenameToPath.clear();
    sAllPaths.clear();
    sRootDir = NormalizeSlashes(rootDir);
    if (!sRootDir.empty() && sRootDir.back() == '/') sRootDir.pop_back();

    // Sanity: directory exists
    if (GetFileAttributesA(sRootDir.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Log("ShaderSourceCatalog: root dir not found: %s", sRootDir.c_str());
        return 0;
    }

    std::vector<std::string> rels;
    EnumerateHLSLFiles(sRootDir, "", rels);
    std::sort(rels.begin(), rels.end());

    size_t parsed = 0;
    for (const auto& rel : rels)
    {
        ShaderFile f;
        if (!ParseOneFile(rel, f)) continue;
        std::string base = BasenameNoExt(rel);
        sFilesByPath.emplace(rel, std::move(f));
        sBasenameToPath[base] = rel;
        sAllPaths.push_back(rel);
        ++parsed;
    }

    LogSummary();
    return parsed;
}

void Shutdown()
{
    sFilesByPath.clear();
    sBasenameToPath.clear();
    sAllPaths.clear();
    sRootDir.clear();
}

const ShaderFile* GetFileByPath(const std::string& relativePath)
{
    auto p = NormalizeSlashes(relativePath);
    auto it = sFilesByPath.find(p);
    return (it != sFilesByPath.end()) ? &it->second : nullptr;
}

const ShaderFile* GetFileByBasename(const std::string& basenameNoExt)
{
    auto it = sBasenameToPath.find(basenameNoExt);
    if (it == sBasenameToPath.end()) return nullptr;
    return GetFileByPath(it->second);
}

const ShaderEntryPoint* FindEntryPoint(const ShaderFile* file, const std::string& entryName)
{
    if (!file) return nullptr;
    for (const auto& ep : file->entryPoints)
        if (ep.name == entryName) return &ep;
    return nullptr;
}

size_t GetFileCount() { return sAllPaths.size(); }
const std::vector<std::string>& GetAllRelativePaths() { return sAllPaths; }

// ----- Phase 1.B: variant resolution -----

static bool IsDef(const std::string& n, const std::vector<std::string>& defines)
{
    for (const auto& d : defines) if (d == n) return true;
    return false;
}

static bool EvalExpr(const GateExpr& e, const std::vector<std::string>& defines)
{
    switch (e.kind)
    {
    case GateExpr::Always:
        return true;
    case GateExpr::Cond:
    {
        if (e.name == "__UNKNOWN_EXPR__" || e.name == "__ALWAYS_FALSE__")
            return false;
        bool d = IsDef(e.name, defines);
        return e.defined ? d : !d;
    }
    case GateExpr::And:
        for (const auto& c : e.children)
            if (!EvalExpr(c, defines)) return false;
        return true;
    case GateExpr::Or:
        for (const auto& c : e.children)
            if (EvalExpr(c, defines)) return true;
        return false;
    case GateExpr::Not:
        return e.children.empty() ? true : !EvalExpr(e.children[0], defines);
    }
    return true;
}

static bool GateHolds(const DefineGate& g, const std::vector<std::string>& defines)
{
    return EvalExpr(g.expr, defines);
}

ResolvedVariant ResolveVariant(const ShaderFile* file,
                               const std::string& entryName,
                               const std::vector<std::string>& defines)
{
    ResolvedVariant rv;
    if (!file) return rv;
    const ShaderEntryPoint* ep = FindEntryPoint(file, entryName);
    if (!ep) return rv;
    rv.file = file;
    rv.entry = ep;
    // Entry-point parameters
    for (const auto& a : ep->inputs)    if (GateHolds(a.gate, defines)) rv.activeInputs.push_back(&a);
    for (const auto& a : ep->outputs)   if (GateHolds(a.gate, defines)) rv.activeOutputs.push_back(&a);
    for (const auto& u : ep->uniforms)  if (GateHolds(u.gate, defines)) rv.activeUniforms.push_back(&u);
    for (const auto& r : ep->resources) if (GateHolds(r.gate, defines)) rv.activeResources.push_back(&r);
    // File-level globals (top-level uniform decls, cbuffer body vars, top-level
    // texture/sampler bindings, SamplerState blocks). Apply to all entry points.
    for (const auto& u : file->globalUniforms)  if (GateHolds(u.gate, defines)) rv.activeUniforms.push_back(&u);
    for (const auto& r : file->globalResources) if (GateHolds(r.gate, defines)) rv.activeResources.push_back(&r);
    rv.valid = true;
    return rv;
}

// ----- Diagnostics -----

const char* TypeToString(HLSLType t)
{
    switch (t)
    {
    case HLSLType::Unknown:      return "?";
    case HLSLType::Void:         return "void";
    case HLSLType::Float:        return "float";
    case HLSLType::Float2:       return "float2";
    case HLSLType::Float3:       return "float3";
    case HLSLType::Float4:       return "float4";
    case HLSLType::Int:          return "int";
    case HLSLType::Int2:         return "int2";
    case HLSLType::Int3:         return "int3";
    case HLSLType::Int4:         return "int4";
    case HLSLType::Uint:         return "uint";
    case HLSLType::Uint2:        return "uint2";
    case HLSLType::Uint3:        return "uint3";
    case HLSLType::Uint4:        return "uint4";
    case HLSLType::Bool:         return "bool";
    case HLSLType::Float2x2:     return "float2x2";
    case HLSLType::Float3x3:     return "float3x3";
    case HLSLType::Float4x4:     return "float4x4";
    case HLSLType::Float3x4:     return "float3x4";
    case HLSLType::Float4x3:     return "float4x3";
    case HLSLType::Sampler:      return "sampler";
    case HLSLType::Sampler2D:    return "sampler2D";
    case HLSLType::Sampler3D:    return "sampler3D";
    case HLSLType::SamplerCUBE:  return "samplerCUBE";
    case HLSLType::SamplerState: return "SamplerState";
    case HLSLType::Texture2D:    return "Texture2D";
    case HLSLType::Texture3D:    return "Texture3D";
    case HLSLType::TextureCUBE:  return "TextureCUBE";
    case HLSLType::Struct:       return "struct";
    }
    return "?";
}

const char* StageToString(ShaderStage s)
{
    switch (s)
    {
    case ShaderStage::Vertex:   return "VS";
    case ShaderStage::Pixel:    return "PS";
    case ShaderStage::Geometry: return "GS";
    case ShaderStage::Hull:     return "HS";
    case ShaderStage::Domain:   return "DS";
    case ShaderStage::Compute:  return "CS";
    default:                    return "?";
    }
}

// ----- Phase 1.C: cross-validation via D3DReflect -----

#ifndef SHADER_CATALOG_STANDALONE_TEST
#include <d3dcompiler.h>
#include <d3d11shader.h>
#endif

static bool sValidationEnabled = false;

void SetValidationEnabled(bool enabled) { sValidationEnabled = enabled; }
bool IsValidationEnabled() { return sValidationEnabled; }

ValidationResult ValidateAgainstBytecode(const char* sourceName,
                                         const char* entryName,
                                         const std::vector<std::string>& defines,
                                         const void* bytecode,
                                         size_t bytecodeSize)
{
    ValidationResult vr;
    if (!sValidationEnabled) return vr;
    if (!sourceName || !entryName || !bytecode || bytecodeSize == 0)
        return vr;

#ifdef SHADER_CATALOG_STANDALONE_TEST
    // No D3DReflect in the standalone test build.
    (void)defines;
    return vr;
#else
    // 1) Find catalog entry by basename.
    std::string base = BasenameNoExt(NormalizeSlashes(sourceName));
    const ShaderFile* file = GetFileByBasename(base);
    if (!file) return vr;
    const ShaderEntryPoint* ep = FindEntryPoint(file, entryName);
    if (!ep) return vr;

    // 2) Resolve the variant.
    ResolvedVariant rv = ResolveVariant(file, entryName, defines);
    if (!rv.valid) return vr;

    // 3) Reflect the bytecode.
    ID3D11ShaderReflection* refl = nullptr;
    HRESULT hr = D3DReflect(bytecode, bytecodeSize,
                            IID_ID3D11ShaderReflection, (void**)&refl);
    if (FAILED(hr) || !refl) return vr;

    D3D11_SHADER_DESC shaderDesc;
    if (FAILED(refl->GetDesc(&shaderDesc))) { refl->Release(); return vr; }

    vr.ranReflection = true;

    // 4) Build the set of CB-variable names from reflection.
    std::vector<std::string> reflVarNames;
    for (UINT cbIdx = 0; cbIdx < shaderDesc.ConstantBuffers; ++cbIdx)
    {
        ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(cbIdx);
        if (!cb) continue;
        D3D11_SHADER_BUFFER_DESC cbDesc;
        if (FAILED(cb->GetDesc(&cbDesc))) continue;
        if (cbDesc.Type != D3D_CT_CBUFFER) continue;
        for (UINT vi = 0; vi < cbDesc.Variables; ++vi)
        {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(vi);
            if (!var) continue;
            D3D11_SHADER_VARIABLE_DESC vd;
            if (SUCCEEDED(var->GetDesc(&vd)) && vd.Name)
                reflVarNames.emplace_back(vd.Name);
        }
    }

    // 5) Build the set of bound resources. D3DCompile lowers OGRE-style
    //    `sampler2D X : register(sN)` to a Texture2D + SamplerState pair both
    //    bound as `$X`, so we strip a leading '$' for name comparison and
    //    deduplicate (texture+sampler share a source-level name).
    struct ReflBinding { std::string name; int slot; D3D_SHADER_INPUT_TYPE type; };
    auto stripDollar = [](const char* s) -> std::string
    {
        if (!s) return std::string();
        if (s[0] == '$') return std::string(s + 1);
        return std::string(s);
    };
    std::vector<ReflBinding> reflBindings;
    for (UINT i = 0; i < shaderDesc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC bd;
        if (FAILED(refl->GetResourceBindingDesc(i, &bd))) continue;
        if (bd.Type == D3D_SIT_CBUFFER) continue; // tracked above
        ReflBinding b;
        b.name = stripDollar(bd.Name);
        b.slot = (int)bd.BindPoint;
        b.type = bd.Type;
        // Dedup: a single OGRE `sampler2D X : register(sN)` produces two
        // reflection bindings (Texture2D + SamplerState), both at slot N.
        // Keep just one entry per (name, slot).
        bool dup = false;
        for (const auto& e : reflBindings)
            if (e.name == b.name && e.slot == b.slot) { dup = true; break; }
        if (!dup) reflBindings.push_back(std::move(b));
    }

    refl->Release();

    vr.catalogUniforms     = (int)rv.activeUniforms.size();
    vr.reflectionUniforms  = (int)reflVarNames.size();
    vr.catalogResources    = (int)rv.activeResources.size();
    vr.reflectionBindings  = (int)reflBindings.size();

    auto addIssue = [&](const std::string& s)
    {
        if (vr.issues.size() < 16) vr.issues.push_back(s);
    };

    // 6) Compare uniforms by name (offset comparison left for a future pass —
    //    the stable signal is "name presence"; offsets depend on packing rules
    //    that vary with compiler version and unused-var stripping).
    {
        std::unordered_set<std::string> reflSet(reflVarNames.begin(), reflVarNames.end());
        std::unordered_set<std::string> catSet;
        for (auto* u : rv.activeUniforms) catSet.insert(u->name);

        for (auto* u : rv.activeUniforms)
        {
            if (reflSet.count(u->name)) ++vr.uniformsMatched;
            else ++vr.uniformsCatalogOnly;
        }
        for (const auto& n : reflVarNames)
        {
            if (!catSet.count(n))
            {
                ++vr.uniformsReflectionOnly;
                addIssue(std::string("uniform reflection-only: ") + n);
            }
        }
    }

    // 7) Compare resources by name + slot.
    {
        std::unordered_set<std::string> reflSet;
        for (const auto& b : reflBindings) reflSet.insert(b.name);
        std::unordered_set<std::string> catSet;
        for (auto* r : rv.activeResources) catSet.insert(r->name);

        for (auto* r : rv.activeResources)
        {
            auto it = std::find_if(reflBindings.begin(), reflBindings.end(),
                [&](const ReflBinding& b) { return b.name == r->name; });
            if (it == reflBindings.end())
            {
                ++vr.resourcesCatalogOnly;
                continue;
            }
            ++vr.resourcesMatched;
            if (r->registerSlot >= 0 && it->slot != r->registerSlot)
            {
                char buf[160];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "resource '%s' slot mismatch: catalog s%d vs reflection s%d",
                    r->name.c_str(), r->registerSlot, it->slot);
                addIssue(buf);
            }
        }
        for (const auto& b : reflBindings)
        {
            if (!catSet.count(b.name))
            {
                ++vr.resourcesReflectionOnly;
                addIssue(std::string("resource reflection-only: ") + b.name +
                         " @s" + std::to_string(b.slot));
            }
        }
    }

    // 8) Log a one-line summary, plus issues if any.
    bool clean = (vr.uniformsReflectionOnly == 0) &&
                 (vr.resourcesReflectionOnly == 0);
    Log("ShaderCatalog xval: %s/%s defs=%zu  uni m=%d c=%d r=%d  res m=%d c=%d r=%d  %s",
        base.c_str(), entryName, defines.size(),
        vr.uniformsMatched, vr.uniformsCatalogOnly, vr.uniformsReflectionOnly,
        vr.resourcesMatched, vr.resourcesCatalogOnly, vr.resourcesReflectionOnly,
        clean ? "OK" : "MISMATCH");

    if (!clean)
    {
        for (const auto& s : vr.issues)
            Log("  ! %s", s.c_str());
    }

    return vr;
#endif
}

void LogSummary()
{
    size_t totalEntries = 0, totalUniforms = 0, totalResources = 0, totalInputs = 0, totalOutputs = 0;
    for (const auto& kv : sFilesByPath)
    {
        const ShaderFile& f = kv.second;
        for (const auto& ep : f.entryPoints)
        {
            ++totalEntries;
            totalUniforms  += ep.uniforms.size();
            totalResources += ep.resources.size();
            totalInputs    += ep.inputs.size();
            totalOutputs   += ep.outputs.size();
        }
    }
    Log("ShaderSourceCatalog: parsed %zu files, %zu entry points "
        "(uniforms=%zu, resources=%zu, inputs=%zu, outputs=%zu)",
        sFilesByPath.size(), totalEntries, totalUniforms, totalResources,
        totalInputs, totalOutputs);

    // Per-file detail at low log volume — useful first time, can be gated later.
    for (const auto& rel : sAllPaths)
    {
        const ShaderFile* f = GetFileByPath(rel);
        if (!f) continue;
        if (f->entryPoints.empty()) continue;
        std::string eps;
        for (const auto& ep : f->entryPoints)
        {
            if (!eps.empty()) eps += ", ";
            char buf[128];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "%s[%s u=%zu r=%zu i=%zu o=%zu]",
                        ep.name.c_str(), StageToString(ep.stage),
                        ep.uniforms.size(), ep.resources.size(),
                        ep.inputs.size(), ep.outputs.size());
            eps += buf;
        }
        Log("  %s: %s", f->path.c_str(), eps.c_str());
    }
}

} // namespace ShaderSourceCatalog
