#include "EffectShaderProbe.h"

int main() {
    EffectShaderProbe p("outline", "DustOutline.dll");
    const std::vector<Pixel> scene(p.W*p.H, Pixel{.7f,.6f,.5f,1});
    const std::vector<Pixel> black(p.W*p.H, Pixel{0,0,0,1});
    // Skinned shaders can store non-unit normals. A constant short normal
    // is a flat surface, not a normal discontinuity covering the entire mesh.
    std::vector<Pixel> normals(p.W*p.H, Pixel{.5f,.5f,.65f,1});
    p.SetNormals(normals);
    p.Setting<bool>("DebugView") = true;
    AssertImagesNear(p.Render(scene,.05f), black);
    p.Setting<bool>("DebugView") = false;
    AssertImagesNear(p.Render(scene,.05f), scene);
    // Vary length alone: the angle remains constant, including 8-bit encoding.
    for (UINT y = 0; y < p.H; ++y) for (UINT x = 0; x < p.W; ++x)
        normals[y*p.W+x] = {.5f,.5f,(x%2 ? 166.f : 230.f)/255.f,1};
    p.SetNormals(normals); p.Setting<bool>("DebugView") = true;
    AssertImagesNear(p.Render(scene,.05f), black);
    // A real 90-degree crease still draws one-pixel green edges.
    for (UINT y = 0; y < p.H; ++y) for (UINT x = 0; x < p.W; ++x)
        normals[y*p.W+x] = x < p.W/2 ? Pixel{.5f,.5f,.65f,1} : Pixel{.9f,.5f,.5f,1};
    p.SetNormals(normals);
    auto expected = black;
    for (UINT y = 0; y < p.H; ++y) expected[y*p.W+p.W/2-1][1] = 1;
    AssertImagesNear(p.Render(scene,.05f), expected);
    p.Setting<bool>("DebugView") = false;
    expected = scene;
    for (UINT y = 0; y < p.H; ++y) for (int c = 0; c < 3; ++c)
        expected[y*p.W+p.W/2-1][c] *= 1 - p.Setting<float>("Strength");
    AssertImagesNear(p.Render(scene,.05f), expected);
    p.Setting<bool>("DebugView") = true;
    AssertImagesNear(p.Render(scene,.6f), black); // debug obeys distance exclusion
    p.SetNormals(std::vector<Pixel>(p.W*p.H, Pixel{128.f/255,128.f/255,128.f/255,1}));
    AssertImagesNear(p.Render(scene,.05f), black); // neutral/degenerate GBuffer normals
    p.hasNormals = false;
    AssertImagesNear(p.Render(scene,.05f), black);
    std::puts("Short/variable/invalid normals preserve flat surfaces; real creases remain outlined in both paths");
}
