#include "GpuRecoveryDistances.h"
#include "D3DComputeBuffer.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <bit>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using D3DCompute::Check;
using D3DCompute::Buffer;
namespace {
// One group compares eight map rows to eight query rows. Cooperatively load
// each descriptor once per tile; edge lanes must reach the same barrier.
constexpr char shaderSource[] = R"(
cbuffer Shape : register(b0) { uint mapRows; uint queryRows; uint2 padding; };
Buffer<float4> mapData : register(t0);
Buffer<float4> queryData : register(t1);
RWBuffer<float> distances : register(u0);
groupshared float4 m[8][32], q[8][32];
[numthreads(8,8,1)]
void main(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID) {
    uint row = group.y * 8 + lane.y;
    uint train = group.x * 8 + lane.y;
    for (uint i = lane.x; i < 32; i += 8) {
        m[lane.y][i] = row < mapRows ? mapData[row * 32 + i] : 0;
        q[lane.y][i] = train < queryRows ? queryData[train * 32 + i] : 0;
    }
    GroupMemoryBarrierWithGroupSync();
    uint col = group.x * 8 + lane.x;
    if (row >= mapRows || col >= queryRows) return;
    precise float4 sum = 0;
    [unroll] for (uint j = 0; j < 32; ++j) {
        float4 diff = m[lane.y][j] - q[lane.x][j];
        sum += diff * diff;
    }
    distances[row * queryRows + col] = sqrt((sum.x + sum.y) + (sum.z + sum.w));
}
)";


}

struct GpuRecoveryDistances::Device {
    ComPtr<ID3D11Device> gpu;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> shape;
    Buffer map, query, result, readback;

    Device() {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
            D3D11_SDK_VERSION, &gpu, nullptr, &context), "create hardware device");
        ComPtr<ID3DBlob> bytecode, errors;
        const auto status = D3DCompile(shaderSource, sizeof(shaderSource)-1, "minimap distances",
            nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
        if (FAILED(status) && errors)
            throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        Check(status, "compile distance shader");
        Check(gpu->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
            nullptr, &shader), "create distance shader");
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 16;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(gpu->CreateBuffer(&desc, nullptr, &shape), "create shape buffer");
    }

    void Upload(Buffer& buffer, const cv::Mat& matrix) {
        const UINT bytes = static_cast<UINT>(matrix.total() * sizeof(float));
        buffer.Reserve(gpu.Get(), bytes, D3D11_BIND_SHADER_RESOURCE);
        const D3D11_BOX box{0, 0, 0, bytes, 1, 1};
        context->UpdateSubresource(buffer.data.Get(), 0, &box, matrix.data, 0, 0);
    }

    void Run(const cv::Mat& rows, const cv::Mat& frame, cv::Mat& output) {
        Upload(map, rows);
        Upload(query, frame);
        const UINT dims[4]{static_cast<UINT>(rows.rows), static_cast<UINT>(frame.rows)};
        const UINT bytes = dims[0] * dims[1] * sizeof(float);
        result.Reserve(gpu.Get(), bytes, D3D11_BIND_UNORDERED_ACCESS);
        readback.Reserve(gpu.Get(), bytes, 0);
        context->UpdateSubresource(shape.Get(), 0, nullptr, dims, 0, 0);
        ID3D11ShaderResourceView* inputs[]{map.input.Get(), query.input.Get()};
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, shape.GetAddressOf());
        context->CSSetShaderResources(0, 2, inputs);
        context->CSSetUnorderedAccessViews(0, 1, result.output.GetAddressOf(), nullptr);
        context->Dispatch((dims[1]+7)/8, (dims[0]+7)/8, 1);
        ID3D11UnorderedAccessView* noOutput = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
        const D3D11_BOX box{0, 0, 0, bytes, 1, 1};
        context->CopySubresourceRegion(readback.data.Get(), 0, 0, 0, 0, result.data.Get(), 0, &box);
        cv::Mat completed(rows.rows, frame.rows, CV_32F);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(readback.data.Get(), 0, D3D11_MAP_READ, 0, &mapped), "read distances");
        std::memcpy(completed.data, mapped.pData, bytes);
        context->Unmap(readback.data.Get(), 0);
        output = std::move(completed);
    }
};

GpuRecoveryDistances::GpuRecoveryDistances() = default;
GpuRecoveryDistances::~GpuRecoveryDistances() = default;
bool GpuRecoveryDistances::Compute(const cv::Mat& map, const cv::Mat& query, cv::Mat& output) {
    if (!failure_.empty() || map.cols != 128 || query.cols != 128 || map.empty() || query.empty() ||
        map.type() != CV_32FC1 || query.type() != CV_32FC1 || !map.isContinuous() || !query.isContinuous() ||
        map.rows > 4096 || query.rows > 4096 || int64_t(map.rows) * query.rows > 512 * 1024) return false;
    try {
        if (!device_) device_ = std::make_unique<Device>();
        device_->Run(map, query, output);
        return true;
    } catch (const std::exception& e) {
        failure_ = e.what();
        device_.reset();
        return false;
    }
}
