# Enhanced Input

When to use this skill: any new input mapping in a UE5 project as of
5.1+. Enhanced Input replaced the legacy "Input Action / Axis" system
in `Project Settings -> Input` and is the only path Epic actively
maintains. Reach for it when we need to bind keyboard / mouse /
gamepad / touch input to gameplay actions, when we need contextual
mapping (drive-mode vs. on-foot mode), or when we need triggers and
modifiers (hold to activate, double-tap, dead zones, axis swizzle).
For UI-only input or a trivial console command, the legacy
`SetupPlayerInputComponent` path is still fine.

The canonical UE5 path: three asset shapes plus one component. A
`UInputAction` is the abstract verb (Jump, Move, Look). A
`UInputMappingContext` is a list of `{Action, Key, [Modifiers],
[Triggers]}` rows that map physical keys onto actions for a given
context. The `UEnhancedInputLocalPlayerSubsystem::AddMappingContext` /
`RemoveMappingContext` calls toggle which contexts are active for a
local player. The `UEnhancedInputComponent` lives on a Pawn or
PlayerController and wires actions to handler functions through
`BindAction(Action, ETriggerEvent::Triggered, this, &AFoo::OnJump)`.
At runtime the UE5 input system delivers an `FInputActionValue` (a
typed struct over Boolean / Axis1D / Axis2D / Axis3D) to the bound
handler; the action's `ValueType` decides which getter to use
(`Get<bool>()`, `Get<float>()`, `Get<FVector2D>()`).

Our wrappers in this fork: `bp_input` is the dedicated Enhanced Input
toolset. Four operations: `create_input_action` (Boolean / Axis1D /
Axis2D / Axis3D), `create_input_mapping_context` (an empty IMC at a
chosen `/Game/...` path), `add_mapping` (one `{Action, Key, [Modifiers],
[Triggers]}` row appended through `UInputMappingContext::MapKey`),
and `add_action_event_node` (spawns a `UK2Node_EnhancedInputAction`
in a target Blueprint's event graph for a given action and optionally
MakeLinkTo's the chosen trigger exec pin to a named function call).
The `asset_factory` tool also exposes an `enhanced_input_bundle`
variant that takes a list of action specs plus a list of mapping rows
and produces one IMC + N UInputActions in a single declarative call.

Gotchas: a UInputAction's `ValueType` decides the modifier shape, so
a Boolean action driven by an Axis1D key will silently coerce; use a
SwizzleInputAxisValues modifier when a 1D key needs to drive a 2D
action's Y axis. Modifiers and triggers are evaluated in order;
`UInputModifierNegate` after `UInputModifierDeadZone` sees the dead-
zoned value, not the raw key. The `EnhancedInput` plugin must be
enabled in the .uproject. The default project Pawn has to install the
mapping context (typically inside `BeginPlay` or `OnPossessed`) or
nothing fires. ETriggerEvent::Started fires once on press,
ETriggerEvent::Triggered fires every tick the trigger condition holds.
