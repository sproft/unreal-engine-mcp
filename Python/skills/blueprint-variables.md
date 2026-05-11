# Blueprint variables

## When to use

Reach for this skill when adding state to a Blueprint: a `Health` int
on the character, a `bIsAiming` bool, an array of `FInventoryItem`,
a `UMaterialInstance*` reference for runtime swapping. Pairs with
the components skill (the runtime SCS tree) and the events skill
(the graph that mutates the state).

## The canonical UE5 path

Blueprint variables live on the asset's `NewVariables` array
(`TArray<FBPVariableDescription>`). Each entry carries a typed
`FEdGraphPinType` (Category / SubCategory / SubCategoryObject /
PinValueType for maps / ContainerType for arrays / sets / maps),
a default value, a category label, and a property-flag set
(BlueprintReadOnly / BlueprintReadWrite / Instance-editable /
ExposeOnSpawn / Replicated, etc.). The compiler turns each entry
into an FProperty on the generated class. The canonical authoring
entry points are `FBlueprintEditorUtils::AddMemberVariable`,
`RemoveMemberVariable`, `SetBlueprintVariableProperty`, and
`SetBlueprintVariableMetaData`; the kismet schema picks the right
FProperty subclass off the FEdGraphPinType through
`FBlueprintEditorUtils::GetPropertyForVariable` and friends.

## Our wrappers in this fork

- `bp_variable list` returns every variable with its typed shape,
  default, category, friendly name, and flag set. Use it to verify
  state before authoring a new variable.
- `bp_variable add` declares a typed variable. `type` accepts
  scalars (`bool` / `int` / `float` / `string` / `name`), structs
  (`vector` / `rotator` / `transform` / `color`), object refs
  (`/Script/Module.ClassName`), Blueprint refs (`/Game/.../BP`
  auto-suffixed `_C`), and user structs (`struct:/Game/...`).
  `container` is `single` (default), `array`, `set`, or `map` (map
  needs `value_type`). Flag toggles plus a `default` payload land
  in the same call.
- `bp_variable remove`, `set_default`, and `set_flags` cover the
  obvious follow-on ops.
- `bp_create properties` lands a flat property dict on the CDO
  before the first compile so callers can seed default values for
  inherited parent-class variables in one shot (no need to
  re-declare what the parent already provides).

## Gotchas

- Map variables need `value_type` AND `container=map` together;
  passing one without the other fails closed.
- Object references resolve to the BP's generated class
  (`_C`-suffixed) when the caller passes a `/Game/...` path; ship
  the `_C`-stripped form and the resolver appends it.
- `bp_variable add` compiles by default. Pass `compile=false` if
  you are about to batch more edits before `bp_commit`.
- `set_default` accepts a JSON literal that flows through the
  property's `ImportText`; nested struct literals (e.g. a Vector
  default) need the `(X=1,Y=2,Z=3)` shape strings, not raw JSON
  objects.
