# Sproft fork: hosted Flop tool backlog

Tracks the gap between the hosted Flop MCP tool surface (~58 tools, 50+ doc'd
in `README.md`) and what exists in the local Python server. The shipped fork
additions live next to this file; what remains is listed below with a one-line
spec lifted from the README and a difficulty estimate (small / medium / large).

All future work in this list must remain clean-room: derived from the public
UE5 API and the documented behaviour, never from the proprietary FlopAI plugin.

This pass shipped `bp_nodes`, `bp_wire`, and `material_inspect`. Together
they let a caller author non-trivial Blueprint logic in three calls
(`bp_nodes` -> `bp_wire` -> `compile_blueprint`) and diagnose a
material before mutating it, which closes the biggest two BACKLOG gaps
flagged on the prior pass.

## Shipped in this fork

- `editor_actions` (small) — save / undo / redo / focus selection / play / stop play.
- `window_capture` (small) — synchronous PNG screenshot of the active viewport.
- `asset_factory` (small) — create DataTable / Enum / Struct / DataAsset
  assets. The Enum variant takes a list of entry names; the Struct variant
  takes a list of `{name, type}` field specs covering the standard scalar
  and small-struct types plus `/Game/`-rooted UScriptStruct paths; the
  DataAsset variant accepts a target UDataAsset class and an optional
  flat property dict applied through `FProperty::ImportText_InContainer`.
- `widget_edit` (small) — create a UWidgetBlueprint (`create_widget_blueprint`)
  and add a typed child widget (`add_child_widget`, e.g. vertical_box,
  progress_bar, text_block, button, image) under a parent panel by FName.
- `widget_inspect` (small) — read-only counterpart to `widget_edit`. Walks
  the UWidgetTree, returns the nested hierarchy, a flat widget list, any
  named slots, and the asset's user-declared variables (excluding entries
  that are themselves widget tree members).
- `editor_log` (small) — tail the project's on-disk log file with optional
  category and minimum-verbosity filters; write a single line through GLog
  under a `LogSproftMCP` category.
- `bp_input` (small) — Enhanced Input data asset factory. Three operations:
  create a `UInputAction` (Boolean / Axis1D / Axis2D / Axis3D), create an
  empty `UInputMappingContext`, and append one key-to-action binding row
  through `UInputMappingContext::MapKey`.
- `bp_component` (small) — add a `UActorComponent` subclass to an existing
  Blueprint's `SimpleConstructionScript`. Accepts a short class name or full
  `/Script/Module.ClassName` path, an optional `parent_component` for
  attachment under an existing scene component, and an optional flat
  property dict applied through `FProperty::ImportText` on the template.
  Compiles and saves on success.
- `scene_query` (small) — read-only multiplexed actor query for the editor
  world. Combines class (substring or exact), `name_pattern`,
  `label_pattern`, single `tag`, and an optional spherical spatial filter
  with a result limit. Returns class / name / label / location / rotation /
  scale / tags / mobility / hidden flags per actor.
- `material_edit` (small) — three operations: `create_material` (with an
  optional `Constant3Vector` base-colour driver wired into `BaseColor`),
  `create_material_instance_constant` from a parent UMaterialInterface, and
  `set_instance_parameter` for scalar / vector / texture overrides on a
  UMaterialInstanceConstant. Expression-graph authoring and Material
  Parameter Collections remain in BACKLOG.md.
- `actor_inspect` (small) — read-only counterpart to `scene_query` for a
  single actor. Resolves the actor by `GetName()` first and then by
  Outliner label, returns transform / tags / replication snapshot / root
  component, and (when asked) the full component tree with each
  component's class, relative transform, attach parent / socket, tags, and
  a short `FProperty::ExportText` value dump per component or per actor.
- `scene_compose` (small) — declarative single-actor scene mutation.
  Three operations on one actor per call: `spawn` (class path + optional
  transform / preferred FName / Outliner label / tags / flat property
  dict), `modify` (partial transform / label / tags / property patch on
  an actor resolved by name or label), and `delete`. Property dicts apply
  through `FProperty::ImportText` on the actor instance.
- `python_execution` (small) — run Python in the editor's interpreter
  through `IPythonScriptPlugin::ExecPythonCommandEx`. Two operations:
  `execute_string` (a string of source, multi-statement by default) and
  `execute_file` (a `.py` path on disk with optional positional args).
  Returns stdout / stderr / command_result and the structured log array.
  Requires `PythonScriptPlugin`; the uplugin manifest references it so
  consumer projects auto-enable it.
