#pragma once

#include <d3d12.h>
#include <wrl.h>
#include "DeviceWrapper.h"

using Microsoft::WRL::ComPtr;

class QueueWrapper
{
public:
    QueueWrapper(DeviceWrapper& deviceWrapper, D3D12_COMMAND_LIST_TYPE type)
        : m_deviceWrapper(deviceWrapper)
    {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = type;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.NodeMask = 0;

        if (m_deviceWrapper.GetDevice()->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_pQueue)) != S_OK)
            throw std::exception("Failed to create commandqueue");
    }

    QueueWrapper(const QueueWrapper& other) = delete;
    QueueWrapper& operator=(const QueueWrapper other) = delete;

    QueueWrapper(QueueWrapper&& other) noexcept
        : m_deviceWrapper(other.m_deviceWrapper), m_pQueue(std::move(other.m_pQueue)) {
        other.m_pQueue = nullptr;
    }

    ID3D12CommandQueue* GetQueue() const { return m_pQueue.Get(); }
    DeviceWrapper &GetDevice() const { return m_deviceWrapper; }

private:
    DeviceWrapper& m_deviceWrapper;
    ComPtr<ID3D12CommandQueue> m_pQueue;
};