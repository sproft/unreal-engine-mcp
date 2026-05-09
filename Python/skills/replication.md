# Replication

When to use this skill: any time we need to keep state in sync between
the server and connected clients in a multiplayer UE5 project. Reach
for it when a value driving gameplay needs to be authoritative on the
server (health, score, inventory, character state) or when the clients
need a server-driven RPC to fire (a damage burst, a pickup, an ability
trigger). If the value never leaves the local instance, replication is
not the right answer.

The canonical UE5 path: AActor and UActorComponent both expose a
`bReplicates` flag plus the `GetLifetimeReplicatedProps` static
override. Properties annotated with `UPROPERTY(Replicated)` are added
to the lifetime list through the `DOREPLIFETIME` macro inside that
override, and changes to them propagate from the server to clients on
the next replication tick. RPCs fall on member functions tagged
`UFUNCTION(Server, Reliable)`, `UFUNCTION(Client, Reliable)`, or
`UFUNCTION(NetMulticast, Reliable)` and run on the role indicated by
the prefix. For per-property notifications the `UPROPERTY(ReplicatedUsing = OnRep_Foo)`
form invokes `OnRep_Foo` after each replication on the receiving role.
For Blueprints the same surface is reached through the Replication
section of each variable's details panel and through the
`Replicates`, `Net Multicast`, `Run on Server`, or `Run on owning
Client` function settings. The actor itself must have its `Replicates`
flag enabled and must be part of the persistent level (or spawned by
an authority).

Our wrappers in this fork: `bp_variable` exposes the per-variable
replication condition surface through the `replicated` and
`rep_notify` fields of the `set_flags` op, so a Blueprint variable can
become replicated (with optional notify) without leaving the MCP
session. `bp_class` reads the parent class's `bReplicates` default and
the implemented interfaces. `bp_export` includes per-variable
replication / instance-editable / expose-on-spawn flags. `actor_inspect`
returns each actor's replication snapshot. Heavier work (a per-RPC
authoring op, a `replication_audit` cross-asset pass) stays on the
backlog.

Gotchas: actors must have `bReplicates=true` AND be spawned by the
authority; client-spawned actors do not replicate. A property that is
a `UObject` reference replicates the reference, not the object's
state, so the referenced object also has to replicate or be a class
default. Replicated arrays on `TArray` produce a delta-replication
stream; very-frequent edits (per-tick mutations) will overwhelm the
stream, so prefer fast-array structs for those. RPCs are dropped
silently when the network owner does not match the prefix (Server RPC
called on the server, Client RPC called on a non-owner). Always test
with at least one dedicated server PIE instance plus a client.
