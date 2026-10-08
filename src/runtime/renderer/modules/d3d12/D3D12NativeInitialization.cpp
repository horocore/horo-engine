#include "D3D12Initialization.h"

#if defined(_WIN32) && defined(_M_X64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <array>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <iomanip>
#include <objbase.h>
#include <sstream>
#include <wrl/client.h>
#endif

namespace Horo::Render {
    namespace {
#if defined(_WIN32) && defined(_M_X64)
        using Microsoft::WRL::ComPtr;

        /** @brief Converts a machine-local DXGI LUID without retaining native handles. */
        RenderAdapterId AdapterIdentity(const LUID luid) {
            std::ostringstream value;
            value << "d3d12:" << std::hex << std::setfill('0') << std::setw(8) << static_cast<std::uint32_t>(luid.HighPart) << std::setw(8)
                  << luid.LowPart;
            return RenderAdapterId{value.str()};
        }

        /** @brief Preserves native cause and distinguishes removal from ordinary stage failures. */
        Result<void> NativeFailure(const char *stage, const char *message, const HRESULT cause) {
            const char *code = cause == DXGI_ERROR_DEVICE_REMOVED || cause == DXGI_ERROR_DEVICE_RESET ? "render.d3d12.device_lost" : stage;
            return Result<void>::Failure(D3D12InitializationError(code, message, cause));
        }

        /** @brief Owns startup objects; no submission API or in-flight GPU references exist yet. */
        class NativeRuntime final : public ID3D12InitializationRuntime {
        public:
            Result<void> Open(const bool enableDebugLayer) override {
                APTTYPE apartment{};
                APTTYPEQUALIFIER qualifier{};
                const HRESULT apartmentResult = CoGetApartmentType(&apartment, &qualifier);
                if (FAILED(apartmentResult)) {
                    return NativeFailure("render.d3d12.com_uninitialized", "Initialize COM on the render-capable owner thread.",
                                         apartmentResult);
                }
                USHORT processMachine{};
                USHORT nativeMachine{};
                if (!IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine) || nativeMachine != IMAGE_FILE_MACHINE_AMD64 ||
                    processMachine != IMAGE_FILE_MACHINE_UNKNOWN) {
                    return NativeFailure("render.d3d12.unsupported_host",
                                         "Use a native Windows x64 process; translated processes are unsupported.", E_NOTIMPL);
                }
                // RtlGetVersion reports the actual build independently of executable manifest compatibility flags.
                using GetVersion = LONG(WINAPI *)(OSVERSIONINFOW *);
                const auto versionQuery = reinterpret_cast<GetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
                OSVERSIONINFOEXW version{};
                version.dwOSVersionInfoSize = sizeof(version);
                if (versionQuery == nullptr || versionQuery(reinterpret_cast<OSVERSIONINFOW *>(&version)) != 0 ||
                    version.dwMajorVersion < 10 || version.dwBuildNumber < 22000 || version.wProductType != VER_NT_WORKSTATION) {
                    return NativeFailure("render.d3d12.unsupported_host", "D3D12 requires native Windows 11 x64.", E_NOTIMPL);
                }
                if (enableDebugLayer) {
                    ComPtr<ID3D12Debug> debug;
                    const HRESULT status = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
                    if (FAILED(status)) {
                        return NativeFailure("render.d3d12.debug_unavailable",
                                             "Install matching diagnostic layers for the explicitly requested debug session.", status);
                    }
                    debug->EnableDebugLayer();
                }
                const HRESULT status = CreateDXGIFactory2(enableDebugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory_));
                if (FAILED(status)) {
                    const char *stage =
                        status == DXGI_ERROR_SDK_COMPONENT_MISSING ? "render.d3d12.debug_unavailable" : "render.d3d12.factory_failed";
                    return NativeFailure(stage, "Repair the DXGI runtime or requested diagnostic configuration.", status);
                }
                return Result<void>::Success();
            }

            Result<std::vector<D3D12AdapterCandidate>> Enumerate(const std::uint32_t maxAdapters) override {
                std::vector<D3D12AdapterCandidate> candidates;
                for (std::uint32_t index = 0; index <= maxAdapters; ++index) {
                    ComPtr<IDXGIAdapter1> adapter;
                    HRESULT status = factory_->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_UNSPECIFIED, IID_PPV_ARGS(&adapter));
                    if (status == DXGI_ERROR_NOT_FOUND) {
                        return Result<std::vector<D3D12AdapterCandidate>>::Success(std::move(candidates));
                    }
                    if (FAILED(status)) {
                        return Result<std::vector<D3D12AdapterCandidate>>::Failure(
                            D3D12InitializationError("render.d3d12.enumeration_failed",
                                                     "Refresh adapter discovery or repair the DXGI driver.", status));
                    }
                    if (index == maxAdapters) {
                        return Result<std::vector<D3D12AdapterCandidate>>::Failure(
                            D3D12InitializationError("render.d3d12.enumeration_limit",
                                                     "Increase the bounded adapter limit; discovery was not silently truncated."));
                    }
                    DXGI_ADAPTER_DESC1 description{};
                    status = adapter->GetDesc1(&description);
                    if (FAILED(status)) {
                        return Result<std::vector<D3D12AdapterCandidate>>::Failure(
                            D3D12InitializationError("render.d3d12.adapter_query_failed",
                                                     "Refresh adapter discovery; adapter facts could not be queried.", status));
                    }
                    LARGE_INTEGER driver{};
                    status = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
                    if (FAILED(status)) {
                        return Result<std::vector<D3D12AdapterCandidate>>::Failure(
                            D3D12InitializationError("render.d3d12.driver_query_failed",
                                                     "Install a qualified driver; its version could not be queried.", status));
                    }
                    const RenderAdapterId identity = AdapterIdentity(description.AdapterLuid);
                    candidates.push_back({identity, static_cast<std::uint64_t>(driver.QuadPart),
                                          (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0,
                                          (description.Flags & DXGI_ADAPTER_FLAG_REMOTE) != 0});
                    adapters_.push_back({identity, std::move(adapter)});
                }
                return Result<std::vector<D3D12AdapterCandidate>>::Success(std::move(candidates));
            }

