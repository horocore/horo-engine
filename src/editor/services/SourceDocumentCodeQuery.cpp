#include "Horo/Editor/SourceDocumentCodeQuery.h"

#include <format>
#include <thread>

namespace Horo::Editor {
    namespace {
        /** @brief Retains producer lifetimes and the exact owner-thread/project fence for synchronous existing-text reads. */
        struct SourceReadOwner final {
            std::shared_ptr<const SourceDocumentService> documents;
            std::shared_ptr<const DocumentIdentityRegistry> identities;
            std::thread::id thread;
            std::string identity;
            std::uint64_t generation;

            /** @brief Reads an already-open immutable snapshot; a wrong thread never touches either producer. */
            Result<std::optional<Application::CodeQueryTextSnapshot>> Capture(const std::string_view path,
                                                                              const Application::CodeQueryContext &context) const {
                using Observation = std::optional<Application::CodeQueryTextSnapshot>;
                if (std::this_thread::get_id() != thread)
                    return Result<Observation>::Failure(MakeError(SourceDocumentErrors::WrongThread));
                if (context.projectIdentity != identity || context.projectGeneration != generation)
                    return Result<Observation>::Failure(MakeError(Application::CodeQueryErrors::Stale));
                if (auto stop = Platform::CheckProjectReadContext(context); stop.HasError())
                    return Result<Observation>::Failure(stop.ErrorValue());
                auto source = SourceDocumentId::Parse(path);
                if (source.HasError())
                    return Result<Observation>::Failure(source.ErrorValue());
                const auto existing = identities->Find(DocumentOpenKey{DocumentKind::Source, std::move(source).Value()});
                if (!existing)
                    return Result<Observation>::Success(std::nullopt);
                auto snapshot = documents->Snapshot(existing->instance);
                if (snapshot.HasError())
                    return Result<Observation>::Failure(snapshot.ErrorValue());
                if (snapshot.Value().Identity() != *existing || snapshot.Value().Revision() == 0)
                    return Result<Observation>::Failure(MakeError(Application::CodeQueryErrors::Stale));
                if (snapshot.Value().Text().size() > (1U << 20U))
                    return Result<Observation>::Failure(MakeError(Application::CodeQueryErrors::Capacity));
                auto bytes = std::make_shared<const std::string>(snapshot.Value().Text());
                const std::string revision = std::format("source:{}:{}", existing->instance.Value(), snapshot.Value().Revision());
                if (auto stop = Platform::CheckProjectReadContext(context); stop.HasError())
                    return Result<Observation>::Failure(stop.ErrorValue());
                return Result<Observation>::Success(
                    Application::CodeQueryTextSnapshot{identity, generation, std::string{path}, revision, std::move(bytes)});
            }
        };
    }  // namespace

    /** @copydoc MakeSourceDocumentCodeQueryProviders */
    Application::CodeQueryProviders MakeSourceDocumentCodeQueryProviders(std::shared_ptr<const SourceDocumentService> documents,
                                                                         std::shared_ptr<const DocumentIdentityRegistry> identities,
                                                                         std::string projectIdentity,
                                                                         const std::uint64_t projectGeneration) {
        Application::CodeQueryProviders providers;
        if (!documents || !identities)
            return providers;
        providers.existingText =
            [read = SourceReadOwner{std::move(documents), std::move(identities), std::this_thread::get_id(), std::move(projectIdentity),
                                    projectGeneration}](const std::string_view path, const Application::CodeQueryContext &context) {
            return read.Capture(path, context);
        };
        return providers;
    }
}  // namespace Horo::Editor
