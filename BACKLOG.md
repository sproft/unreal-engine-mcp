# Sproft fork: hosted Flop tool backlog

Tracks the gap between the hosted Flop MCP tool surface (~58 tools, 50+ doc'd
in `README.md`) and what exists in the local Python server. The shipped fork
additions live next to this file; what remains is listed below with a one-line
spec lifted from the README and a difficulty estimate (small / medium / large).

All future work in this list must remain clean-room: derived from the public
UE5 API and the documented behaviour, never from the proprietary FlopAI plugin.

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

## Blueprint authoring (medium to large each)

- `bp_create` — create Actor / Pawn / Character / GameMode / etc. Blueprints with a parent class.
- `bp_class` — read or change the parent class on an existing Blueprint.
- `bp_variable` — declare typed variables, expose as instance editable, set defaults.
- `bp_component` (single-add variant ships in this fork) — broader hosted
  surface still pending: move / rename / remove component nodes, deeper
  per-component property control, and full reparenting under an arbitrary
  attach socket name.
- `bp_graph` — create or fetch event / function graphs by name.
- `bp_nodes` — batched node creation across an event graph (event nodes, branch, sequence, casts).
- `bp_wire` — connect / disconnect named pins between nodes.
- `bp_input` (asset side ships in this fork) — wire Enhanced Input action
  events into a Blueprint's event graph. The data asset side
  (`create_input_action`, `create_input_mapping_context`, `add_mapping`)
  is shipped; the graph-side `K2Node_EnhancedInputAction` wiring on top of
  the existing `add_node` / `connect_nodes` helpers is still pending.
- `bp_commit` — compile and save with verification.
- `bp_author` — high-level "write me a feature" composite.
- `bp_dry_run` — verify what `bp_commit` would do without applying.
- `bp_skills` — list available Blueprint authoring skills.

The local repo already implements `add_node`, `connect_nodes`,
`create_variable`, etc. The hosted batched variants would sit on top of those
helpers.

## Blueprint inspection (medium)

- `bp_brief` — read-only orientation summary of a Blueprint.
- `bp_inspect` — 21 targeted query operations (variables, components, graphs, parents).
- `bp_export` — full GraphSpec JSON export.

## Scene & level (medium each)

- `scene_query` (small variant ships in this fork) — broader hosted scope
  still pending: bounding-box / convex-volume spatial filters, actor
  component listings as part of each record, and multi-tag / boolean tag
  filters.
- `scene_brief` — short level summary.
- `scene_compose` (small variant ships in this fork) — broader hosted
  scope still pending: batched spawn / modify / delete in a single call,
  prefab / level snippet rollouts, and child-actor reparenting.
- `actor_inspect` (small variant ships in this fork) — broader hosted
  scope still pending: full component child-actor recursion, deeper
  per-component property control, and component-by-name lookups inline.
- `level_inspect` — current level + sublevels + streaming volumes.
- `search_assets` — Content Browser search.
- `asset_references` — dependency graph for an asset.
- `project_context` — project settings, plugins, content roots.

## Materials & shading (large)

- `material_inspect` — read material / instance / parameter collection.
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
- `tag_registry_edit` — Gameplay Tags.

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

After the latest pass (`actor_inspect`, `scene_compose`, `python_execution`),
the next set should pick up:

1. `bp_input` (graph wiring) — `K2Node_EnhancedInputAction` setup on top of
   the existing `add_node` / `connect_nodes` helpers, so an agent can wire
   an InputAction into a Blueprint's event graph in one call. The asset
   side (`bp_input`) and the underlying graph helpers already exist; this
   is mostly a node-class registration plus an exec-pin route to a named
   function on the same Blueprint.
2. `material_edit` (expressions) — extend the small variant with material
   expression graph authoring. The verbose `UMaterialExpression*` surface
   is the main cost; a "create texture sample wired into BaseColor" cut is
   a reasonable second hop.
3. `scene_brief` — short level summary on top of `scene_query`. Pulls
   counts, level / sublevel names, streaming volume names, and a small
   selection of "interesting" actors (player start, post process volumes,
   directional lights). Cheap once `scene_query` is in place.
4. `level_inspect` — read-only dump of the active level plus loaded
   sublevels, world settings, world partition state where present.

`python_execution` now covers any operation we have not wrapped natively;
prefer wrapping the high-frequency calls (`bp_brief`, `bp_inspect`) as
dedicated tools so the agent does not need to author Python every time.

The `widget_edit` slot-property surface (alignment, fill, padding) is small
follow-up work if the consumer game needs it during smoke-testing.
