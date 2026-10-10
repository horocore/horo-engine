#pragma once

#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveOperationArbiter.h"
#include "SaveTestUtils.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime::ArbiterTestSupport {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Test;

    inline SaveNamespaceId NameSpace() {
        return {.product = Id<ProductStorageId>(1),
                .environment = Id<EnvironmentStorageId>(2),
                .owner = ServerWorldOwner{.owner = Id<ServerStorageOwnerId>(3)}};
    }

    inline SaveArbiterRequest Request(const OperationId operation, const SaveOperationKind kind = SaveOperationKind::Save,
                                      const std::uint8_t slot = 1, const SaveArbiterPriority priority = SaveArbiterPriority::Normal,
                                      const SaveArbiterConflictPolicy conflict = SaveArbiterConflictPolicy::Queue,
                                      const SavePolicyMode mode = SavePolicyMode::Manual) {
        return {.operation = {.operation = operation, .kind = kind, .maximumCompletionCallbacks = 4},
                .mode = mode,
                .address = kind == SaveOperationKind::RefreshCatalog
                               ? std::nullopt
                               : std::optional<SaveArbiterAddress>{{.nameSpace = NameSpace(), .slot = Id<SaveGameSlotId>(slot)}},
                .priority = priority,
                .conflict = conflict};
    }

    inline SaveOperationArbiter Arbiter(const std::size_t capacity = 8) {
        auto created = CreateSaveOperationArbiter({.maximumRetainedOperations = capacity});
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    inline SaveArbiterAdmission Admit(SaveOperationArbiter &arbiter, SaveArbiterRequest request) {
        auto admitted = arbiter.Admit(std::move(request));
        REQUIRE(admitted.HasValue());
        return std::move(admitted).Value();
    }

    inline SaveArbiterSnapshot Snapshot(const SaveOperationArbiter &arbiter, const OperationId operation) {
        const auto snapshot = arbiter.Snapshot(operation);
        REQUIRE(snapshot.has_value());
        return *snapshot;
    }

    inline void RequireError(const Error &error, const ErrorCodeDescriptor &descriptor) {
        CHECK(error.domain.Value() == descriptor.domain.Value());
        CHECK(error.code.Value() == descriptor.code.Value());
    }
}  // namespace Horo::Runtime::ArbiterTestSupport
