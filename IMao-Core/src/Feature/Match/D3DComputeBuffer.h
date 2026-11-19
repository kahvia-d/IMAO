#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <bit>
#include <stdexcept>
#include <string>

namespace D3DCompute {
inline void Check(HRESULT status, const char* operation) {
    if (FAILED(status)) throw std::runtime_error(std::string(operation) + " HRESULT=" +
        std::to_string(static_cast<unsigned long>(status)));
}

struct Buffer {
    UINT bytes = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> data;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> input;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> output;

    void Reserve(ID3D11Device* device, UINT required, UINT bind,
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN) {
        if (bytes >= required) return;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = std::bit_ceil(required);
        desc.BindFlags = bind;
        desc.Usage = bind ? D3D11_USAGE_DEFAULT : D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = bind ? 0 : D3D11_CPU_ACCESS_READ;
        Check(device->CreateBuffer(&desc, nullptr, data.ReleaseAndGetAddressOf()), "create buffer");
        if (bind == D3D11_BIND_SHADER_RESOURCE) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R32G32B32A32_FLOAT : format;
            view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            view.Buffer.NumElements = desc.ByteWidth / (view.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16 : 4);
            Check(device->CreateShaderResourceView(data.Get(), &view, input.ReleaseAndGetAddressOf()), "create input");
        } else if (bind == D3D11_BIND_UNORDERED_ACCESS) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R32_FLOAT : format;
            view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            view.Buffer.NumElements = desc.ByteWidth / sizeof(float);
            Check(device->CreateUnorderedAccessView(data.Get(), &view, output.ReleaseAndGetAddressOf()), "create output");
        }
        bytes = desc.ByteWidth;
    }
};
}
