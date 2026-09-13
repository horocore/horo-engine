#include "VulkanNativeRuntime.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace Horo::Render::Detail {
    std::string VulkanAdapterIdentity(const VkPhysicalDeviceIDProperties &identity) {
        std::ostringstream stream;
        stream << "vk-" << std::hex << std::setfill('0');
        for (const std::uint8_t value : identity.deviceUUID) {
            stream << std::setw(2) << static_cast<unsigned int>(value);
        }
        return stream.str();
    }

    std::uint64_t VulkanAdapterRevision(const std::vector<VulkanAdapterCandidate> &candidates) {
        std::vector<const VulkanAdapterCandidate *> ordered;
        ordered.reserve(candidates.size());
        for (const VulkanAdapterCandidate &candidate : candidates) {
            ordered.push_back(&candidate);
        }
        std::ranges::sort(ordered, {}, [](const VulkanAdapterCandidate *candidate) -> const std::string & {
            return candidate->id.Value();
        });
        std::uint64_t revision{1469598103934665603ULL};
        const auto mix = [&](const std::uint64_t value) {
            revision = (revision ^ value) * 1099511628211ULL;
        };
        for (const VulkanAdapterCandidate *candidate : ordered) {
            for (const unsigned char value : candidate->id.Value()) {
                mix(value);
            }
            mix(candidate->apiVersion.major);
            mix(candidate->apiVersion.minor);
            mix(candidate->apiVersion.patch);
            mix(candidate->driverVersion);
            mix(candidate->requiredFeatures.dynamicRendering);
            mix(candidate->requiredFeatures.synchronization2);
            mix(candidate->requiredFeatures.timelineSemaphore);
            mix(candidate->driverAllowed);
        }
        return revision == 0 ? 1 : revision;
    }
}  // namespace Horo::Render::Detail
