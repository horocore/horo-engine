#include "Horo/Extensions/EditorCommandRegistry.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace Horo::Extensions {
    struct EditorCommandEntry final {
        EditorCommandDescriptor descriptor;
        EditorSurfaceContextRegistration contextRegistration;
        EditorSurfaceContext context;
        std::atomic_bool registered{true};

        EditorCommandEntry(EditorCommandDescriptor command, EditorSurfaceContextRegistration registration)
            : descriptor(std::move(command)), contextRegistration(std::move(registration)), context(contextRegistration.Context()) {}
    };

    struct EditorCommandRegistryState final {
        mutable std::mutex mutex;
        EditorCommandRegistryLimits limits;
        std::vector<std::shared_ptr<EditorCommandEntry>> entries;
        std::vector<EditorCommandDiagnostic> diagnostics;
        bool shutdown{};
    };

    namespace {
        [[nodiscard]] Result<void> Invalid(const std::string_view reason) {
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorCommandInvalid, std::string{reason}));
        }

        [[nodiscard]] bool IsCanonicalIdentity(const std::string_view value, const std::size_t maximumBytes) {
            if (value.empty() || value.size() > maximumBytes || value.front() == '.' || value.back() == '.')
                return false;
            bool previousDot = false;
            for (const unsigned char character : value) {
                if (character == '.') {
                    if (previousDot)
                        return false;
                    previousDot = true;
                    continue;
                }
                if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-' ||
                      character == '_'))
                    return false;
                previousDot = false;
            }
            return !previousDot;
        }

        [[nodiscard]] bool IsCanonicalLocalizationKey(const std::string_view value, const std::size_t maximumBytes) {
            return value.find('.') != std::string_view::npos && IsCanonicalIdentity(value, maximumBytes);
        }

        [[nodiscard]] bool IsKnownPredicate(const EditorCommandPredicateKind kind) noexcept {
            return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(EditorCommandPredicateKind::CapabilityAvailable);
        }

        [[nodiscard]] bool IsSupportedSurface(const EditorSurfaceKind kind) noexcept {
            return kind == EditorSurfaceKind::MenuItem || kind == EditorSurfaceKind::ToolbarAction || kind == EditorSurfaceKind::StatusItem;
        }

        [[nodiscard]] bool EqualsAsciiCaseInsensitive(const std::string_view left, const std::string_view right) noexcept {
            if (left.size() != right.size())
                return false;
            for (std::size_t index = 0; index < left.size(); ++index) {
                const auto lowercase = [](const unsigned char character) {
                    return character >= 'A' && character <= 'Z' ? static_cast<unsigned char>(character + ('a' - 'A')) : character;
                };
                if (lowercase(static_cast<unsigned char>(left[index])) != lowercase(static_cast<unsigned char>(right[index])))
                    return false;
            }
            return true;
        }

        enum class ShortcutModifier : std::uint8_t {
            None,
            Control,
            Shift,
            Alt,
            Meta,
        };

        [[nodiscard]] ShortcutModifier ParseShortcutModifier(const std::string_view token) noexcept {
            if (EqualsAsciiCaseInsensitive(token, "ctrl") || EqualsAsciiCaseInsensitive(token, "control"))
                return ShortcutModifier::Control;
            if (EqualsAsciiCaseInsensitive(token, "shift"))
                return ShortcutModifier::Shift;
            if (EqualsAsciiCaseInsensitive(token, "alt"))
                return ShortcutModifier::Alt;
            if (EqualsAsciiCaseInsensitive(token, "meta") || EqualsAsciiCaseInsensitive(token, "cmd") ||
                EqualsAsciiCaseInsensitive(token, "command"))
                return ShortcutModifier::Meta;
            return ShortcutModifier::None;
        }

        [[nodiscard]] std::optional<std::string> NormalizeShortcut(const std::string_view shortcut, const std::size_t maximumBytes) {
            if (shortcut.empty())
                return std::string{};
            if (shortcut.size() > maximumBytes || shortcut.front() == ' ' || shortcut.back() == ' ' ||
                !std::ranges::all_of(shortcut, [](const unsigned char character) {
                return character >= 0x21U && character <= 0x7eU;
            }))
                return std::nullopt;

            std::array<bool, 4> modifiers{};
            std::string_view key;
            std::size_t start = 0U;
            while (start <= shortcut.size()) {
                const std::size_t separator = shortcut.find('+', start);
                const std::size_t end = separator == std::string_view::npos ? shortcut.size() : separator;
                const std::string_view token = shortcut.substr(start, end - start);
                if (token.empty())
                    return std::nullopt;

                const ShortcutModifier modifier = ParseShortcutModifier(token);
                if (modifier == ShortcutModifier::None) {
                    if (!key.empty())
                        return std::nullopt;
                    key = token;
                } else {
                    const std::size_t modifierIndex = static_cast<std::size_t>(modifier) - 1U;
                    if (modifiers[modifierIndex])
                        return std::nullopt;
                    modifiers[modifierIndex] = true;
                }

                if (separator == std::string_view::npos)
                    break;
                start = separator + 1U;
            }
            if (key.empty())
                return std::nullopt;

            std::string normalized;
            normalized.reserve(shortcut.size());
            constexpr std::array<std::string_view, 4> modifierNames{"Ctrl", "Shift", "Alt", "Meta"};
            for (std::size_t index = 0; index < modifiers.size(); ++index) {
                if (!modifiers[index])
                    continue;
                if (!normalized.empty())
                    normalized += '+';
                normalized += modifierNames[index];
            }
            if (!normalized.empty())
                normalized += '+';
            for (const unsigned char character : key) {
                normalized +=
                    character >= 'a' && character <= 'z' ? static_cast<char>(character - ('a' - 'A')) : static_cast<char>(character);
            }
            if (normalized.size() > maximumBytes)
                return std::nullopt;
            return normalized;
        }

        [[nodiscard]] bool ContainsCapability(const EditorSurfaceDescriptor &surface, const std::string_view capability) {
            return std::ranges::any_of(surface.requiredCapabilities, [capability](const ExtensionCapabilityId &declared) {
                return declared.value == capability;
            });
        }

        [[nodiscard]] Result<void> ValidateRegistryLimits(const EditorCommandRegistryLimits &limits) {
            if (limits.maximumCommands == 0U || limits.maximumPredicates == 0U || limits.maximumIdentityBytes == 0U ||
                limits.maximumShortcutBytes == 0U || limits.maximumLocalizationKeyBytes == 0U || limits.maximumDiagnostics == 0U)
                return Invalid("Editor command registry limits must be non-zero.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateSurfaceAndCommandId(const EditorSurfaceContext &context,
                                                               const EditorCommandDescriptor &descriptor,
                                                               const EditorCommandRegistryLimits &limits) {
            if (!context.IsUsable())
                return Result<void>::Failure(MakeError(ExtensionErrors::EditorCommandProviderRevoked));
            if (!IsSupportedSurface(context.Surface().kind))
                return Invalid("Editor commands require a menu, toolbar, or status surface.");
            if (!IsCanonicalIdentity(descriptor.id.value, limits.maximumIdentityBytes))
                return Invalid("Editor command ID must be a canonical lowercase identity.");
            if (!context.Allows(EditorSurfaceCommandId{descriptor.id.value}))
                return Invalid("Editor command ID is not allowlisted by its surface context.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateLocalizationKeys(const EditorSurfaceContext &context, const EditorCommandDescriptor &descriptor,
                                                            const EditorCommandRegistryLimits &limits) {
            if (!IsCanonicalLocalizationKey(descriptor.labelLocalizationKey, limits.maximumLocalizationKeyBytes) ||
                !context.Allows(EditorSurfaceLocalizationKey{descriptor.labelLocalizationKey}))
                return Invalid("Editor command label must be a bounded localization key allowlisted by its surface context.");
            if (!descriptor.tooltipLocalizationKey.empty() &&
                (!IsCanonicalLocalizationKey(descriptor.tooltipLocalizationKey, limits.maximumLocalizationKeyBytes) ||
                 !context.Allows(EditorSurfaceLocalizationKey{descriptor.tooltipLocalizationKey})))
                return Invalid("Editor command tooltip must be a bounded localization key allowlisted by its surface context.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePredicates(const EditorSurfaceContext &context, const EditorCommandDescriptor &descriptor,
                                                      const EditorCommandRegistryLimits &limits) {
            if (descriptor.predicates.size() > limits.maximumPredicates)
                return Invalid("Editor command predicate count exceeds the configured limit.");

            for (std::size_t index = 0; index < descriptor.predicates.size(); ++index) {
                const EditorCommandPredicate &predicate = descriptor.predicates[index];
                if (!IsKnownPredicate(predicate.kind))
                    return Invalid("Editor command predicate kind is unsupported.");
                const bool requiresOperand = predicate.kind == EditorCommandPredicateKind::SurfaceOpen ||
                                             predicate.kind == EditorCommandPredicateKind::CapabilityAvailable;
                if (requiresOperand != !predicate.operand.empty())
                    return Invalid("Editor command predicate operand shape is invalid.");
                if (requiresOperand && !IsCanonicalIdentity(predicate.operand, limits.maximumIdentityBytes))
                    return Invalid("Editor command predicate operand must be a canonical identity.");
                if (predicate.kind == EditorCommandPredicateKind::CapabilityAvailable) {
                    const ExtensionCapabilityId capability{predicate.operand};
                    if (!ContainsCapability(context.Surface(), predicate.operand) || context.AcquireCapabilityUse(capability).HasError())
                        return Invalid("Editor command capability predicate is not declared and granted by its surface.");
                }
                for (std::size_t previous = 0; previous < index; ++previous) {
                    if (descriptor.predicates[previous] == predicate)
                        return Invalid("Editor command predicates must be unique.");
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateDescriptor(const EditorSurfaceContext &context, EditorCommandDescriptor &descriptor,
                                                      const EditorCommandRegistryLimits &limits) {
            if (Result<void> validation = ValidateRegistryLimits(limits); validation.HasError())
                return validation;
            if (Result<void> validation = ValidateSurfaceAndCommandId(context, descriptor, limits); validation.HasError())
                return validation;
            if (Result<void> validation = ValidateLocalizationKeys(context, descriptor, limits); validation.HasError())
                return validation;
            if (!descriptor.shortcut.empty()) {
                std::optional<std::string> normalizedShortcut = NormalizeShortcut(descriptor.shortcut, limits.maximumShortcutBytes);
                if (!normalizedShortcut.has_value())
                    return Invalid("Editor command shortcut is not a valid bounded shortcut identity.");
                descriptor.shortcut = std::move(*normalizedShortcut);
            }
            if (Result<void> validation = ValidatePredicates(context, descriptor, limits); validation.HasError())
                return validation;
            return Result<void>::Success();
        }

        [[nodiscard]] std::shared_ptr<EditorCommandEntry> FindEntry(const std::shared_ptr<EditorCommandRegistryState> &state,
                                                                    const std::string_view id) {
            std::scoped_lock lock{state->mutex};
            const auto found = std::ranges::find_if(state->entries, [id](const std::shared_ptr<EditorCommandEntry> &entry) {
                return entry->descriptor.id.value == id;
            });
            return found == state->entries.end() ? nullptr : *found;
        }

        void RemoveEntry(const std::shared_ptr<EditorCommandRegistryState> &state,
                         const std::shared_ptr<EditorCommandEntry> &entry) noexcept {
            bool revokeContext = false;
            if (state != nullptr) {
                std::scoped_lock lock{state->mutex};
                revokeContext = entry->registered.exchange(false, std::memory_order_acq_rel);
                std::erase(state->entries, entry);
            } else {
                revokeContext = entry->registered.exchange(false, std::memory_order_acq_rel);
            }
            if (revokeContext)
                entry->contextRegistration.Reset();
        }

        void AppendDiagnostic(EditorCommandRegistryState &state, EditorCommandDiagnostic diagnostic) {
            if (state.diagnostics.size() < state.limits.maximumDiagnostics)
                state.diagnostics.push_back(std::move(diagnostic));
        }

        [[nodiscard]] bool ContainsSurface(const std::span<const std::string_view> surfaces, const std::string_view id) noexcept {
            return std::ranges::find(surfaces, id) != surfaces.end();
        }

        [[nodiscard]] bool ContainsCapability(const std::span<const ExtensionCapabilityId> capabilities,
                                              const std::string_view id) noexcept {
            return std::ranges::any_of(capabilities, [id](const ExtensionCapabilityId &capability) {
                return capability.value == id;
            });
        }

        [[nodiscard]] bool PredicateMatches(const EditorCommandPredicate &predicate,
                                            const EditorCommandEvaluationContext &evaluation) noexcept {
            bool value = false;
            switch (predicate.kind) {
                case EditorCommandPredicateKind::ProjectOpen:
                    value = evaluation.projectOpen;
                    break;
                case EditorCommandPredicateKind::SelectionPresent:
                    value = evaluation.selectionPresent;
                    break;
                case EditorCommandPredicateKind::SurfaceOpen:
                    value = ContainsSurface(evaluation.openSurfaceIds, predicate.operand);
                    break;
                case EditorCommandPredicateKind::CapabilityAvailable:
                    value = ContainsCapability(evaluation.availableCapabilities, predicate.operand);
                    break;
            }
            return value == predicate.expected;
        }

        [[nodiscard]] bool IsEnabled(const EditorCommandEntry &entry, const EditorCommandEvaluationContext &evaluation) noexcept {
            return std::ranges::all_of(entry.descriptor.predicates, [&evaluation](const EditorCommandPredicate &predicate) {
                return PredicateMatches(predicate, evaluation);
            });
        }
    }  // namespace

    EditorCommandRegistration::EditorCommandRegistration(std::weak_ptr<EditorCommandRegistryState> registry,
                                                         std::shared_ptr<EditorCommandEntry> entry) noexcept
        : registry_(std::move(registry)), entry_(std::move(entry)) {}

    EditorCommandRegistration::~EditorCommandRegistration() noexcept {
        Reset();
    }

    EditorCommandRegistration::EditorCommandRegistration(EditorCommandRegistration &&other) noexcept
        : registry_(std::move(other.registry_)), entry_(std::move(other.entry_)) {}

    EditorCommandRegistration &EditorCommandRegistration::operator=(EditorCommandRegistration &&other) noexcept {
        if (this == &other)
            return *this;
        Reset();
        registry_ = std::move(other.registry_);
        entry_ = std::move(other.entry_);
        return *this;
    }

    /** @copydoc EditorCommandRegistration::Reset */
    void EditorCommandRegistration::Reset() noexcept {
        if (entry_ == nullptr)
            return;
        RemoveEntry(registry_.lock(), entry_);
        entry_.reset();
        registry_.reset();
    }

    /** @copydoc EditorCommandRegistration::IsRegistered */
    bool EditorCommandRegistration::IsRegistered() const noexcept {
        return entry_ != nullptr && entry_->registered.load(std::memory_order_acquire) && entry_->context.IsUsable();
    }

    /** @copydoc EditorCommandRegistration::Id */
    const EditorCommandId &EditorCommandRegistration::Id() const noexcept {
        static const EditorCommandId empty{};
        return entry_ == nullptr ? empty : entry_->descriptor.id;
    }

    EditorCommandRegistry::EditorCommandRegistry(const EditorCommandRegistryLimits &limits)
        : state_(std::make_shared<EditorCommandRegistryState>()) {
        state_->limits = limits;
        state_->entries.reserve(limits.maximumCommands);
        state_->diagnostics.reserve(limits.maximumDiagnostics);
    }

    EditorCommandRegistry::~EditorCommandRegistry() noexcept {
        BeginShutdown();
    }

    EditorCommandRegistry &EditorCommandRegistry::operator=(EditorCommandRegistry &&other) noexcept {
        if (this == &other)
            return *this;
        BeginShutdown();
        state_ = std::move(other.state_);
        return *this;
    }

    /** @copydoc EditorCommandRegistry::Register */
    Result<EditorCommandRegistration> EditorCommandRegistry::Register(EditorSurfaceContextRegistration context,
                                                                      EditorCommandDescriptor descriptor) {
        if (state_ == nullptr)
            return Result<EditorCommandRegistration>::Failure(MakeError(ExtensionErrors::EditorCommandShutdown));
        Result<void> validation = ValidateDescriptor(context.Context(), descriptor, state_->limits);
        if (validation.HasError())
            return Result<EditorCommandRegistration>::Failure(std::move(validation).ErrorValue());

        std::shared_ptr<EditorCommandEntry> displaced;
        std::shared_ptr<EditorCommandEntry> entry;
        {
            std::scoped_lock lock{state_->mutex};
            if (state_->shutdown)
                return Result<EditorCommandRegistration>::Failure(MakeError(ExtensionErrors::EditorCommandShutdown));

            if (const auto duplicate = std::ranges::find_if(state_->entries,
                                                            [&descriptor](const std::shared_ptr<EditorCommandEntry> &registeredEntry) {
                return registeredEntry->descriptor.id.value == descriptor.id.value;
            });
                duplicate != state_->entries.end()) {
                AppendDiagnostic(*state_, EditorCommandDiagnostic{.kind = EditorCommandDiagnosticKind::DuplicateId,
                                                                  .commandId = descriptor.id.value,
                                                                  .conflictingCommandId = (*duplicate)->descriptor.id.value,
                                                                  .detail = "The command ID is already published."});
                return Result<EditorCommandRegistration>::Failure(MakeError(ExtensionErrors::EditorCommandDuplicate, descriptor.id.value));
            }

            if (state_->entries.size() >= state_->limits.maximumCommands) {
                const auto lowestPriority = std::ranges::min_element(state_->entries, [](const std::shared_ptr<EditorCommandEntry> &left,
                                                                                         const std::shared_ptr<EditorCommandEntry> &right) {
                    return left->descriptor.priority < right->descriptor.priority;
                });
                if (lowestPriority == state_->entries.end() || descriptor.priority <= (*lowestPriority)->descriptor.priority)
                    return Result<EditorCommandRegistration>::Failure(MakeError(ExtensionErrors::EditorCommandCapacityExceeded));
                displaced = *lowestPriority;
            }

            if (!descriptor.shortcut.empty()) {
                const auto shortcutConflict =
                    std::ranges::find_if(state_->entries,
                                         [&descriptor, &displaced](const std::shared_ptr<EditorCommandEntry> &registeredEntry) {
                    return registeredEntry != displaced && registeredEntry->descriptor.shortcut == descriptor.shortcut;
                });
                if (shortcutConflict != state_->entries.end()) {
                    AppendDiagnostic(*state_, EditorCommandDiagnostic{.kind = EditorCommandDiagnosticKind::ShortcutConflict,
                                                                      .commandId = descriptor.id.value,
                                                                      .conflictingCommandId = (*shortcutConflict)->descriptor.id.value,
                                                                      .shortcut = descriptor.shortcut,
                                                                      .detail = "The shortcut is already claimed by another command."});
                    return Result<EditorCommandRegistration>::Failure(
                        MakeError(ExtensionErrors::EditorCommandShortcutConflict, descriptor.shortcut));
                }
            }

            entry = std::make_shared<EditorCommandEntry>(std::move(descriptor), std::move(context));
            if (displaced != nullptr) {
                (void)displaced->registered.exchange(false, std::memory_order_acq_rel);
                std::erase(state_->entries, displaced);
            }
            state_->entries.push_back(entry);
        }

        if (displaced != nullptr)
            displaced->contextRegistration.Reset();
        return Result<EditorCommandRegistration>::Success(EditorCommandRegistration{state_, std::move(entry)});
    }

    /** @copydoc EditorCommandRegistry::Evaluate */
    Result<EditorCommandState> EditorCommandRegistry::Evaluate(const std::string_view id,
                                                               const EditorCommandEvaluationContext &evaluation) const {
        if (state_ == nullptr)
            return Result<EditorCommandState>::Failure(MakeError(ExtensionErrors::EditorCommandShutdown));
        const std::shared_ptr<EditorCommandEntry> entry = FindEntry(state_, id);
        if (entry == nullptr)
            return Result<EditorCommandState>::Failure(MakeError(ExtensionErrors::EditorCommandUnknown, std::string{id}));
        if (!entry->registered.load(std::memory_order_acquire) || !entry->context.IsUsable())
            return Result<EditorCommandState>::Failure(MakeError(ExtensionErrors::EditorCommandProviderRevoked));
        return Result<EditorCommandState>::Success(EditorCommandState{.enabled = IsEnabled(*entry, evaluation)});
    }

    /** @copydoc EditorCommandRegistry::Invoke */
    Result<EditorCommandInvocation> EditorCommandRegistry::Invoke(const std::string_view id,
                                                                  const EditorCommandEvaluationContext &evaluation) const {
        if (state_ == nullptr)
            return Result<EditorCommandInvocation>::Failure(MakeError(ExtensionErrors::EditorCommandShutdown));
        const std::shared_ptr<EditorCommandEntry> entry = FindEntry(state_, id);
        if (entry == nullptr)
            return Result<EditorCommandInvocation>::Failure(MakeError(ExtensionErrors::EditorCommandUnknown, std::string{id}));
        if (!entry->registered.load(std::memory_order_acquire) || !entry->context.IsUsable())
            return Result<EditorCommandInvocation>::Failure(MakeError(ExtensionErrors::EditorCommandProviderRevoked));
        if (!IsEnabled(*entry, evaluation))
            return Result<EditorCommandInvocation>::Failure(MakeError(ExtensionErrors::EditorCommandNotEnabled, std::string{id}));

        EditorCommandInvocation invocation{
            .id = entry->descriptor.id,
            .provider = entry->context.Provider(),
            .context = entry->context,
        };
        if (!entry->registered.load(std::memory_order_acquire) || !entry->context.IsUsable())
            return Result<EditorCommandInvocation>::Failure(MakeError(ExtensionErrors::EditorCommandProviderRevoked));
        return Result<EditorCommandInvocation>::Success(std::move(invocation));
    }

    /** @copydoc EditorCommandRegistry::Snapshot */
    std::vector<EditorCommandSnapshot> EditorCommandRegistry::Snapshot() const {
        if (state_ == nullptr)
            return {};
        std::scoped_lock lock{state_->mutex};
        std::vector<EditorCommandSnapshot> snapshot;
        snapshot.reserve(state_->entries.size());
        for (const std::shared_ptr<EditorCommandEntry> &entry : state_->entries) {
            if (!entry->context.IsUsable())
                continue;
            snapshot.push_back(EditorCommandSnapshot{
                .command = entry->descriptor,
                .surface = entry->context.Surface(),
                .provider = entry->context.Provider(),
            });
        }
        std::ranges::sort(snapshot, [](const EditorCommandSnapshot &left, const EditorCommandSnapshot &right) {
            if (left.command.order != right.command.order)
                return left.command.order < right.command.order;
            if (left.command.priority != right.command.priority)
                return left.command.priority > right.command.priority;
            return left.command.id.value < right.command.id.value;
        });
        return snapshot;
    }

    /** @copydoc EditorCommandRegistry::Diagnostics */
    std::vector<EditorCommandDiagnostic> EditorCommandRegistry::Diagnostics() const {
        if (state_ == nullptr)
            return {};
        std::scoped_lock lock{state_->mutex};
        return state_->diagnostics;
    }

    /** @copydoc EditorCommandRegistry::BeginShutdown */
    void EditorCommandRegistry::BeginShutdown() noexcept {
        if (state_ == nullptr)
            return;
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return;
        state_->shutdown = true;
        for (const std::shared_ptr<EditorCommandEntry> &entry : state_->entries) {
            entry->registered.store(false, std::memory_order_release);
            entry->contextRegistration.Reset();
        }
        state_->entries.clear();
    }

    /** @copydoc EditorCommandRegistry::IsShutdown */
    bool EditorCommandRegistry::IsShutdown() const noexcept {
        if (state_ == nullptr)
            return true;
        std::scoped_lock lock{state_->mutex};
        return state_->shutdown;
    }
}  // namespace Horo::Extensions
