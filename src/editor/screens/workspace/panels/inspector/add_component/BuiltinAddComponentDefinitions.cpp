#include "AddComponentRegistry.h"
#include "definitions/AudioListener.h"
#include "definitions/AudioSource.h"
#include "definitions/BoxCollider.h"
#include "definitions/Camera.h"
#include "definitions/CharacterController.h"
#include "definitions/Joint.h"
#include "definitions/Light.h"
#include "definitions/RigidBody.h"
#include "definitions/Script.h"
#include "definitions/TriggerCollider.h"
#include "definitions/TriggerVolume.h"

namespace Horo::Editor {
    namespace {
        constexpr std::array definitions{
            AddComponentDefinitions::Camera,
            AddComponentDefinitions::Light,
            AddComponentDefinitions::AudioSource,
            AddComponentDefinitions::AudioListener,
            AddComponentDefinitions::TriggerVolume,
            AddComponentDefinitions::Script,
            AddComponentDefinitions::RigidBody,
            AddComponentDefinitions::BoxCollider,
            AddComponentDefinitions::TriggerCollider,
            AddComponentDefinitions::Joint,
            AddComponentDefinitions::CharacterController,
        };

        /** @brief Rejects malformed or colliding built-in identities before runtime composition. */
        constexpr bool ValidDefinitions() {
            for (std::size_t i = 0; i < definitions.size(); ++i) {
                if (definitions[i].id.empty() || definitions[i].labelKey.empty() || definitions[i].available == nullptr)
                    return false;
                for (std::size_t j = 0; j < i; ++j)
                    if (definitions[i].id == definitions[j].id)
                        return false;
            }
            return true;
        }

        static_assert(ValidDefinitions());
    }  // namespace

    /** @copydoc BuiltinAddComponentDefinitions */
    std::span<const BuiltinAddComponentDefinition> BuiltinAddComponentDefinitions() noexcept {
        return definitions;
    }
}  // namespace Horo::Editor
