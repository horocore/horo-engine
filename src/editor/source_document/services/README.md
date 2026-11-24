# Source document services

[SourceFileOpenService.cpp](SourceFileOpenService.cpp) opens source through the
project-scoped file service. [SourceEditingCommands.cpp](SourceEditingCommands.cpp)
maps editor actions to document edits.
[SourceLanguageSession.cpp](SourceLanguageSession.cpp) tracks revisions sent to the
language provider and rejects stale results.

The [source document owner](../) keeps text and controls edits and saves.
