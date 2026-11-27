# Source documents

[SourceDocumentService.cpp](SourceDocumentService.cpp) owns open source documents,
their identities, and their revisions. The snapshot and text helpers support
versioned reads and edits. [SourceDocumentChanges.cpp](SourceDocumentChanges.cpp)
handles multi-document changes, and [SourceDocumentSave.cpp](SourceDocumentSave.cpp)
owns the source save workflow with disk operations in
[SourceDocumentBatchDisk.cpp](SourceDocumentBatchDisk.cpp).

[`services/`](services/) contains editor commands, file opening, and language
session adapters. [Language providers](../language/) and
[syntax tooling](../source_editor/) consume the document state.
See the [source save migration](../../../docs/migrations/HORO-230_source_document_save.md)
for the persistence contract.
