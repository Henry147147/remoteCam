#include "rcplatform/d3d11_transform.h"

#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

#include "rcplatform/shader_constants.h"

namespace rcplatform {
namespace {

using Microsoft::WRL::ComPtr;

constexpr char kVertexShader[] = R"hlsl(
struct Output { float4 position : SV_Position; };
Output main(uint vertexId : SV_VertexID) {
  const float2 positions[3] = {
    float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0)
  };
  Output output;
  output.position = float4(positions[vertexId], 0.0, 1.0);
  return output;
}
)hlsl";

constexpr char kPixelShader[] = R"hlsl(
Texture2DArray<float> yPlane : register(t0);
Texture2DArray<float2> uvPlane : register(t1);
SamplerState linearClamp : register(s0);

cbuffer Transform : register(b0) {
  float4 row0;
  float4 row1;
  float4 row2;
  float4 inverseSourceSize;
};

float4 main(float4 position : SV_Position) : SV_Target {
  const float3 destination = float3(position.xy, 1.0);
  const float divisor = dot(row2.xyz, destination);
  const float2 sourcePixels =
      float2(dot(row0.xyz, destination), dot(row1.xyz, destination)) / divisor;
  const float2 uv = sourcePixels * inverseSourceSize.xy;

  // VideoToolbox's NV12 is video-range Rec.709. Sampling the two planes directly
  // avoids a CPU readback and keeps transform + colour conversion in one pass.
  const float y = (yPlane.SampleLevel(linearClamp, float3(uv, 0.0), 0).r - 16.0 / 255.0)
                  * (255.0 / 219.0);
  const float2 chroma =
      (uvPlane.SampleLevel(linearClamp, float3(uv, 0.0), 0).rg - 0.5)
      * (255.0 / 224.0);
  const float3 rgb = float3(
      y + 1.5748 * chroma.y,
      y - 0.1873 * chroma.x - 0.4681 * chroma.y,
      y + 1.8556 * chroma.x);
  return float4(saturate(rgb), 1.0);
}
)hlsl";

HRESULT compileShader(const char* source, size_t size, const char* target, ID3DBlob** out) {
  if (out == nullptr) return E_POINTER;
  *out = nullptr;
  ComPtr<ID3DBlob> errors;
  constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
  return ::D3DCompile(source, size, "RemoteCamTransform", nullptr, nullptr, "main", target,
                      flags, 0, out, &errors);
}

}  // namespace

struct D3D11TransformPass::Impl {
  struct SourceViews {
    ComPtr<ID3D11Texture2D> texture;
    uint32_t slice = 0;
    ComPtr<ID3D11ShaderResourceView1> y;
    ComPtr<ID3D11ShaderResourceView1> uv;
  };

  HRESULT initialize(ID3D11Device* sourceDevice) {
    if (sourceDevice == nullptr) return E_POINTER;
    if (device.Get() == sourceDevice) return S_OK;

    reset();
    HRESULT hr = sourceDevice->QueryInterface(IID_PPV_ARGS(&device));
    if (FAILED(hr)) return hr;
    device->GetImmediateContext(&context);
    if (!context) return E_FAIL;

    ComPtr<ID3DBlob> vertexBytes;
    hr = compileShader(kVertexShader, std::strlen(kVertexShader), "vs_5_0", &vertexBytes);
    if (FAILED(hr)) return hr;
    hr = device->CreateVertexShader(vertexBytes->GetBufferPointer(), vertexBytes->GetBufferSize(),
                                    nullptr, &vertexShader);
    if (FAILED(hr)) return hr;

    ComPtr<ID3DBlob> pixelBytes;
    hr = compileShader(kPixelShader, std::strlen(kPixelShader), "ps_5_0", &pixelBytes);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(pixelBytes->GetBufferPointer(), pixelBytes->GetBufferSize(),
                                   nullptr, &pixelShader);
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(TransformConstants);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device->CreateBuffer(&bufferDesc, nullptr, &constantBuffer);
    if (FAILED(hr)) return hr;

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    return device->CreateSamplerState(&samplerDesc, &sampler);
  }

  void reset() {
    sourceViews.clear();
    outputRtv.Reset();
    output.Reset();
    sampler.Reset();
    constantBuffer.Reset();
    pixelShader.Reset();
    vertexShader.Reset();
    context.Reset();
    device.Reset();
    outputWidth = 0;
    outputHeight = 0;
  }