- `scene_brief` (small) — read-only one-shot orientation summary of the
  active editor world: persistent level name + path, attached streaming
  sublevels, total actor count + per-class counts, world bounds union,
  GameMode override + default pawn class, level-blueprint has-user-events
  flag, deduplicated tags in use, and a short list of notable landmark
  actors (player starts, directional lights, post-process volumes).
- `level_inspect` (small) — read-only structured per-actor record list for
  the editor world plus any loaded sublevels. Sits between `scene_brief`
  and `scene_query`: always returns a uniformly-shaped per-actor block
  plus a per-level summary, with optional `class` / `name_pattern` /
  `label_pattern` / `tag` / `level_filter` filters and an optional
  `include_components` toggle for a compact per-component list.
- `bp_input` (graph wiring extension) — `add_action_event_node` operation
  on the existing `bp_input` tool. Spawns a `UK2Node_EnhancedInputAction`
  in a target Blueprint's event graph for a given `UInputAction` asset,
  reusing an existing node for the same action. Optionally MakeLinkTo's
  the chosen trigger exec pin (default "Triggered") to a named function
  call on the same Blueprint through a `UK2Node_CallFunction` follow-on.
- `tag_registry_edit` (small) — manage Gameplay Tags through the editor
  module. Three operations: `add_tag` writes a tag (with optional dev
  comment) into a chosen `Config/Default*Tags.ini` source through
  `IGameplayTagsEditorModule::AddNewGameplayTagToINI`; `remove_tag`
  deletes a tag through `IGameplayTagsEditorModule::DeleteTagFromINI`;
  `list_tags` is a read-only substring search over
  `UGameplayTagsManager::RequestAllGameplayTags` returning each tag's
  owning source name, source ini path, and dev comment. The editor
  module handles ini rewrites, tag-tree refresh, and the broadcast that
  live tag pickers listen on.
- `bp_create` (small) — create a UBlueprint asset with a chosen parent
  class through `FKismetEditorUtilities::CreateBlueprint`. Resolves the
  parent class from a short name (Actor, Pawn, Character, ActorComponent,
  SceneComponent, GameMode, GameModeBase, PlayerController, AIController,
  UserWidget, DataAsset, BlueprintFunctionLibrary, etc.), a full
  `/Script/Module.ClassName` path, or a `/Game/...` Blueprint class
  path. Output package path is configurable under `/Game/`. An optional
  flat property dict applies through `FProperty::ImportText` on the
  generated CDO before the first compile. Compiles and saves on success.
- `bp_brief` (small) — read-only one-page orientation summary of a
  Blueprint asset. Returns name, path, parent class (short + full path),
  blueprint type, variable count, function count, macro count,
  event-graph node count, named-event list (UK2Node_Event +
  UK2Node_CustomEvent), SCS component summary, implemented Blueprint
  interfaces, and a data-only flag.
- `bp_inspect` (small) — read-only targeted query operations on a
  Blueprint asset, keyed by `op`. `list_variables`, `list_functions`,
  `list_events`, `list_components`, and `find_node` (substring against
  node short class name and / or node title across all graphs).
- `bp_variable` (small) — declarative Blueprint variable management.
  One multi-op tool covering `list` / `add` / `remove` / `set_default` /
  `set_flags` against `UBlueprint::NewVariables`. The `add` path uses a
  wider type resolver than the existing local helper: scalar tokens,
  built-in structs (vector, rotator, transform, color, linear_color),
  `/Script/Module.ClassName` object refs, `/Game/...` Blueprint class
  refs (auto-suffixed with `_C`), and `struct:/...` UScriptStruct
  paths. Container types cover `single` / `array` / `set` / `map` (with
  a `value_type` for the map case). Each mutating op compiles + saves
  on success unless `compile=false` or `save=false`.
- `bp_class` (small) — manage class-level settings on an existing
  UBlueprint. One multi-op tool covering `read` / `set_parent` /
  `set_class_settings` / `add_interface` / `remove_interface`. The
  reparent path mirrors the editor's flow: assigns the new ParentClass,
  runs `RefreshAllNodes` and `MarkBlueprintAsStructurallyModified`,
  then recompiles. The interface ops use the `FTopLevelAssetPath`
  overloads of `ImplementNewInterface` / `RemoveInterface` and accept
  short names, full `/Script/Module.IName` paths, or `/Game/...`
  Blueprint Interface paths. `set_class_settings` writes the
  BlueprintOptions property surface (description, display name,
  namespace, category, hide categories) under `WITH_EDITORONLY_DATA`.
