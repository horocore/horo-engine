#include "Horo/Physics/CharacterErrors.h"

namespace Horo::Character::CharacterErrors {
    namespace {
        const ErrorDomainId CharacterDomain{"horo.character"};

        /** @brief Creates one immutable Character error descriptor with the shared domain policy. */
        ErrorCodeDescriptor Descriptor(const std::string_view code, const std::string_view summary, const std::string_view remediationHint,
                                       const bool userActionable = false) {
            return {
                .domain = CharacterDomain,
                .code = ErrorCode{std::string{code}},
                .defaultSeverity = ErrorSeverity::Error,
                .summary = summary,
                .remediationHint = remediationHint,
                .retryable = false,
                .userActionable = userActionable,
            };
        }
    }  // namespace

    const ErrorCodeDescriptor WorldInvalid = Descriptor("character.world.invalid", "The Character world identity is invalid.",
                                                        "Use the non-zero Character world generation published with the active scene.");
    const ErrorCodeDescriptor HandleMalformed = Descriptor("character.handle.malformed", "The controller handle is malformed.",
                                                           "Use a controller handle with non-zero scene, world and slot generations.");
    const ErrorCodeDescriptor HandleWorldMismatch =
        Descriptor("character.handle.world_mismatch", "The controller handle belongs to another owner generation.",
                   "Resolve the stable controller binding against the active scene and Character world.");
    const ErrorCodeDescriptor HandleStale = Descriptor("character.handle.stale", "The controller slot is absent, retired or replaced.",
                                                       "Discard the handle and resolve its stable authored binding again.");
    const ErrorCodeDescriptor DescriptorInvalid =
        Descriptor("character.descriptor.invalid", "The controller descriptor is invalid.",
                   "Provide finite geometry, unit basis, valid filtering and coherent bounds.", true);
    const ErrorCodeDescriptor RequestInvalid = Descriptor("character.request.invalid", "The Character movement request is invalid.",
                                                          "Provide one finite, explicitly tick-addressed movement intent.");
    const ErrorCodeDescriptor CommandOrderInvalid =
        Descriptor("character.command.order_invalid", "The Character command order is invalid.",
                   "Submit one future tick-addressed command per producer sequence before that tick closes.");
    const ErrorCodeDescriptor CapacityExceeded =
        Descriptor("character.capacity.exceeded", "A Character operation exceeded its admitted bounded capacity.",
                   "Lower the requested contact count or admit a larger qualified profile.", true);
    const ErrorCodeDescriptor GenerationExhausted =
        Descriptor("character.generation.exhausted", "Every Character controller slot reached its generation ceiling.",
                   "Replace the Character world with a fresh process-local world generation.", true);
    const ErrorCodeDescriptor InvalidState =
        Descriptor("character.state.invalid", "The Character world lifecycle cannot admit this operation.",
                   "Submit work only during the declared fixed-tick owner phase.");
    const ErrorCodeDescriptor OperationUnsupported =
        Descriptor("character.operation.unsupported", "The Character operation contains an unknown typed value.",
                   "Use a stance, collision flag or operation supported by this contract version.", true);
}  // namespace Horo::Character::CharacterErrors
