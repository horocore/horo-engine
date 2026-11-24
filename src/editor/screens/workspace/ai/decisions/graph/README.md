# Decision graph presentation

[EditorWorkspaceViewDecisionGraph.cpp](EditorWorkspaceViewDecisionGraph.cpp) presents the
document through [DecisionGraphCanvas.cpp](DecisionGraphCanvas.cpp). The canvas model is
separate from node placement in [DecisionGraphLayout.cpp](DecisionGraphLayout.cpp).
Document mutations go through the [decision graph owner](../README.md).