- `bp_graph` (small) — read-only graph traversal beyond `bp_inspect`.
  One multi-op tool covering `list_graphs` / `list_nodes` / `get_node`
  / `list_connections`. `list_graphs` walks `UbergraphPages`,
  `FunctionGraphs`, `MacroGraphs`, and each
  `ImplementedInterfaces[*].Graphs`. `list_nodes` accepts substring
  filters on node class and title and caps at 256 by default.
  `get_node` returns full pin info (direction, type, default value /
  object, exec / data flag) plus each pin's connected targets.
  `list_connections` returns a flat edge list with exec / data filters
  and a 1024-edge cap.
- `bp_nodes` (small) — batched K2 node creation in a chosen graph.
  Defaults to the first event graph; pass `graph` to target a
  function / macro / interface graph. Supports the most-used K2 node
  classes (variable_get, variable_set, call_function, branch /
  if_then_else, dynamic_cast, self, format_text, execution_sequence,
  knot, make_array, custom_event, event), optional FName + position +
  pin defaults per entry. Compile is NOT automatic so a caller can
  stitch wires through `bp_wire` first and run a single
  `compile_blueprint` at the end of the batch.
- `bp_wire` (small) — declarative connect / disconnect of named pins
  between named nodes in a Blueprint graph. Validates pin direction
  and runs `UEdGraphSchema_K2::CanCreateConnection` for type checks.
  Per-entry `disconnect=true` breaks an existing wire instead of
  making a new one; `op="disconnect"` is the per-call shortcut.
- `material_inspect` (small) — read-only material / material instance
  dump. For UMaterial: expression list (each with FName, class,
  position, parameter name), parameter set (scalar / vector / texture
  / static_switch), per-attribute connected output expression for
  BaseColor / Metallic / Specular / Roughness / Anisotropy / Normal /
  Tangent / EmissiveColor / Opacity / OpacityMask /
  WorldPositionOffset / AmbientOcclusion / Refraction / Displacement,
  and the used-texture list. For UMaterialInstance: parent material
  path, the parent's parameter list, and the instance's scalar /
  vector / texture overrides.

## Blueprint authoring (medium to large each)

- `bp_create` (small variant ships in this fork) — short-name parent
  class resolution, optional flat property dict on the CDO, compile +
  save on success. Open follow-ons: assign Blueprint interfaces at
  creation time, post-create `bp_class` reparenting, and post-create
  default-component dict beyond CDO properties.
- `bp_class` (small variant ships in this fork) — `read` / `set_parent`
  / `set_class_settings` / `add_interface` / `remove_interface`. Open
  follow-ons: structural fixups when the new parent removes parent
  members the BP still references (member-fixup), and per-class flag
  toggles (Const / Abstract / NotPlaceable) we don't expose yet.
- `bp_variable` (small variant ships in this fork) — `list` / `add` /
  `remove` / `set_default` / `set_flags` against `NewVariables`. Open
  follow-ons: per-variable replication-condition surface beyond the
  bool toggle, default-value parsing for nested struct literals through
  `FProperty::ImportText` (instead of the current raw-text fallback),
  and a `rename` op that fixes up downstream Get / Set nodes.
- `bp_component` (single-add variant ships in this fork) — broader hosted
  surface still pending: move / rename / remove component nodes, deeper
  per-component property control, and full reparenting under an arbitrary
  attach socket name.
- `bp_graph` (small read-only variant ships in this fork) —
  `list_graphs` / `list_nodes` / `get_node` / `list_connections`. Open
  follow-ons: a `get_or_create_event_graph` op for declarative graph
  authoring, function-graph creation with typed inputs / outputs in a
  single call, and a `compact` mode that returns a graph as a single
  edge-list dump (one payload, no per-node fan-out).
- `bp_nodes` (small variant ships in this fork) — batched K2 node
  creation in a chosen graph for the most-used node classes
  (variable_get, variable_set, call_function, branch, dynamic_cast,
  self, format_text, execution_sequence, knot, make_array,
  custom_event, event). Open follow-ons: SwitchEnum / SwitchInteger /
  SwitchString / SwitchName, MakeStruct / BreakStruct, MathExpression,
  AddComponentByClass, and a per-call `auto_wire` flag that infers
  obvious exec connections from a sequential `nodes` array.
