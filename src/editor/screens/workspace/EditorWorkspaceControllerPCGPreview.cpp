#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::HasPCGPreviewGrant */
    bool EditorWorkspaceController::HasPCGPreviewGrant() const noexcept {
        return m_pcgPreviewCatalog.IsValid() && m_pcgPreviewCatalog.Capabilities().granted.Contains(PCG::PCGCapability::EditorPreview);
    }

    /** @copydoc EditorWorkspaceController::RequestPCGPreview */
    Result<void> EditorWorkspaceController::RequestPCGPreview(DocumentInstanceId instance, CancellationToken cancellation) {
        if (m_pcgDocumentsShutdown || !m_pcgPreviewJobs || !m_pcgPreviewInputs)
            return Result<void>::Failure(MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Closed)));
        if (!HasPCGPreviewGrant())
            return Result<void>::Failure(MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Unauthorized)));
        auto document = m_pcgDocuments.Snapshot(instance);
        if (document.HasError())
            return Result<void>::Failure(document.ErrorValue());
        auto found = std::ranges::find(m_pcgPreviews, instance, &PCGPreviewOwner::document);
        if (found == m_pcgPreviews.end()) {
            if (m_pcgPreviews.size() >= 8 || m_nextPCGPreviewWorld == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Limit)));
            auto session = std::make_unique<PCGPreviewSession>(*m_pcgPreviewJobs, PCG::PCGWorldId::Create(m_nextPCGPreviewWorld).Value());
            m_pcgPreviews.push_back({instance, std::move(session)});
            ++m_nextPCGPreviewWorld;
            found = std::prev(m_pcgPreviews.end());
        }
        const auto &inputs = *m_pcgPreviewInputs;
        if (auto submitted = found->session->Request({document.Value(),
                                                      m_pcgPreviewCatalog,
                                                      inputs.spatial,
                                                      inputs.schema,
                                                      {},
                                                      inputs.evaluation,
                                                      inputs.maximumPointsPerOutput,
                                                      cancellation});
            submitted.HasError())
            return Result<void>::Failure(submitted.ErrorValue());
        UpdatePCGPreviews();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::UpdatePCGPreviews */
    void EditorWorkspaceController::UpdatePCGPreviews() {
        for (auto &owner : m_pcgPreviews) {
            const auto document = m_pcgDocuments.Snapshot(owner.document);
            auto current = owner.session->Advance(document.HasValue() ? &document.Value() : nullptr,
                                                  m_pcgPreviewInputs ? &m_pcgPreviewInputs->spatial : nullptr);
            const auto projection = std::ranges::find(m_viewModel.pcgDocuments, owner.document, [](const auto &entry) {
                return entry.snapshot.identity.instance;
            });
            if (projection != m_viewModel.pcgDocuments.end() && current.HasValue()) {
                projection->preview = std::move(current).Value();
                if (m_pcgPreviewInputs)
                    projection->previewRegion = m_pcgPreviewInputs->spatial.Bounds();
            }
        }
    }

    /** @copydoc EditorWorkspaceController::ReplacePCGPreviewInputs */
    Result<void> EditorWorkspaceController::ReplacePCGPreviewInputs(PCGPreviewInputs inputs) {
        if (m_pcgDocumentsShutdown)
            return Result<void>::Failure(MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Closed)));
        // Document query also enforces the workspace/document owner lane before touching preview state.
        if (const auto documents = m_pcgDocuments.Snapshots(); documents.HasError())
            return Result<void>::Failure(documents.ErrorValue());
        if (!inputs.schema || inputs.maximumPointsPerOutput == 0 || inputs.maximumPointsPerOutput > 65'536 ||
            inputs.evaluation.providerContent == Sha256Digest{})
            return Result<void>::Failure(MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Invalid)));
        for (auto &owner : m_pcgPreviews)
            static_cast<void>(owner.session->Cancel());
        m_pcgPreviewInputs = std::move(inputs);
        for (auto &projection : m_viewModel.pcgDocuments)
            projection.previewAvailable = m_pcgPreviewJobs && HasPCGPreviewGrant();
        UpdatePCGPreviews();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::RetirePCGPreview */
    void EditorWorkspaceController::RetirePCGPreview(DocumentInstanceId instance) {
        // Explicit document close only. Session destruction cancels/joins; panel focus/hide does not release jobs.
        std::erase_if(m_pcgPreviews, [instance](const auto &owner) {
            return owner.document == instance;
        });
    }
}  // namespace Horo::Editor
