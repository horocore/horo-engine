#include "Horo/Runtime/Render/RenderAdapterErrors.h"
#include "MetalNativeDeviceFacts.h"
#include "MetalRenderBackendErrors.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <vector>

namespace Horo::Render::Detail {
    namespace {
        [[nodiscard]] MetalHostArchitecture HostArchitecture() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
            return MetalHostArchitecture::Arm64;
#elif defined(__x86_64__)
            return MetalHostArchitecture::X86_64;
#else
            return MetalHostArchitecture::Unsupported;
#endif
        }

        [[nodiscard]] RenderAdapterId AdapterId(id<MTLDevice> device) {
            return RenderAdapterId{std::format("metal:{:016x}", static_cast<std::uint64_t>(device.registryID))};
        }

        [[nodiscard]] std::uint64_t DiscoveryRevisionForDevices(NSArray<id<MTLDevice>> *devices) {
            std::vector<std::string> identities;
            identities.reserve(devices.count);
            for (id<MTLDevice> device in devices) {
                identities.push_back(AdapterId(device).Value());
            }
            std::ranges::sort(identities);

            constexpr std::uint64_t offsetBasis = 14'695'981'039'346'656'037ULL;
            constexpr std::uint64_t prime = 1'099'511'628'211ULL;
            std::uint64_t revision = offsetBasis;
            for (const std::string &identity : identities) {
                for (const unsigned char byte : identity) {
                    revision = (revision ^ byte) * prime;
                }
                revision = (revision ^ 0xffU) * prime;
            }
            return revision == 0 ? 1 : revision;
        }

        [[nodiscard]] MetalFormatCapabilities QueryFormats(id<MTLDevice> device) noexcept {
            MetalFormatCapabilities formats;
            const auto sampledAttachment = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment;
            const auto enableUsage = [&formats](const RenderTextureFormat format, const RenderTextureUsage usage) {
                formats.usages[static_cast<std::size_t>(format)] = usage;
            };
            enableUsage(RenderTextureFormat::Rgba8Unorm, sampledAttachment);
            enableUsage(RenderTextureFormat::Bgra8Unorm, sampledAttachment);
            enableUsage(RenderTextureFormat::Depth32Float, RenderTextureUsage::RenderAttachment);
            if (device.depth24Stencil8PixelFormatSupported) {
                enableUsage(RenderTextureFormat::Depth24Stencil8, RenderTextureUsage::RenderAttachment);
            }
            constexpr std::array sampleCounts{1U, 2U, 4U, 8U};
            for (const std::uint32_t sampleCount : sampleCounts) {
                if ([device supportsTextureSampleCount:sampleCount]) {
                    formats.sampleCountMask |= std::uint64_t{1} << sampleCount;
                }
            }
            return formats;
        }

        [[nodiscard]] RenderAdapterProperties AdapterProperties(id<MTLDevice> device) {
            const char *name = device.name.UTF8String;
            std::string displayName = name == nullptr ? "Unnamed Metal device" : std::string{name};
            constexpr std::size_t maximumDisplayNameLength = 256;
            if (displayName.size() > maximumDisplayNameLength) {
                std::size_t truncatedLength = maximumDisplayNameLength;
                while (truncatedLength > 0 && (static_cast<unsigned char>(displayName[truncatedLength]) & 0xc0U) == 0x80U) {
                    --truncatedLength;
                }
                displayName.resize(truncatedLength);
            }
            const RenderAdapterKind kind = device.lowPower ? RenderAdapterKind::Integrated
                                                           : (device.removable ? RenderAdapterKind::Discrete : RenderAdapterKind::Unknown);
            return {
                .id = AdapterId(device),
                .displayName = std::move(displayName),
                .kind = kind,
                .availability = RenderAdapterAvailability::Available,
                .dedicatedVideoMemoryBytes = 0,
                .supportsPresentation = !device.headless,
            };
        }

        [[nodiscard]] MetalDeviceFacts QueryDeviceFacts(id<MTLDevice> device, const std::uint64_t discoveryRevision,
                                                        const bool commandQueueAvailable) {
            const NSOperatingSystemVersion version = NSProcessInfo.processInfo.operatingSystemVersion;
            return {
                .adapter = AdapterProperties(device),
                .discoveryRevision = discoveryRevision,
                .operatingSystemMajor = static_cast<std::uint32_t>(version.majorVersion),
                .operatingSystemMinor = static_cast<std::uint32_t>(version.minorVersion),
                .architecture = HostArchitecture(),
                .supportsApple7 = [device supportsFamily:MTLGPUFamilyApple7],
                .supportsMac2 = [device supportsFamily:MTLGPUFamilyMac2],
                .commandQueueAvailable = commandQueueAvailable,
                .maxBufferLength = static_cast<std::uint64_t>(device.maxBufferLength),
                .maxTextureDimension2D = 16'384,
                .formats = QueryFormats(device),
            };
        }

