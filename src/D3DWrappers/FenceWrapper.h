#pragma once

#include <d3d12.h>
#include <wrl.h>
#include "QueueWrapper.h"

using Microsoft::WRL::ComPtr;

class FenceWrapper
{
public:
    FenceWrapper(QueueWrapper& queueWrapper)
        : m_queueWrapper(queueWrapper), m_fenceValue(0)
    {
        if (m_queueWrapper.GetDevice().GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pFence)) != S_OK)
            throw std::exception("Failed to create fence");
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent)
            throw std::exception("Failed to create fence event");
    }

    FenceWrapper(const FenceWrapper& other) = delete;

    FenceWrapper& operator=(const FenceWrapper& other) = delete;

    FenceWrapper(FenceWrapper&& other) noexcept
        : m_pFence(std::move(other.m_pFence)), m_fenceEvent(other.m_fenceEvent), m_queueWrapper(other.m_queueWrapper), m_fenceValue(other.m_fenceValue) {
        other.m_pFence = nullptr;
        other.m_fenceEvent = nullptr;
    }

    ~FenceWrapper()
    {
        if(m_fenceEvent)
            CloseHandle(m_fenceEvent);
    }

    UINT64 GetFenceValue() const {
        return m_fenceValue;
    }

    ComPtr<ID3D12Fence> GetFence() {
        return m_pFence;
    }

    void Signal()
    {
        if (m_queueWrapper.GetQueue()->Signal(m_pFence.Get(), ++m_fenceValue) != S_OK)
            throw std::exception("Failed to signal fence");
    }

    void WaitForFence()
    {
        if (m_pFence->GetCompletedValue() < m_fenceValue)
        {
            if (m_pFence->SetEventOnCompletion(m_fenceValue, m_fenceEvent) != S_OK)
                throw std::exception("Failed to set event on fence completion");

            if (WaitForSingleObject(m_fenceEvent, INFINITE) == WAIT_FAILED)
                throw std::exception("Failed to wait on fence event");
        }
    }

private:
    QueueWrapper& m_queueWrapper;
    ComPtr<ID3D12Fence> m_pFence;
    UINT64 m_fenceValue;
    HANDLE m_fenceEvent;
};