            Result<void> CreateDevice(const RenderAdapterId &identity) override {
                for (const auto &adapter : adapters_) {
                    if (adapter.identity != identity) {
                        continue;
                    }
                    HRESULT status = D3D12CreateDevice(adapter.native.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_));
                    if (FAILED(status)) {
                        device_.Reset();
                        const char *stage = status == DXGI_ERROR_UNSUPPORTED || status == E_INVALIDARG
                                                ? "render.d3d12.feature_level_unsupported"
                                                : "render.d3d12.device_failed";
                        return NativeFailure(stage,
                                             "The selected adapter must support feature level 12_0 and the verified Agility runtime.",
                                             status);
                    }
                    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_0};
                    status = device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
                    if (FAILED(status) || model.HighestShaderModel < D3D_SHADER_MODEL_6_0) {
                        device_.Reset();
                        const char *stage = SUCCEEDED(status) || status == E_INVALIDARG ? "render.d3d12.shader_model_unsupported"
                                                                                        : "render.d3d12.shader_model_query_failed";
                        return NativeFailure(stage, "Select a device with independently queried Shader Model 6.0 support.",
                                             FAILED(status) ? status : DXGI_ERROR_UNSUPPORTED);
                    }
                    return Result<void>::Success();
                }
                return NativeFailure("render.d3d12.adapter_unavailable", "Refresh the exact machine-local adapter selection.",
                                     DXGI_ERROR_NOT_FOUND);
            }

            Result<void> CreateQueue(const D3D12QueueKind kind) override {
                if (kind != D3D12QueueKind::Direct && kind != D3D12QueueKind::Compute && kind != D3D12QueueKind::Copy) {
                    return NativeFailure("render.d3d12.invalid_queue", "Request a direct, compute or copy command queue.", E_INVALIDARG);
                }
                D3D12_COMMAND_QUEUE_DESC description{};
                description.Type = kind == D3D12QueueKind::Direct    ? D3D12_COMMAND_LIST_TYPE_DIRECT
                                   : kind == D3D12QueueKind::Compute ? D3D12_COMMAND_LIST_TYPE_COMPUTE
                                                                     : D3D12_COMMAND_LIST_TYPE_COPY;
                const HRESULT status = device_->CreateCommandQueue(&description, IID_PPV_ARGS(&queues_[static_cast<std::size_t>(kind)]));
                if (FAILED(status)) {
                    const char *stage = kind == D3D12QueueKind::Direct    ? "render.d3d12.direct_queue_failed"
                                        : kind == D3D12QueueKind::Compute ? "render.d3d12.compute_queue_failed"
                                                                          : "render.d3d12.copy_queue_failed";
                    return NativeFailure(stage, "The selected device could not create an explicitly required command queue.", status);
                }
                return Result<void>::Success();
            }

            void Release() noexcept override {
                for (auto &queue : queues_) {
                    queue.Reset();
                }
                device_.Reset();
                adapters_.clear();
                factory_.Reset();
            }

        private:
            struct Adapter {
                RenderAdapterId identity;
                ComPtr<IDXGIAdapter1> native;
            };

            ComPtr<IDXGIFactory6> factory_;
            std::vector<Adapter> adapters_;
            ComPtr<ID3D12Device> device_;
            std::array<ComPtr<ID3D12CommandQueue>, 3> queues_;
        };
#else
        /** @brief Returns actionable unsupported-host errors without native acquisition. */
        class NativeRuntime final : public ID3D12InitializationRuntime {
        public:
            Result<void> Open(bool) override {
                return Unsupported();
            }

            Result<std::vector<D3D12AdapterCandidate>> Enumerate(std::uint32_t) override {
                return Result<std::vector<D3D12AdapterCandidate>>::Failure(Unsupported().ErrorValue());
            }

            Result<void> CreateDevice(const RenderAdapterId &) override {
                return Unsupported();
            }

            Result<void> CreateQueue(D3D12QueueKind) override {
                return Unsupported();
            }

            void Release() noexcept override {}

        private:
            static Result<void> Unsupported() {
                return Result<void>::Failure(
                    D3D12InitializationError("render.d3d12.unsupported_host", "Use native Windows 11 x64 for the D3D12 component."));
            }
        };
#endif
    }  // namespace

    /** @copydoc CreateD3D12InitializationRuntime */
    std::unique_ptr<ID3D12InitializationRuntime> CreateD3D12InitializationRuntime() {
        return std::make_unique<NativeRuntime>();
    }
}  // namespace Horo::Render
