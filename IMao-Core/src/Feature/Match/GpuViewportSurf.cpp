#include "GpuViewportSurf.h"
#include "D3DComputeBuffer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

using D3DCompute::Check;
using Microsoft::WRL::ComPtr;
namespace {
// Each group checks one 8x8 tile in one of the three interior scale layers.
// Its halo includes the adjacent scales, so only strict maxima travel back.
constexpr char source[] = R"(
cbuffer Params : register(b0) {
    uint stride, height, step, capacity;
    int4 offsets[50];
    float4 weights[13];
};
Buffer<int> integral : register(t0);
RWBuffer<uint> result : register(u0);
groupshared float det[300];
groupshared int signs[100];

float term(uint f, uint base) {
    int4 p = offsets[f] + (int)base;
    int area = integral[p.x] + integral[p.w] - integral[p.y] - integral[p.z];
    return (float)area * weights[f / 4][f % 4];
}
[numthreads(8,8,1)]
void main(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID, uint thread : SV_GroupIndex) {
    int2 start = int2(group.xy * 8);
    uint middle = group.z + 1;
    for (uint i = thread; i < 300; i += 64) {
        uint plane = i / 100, pos = i % 100, layer = group.z + plane;
        uint size = (9 + 6 * layer) * step;
        int2 xy = start + int2(pos % 10, pos / 10) - 1;
        int2 corner = (xy - int((size / 2) / step)) * int(step);
        float value = 0;
        int signValue = 0;
        if (all(corner >= 0) && corner.x + int(size) < int(stride) && corner.y + int(size) <= int(height)) {
            uint base = corner.x + corner.y * stride;
            uint f = layer * 10;
            precise double x = (double)term(f,base) + (double)term(f+1,base) + (double)term(f+2,base);
            precise double y = (double)term(f+3,base) + (double)term(f+4,base) + (double)term(f+5,base);
            precise double z = (double)term(f+6,base) + (double)term(f+7,base) + (double)term(f+8,base) + (double)term(f+9,base);
            precise float dx = (float)x, dy = (float)y, dz = (float)z;
            precise float determinant = dx*dy - 0.81f*dz*dz;
            value = determinant;
            float trace = dx + dy;
            signValue = (int)(trace > 0) - (int)(trace < 0);
        }
        det[i] = value;
        if (plane == 1) signs[pos] = signValue;
    }
    GroupMemoryBarrierWithGroupSync();
    uint2 xy = group.xy * 8 + lane.xy;
    uint margin = ((9+6*(middle+1))*step/2)/step+1;
    if (any(xy < margin) || xy.x + margin >= (stride-1)/step || xy.y + margin >= height/step) return;
    int center = (lane.y+1)*10 + lane.x+1;
    float v = det[100+center];
    if (v <= 100.f) return;
    [unroll] for (int p=0; p<3; ++p)
        [unroll] for (int y=-1; y<=1; ++y)
            [unroll] for (int x=-1; x<=1; ++x)
                if ((p!=1 || y!=0 || x!=0) && v <= det[p*100+center+y*10+x]) return;
    uint slot;
    InterlockedAdd(result[0],1,slot);
    if (slot >= capacity) return;
    uint at = 1 + slot*31;
    result[at++] = xy.x; result[at++] = xy.y; result[at++] = middle; result[at++] = asuint(signs[center]);
    [unroll] for (int p=0; p<3; ++p)
        [unroll] for (int y=-1; y<=1; ++y)
            [unroll] for (int x=-1; x<=1; ++x)
                result[at++] = asuint(det[p*100+center+y*10+x]);
}
)";
struct Constants {
    UINT stride, height, step, capacity;
    int offsets[50][4];
    float weights[52];
};
static_assert(sizeof(Constants) % 16 == 0 && sizeof(ViewportSurfPeak) == 31 * sizeof(UINT));
}

struct GpuViewportSurf::Device {
    ComPtr<ID3D11Device> gpu;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> parameters;
    D3DCompute::Buffer sum, result, staging;
    Constants settings{};

