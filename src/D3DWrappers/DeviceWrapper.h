#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include <vector>
#include <iostream>
#define _DEBUG

using Microsoft::WRL::ComPtr;

class DeviceWrapper
{
public:
    DeviceWrapper()
    {
        UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
            debugController->EnableDebugLayer();
        else
            throw std::exception("Failed to create debug controller");
#endif

        ComPtr<IDXGIFactory4> factory;
        CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory));

        ComPtr<IDXGIAdapter1> hardwareAdapter;
        GetHardwareAdapter(factory.Get(), &hardwareAdapter);

        if(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_pDevice)) != S_OK)
            throw std::exception("Failed to create Descriptor Heap");

#if defined(_DEBUG)
        if (debugController) {
            ID3D12InfoQueue* pInfoQueue = nullptr;
            if (m_pDevice->QueryInterface(IID_PPV_ARGS(&pInfoQueue)) != S_OK)
                throw std::exception("Failed to start debug info");
            pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
            pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
            pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
            pInfoQueue->Release();
        }
#endif
    }
    
    DeviceWrapper(const DeviceWrapper& other) = delete;

    DeviceWrapper& operator=(const DeviceWrapper& other) = delete;

    DeviceWrapper(DeviceWrapper&& other) noexcept
        : m_pDevice(std::move(other.m_pDevice)) {
        other.m_pDevice = nullptr;
    }

    ID3D12Device* GetDevice() const { return m_pDevice.Get(); }

    ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT numDescriptors, D3D12_DESCRIPTOR_HEAP_FLAGS flags) {

        ComPtr<ID3D12DescriptorHeap> heap;
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = type;
        heapDesc.NumDescriptors = numDescriptors;
        heapDesc.Flags = flags;

        if(m_pDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)) != S_OK)
            throw std::exception("Failed to create Descriptor Heap");

        return heap;
    }

    void OutputDebugMessages() {
#if defined(_DEBUG)
        ID3D12InfoQueue* infoQueue = nullptr;
        if (m_pDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)) != S_OK)
            throw std::exception("Failed to create info queue");

        UINT64 messageCount = infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < messageCount; ++i) {
            SIZE_T messageLength = 0;
            infoQueue->GetMessage(i, nullptr, &messageLength);
            std::vector<char> messageData(messageLength);
            D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(messageData.data());
            infoQueue->GetMessage(i, message, &messageLength);
            std::cerr << "D3D12 Message: " << message->pDescription << "\n";
        }
        infoQueue->ClearStoredMessages();
        infoQueue->Release();
#endif
    }

private:
    void GetHardwareAdapter(IDXGIFactory1* pFactory, IDXGIAdapter1** ppAdapter)
    {
        *ppAdapter = nullptr;

        for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != pFactory->EnumAdapters1(adapterIndex, ppAdapter); ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc;
            (*ppAdapter)->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                continue;
            }

            if (D3D12CreateDevice(*ppAdapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr) == S_OK)
            {
                break;
            }
        }
    }

    ComPtr<ID3D12Device> m_pDevice;
};