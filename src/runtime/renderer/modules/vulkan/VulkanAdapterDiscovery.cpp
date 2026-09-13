#include "VulkanBackendInternal.h"
#include "VulkanRenderBackendErrors.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

namespace Horo::Render {
    namespace {
        [[nodiscard]] bool HasValidQueueData(const VulkanAdapterCandidate &candidate) {
            if (candidate.queueFamilies.size() > 256) {
                return false;
            }
            std::set<std::uint32_t, std::less<>> indices;
            return std::ranges::all_of(candidate.queueFamilies, [&](const VulkanQueueFamily &queue) {
                return indices.insert(queue.index).second;
            });
        }

        [[nodiscard]] bool IsAvailable(const VulkanAdapterCandidate &candidate) {
            return candidate.standardApiVariant && !candidate.portabilitySubset && candidate.driverAllowed &&
                   Detail::VulkanSupportsVersion(candidate.apiVersion) && candidate.requiredFeatures.AreAvailable() &&
                   Detail::VulkanHasQueue(candidate, false);
        }

        [[nodiscard]] bool HasValidIdentity(const VulkanAdapterCandidate &candidate) {
            return candidate.id.IsValid() && !candidate.displayName.empty() && candidate.displayName.size() <= 256;
        }

        [[nodiscard]] bool SupportsPresentation(const VulkanAdapterCandidate &candidate, const bool available) {
            return available && Detail::VulkanContains(candidate.deviceExtensions, "VK_KHR_swapchain") &&
                   Detail::VulkanHasQueue(candidate, true);
        }

        struct LoaderGuard {
            explicit LoaderGuard(IVulkanRuntimePort &value) noexcept : port(&value) {}

            LoaderGuard(const LoaderGuard &) = delete;
            LoaderGuard &operator=(const LoaderGuard &) = delete;
            LoaderGuard(LoaderGuard &&) = delete;
            LoaderGuard &operator=(LoaderGuard &&) = delete;

            ~LoaderGuard() {
                port->ReleaseLoader();
            }

            IVulkanRuntimePort *port;
        };

        struct InstanceGuard {
            explicit InstanceGuard(IVulkanRuntimePort &value) noexcept : port(&value) {}

            InstanceGuard(const InstanceGuard &) = delete;
            InstanceGuard &operator=(const InstanceGuard &) = delete;
            InstanceGuard(InstanceGuard &&) = delete;
            InstanceGuard &operator=(InstanceGuard &&) = delete;

            ~InstanceGuard() {
                port->DestroyInstance();
            }

            IVulkanRuntimePort *port;
        };

        [[nodiscard]] Result<VulkanAdapterEnumeration> EnumerateCandidates(IVulkanRuntimePort &runtimePort,
                                                                           const std::uint32_t maxAdapters) {
            if (auto acquired = runtimePort.AcquireLoader(); acquired.HasError()) {
                return Result<VulkanAdapterEnumeration>::Failure(std::move(acquired).ErrorValue());
            }
            LoaderGuard loaderGuard{runtimePort};
            auto capabilities = runtimePort.QueryLoaderCapabilities();
            if (capabilities.HasError()) {
                return Result<VulkanAdapterEnumeration>::Failure(std::move(capabilities).ErrorValue());
            }
            const VulkanLoaderCapabilities &loader = capabilities.Value();
            if (!Detail::VulkanLoaderMeetsBaseline(loader)) {
                return Result<VulkanAdapterEnumeration>::Failure(MakeError(VulkanBackendErrors::LoaderVersionUnsupported));
            }
            if (!loader.standardApiVariant) {
                return Result<VulkanAdapterEnumeration>::Failure(MakeError(VulkanBackendErrors::ApiVariantUnsupported));
            }
            if (auto instance = runtimePort.CreateInstance(VulkanInstanceRequest{.apiVersion = Detail::RequiredVulkanApiVersion});
                instance.HasError()) {
                return Result<VulkanAdapterEnumeration>::Failure(std::move(instance).ErrorValue());
            }
            InstanceGuard instanceGuard{runtimePort};
            return runtimePort.EnumeratePhysicalDevices(maxAdapters);
        }