    Device() {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
            D3D11_SDK_VERSION, &gpu, nullptr, &context), "create viewport device");
        D3D11_FEATURE_DATA_DOUBLES doubles{};
        Check(gpu->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &doubles, sizeof(doubles)), "query FP64");
        if (!doubles.DoublePrecisionFloatShaderOps)
            throw std::runtime_error("viewport detector requires double precision accumulation");
        ComPtr<ID3DBlob> code, errors;
        const auto hr = D3DCompile(source, sizeof(source)-1, "viewport SURF", nullptr, nullptr, "main",
            "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr) && errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        Check(hr, "compile viewport shader");
        Check(gpu->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
            "create viewport shader");
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(Constants);
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(gpu->CreateBuffer(&desc, nullptr, &parameters), "create viewport parameters");
    }

    UINT Dispatch(std::vector<ViewportSurfPeak>& peaks) {
        const UINT bytes = sizeof(UINT) + settings.capacity * sizeof(ViewportSurfPeak);
        result.Reserve(gpu.Get(), bytes, D3D11_BIND_UNORDERED_ACCESS, DXGI_FORMAT_R32_UINT);
        staging.Reserve(gpu.Get(), bytes, 0);
        const D3D11_BOX counter{0,0,0,sizeof(UINT),1,1};
        const UINT zero = 0;
        context->UpdateSubresource(result.data.Get(),0,&counter,&zero,0,0);
        context->UpdateSubresource(parameters.Get(),0,nullptr,&settings,0,0);
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, parameters.GetAddressOf());
        context->CSSetShaderResources(0, 1, sum.input.GetAddressOf());
        context->CSSetUnorderedAccessViews(0, 1, result.output.GetAddressOf(), nullptr);
        context->Dispatch(((settings.stride-1)/settings.step+7)/8,
            (settings.height/settings.step+7)/8, 3);
        ID3D11UnorderedAccessView* unbound = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &unbound, nullptr);
        const D3D11_BOX box{0,0,0,bytes,1,1};
        context->CopySubresourceRegion(staging.data.Get(),0,0,0,0,result.data.Get(),0,&box);
        peaks.resize(settings.capacity);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(staging.data.Get(),0,D3D11_MAP_READ,0,&mapped), "read viewport peaks");
        const auto* words = static_cast<const UINT*>(mapped.pData);
        const UINT count = words[0];
        if (count <= settings.capacity) std::memcpy(peaks.data(), words+1, count*sizeof(ViewportSurfPeak));
        context->Unmap(staging.data.Get(),0);
        peaks.resize(count <= settings.capacity ? count : 0);
        return count;
    }
};

GpuViewportSurf::GpuViewportSurf() = default;
GpuViewportSurf::~GpuViewportSurf() = default;
bool GpuViewportSurf::BeginImage(const cv::Mat& integral) {
    if (!failure_.empty() || !integral.isContinuous() || integral.type() != CV_32S ||
        integral.total() > 16 * 1024 * 1024 || integral.cols < 2 || integral.rows < 2 ||
        integral.cols-1 > 8*65535 || integral.rows-1 > 8*65535) return false;
    try {
        if (!device_) device_ = std::make_unique<Device>();
        auto& d = *device_;
        const UINT bytes = static_cast<UINT>(integral.total()*sizeof(int));
        d.sum.Reserve(d.gpu.Get(), bytes, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R32_SINT);
        const D3D11_BOX box{0,0,0,bytes,1,1};
        d.context->UpdateSubresource(d.sum.data.Get(),0,&box,integral.data,0,0);
        d.settings.stride = integral.cols;
        d.settings.height = integral.rows-1;
        return true;
    } catch (const std::exception& e) {
        failure_ = e.what();
        device_.reset();
        return false;
    }
}

bool GpuViewportSurf::FindMaxima(int octave, const ViewportHaarFilter (&filters)[5][10],
    std::vector<ViewportSurfPeak>& peaks) {
    if (!device_) return false;
    try {
        auto& d = *device_;
        auto& s = d.settings;
        s.step = 1u << octave;
        const UINT pixels = ((s.stride-1)/s.step) * (s.height/s.step);
        if (!pixels) { peaks.clear(); return true; }
        for (int layer=0; layer<5; ++layer) for (int i=0; i<10; ++i) {
            const auto& f = filters[layer][i];
            auto& p = s.offsets[layer*10+i];
            p[0]=f.p0; p[1]=f.p1; p[2]=f.p2; p[3]=f.p3;
            s.weights[layer*10+i]=f.w;
        }
        constexpr UINT maxPeaks = 65536;
        s.capacity = std::clamp(pixels/128, 64u, maxPeaks);
        const auto count = d.Dispatch(peaks);
        if (count <= s.capacity) return true;
        if (count > maxPeaks) return false; // CPU computes this octave without truncation.
        s.capacity = count;
        return d.Dispatch(peaks) == count;
    } catch (const std::exception& e) {
        failure_ = e.what();
        device_.reset();
        return false;
    }
}
