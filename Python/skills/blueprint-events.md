# Blueprint event graph authoring

## When to use

Reach for this skill when laying down event-driven Blueprint logic:
hooking `ReceiveBeginPlay` / `ReceiveTick`, declaring a Custom Event,
or wiring a UMG widget's `OnClicked` to a handler function in the
same Blueprint. Pairs with the variables skill (state) and the
component skill (the thing the event drives).

## The canonical UE5 path

Every event lives on a Blueprint graph: usually the EventGraph
(`UbergraphPages[0]`), occasionally a function graph for a
Blueprint-implementable event override. The two node classes
that matter are `UK2Node_Event` (override-style: `ReceiveBeginPlay`,
`ReceiveOnComponentBeginOverlap`, etc., declared on a parent class)
and `UK2Node_CustomEvent` (declared inside the Blueprint itself, the
designer picks the name and signature). UMG widget events flow
through a third class, `UK2Node_ComponentBoundEvent`: the editor's
"+ event" picker on a button's `OnClicked` spawns one of these and
wires it to the child widget's FObjectProperty on the WBP's
generated class. The runtime wiring engine reads
`Blueprint->Bindings` (an array of `FBlueprintComponentDelegateBinding`
rows) and hooks each bound event up at construction.

## Our wrappers in this fork

- `bp_nodes` with `class=event` spawns an override-style
  `UK2Node_Event` in the chosen graph; pass `event_name` (e.g.
  `ReceiveBeginPlay`) and optionally `event_class` to disambiguate
  when more than one parent class declares the same override.
- `bp_nodes` with `class=custom_event` spawns a `UK2Node_CustomEvent`
  named through the `event_name` arg.
- `bp_input add_action_event_node` spawns a
  `UK2Node_EnhancedInputAction` for an Input Action and (optionally)
  wires the chosen trigger exec pin into a named function call.
- `widget_edit add_event_binding` spawns a `UK2Node_ComponentBoundEvent`
  for a child widget's multicast delegate (the UMG "+ event" path).
  The first call writes the row; a second call with the same
  `(widget, event)` pair reuses the existing node.

## Gotchas

- Override events must match a parent-class virtual exactly: typos
  spawn a CustomEvent with the same name instead.
- A UMG bound event needs the child widget to be exposed as a BP
  variable. The `add_event_binding` op flips `bIsVariable` + compiles
  when needed, but `add_child_widget` defaults to `expose_as_variable=true`
  so you rarely hit that path.
- `bp_nodes` does not auto-wire exec pins. Plan the edge list (the
  `bp_wire` call) at the same time you plan the node list.
- Compile is not automatic on `bp_nodes` / `bp_wire`. Land
  `bp_commit` at the end of the batch.
