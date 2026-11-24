# Add Component menu

[AddComponentMenu.cpp](AddComponentMenu.cpp) draws available creation actions.
[AddComponentRegistry.h](AddComponentRegistry.h) defines the inert menu metadata and context,
and [BuiltinAddComponentDefinitions.cpp](BuiltinAddComponentDefinitions.cpp) assembles the
[built-in definitions](definitions/README.md). The workspace owns the submitted action and document mutation.