  HRESULT ensureOutput(uint32_t width, uint32_t height) {
    if (output && outputWidth == width && outputHeight == height) return S_OK;
    outputRtv.Reset();
    output.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &output);
    if (FAILED(hr)) return hr;
    hr = device->CreateRenderTargetView(output.Get(), nullptr, &outputRtv);
    if (FAILED(hr)) return hr;
    outputWidth = width;
    outputHeight = height;
    return S_OK;
  }

  HRESULT viewsFor(const TextureFrame& input, SourceViews*& out) {
    for (SourceViews& entry : sourceViews) {
      if (entry.texture.Get() == input.texture.Get() && entry.slice == input.arraySlice) {
        out = &entry;
        return S_OK;
      }
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    input.texture->GetDesc(&textureDesc);
    if (textureDesc.Format != DXGI_FORMAT_NV12 || input.arraySlice >= textureDesc.ArraySize) {
      return E_INVALIDARG;
    }

    SourceViews entry;
    entry.texture = input.texture;
    entry.slice = input.arraySlice;

    D3D11_SHADER_RESOURCE_VIEW_DESC1 desc{};
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MostDetailedMip = 0;
    desc.Texture2DArray.MipLevels = 1;
    desc.Texture2DArray.FirstArraySlice = input.arraySlice;
    desc.Texture2DArray.ArraySize = 1;
    desc.Texture2DArray.PlaneSlice = 0;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    HRESULT hr = device->CreateShaderResourceView1(input.texture.Get(), &desc, &entry.y);
    if (FAILED(hr)) return hr;

    desc.Texture2DArray.PlaneSlice = 1;
    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    hr = device->CreateShaderResourceView1(input.texture.Get(), &desc, &entry.uv);
    if (FAILED(hr)) return hr;

    sourceViews.push_back(std::move(entry));
    out = &sourceViews.back();
    return S_OK;
  }

  ComPtr<ID3D11Device3> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11VertexShader> vertexShader;
  ComPtr<ID3D11PixelShader> pixelShader;
  ComPtr<ID3D11Buffer> constantBuffer;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11Texture2D> output;
  ComPtr<ID3D11RenderTargetView> outputRtv;
  uint32_t outputWidth = 0;
  uint32_t outputHeight = 0;
  std::vector<SourceViews> sourceViews;
};

D3D11TransformPass::D3D11TransformPass() : impl_(std::make_unique<Impl>()) {}
D3D11TransformPass::~D3D11TransformPass() = default;

HRESULT D3D11TransformPass::apply(const TextureFrame& input, const rc::TransformParams& params,
                                  TextureFrame& out) {
  if (!input.texture) return E_POINTER;
  if (params.srcWidth <= 0 || params.srcHeight <= 0 || params.dstWidth <= 0 ||
      params.dstHeight <= 0 || input.width != static_cast<uint32_t>(params.srcWidth) ||
      input.height != static_cast<uint32_t>(params.srcHeight)) {
    return E_INVALIDARG;
  }

  ComPtr<ID3D11Device> sourceDevice;
  input.texture->GetDevice(&sourceDevice);
  HRESULT hr = impl_->initialize(sourceDevice.Get());
  if (FAILED(hr)) return hr;
  hr = impl_->ensureOutput(static_cast<uint32_t>(params.dstWidth),
                           static_cast<uint32_t>(params.dstHeight));
  if (FAILED(hr)) return hr;

  Impl::SourceViews* views = nullptr;
  hr = impl_->viewsFor(input, views);
  if (FAILED(hr)) return hr;

  const TransformConstants constants = packTransformConstants(params);
  impl_->context->UpdateSubresource(impl_->constantBuffer.Get(), 0, nullptr, &constants, 0, 0);

  D3D11_VIEWPORT viewport{};
  viewport.Width = static_cast<float>(params.dstWidth);
  viewport.Height = static_cast<float>(params.dstHeight);
  viewport.MaxDepth = 1.0f;
  impl_->context->RSSetViewports(1, &viewport);
  ID3D11RenderTargetView* renderTarget = impl_->outputRtv.Get();
  impl_->context->OMSetRenderTargets(1, &renderTarget, nullptr);
  impl_->context->IASetInputLayout(nullptr);
  impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  impl_->context->VSSetShader(impl_->vertexShader.Get(), nullptr, 0);
  impl_->context->PSSetShader(impl_->pixelShader.Get(), nullptr, 0);
  ID3D11Buffer* constantBuffer = impl_->constantBuffer.Get();
  impl_->context->PSSetConstantBuffers(0, 1, &constantBuffer);
  ID3D11SamplerState* sampler = impl_->sampler.Get();
  impl_->context->PSSetSamplers(0, 1, &sampler);
  ID3D11ShaderResourceView* resources[] = {views->y.Get(), views->uv.Get()};
  impl_->context->PSSetShaderResources(0, 2, resources);
  impl_->context->Draw(3, 0);

  // Do not leave decoder surfaces bound across calls. FFmpeg needs to write them again
  // when the pool recycles this array slice.
  ID3D11ShaderResourceView* nullResources[] = {nullptr, nullptr};
  impl_->context->PSSetShaderResources(0, 2, nullResources);

  out.texture = impl_->output;
  out.arraySlice = 0;
  out.width = static_cast<uint32_t>(params.dstWidth);
  out.height = static_cast<uint32_t>(params.dstHeight);
  out.ptsMicros = input.ptsMicros;
  return S_OK;
}

}  // namespace rcplatform
