#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include "QueueWrapper.h"
#include "FenceWrapper.h"
#include <exception>
#include <format>

using Microsoft::WRL::ComPtr;

class SwapChainWrapper
{
public:
    SwapChainWrapper(QueueWrapper& queueWrapper, HWND hWnd, UINT width, UINT height, UINT bufferCount)
        : m_queueWrapper(queueWrapper), m_hWnd(hWnd), m_width(width), m_height(height), m_bufferCount(bufferCount), m_frameIndex(0)
    {
        CreateSwapChain();

        m_pRtvHeap = m_queueWrapper.GetDevice().CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, bufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

        UINT rtvDescriptorSize = m_queueWrapper.GetDevice().GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_pRtvHeap->GetCPUDescriptorHandleForHeapStart());

        m_renderTargetDescriptors.reserve(bufferCount);
        m_renderTargets.reserve(bufferCount);
        for (UINT i = 0; i < bufferCount; ++i) {
            m_renderTargetDescriptors.emplace_back(rtvHandle);
            rtvHandle.ptr += rtvDescriptorSize;
        }

        CreateRenderTargets();
    }

    SwapChainWrapper(const SwapChainWrapper& other) = delete;

    SwapChainWrapper& operator=(const SwapChainWrapper& other) = delete;

    SwapChainWrapper(SwapChainWrapper&& other) noexcept
        : m_bufferCount(other.m_bufferCount), m_frameIndex(other.m_frameIndex), m_width(other.m_width), m_height(other.m_height), m_hWnd(other.m_hWnd)
        , m_pRtvHeap(std::move(other.m_pRtvHeap)), m_pSwapChain(std::move(other.m_pSwapChain)), m_queueWrapper(other.m_queueWrapper), m_renderTargetDescriptors(std::move(other.m_renderTargetDescriptors))
        , m_renderTargets(std::move(other.m_renderTargets)) {
        other.m_pSwapChain = nullptr;
        other.m_pRtvHeap = nullptr;
        other.m_bufferCount = 0;
        other.m_hWnd = nullptr;
        other.m_width = 0;
        other.m_height = 0;
    }

    HRESULT Present(UINT syncInterval, UINT flags)
    {
        HRESULT hr = m_pSwapChain->Present(syncInterval, flags);
        m_frameIndex = m_pSwapChain->GetCurrentBackBufferIndex();
        return hr;
    }

    void Resize(UINT width, UINT height)
    {
        m_width = width;
        m_height = height;

        for (auto& renderTarget : m_renderTargets) {
            renderTarget = nullptr;
        }

        m_renderTargets.clear();

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        m_pSwapChain->GetDesc1(&desc);

        HRESULT hr = m_pSwapChain->ResizeBuffers(0, m_width, m_height, desc.Format, desc.Flags);
        if (hr != S_OK) {
            m_queueWrapper.GetDevice().OutputDebugMessages();
            std::cerr << "ResizeBuffers failed with HRESULT: " << std::hex << hr << "\n";
            throw std::exception("Failed to resize swapchain");
        }

        CreateRenderTargets();

        m_frameIndex = m_pSwapChain->GetCurrentBackBufferIndex();
    }

    UINT GetCurrentFrameIndex() const { return m_frameIndex; }
    ComPtr<ID3D12Resource> GetCurrentBackBuffer() const { return m_renderTargets[m_frameIndex]; }

    ComPtr<ID3D12DescriptorHeap> GetRtvHeap() const { return m_pRtvHeap; }

    const D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRtvDescriptorHandle() const {
        return m_renderTargetDescriptors[m_frameIndex];
    }

private:
    void CreateSwapChain()
    {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.BufferCount = m_bufferCount;
        sd.Width = m_width;
        sd.Height = m_height;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.Flags = 0;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.Stereo = FALSE;

        ComPtr<IDXGIFactory4> factory;
        if (CreateDXGIFactory1(IID_PPV_ARGS(&factory)) != S_OK)
            throw std::exception("Failed to create DXGIFactory");

        ComPtr<IDXGISwapChain1> swapChain;
        if(factory->CreateSwapChainForHwnd(m_queueWrapper.GetQueue(), m_hWnd, &sd, nullptr, nullptr, &swapChain) != S_OK)
            throw std::exception("Failed to create swapchain");

        if (swapChain->QueryInterface(IID_PPV_ARGS(&m_pSwapChain)) != S_OK)
            throw std::exception("Failed to create swapchain");

        m_frameIndex = m_pSwapChain->GetCurrentBackBufferIndex();
    }

    void CreateRenderTargets()
    {
        UINT rtvDescriptorSize = m_queueWrapper.GetDevice().GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        m_renderTargets.resize(m_bufferCount);
        for (UINT i = 0; i < m_bufferCount; ++i)
        {
            if(m_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])) != S_OK)
                throw std::exception("Failed to fetch swapchain buffer");

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtvHandle.ptr += (UINT64)i * rtvDescriptorSize;

            m_queueWrapper.GetDevice().GetDevice()->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
            m_renderTargets[i]->SetName(std::vformat(L"Swapchain Buffer {}", std::make_wformat_args(i)).c_str());
        }
    }

    QueueWrapper& m_queueWrapper;
    ComPtr<IDXGISwapChain3> m_pSwapChain;
    ComPtr<ID3D12DescriptorHeap> m_pRtvHeap;
    std::vector<ComPtr<ID3D12Resource>> m_renderTargets;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_renderTargetDescriptors;
    UINT m_frameIndex;
    UINT m_bufferCount;
    HWND m_hWnd;
    UINT m_width;
    UINT m_height;
};