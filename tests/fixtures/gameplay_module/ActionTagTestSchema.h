#pragma once

#include "Horo/Network/ReplicationDeclarationPolicy.h"

namespace Horo::Tests {
    inline constexpr auto ActionTagModule = "game.action_tag_test";

    /** @brief Exact signed scalar declaration shared by the fixture module and product policy. */
    inline Network::ReplicationDeclaration ActionTagDeclaration() {
        return {.schema = {.id = Network::ReplicationSchemaId::Create(1002).Value(),
                           .version = {1, 0},
                           .compatibility = {{1, 0}, {1, 0}},
                           .owner = {.value = ActionTagModule},
                           .fields = {{.id = Network::FieldId::Create(1).Value(),
                                       .valueType = Network::ReplicationValueTypeId::Create(2).Value(),
                                       .codec = Network::ReplicationCodecId::Create(2).Value(),
                                       .introducedVersion = {1, 0},
                                       .limits = {8, 1}}}}};
    }
}  // namespace Horo::Tests
