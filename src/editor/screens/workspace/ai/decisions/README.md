# Decision graph documents

[EditorWorkspaceDecisionGraph.cpp](EditorWorkspaceDecisionGraph.cpp) coordinates decision graph
commands and document state. Its conflict and recovery adapters handle revision changes, while
[DecisionSaveJournal.cpp](DecisionSaveJournal.cpp) tracks save recovery.
See [graph presentation](graph/README.md) for the canvas and [authoring panels](panels/README.md)
for node properties, task choices, and problems.
