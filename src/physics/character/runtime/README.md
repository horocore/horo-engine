# Character world runtime

The character world owner and the command pipeline that applies admitted changes.

Start with [CharacterWorld.cpp](CharacterWorld.cpp) for the controller lifecycle.
[CharacterWorldCommandPipeline.cpp](CharacterWorldCommandPipeline.cpp) stages movement commands and
publishes the solved results after the fixed-tick commit. The [private Scene contracts](../../internal/)
let the host compose that work with other owners.