        [[nodiscard]] bool IsDeviceAvailable(const MetalDeviceFacts &facts) noexcept {
            const bool hostSupported = facts.operatingSystemMajor >= 14;
            const bool familySupported = (facts.architecture == MetalHostArchitecture::Arm64 && facts.supportsApple7) ||
                                         (facts.architecture == MetalHostArchitecture::X86_64 && facts.supportsMac2);
            return hostSupported && familySupported;
        }

        [[nodiscard]] std::vector<RenderAdapterProperties> DiscoverAdapters(NSArray<id<MTLDevice>> *devices, const std::uint64_t revision) {
            std::vector<RenderAdapterProperties> adapters;
            adapters.reserve(devices.count);
            for (id<MTLDevice> device in devices) {
                MetalDeviceFacts facts = QueryDeviceFacts(device, revision, false);
                facts.adapter.availability =
                    IsDeviceAvailable(facts) ? RenderAdapterAvailability::Available : RenderAdapterAvailability::Unavailable;
                adapters.push_back(std::move(facts.adapter));
            }
            std::ranges::sort(adapters, {}, [](const RenderAdapterProperties &adapter) {
                return adapter.id.Value();
            });
            return adapters;
        }

        [[nodiscard]] id<MTLDevice> FindDevice(const std::optional<RenderAdapterId> &requested, std::uint64_t &discoveryRevision) {
            NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
            discoveryRevision = DiscoveryRevisionForDevices(devices);
            if (!requested) {
                return MTLCreateSystemDefaultDevice();
            }
            for (id<MTLDevice> candidate in devices) {
                if (AdapterId(candidate) == *requested) {
                    return candidate;
                }
            }
            return nil;
        }

        class MetalAdapterDiscovery final : public IRenderAdapterDiscovery {
        public:
            Result<RenderAdapterSnapshot> Discover(const RenderAdapterDiscoveryRequest &request) override {
                if (const std::optional<Error> invalidState = ValidateState(request)) {
                    return Result<RenderAdapterSnapshot>::Failure(*invalidState);
                }

                NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
                const std::uint64_t revision = DiscoveryRevisionForDevices(devices);
                std::vector<RenderAdapterProperties> adapters = DiscoverAdapters(devices, revision);
                if (adapters.size() > request.maxAdapters) {
                    adapters.resize(request.maxAdapters);
                }
                RenderAdapterSnapshot snapshot{revision, std::move(adapters)};
                if (!snapshot.IsValid()) {
                    return Result<RenderAdapterSnapshot>::Failure(
                        MakeError(MetalBackendErrors::InvalidDeviceFacts,
                                  "Metal discovery returned duplicate or malformed adapter facts."));
                }
                return Result<RenderAdapterSnapshot>::Success(std::move(snapshot));
            }

            void Stop() noexcept override {
                stopped_ = true;
            }

        private:
            [[nodiscard]] std::optional<Error> ValidateState(const RenderAdapterDiscoveryRequest &request) const {
                if (stopped_) {
                    return MakeError(RenderAdapterErrors::DiscoveryStopped);
                }
                if (!request.IsValid()) {
                    return MakeError(RenderAdapterErrors::InvalidDiscoveryRequest);
                }
                return std::nullopt;
            }

            bool stopped_{false};
        };

    }  // namespace

    /** @copydoc QueryMetalDeviceFacts */
    MetalDeviceFacts QueryMetalDeviceFacts(id<MTLDevice> device, const std::uint64_t revision, const bool queueAvailable) {
        return QueryDeviceFacts(device, revision, queueAvailable);
    }

    /** @copydoc FindMetalDevice */
    id<MTLDevice> FindMetalDevice(const std::optional<RenderAdapterId> &requested, std::uint64_t &revision) {
        return FindDevice(requested, revision);
    }
}  // namespace Horo::Render::Detail

namespace Horo::Render {
    /** @copydoc CreateMetalAdapterDiscovery */
    Result<std::unique_ptr<IRenderAdapterDiscovery>> CreateMetalAdapterDiscovery() {
        return Result<std::unique_ptr<IRenderAdapterDiscovery>>::Success(std::make_unique<Detail::MetalAdapterDiscovery>());
    }
}  // namespace Horo::Render
