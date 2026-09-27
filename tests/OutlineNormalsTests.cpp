#include "EffectShaderProbe.h"
#include <limits>

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
    // NaN and +/-Inf normals leave flat surfaces unoutlined and never reach the output.
    const float inf = std::numeric_limits<float>::infinity();
    normals.assign(p.W*p.H, Pixel{.5f,.5f,.65f,1});
    normals[p.W+1] = {std::numeric_limits<float>::quiet_NaN(),.5f,.65f,1};
    normals[2*p.W+3] = {.5f,inf,.65f,1};
    normals[3*p.W+5] = {.5f,.5f,-inf,1};
    p.SetNormals(normals); p.Setting<bool>("DebugView") = false;
    AssertImagesNear(p.Render(scene,.05f), scene);
    p.Setting<bool>("DebugView") = true; // the checks below expect the debug view
    AssertImagesNear(p.Render(scene,.05f), black);
    p.hasNormals = false;
    AssertImagesNear(p.Render(scene,.05f), black);
    std::puts("Short/variable/invalid/non-finite normals preserve flat surfaces; real creases remain outlined in both paths");
}
