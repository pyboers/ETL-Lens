#pragma once

#include <d3d12.h>
#include <wrl.h>
#include "CommandAllocatorWrapper.h"

using Microsoft::WRL::ComPtr;

class CommandListWrapper
{
public:
    CommandListWrapper(CommandAllocatorWrapper& allocatorWrapper, D3D12_COMMAND_LIST_TYPE type)
        : m_allocatorWrapper(allocatorWrapper)
    {
        if (m_allocatorWrapper.GetQueueWrapper().GetDevice().GetDevice()->CreateCommandList(0, type, m_allocatorWrapper.GetCommandAllocator(), nullptr, IID_PPV_ARGS(&m_pCommandList)) != S_OK)
            throw std::exception("Failed to create d3d command list");
        m_pCommandList->Close();
    }

    CommandListWrapper(const CommandListWrapper& other) = delete;

    CommandListWrapper& operator=(const CommandListWrapper& other) = delete;

    CommandListWrapper(CommandListWrapper&& other) noexcept
        : m_allocatorWrapper(other.m_allocatorWrapper), m_pCommandList(std::move(other.m_pCommandList)) {
        other.m_pCommandList = nullptr;
    }

    ComPtr<ID3D12GraphicsCommandList> GetCommandList() const { return m_pCommandList; }

    void Close()
    {
        if (m_pCommandList->Close() != S_OK)
            throw std::exception("Failed to close commandlist");
    }

    void Reset()
    {
        if (m_pCommandList->Reset(m_allocatorWrapper.GetCommandAllocator(), nullptr) != S_OK)
            throw std::exception("Failed to reset commandlist");
    }

    void ResourceBarrier(D3D12_RESOURCE_BARRIER &barrier)
    {
        m_pCommandList->ResourceBarrier(1, &barrier);
    }

    void TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = stateBefore;
        barrier.Transition.StateAfter = stateAfter;
        ResourceBarrier(barrier);
    }

    void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, const FLOAT color[4])
    {
        m_pCommandList->ClearRenderTargetView(rtvHandle, color, 0, nullptr);
    }

    void OMSetRenderTargets(UINT numRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvHandles, BOOL rtsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* dsvHandle)
    {
        m_pCommandList->OMSetRenderTargets(numRenderTargetDescriptors, rtvHandles, rtsSingleHandleToDescriptorRange, dsvHandle);
    }

private:
    CommandAllocatorWrapper& m_allocatorWrapper;
    ComPtr<ID3D12GraphicsCommandList> m_pCommandList;
};