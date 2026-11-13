#include "Horo/PlatformServices/PlatformCloudObjects.h"
#include "Horo/PlatformServices/PlatformDiagnostics.h"
#include "Horo/PlatformServices/PlatformOfflineQueueStorage.h"
#include "Horo/PlatformServices/PlatformProgressionIdempotency.h"

#include <type_traits>
#include <utility>

namespace {
    namespace Storage = Horo::PlatformOfflineQueue;
    using Publication = Horo::Result<void> (Storage::PlatformOfflineQueueStorage::*)(const Storage::PlatformOfflineSubjectPartition &,
                                                                                     std::span<const Storage::PlatformOfflineQueueRecord>,
                                                                                     const Storage::PlatformOfflineQueueRevision &) const;
    static_assert(std::is_same_v<decltype(&Storage::PlatformOfflineQueueStorage::Publish), Publication>);
    static_assert(std::is_same_v<decltype(std::declval<const Storage::PlatformOfflineQueueStorage &>().LoadSnapshot(
                                     std::declval<const Storage::PlatformOfflineSubjectPartition &>())),
                                 Horo::Result<Storage::PlatformOfflineQueueSnapshot>>);
    static_assert(!std::is_aggregate_v<Storage::PlatformOfflineQueueRevision>);
}  // namespace

static_assert(std::is_trivially_copyable_v<Horo::PlatformServices::PlatformOfflineQueueDiagnostics>);
static_assert(!std::is_same_v<Horo::PlatformServices::PlatformOfflineIntentId, Horo::PlatformOfflineQueue::PlatformOfflineIntentId>);
static_assert(!std::is_constructible_v<Horo::PlatformServices::PlatformOfflineOperation, Horo::PlatformServices::CloudMutationIntent>);
static_assert(!std::is_constructible_v<Horo::PlatformServices::PlatformOfflineOperation, Horo::PlatformServices::FriendsQuery>);
static_assert(!std::is_constructible_v<Horo::PlatformServices::PlatformOfflineOperation, Horo::PlatformServices::LeaderboardRankedQuery>);
