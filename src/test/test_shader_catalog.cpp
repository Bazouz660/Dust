// Standalone test driver for ShaderSourceCatalog.
// Build with cl.exe, link only against kernel32. Bypasses DustLog via the
// SHADER_CATALOG_STANDALONE_TEST define.
//
//   cl /nologo /EHsc /std:c++17 /DSHADER_CATALOG_STANDALONE_TEST
//      test\test_shader_catalog.cpp ShaderSourceCatalog.cpp
//      /Fe:test_catalog.exe kernel32.lib

#include <cstdio>
#include <cstring>
#include "../ShaderSourceCatalog.h"

namespace SC = ShaderSourceCatalog;

static void DumpGate(const SC::GateExpr& e, std::string& out)
{
    using K = SC::GateExpr;
    switch (e.kind)
    {
    case K::Always: out += "1"; return;
    case K::Cond:
        if (!e.defined) out += "!";
        out += "defined("; out += e.name; out += ")"; return;
    case K::And:
    {
        out += "(";
        for (size_t i = 0; i < e.children.size(); ++i)
        {
            if (i) out += " && ";
            DumpGate(e.children[i], out);
        }
        out += ")"; return;
    }
    case K::Or:
    {
        out += "(";
        for (size_t i = 0; i < e.children.size(); ++i)
        {
            if (i) out += " || ";
            DumpGate(e.children[i], out);
        }
        out += ")"; return;
    }
    case K::Not:
        out += "!";
        if (!e.children.empty()) DumpGate(e.children[0], out);
        return;
    }
}

static std::string GateString(const SC::DefineGate& g)
{
    std::string s;
    DumpGate(g.expr, s);
    return s;
}

int main(int argc, char** argv)
{
    const char* root = (argc > 1) ? argv[1] : "vanilla_shaders";
    fprintf(stderr, "Test: parsing %s\n", root);
    size_t n = SC::Init(root);
    fprintf(stderr, "Test: parsed %zu files\n", n);
    if (n == 0) return 1;

    // Detail dump for selected files
    const char* targets[] = {
        "deferred/objects.hlsl",
        "deferred/skin.hlsl",
        "deferred/deferred.hlsl",
        "deferred/foliage.hlsl",
        "deferred/terrain.hlsl",
    };

    for (const char* tgt : targets)
    {
        const SC::ShaderFile* f = SC::GetFileByPath(tgt);
        if (!f) { printf("\n=== %s NOT FOUND ===\n", tgt); continue; }
        printf("\n=== %s ===\n", tgt);
        printf("  category: %s\n", f->category.c_str());
        printf("  includes: ");
        for (auto& inc : f->includes) printf("%s ", inc.c_str());
        printf("\n");
        printf("  defines: ");
        for (auto& kv : f->defines) printf("%s=%s ", kv.first.c_str(), kv.second.c_str());
        printf("\n");

        for (const auto& ep : f->entryPoints)
        {
            printf("  entry [%s] %s:\n", SC::StageToString(ep.stage), ep.name.c_str());

            if (!ep.bodyReferencedDefines.empty())
            {
                printf("    body-defines:");
                for (auto& d : ep.bodyReferencedDefines) printf(" %s", d.c_str());
                printf("\n");
            }

            for (const auto& a : ep.inputs)
            {
                printf("    in  %-12s %-20s : %-12s %s\n",
                       a.typeRaw.c_str(), a.name.c_str(),
                       a.semantic.empty() ? "-" : a.semantic.c_str(),
                       a.gate.empty() ? "" : GateString(a.gate).c_str());
            }
            for (const auto& a : ep.outputs)
            {
                printf("    out %-12s %-20s : %-12s %s\n",
                       a.typeRaw.c_str(), a.name.c_str(),
                       a.semantic.empty() ? "-" : a.semantic.c_str(),
                       a.gate.empty() ? "" : GateString(a.gate).c_str());
            }
            for (const auto& u : ep.uniforms)
            {
                printf("    UNI %-12s %-20s%s %s\n",
                       u.typeRaw.c_str(), u.name.c_str(),
                       u.arraySize ? "[ARR]" : "     ",
                       u.gate.empty() ? "" : GateString(u.gate).c_str());
                if (u.arraySize)
                    printf("      arraySize=%u\n", u.arraySize);
            }
            for (const auto& r : ep.resources)
            {
                printf("    RES %-12s %-20s : reg %c%-2d %s\n",
                       r.typeRaw.c_str(), r.name.c_str(),
                       r.registerKind, r.registerSlot,
                       r.gate.empty() ? "" : GateString(r.gate).c_str());
            }
        }
    }

    // Phase 1.B sanity: resolve a few variants and dump the active-only set.
    {
        printf("\n--- VARIANT RESOLUTION CHECKS ---\n");
        const SC::ShaderFile* obj = SC::GetFileByPath("deferred/objects.hlsl");
        if (obj)
        {
            std::vector<std::string> none;
            std::vector<std::string> dust = {"DUST"};
            std::vector<std::string> dustInterior = {"DUST", "INTERIOR"};
            std::vector<std::string> dual = {"DUAL_TEXTURE"};
            std::vector<std::string> dualConstr = {"DUAL_TEXTURE", "CONSTRUCTION"};

            auto dump = [](const SC::ResolvedVariant& rv, const char* label)
            {
                if (!rv.valid) { printf("  [%s] INVALID\n", label); return; }
                printf("  [%s] uniforms=%zu resources=%zu inputs=%zu outputs=%zu\n",
                       label, rv.activeUniforms.size(), rv.activeResources.size(),
                       rv.activeInputs.size(), rv.activeOutputs.size());
                for (auto* r : rv.activeResources)
                    printf("        RES %s @s%d\n", r->name.c_str(), r->registerSlot);
            };

            dump(SC::ResolveVariant(obj, "main_ps", none),         "objects.main_ps()");
            dump(SC::ResolveVariant(obj, "main_ps", dust),         "objects.main_ps(DUST)");
            dump(SC::ResolveVariant(obj, "main_ps", dustInterior), "objects.main_ps(DUST,INTERIOR)");
            dump(SC::ResolveVariant(obj, "main_ps", dual),         "objects.main_ps(DUAL_TEXTURE)");
            dump(SC::ResolveVariant(obj, "main_ps", dualConstr),   "objects.main_ps(DUAL_TEXTURE,CONSTRUCTION)");
        }

        const SC::ShaderFile* skin = SC::GetFileByPath("deferred/skin.hlsl");
        if (skin)
        {
            std::vector<std::string> none;
            std::vector<std::string> rtw = {"RTW"};
            std::vector<std::string> blood = {"BLOOD"};

            auto dump = [](const SC::ResolvedVariant& rv, const char* label)
            {
                if (!rv.valid) { printf("  [%s] INVALID\n", label); return; }
                printf("  [%s] uniforms=%zu resources=%zu inputs=%zu outputs=%zu\n",
                       label, rv.activeUniforms.size(), rv.activeResources.size(),
                       rv.activeInputs.size(), rv.activeOutputs.size());
            };

            dump(SC::ResolveVariant(skin, "main_vs", none),  "skin.main_vs()");
            dump(SC::ResolveVariant(skin, "main_vs", blood), "skin.main_vs(BLOOD)");
            dump(SC::ResolveVariant(skin, "shadow_vs", none),"skin.shadow_vs()");
            dump(SC::ResolveVariant(skin, "shadow_vs", rtw), "skin.shadow_vs(RTW)");
        }
    }

    SC::Shutdown();
    return 0;
}
