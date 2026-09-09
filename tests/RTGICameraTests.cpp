#include "EffectShaderProbe.h"
#include "../effects/rtgi/RTGICamera.h"
#include "../effects/rtgi/RTGITemporal.h"

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    EffectShaderProbe p("rtgi","DustRTGI.dll");
    RTGICamera camera;
    const char* source = R"(
        #include "rtgi_camera.hlsl"
        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            float2 previousUV; float depth;
            bool valid = RTGIPreviousPosition(uv,.1,.5,1.5,previousUV,depth);
            return float4(previousUV,depth,valid ? 1 : 0);
        })";
    const auto path = std::filesystem::absolute("../effects/rtgi/shaders/camera_probe.hlsl").string();
    ComPtr<ID3DBlob> code,errors;
    HRESULT hr = D3DCompile(source,std::strlen(source),path.c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if (errors) std::fprintf(stderr,"%s",(const char*)errors->GetBufferPointer());
    assert(SUCCEEDED(hr));
    ComPtr<ID3D11PixelShader> ps; assert(SUCCEEDED(p.device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps)));
    code.Attach(EffectShaderProbe::Compile("fullscreen_vs.hlsl","main","vs_5_0"));
    ComPtr<ID3D11VertexShader> vs; assert(SUCCEEDED(p.device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs)));
    ComPtr<ID3D11Buffer> gameCB,flagsCB;
    gameCB.Attach(p.host.CreateConstantBuffer(p.device.Get(),192));
    flagsCB.Attach(p.host.CreateConstantBuffer(p.device.Get(),80));
    float flags[20] = {}; flags[18] = flags[19] = 1;
    p.host.UpdateConstantBuffer(p.ctx.Get(),flagsCB.Get(),flags,sizeof(flags));
    std::vector<ComPtr<ID3D11Texture2D>> results;
    std::vector<std::array<float,16>> matrices;
    float previous[16] = {};
    // Enqueue all snapshots, draws and copies before doing ANY readback. A late
    // GPU must see the right camera pair even after the CPU has submitted 32 frames.
    for (uint64_t frame = 0; frame < 32; ++frame) {
        float raw[48] = {}; raw[8] = 2000;
        float* pose = raw+32; float angle = float(frame)*.007f;
        pose[0] = pose[10] = std::cos(angle); pose[2] = std::sin(angle); pose[8] = -pose[2];
        pose[5] = pose[15] = 1; pose[12] = 85000+float(frame)*2; pose[13] = 4000; pose[14] = 19000;
        p.host.UpdateConstantBuffer(p.ctx.Get(),gameCB.Get(),raw,sizeof(raw));
        auto game = gameCB.Get(); p.ctx->PSSetConstantBuffers(0,1,&game);
        camera.Capture(p.ctx.Get(),frame);
        assert(camera.CurrentReady(frame)); assert(camera.HistoryReady(frame) == (frame > 0));
        if (frame) {
            std::array<float,16> matrix;
            assert(RTGITemporal::Reprojection(pose,previous,2000,matrix.data())); matrices.push_back(matrix);
            p.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            p.ctx->VSSetShader(vs.Get(),nullptr,0); p.ctx->PSSetShader(ps.Get(),nullptr,0);
            ID3D11Buffer* buffers[] = {camera.Current(),camera.Previous(),flagsCB.Get()};
            p.ctx->PSSetConstantBuffers(1,3,buffers);
            D3D11_VIEWPORT viewport = {0,0,float(p.W),float(p.H),0,1}; p.ctx->RSSetViewports(1,&viewport);
            auto rtv = p.outputRTV.Get(); p.ctx->OMSetRenderTargets(1,&rtv,nullptr); p.ctx->Draw(3,0);
            D3D11_TEXTURE2D_DESC td; p.staging->GetDesc(&td);
            ComPtr<ID3D11Texture2D> readback; assert(SUCCEEDED(p.device->CreateTexture2D(&td,nullptr,&readback)));
            p.ctx->CopyResource(readback.Get(),p.output.Get()); results.push_back(readback);
        }
        std::memcpy(previous,pose,sizeof(previous)); camera.Commit(frame);
    }
    for (size_t frame = 0; frame < results.size(); ++frame) {
        D3D11_MAPPED_SUBRESOURCE map = {}; assert(SUCCEEDED(p.ctx->Map(results[frame].Get(),0,D3D11_MAP_READ,0,&map)));
        const auto& matrix = matrices[frame];
        for (UINT y = 0; y < p.H; ++y) for (UINT x = 0; x < p.W; ++x) {
            double ray[] = {(2*(x+.5)/p.W-1)*.75,(1-2*(y+.5)/p.H)*.5,1};
            double length = std::sqrt(ray[0]*ray[0]+ray[1]*ray[1]+1);
            double prev[3] = {};
            for (int c = 0; c < 3; ++c) {
                prev[c] = matrix[12+c];
                for (int k = 0; k < 3; ++k) prev[c] += ray[k]/length*.1*matrix[k*4+c];
            }
            double u = prev[0]/prev[2]/.75*.5+.5, v = .5-prev[1]/prev[2]/.5*.5;
            Pixel expected = {float(u),float(v),float(std::sqrt(prev[0]*prev[0]+prev[1]*prev[1]+prev[2]*prev[2])),float(u>=0 && u<=1 && v>=0 && v<=1)};
            const auto& actual = ((Pixel*)((char*)map.pData+y*map.RowPitch))[x];
            for (int c = 0; c < 4; ++c) assert(std::abs(actual[c]-expected[c]) < 1e-5);
        }
        p.ctx->Unmap(results[frame].Get(),0);
    }
    camera.Reset();
    std::puts("32 queued GPU camera frames retain matching poses and agree with double-precision reprojection");
}
