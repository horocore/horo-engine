# Network debugger source parameter migration

The new debugger producer and application control methods borrow
`const NetworkDiagnosticSource&` for each synchronous call, avoiding repeated
copies of the process/session/scene identity. Activation state, transport consumers
and queued capture commands still copy that identity into their own storage;
no argument reference is retained after the call.

Existing calls remain source compatible, including temporary aggregate arguments.
Implementations of `INetworkDebuggerControl::Request` must update their override
parameter to `const Network::NetworkDiagnosticSource&`. Rebuild consumers with the
matching headers/library before using this newly introduced API.