        [[nodiscard]] Result<RenderAdapterSnapshot> BuildSnapshot(const VulkanAdapterEnumeration &enumeration,
                                                                  const std::uint32_t maxAdapters) {
            if (enumeration.revision == 0 || enumeration.adapters.size() > maxAdapters) {
                return Result<RenderAdapterSnapshot>::Failure(
                    MakeError(VulkanBackendErrors::AdapterDataInvalid, "Vulkan physical-device enumeration is invalid."));
            }
            RenderAdapterSnapshot snapshot{.revision = enumeration.revision};
            std::set<std::string, std::less<>> identities;
            snapshot.adapters.reserve(enumeration.adapters.size());
            for (const VulkanAdapterCandidate &candidate : enumeration.adapters) {
                if (!HasValidIdentity(candidate) || !HasValidQueueData(candidate) || !identities.insert(candidate.id.Value()).second) {
                    return Result<RenderAdapterSnapshot>::Failure(
                        MakeError(VulkanBackendErrors::AdapterDataInvalid, "Vulkan adapter identity data is invalid."));
                }
                const bool available = IsAvailable(candidate);
                snapshot.adapters.push_back(RenderAdapterProperties{
                    .id = candidate.id,
                    .displayName = candidate.displayName,
                    .kind = candidate.kind,
                    .availability = available ? RenderAdapterAvailability::Available : RenderAdapterAvailability::Unavailable,
                    .dedicatedVideoMemoryBytes = candidate.dedicatedVideoMemoryBytes,
                    .supportsPresentation = SupportsPresentation(candidate, available),
                });
            }
            std::ranges::sort(snapshot.adapters, {}, [](const RenderAdapterProperties &adapter) -> const std::string & {
                return adapter.id.Value();
            });
            if (!snapshot.IsValid()) {
                return Result<RenderAdapterSnapshot>::Failure(
                    MakeError(VulkanBackendErrors::AdapterDataInvalid, "Vulkan adapter snapshot validation failed."));
            }
            return Result<RenderAdapterSnapshot>::Success(std::move(snapshot));
        }

        class VulkanAdapterDiscovery final : public IRenderAdapterDiscovery {
        public:
            explicit VulkanAdapterDiscovery(IVulkanRuntimePort &runtimePort) noexcept : runtimePort_(&runtimePort) {}

            explicit VulkanAdapterDiscovery(std::shared_ptr<IVulkanRuntimePort> runtimePort)
                : runtimeOwner_(std::move(runtimePort)), runtimePort_(runtimeOwner_.get()) {}

            Result<RenderAdapterSnapshot> Discover(const RenderAdapterDiscoveryRequest &request) override {
                if (stopped_) {
                    return Result<RenderAdapterSnapshot>::Failure(
                        MakeError(VulkanBackendErrors::DiscoveryStopped, "Vulkan adapter discovery has stopped."));
                }
                if (!request.IsValid()) {
                    return Result<RenderAdapterSnapshot>::Failure(
                        MakeError(VulkanBackendErrors::AdapterDataInvalid, "Vulkan adapter discovery request is invalid."));
                }

                auto enumeration = EnumerateCandidates(*runtimePort_, request.maxAdapters);
                if (enumeration.HasError()) {
                    return Result<RenderAdapterSnapshot>::Failure(std::move(enumeration).ErrorValue());
                }
                return BuildSnapshot(enumeration.Value(), request.maxAdapters);
            }

            void Stop() noexcept override {
                stopped_ = true;
            }

        private:
            std::shared_ptr<IVulkanRuntimePort> runtimeOwner_;
            IVulkanRuntimePort *runtimePort_{nullptr};
            bool stopped_{false};
        };
    }  // namespace

    namespace Detail {
        std::unique_ptr<IRenderAdapterDiscovery> CreateVulkanAdapterDiscoveryWithRuntimePort(IVulkanRuntimePort &runtimePort) {
            return std::make_unique<VulkanAdapterDiscovery>(runtimePort);
        }

        std::unique_ptr<IRenderAdapterDiscovery> CreateVulkanAdapterDiscoveryWithOwnedRuntimePort(
            std::shared_ptr<IVulkanRuntimePort> runtimePort) {
            return std::make_unique<VulkanAdapterDiscovery>(std::move(runtimePort));
        }
    }  // namespace Detail
}  // namespace Horo::Render
