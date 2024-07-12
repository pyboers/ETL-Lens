#pragma once

#include <d3d12.h>
#include <wrl.h>
#include "QueueWrapper.h"

class CommandAllocatorWrapper
{
public:
    CommandAllocatorWrapper(QueueWrapper& queueWrapper, D3D12_COMMAND_LIST_TYPE type)
        : m_queueWrapper(queueWrapper)
    {
        HRESULT hr = m_queueWrapper.GetDevice().GetDevice()->CreateCommandAllocator(type, IID_PPV_ARGS(&m_pCommandAllocator));
        if (hr != S_OK)
        {
            throw std::exception("Failed to create d3d command allocator");
        }
    }

    CommandAllocatorWrapper(const CommandAllocatorWrapper& other) = delete;

    CommandAllocatorWrapper& operator=(const CommandAllocatorWrapper& other) = delete;

    CommandAllocatorWrapper(CommandAllocatorWrapper&& other) noexcept
        : m_pCommandAllocator(std::move(other.m_pCommandAllocator)), m_queueWrapper(other.m_queueWrapper) {
        other.m_pCommandAllocator = nullptr;
    }

    ID3D12CommandAllocator* GetCommandAllocator() const { return m_pCommandAllocator.Get(); }

    QueueWrapper& GetQueueWrapper() {
        return m_queueWrapper;
    }

private:
    QueueWrapper& m_queueWrapper;
    ComPtr<ID3D12CommandAllocator> m_pCommandAllocator;
};