- `bp_wire` (small variant ships in this fork) — connect / disconnect
  named pins through `MakeLinkTo` / `BreakLinkTo` with K2 schema
  compatibility checks. Open follow-ons: pin-default value writes
  during the same call, batched promoted-default literal nodes when a
  type does not match, and a `breakall` op that drops every wire on
  a named pin in one shot.
- `bp_input` (asset side + first graph-wiring slice ship in this fork) —
  the data asset side (`create_input_action`,
  `create_input_mapping_context`, `add_mapping`) and a focused
  `add_action_event_node` operation are shipped. Open follow-ons: select
  multiple trigger exec pins in one call, parameter-binding from the
  enhanced-input action value pin into the connected function, and
  pin-by-pin `set_node_property` overrides on the spawned node.
- `bp_commit` — compile and save with verification.
- `bp_author` — high-level "write me a feature" composite.
- `bp_dry_run` — verify what `bp_commit` would do without applying.
- `bp_skills` — list available Blueprint authoring skills.

The local repo already implements `add_node`, `connect_nodes`,
`create_variable`, etc. The hosted batched variants would sit on top of those
helpers.

## Blueprint inspection (medium)

- `bp_brief` (small variant ships in this fork) — read-only one-page
  orientation summary. Open follow-ons: per-variable replication-condition
  surface, deeper interface-method-by-method readout, and a "constructed
  archetype" comparison against the parent class CDO.
- `bp_inspect` (small variant ships in this fork) — `list_variables`,
  `list_functions`, `list_events`, `list_components`, `find_node`. Open
  follow-ons: per-pin readback for matched nodes, per-function
  parameter-list readout, and timeline / sequencer node summaries.
- `bp_export` — full GraphSpec JSON export.

## Scene & level (medium each)

- `scene_query` (small variant ships in this fork) — broader hosted scope
  still pending: bounding-box / convex-volume spatial filters, actor
  component listings as part of each record, and multi-tag / boolean tag
  filters.
- `scene_brief` (small variant ships in this fork) — broader hosted
  scope still pending: post-process volume settings inline, world
  partition state surface where present, world settings nav-mesh /
  lighting summary.
- `scene_compose` (small variant ships in this fork) — broader hosted
  scope still pending: batched spawn / modify / delete in a single call,
  prefab / level snippet rollouts, and child-actor reparenting.
- `actor_inspect` (small variant ships in this fork) — broader hosted
  scope still pending: full component child-actor recursion, deeper
  per-component property control, and component-by-name lookups inline.
- `level_inspect` (small variant ships in this fork) — broader hosted
  scope still pending: streaming-volume listings with their bound
  ULevelStreaming entries, World Partition cell state, world settings
  fragments inline.
- `search_assets` — Content Browser search.
- `asset_references` — dependency graph for an asset.
- `project_context` — project settings, plugins, content roots.

## Materials & shading (large)

- `material_inspect` (small variant ships in this fork) — read-only
  dump of a UMaterial or UMaterialInstance: expression list, parameter
  set across scalar / vector / texture / static_switch, per-attribute
  connected output expression for the standard GBuffer attributes,
  used texture list, and instance overrides. Open follow-ons:
  Material Function and Material Parameter Collection inspection,
  per-expression connected-output dump (which output of which child
  drives each input of this expression), and an `include_graph`
  toggle that returns the expression graph as an edge list.
- `material_edit` (small variant ships in this fork: create material with
  a Constant3Vector base colour, create material instance constant, and
  set scalar / vector / texture parameters on an instance). Pending: full
  MaterialExpression-graph authoring, Material Functions, and Material
  Parameter Collections.

## VFX (large each)

- `niagara_inspect` — read Niagara system / emitter / module structure.
- `niagara_edit` — Niagara particle system editing.
- `niagara_script_edit` — reusable Niagara module authoring.
- `chaos_edit` — Geometry Collection destruction setup.

## Animation (large each)

- `animation_inspect` — read sequences, montages, BlendSpaces, AnimBP graphs.
- `animation_edit` — create / modify the same.
- `animation_graph_edit` — AnimBP state machines, transitions, blend nodes.
- `ik_rig_edit` — IK rig setup.
- `ik_retarget` — retarget animations.

## UMG / Widgets (medium)

- `widget_inspect` — the small variant ships in this fork. The remaining
  hosted-Flop scope (style readback, MVVM binding readback) is still
  outstanding.
