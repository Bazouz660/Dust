#include "EffectShaderProbe.h"
#include "../effects/rtgi/RTGITemporal.h"
#include <algorithm>

static void Camera(float* m, float x, float yaw = 0)
{
    std::fill(m, m+16, 0.f);
    m[0] = m[10] = std::cos(yaw); m[2] = std::sin(yaw); m[8] = -m[2];
    m[5] = m[15] = 1; m[12] = x; m[13] = 4000; m[14] = 19000;
}

struct TemporalCB {
    float size[2] = {1920,1080}, invSize[2] = {1.f/24,1.f/16};
    float fov = .5f, aspect = 1920.f/1080, blend = .9f, frame = 3;
    float matrix[16] = {}, reserved[4] = {};
};
static_assert(sizeof(TemporalCB) == 112);
static std::vector<TemporalCB> temporalUploads;
static std::vector<float> bounceUploads;

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // Quantized bases at large world coordinates must still produce identity
    // for a stationary camera. Moving +X maps right; moving +Z maps backwards
    // in the temporal shader's left-handed space.
    for (float yaw : {0.f,.028f,.7f,1.5f}) {
        float cur[16], prev[16], m[16]; Camera(cur, 85000, yaw);
        assert(RTGITemporal::Reprojection(cur,cur,2500,m));
        for (int i = 0; i < 16; ++i) assert(std::abs(m[i] - (i%5 == 0 ? 1.f : 0.f)) < 1e-6);
        std::memcpy(prev,cur,sizeof(cur)); cur[12] += .5f;
        assert(RTGITemporal::Reprojection(cur,prev,2500,m));
        assert(std::abs(m[12] - .5f*std::cos(yaw)/2500) < 1e-8);
        assert(std::abs(m[14] - .5f*std::sin(yaw)/2500) < 1e-8);
    }
    float cur[16], prev[16], m[16]; Camera(prev,0); Camera(cur,0);
    cur[14] += 2;
    assert(RTGITemporal::Reprojection(cur,prev,2000,m));
    assert(std::abs(m[14] + .001f) < 1e-8);
    // Independent world-point projection checks rotation and translation together.
    for (float yaw : {-.7f,.2f,1.2f}) {
        Camera(prev,85000,.3f); Camera(cur,85002,yaw);
        assert(RTGITemporal::Reprojection(cur,prev,2000,m));
        const double point[] = {.04,.02,.08};
        const double rh[] = {point[0]*2000,point[1]*2000,-point[2]*2000};
        double world[3] = {};
        for (int c = 0; c < 3; ++c) {
            world[c] = double(cur[12+c])-prev[12+c];
            for (int k = 0; k < 3; ++k) world[c] += rh[k]*cur[k*4+c];
        }
        for (int c = 0; c < 3; ++c) {
            double expected = 0, actual = m[12+c];
            for (int k = 0; k < 3; ++k) {
                expected += world[k]*prev[c*4+k];
                actual += point[k]*m[k*4+c];
            }
            expected *= (c == 2 ? -1. : 1.)/2000;
            assert(std::abs(actual-expected) < 1e-7);
        }
    }
    assert(!RTGITemporal::Reprojection(cur,prev,0,m));
    float singular[16] = {};
    assert(!RTGITemporal::Reprojection(cur,singular,2000,m));

    EffectShaderProbe p("rtgi","DustRTGI.dll");
    // Execute the real temporal PS with controlled radiance and history. A tiny
    // world-space camera move must not suddenly replace 80% of the image.
    ComPtr<ID3DBlob> psCode; psCode.Attach(EffectShaderProbe::Compile("rtgi_temporal_ps.hlsl","main","ps_5_0"));
    ComPtr<ID3DBlob> vsCode; vsCode.Attach(EffectShaderProbe::Compile("fullscreen_vs.hlsl","main","vs_5_0"));
    ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11VertexShader> vs;
    assert(SUCCEEDED(p.device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps)));
    assert(SUCCEEDED(p.device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)));
    ComPtr<ID3D11Buffer> buffer; buffer.Attach(p.host.CreateConstantBuffer(p.device.Get(),sizeof(TemporalCB)));
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler; assert(SUCCEEDED(p.device->CreateSamplerState(&sd,&sampler)));
    std::vector<Pixel> current(p.W*p.H,Pixel{.8f,.8f,.8f,.8f});
    p.ctx->UpdateSubresource(p.scene.Get(),0,nullptr,current.data(),p.W*sizeof(Pixel),0);
    p.SetNormals(std::vector<Pixel>(p.W*p.H,Pixel{.2f,.2f,.2f,.2f})); // history
    std::vector<float> depths(p.W*p.H,.1f);
    p.ctx->UpdateSubresource(p.depth.Get(),0,nullptr,depths.data(),p.W*sizeof(float),0);
    auto draw = [&](const TemporalCB& cb) {
        p.ctx->ClearState();
        p.host.UpdateConstantBuffer(p.ctx.Get(),buffer.Get(),&cb,sizeof(cb));
        p.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        p.ctx->VSSetShader(vs.Get(),nullptr,0); p.ctx->PSSetShader(ps.Get(),nullptr,0);
        auto b = buffer.Get(); p.ctx->PSSetConstantBuffers(0,1,&b);
        ID3D11ShaderResourceView* views[] = {p.sceneSRV.Get(),p.normalsSRV.Get(),p.depthSRV.Get(),p.depthSRV.Get()};
        p.ctx->PSSetShaderResources(0,4,views); auto sam = sampler.Get(); p.ctx->PSSetSamplers(0,1,&sam);
        auto rt = p.outputRTV.Get(); p.ctx->OMSetRenderTargets(1,&rt,nullptr);
        D3D11_VIEWPORT vp = {0,0,float(p.W),float(p.H),0,1}; p.ctx->RSSetViewports(1,&vp);
        p.ctx->Draw(3,0); p.ctx->CopyResource(p.staging.Get(),p.output.Get());
        D3D11_MAPPED_SUBRESOURCE map = {}; assert(SUCCEEDED(p.ctx->Map(p.staging.Get(),0,D3D11_MAP_READ,0,&map)));
        std::vector<Pixel> result(p.W*p.H);
        for (UINT y = 0; y < p.H; ++y) std::memcpy(result.data()+y*p.W,(char*)map.pData+y*map.RowPitch,p.W*sizeof(Pixel));
        p.ctx->Unmap(p.staging.Get(),0); return result;
    };
    for (float dx : {0.f,.02f,.5f,2.f}) {
        TemporalCB cb; Camera(prev,20000); Camera(cur,20000+dx);
        assert(RTGITemporal::Reprojection(cur,prev,2000,cb.matrix));
        auto result = draw(cb);
        // Independent projection of a world point: normalized ray * depth * farClip.
        for (UINT y = 0; y < p.H; ++y) for (UINT x = 0; x < p.W; ++x) {
            double rx = (2*(x+.5)/p.W-1)*cb.aspect*cb.fov;
            double ry = (1-2*(y+.5)/p.H)*cb.fov;
            double z = .1*2000/std::sqrt(rx*rx+ry*ry+1);
            double pixels = (double(cur[12])-prev[12])/z/cb.aspect/cb.fov*.5*cb.size[0];
            double dynamic = std::clamp((pixels-1.5)*.2,0.,1.);
            float alpha = float(.1+.7*dynamic), aoAlpha = float(.1+.8*dynamic);
            for (int c = 0; c < 4; ++c)
                assert(std::abs(result[y*p.W+x][c] - (.2f+.6f*(c==3 ? aoAlpha : alpha))) < 3e-5);
        }
    }
    TemporalCB flush; flush.blend = 0;
    AssertImagesNear(draw(flush),current);

    // Observe uploads from the real renderer/DLL. No history (including bounce)
    // until a successful consecutive frame, and none after skipped work/reset.
    p.host.UpdateConstantBuffer = [](ID3D11DeviceContext* ctx,ID3D11Buffer* cb,const void* data,uint32_t bytes) {
        if (bytes == sizeof(TemporalCB)) temporalUploads.push_back(*static_cast<const TemporalCB*>(data));
        if (bytes == 128) bounceUploads.push_back(static_cast<const float*>(data)[10]);
        D3D11_MAPPED_SUBRESOURCE map = {}; assert(SUCCEEDED(ctx->Map(cb,0,D3D11_MAP_WRITE_DISCARD,0,&map)));
        std::memcpy(map.pData,data,bytes); ctx->Unmap(cb,0);
    };
    p.host.DrawFullscreenTriangle = [](ID3D11DeviceContext* ctx,ID3D11PixelShader* shader) {
        ctx->PSSetShader(shader,nullptr,0); ctx->Draw(3,0);
    };
    p.host.SetLightVolumeAoTexture = [](ID3D11ShaderResourceView*) {};
    p.SetNormals(std::vector<Pixel>(p.W*p.H,Pixel{.5f,.5f,0,1}));
    DustFrameContext frame = {}; frame.device = p.device.Get(); frame.context = p.ctx.Get();
    frame.width = p.W; frame.height = p.H; frame.point = DUST_INJECT_POST_LIGHTING;
    frame.camera.valid = 1; frame.camera.farZ = 2000; frame.camera.tanHalfFov = .5f;
    Camera(frame.camera.inverseView,20000);
    auto render = [&](uint64_t index,bool history) {
        p.ctx->ClearState(); temporalUploads.clear(); bounceUploads.clear(); frame.frameIndex = index;
        frame.timing = DUST_TIMING_PRE; p.effect.preExecute(&frame,&p.host);
        frame.timing = DUST_TIMING_POST; p.effect.postExecute(&frame,&p.host);
        assert(temporalUploads.size() == 1 && bounceUploads.size() == 1);
        assert((temporalUploads[0].blend > 0) == history);
        assert((bounceUploads[0] > 0) == history);
    };
    render(100,false); render(101,true);
    p.Setting<bool>("Enabled") = false;
    frame.frameIndex = 102; p.effect.preExecute(&frame,&p.host); p.effect.postExecute(&frame,&p.host);
    p.Setting<bool>("Enabled") = true;
    render(103,false); render(104,true);
    p.hasDepth = false; frame.frameIndex = 105; p.effect.preExecute(&frame,&p.host); p.effect.postExecute(&frame,&p.host);
    p.hasDepth = true; render(106,false); render(107,true);
    frame.camera.valid = 0; render(108,false);
    frame.camera.valid = 1; render(109,false); render(110,true);
    frame.camera.farZ = 3000; render(111,false); render(112,true);
    frame.camera.tanHalfFov = .7f; render(113,false); render(114,true);
    p.Setting<int>("ResolutionScale") = 100; render(115,false); render(116,true);
    p.Setting<float>("GIIntensity") = p.Setting<float>("AOIntensity") = 0;
    frame.frameIndex = 117; p.effect.preExecute(&frame,&p.host); p.effect.postExecute(&frame,&p.host);
    p.Setting<float>("GIIntensity") = 3; p.Setting<float>("AOIntensity") = 1;
    render(118,false); render(119,true);
    std::puts("RTGI world/depth units, handedness, large-coordinate stability, GPU blending and history/bounce resets passed");
}
