#include "Horo/PlatformServices/PlatformServicesBackend.h"

#include <type_traits>
#include <utility>

namespace Horo::PlatformServices::Tests {
    static_assert(std::is_move_constructible_v<CloudBlobStagingBuffer>);
    static_assert(!std::is_copy_constructible_v<CloudBlobStagingBuffer>);
    static_assert(std::is_same_v<decltype(std::declval<CloudBlobStagingBuffer &>().Finish()), Result<CloudBlobOwnedBytes>>);
    static_assert(std::is_same_v<decltype(std::declval<NullPlatformServicesBackend &>().ListCloudObjects(CloudListRequest{})),
                                 Result<PlatformRequestHandle<CloudObjectPage>>>);
    static_assert(std::is_same_v<decltype(std::declval<NullPlatformServicesBackend &>().ReadCloudObject(CloudBlobReadRequest{})),
                                 Result<PlatformRequestHandle<CloudBlobReadResult>>>);
    static_assert(std::is_same_v<decltype(std::declval<NullPlatformServicesBackend &>().QueryCloudQuota(PlatformSubjectHandle{})),
                                 Result<PlatformRequestHandle<CloudQuotaObservation>>>);
}  // namespace Horo::PlatformServices::Tests