- `widget_edit` — the small variant ships in this fork. The remaining
  hosted-Flop scope (animations, MVVM bindings, advanced styles, event
  binding, slot-property assignment beyond defaults) is still on the table.

## AI & abilities (large each)

- `behavior_tree` — BTs, Blackboards, AI Controllers, EQS.
- `gas_edit` — Gameplay Abilities, Effects, Attribute Sets.
- `tag_registry_edit` (small variant ships in this fork) — `add_tag`,
  `remove_tag`, `list_tags` through `IGameplayTagsEditorModule`. Open
  follow-ons: rename through `RenameTagInINI`, restricted-tag source
  creation through `AddNewGameplayTagSource`, and per-tag-asset usage
  search over the AssetRegistry for "where is this tag referenced".

## Landscape & foliage (large each)

- `landscape_inspect` — read landscape state.
- `landscape_edit` — sculpting, paint layers, heightmap import / export.
- `foliage_inspect` — read foliage instance state.
- `foliage_edit` — paint, scatter, remove instances.

## Cinematics & audio (large each)

- `sequencer_edit` — Level Sequences, camera cuts, transform tracks.
- `metasound_edit` — MetaSound graphs.
- `sound_asset_edit` — SoundCue graphs.

## Procedural (large)

- `pcg_graph_edit` — PCG graph authoring.

## Data assets (small to medium each, on the asset_factory umbrella)

- `asset_factory` (DataAsset) — shipped in this fork.
- `asset_factory` (Enhanced Input bundle) — superseded by the dedicated
  `bp_input` tool, which creates InputActions and InputMappingContexts
  individually and lets the agent bind keys at the row level.

## Editor & diagnostics (medium each)

- `editor_log` — the on-disk-log tail variant ships in this fork. A future
  pass could attach a buffering FOutputDevice to GLog so the tool can read
  log entries that arrived after the editor started without re-parsing the
  full file. We can also expose the in-editor SOutputLog widget filter
  helpers if a hook is added to the OutputLog module.
- `performance_audit` — gather perf stats and run a basic audit.
- `cpp_source` — read or write C++ source and trigger Live Coding.

## Runtime verification (large each)

- `pie_test_bp` — Blueprint test harness with assertions during PIE.
- `pie_test_scene` — scene-state assertion harness during PIE.

## Execution (large each)

- `python_execution` (small variant ships in this fork) — broader hosted
  scope still pending: persistent shared interpreter scope across calls,
  output streaming for long-running scripts, and richer typed result
  marshalling beyond the current stdout / stderr / repr capture.
- `unreal_api` — query the 15,000+ entry API surface.
- `skills` — fetch on-demand workflow docs.

## Suggested next-pass shortlist for a single-player game project

After the latest pass (`bp_nodes`, `bp_wire`, `material_inspect`), the
next set should pick up:

1. `material_edit` (expressions) — extend the small variant with
   MaterialExpression graph authoring. Three ops are the right cut:
   `add_expression` (short-name resolver against
   `UMaterialExpressionConstant3Vector` / `Multiply` / `Add` / `Lerp` /
   `TextureSampleParameter2D` / `ScalarParameter` / etc., going
   through `UMaterialEditingLibrary::CreateMaterialExpression`),
   `connect_expressions` (source expression + output name -> dest
   expression + input name, going through
   `UMaterialEditingLibrary::ConnectMaterialExpressions`), and
   `set_expression_property` (constant value, parameter name, default
   scalar applied through `FProperty::ImportText`).
2. `bp_function_create` — function-graph creation with typed inputs /
   outputs in a single call. The local repo has `create_function`
   plus `add_function_input` / `add_function_output`; the gap is a
   declarative one-call wrapper that lays the function down with its
   signature in one round trip.
3. `niagara_inspect` — read-only Niagara dump (system / emitter list,
   module list per emitter, parameter readback). Pairs with
   `material_inspect` for the VFX side.
4. `bp_commit` — batched compile + save with verification. Surfaces
   the engine's compiler output and a structural diff so a long
   authoring chain (`bp_nodes` -> `bp_wire` -> `bp_commit`) reports
   one consolidated outcome.

`python_execution` still covers any operation we have not wrapped
natively; prefer wrapping the high-frequency calls as dedicated tools
so the agent does not need to author Python every time.

The `widget_edit` slot-property surface (alignment, fill, padding) is
small follow-up work if the consumer game needs it during smoke-testing.
