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

## Blueprint authoring (medium to large each)

- `bp_create` — create Actor / Pawn / Character / GameMode / etc. Blueprints with a parent class.
- `bp_class` — read or change the parent class on an existing Blueprint.
- `bp_variable` — declare typed variables, expose as instance editable, set defaults.
- `bp_component` — add components (StaticMesh / Skeletal / Camera / Spring Arm / custom).
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

- `scene_query` — find actors by class / label / tag with spatial filters.
- `scene_brief` — short level summary.
- `scene_compose` — declarative spawn / modify / delete.
- `actor_inspect` — single-actor read with components and properties.
- `level_inspect` — current level + sublevels + streaming volumes.
- `search_assets` — Content Browser search.
- `asset_references` — dependency graph for an asset.
- `project_context` — project settings, plugins, content roots.

## Materials & shading (large)

- `material_inspect` — read material / instance / parameter collection.
- `material_edit` — create materials, instances, functions, parameter
  collections; author expression graphs. The MaterialExpression API is verbose;
  starting with a constrained "set parameter on a material instance" cut would
  be a small-to-medium pass.

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

- `python_execution` — run Python in-editor against the unreal module.
- `unreal_api` — query the 15,000+ entry API surface.
- `skills` — fetch on-demand workflow docs.

## Suggested next-pass shortlist for a single-player game project

After the latest pass (`bp_input`, `asset_factory` data asset variant,
`widget_inspect`), the next set should pick up:

1. `material_edit` (small variant: create material instance + set scalar /
   vector / texture parameters) — supports basic look development. The
   MaterialExpression API is verbose, so authoring expression graphs is a
   later pass.
2. `bp_input` (graph wiring) — `K2Node_EnhancedInputAction` setup on top of
   the existing `add_node` / `connect_nodes` helpers, so an agent can wire
   an InputAction into a Blueprint's event graph in one call.
3. `bp_component` (small) — add SkeletalMesh / Camera / SpringArm
   components to existing Blueprints alongside the current
   `add_component_to_blueprint`.
4. `scene_query` (small) — lift the existing `find_actors_by_name` /
   `get_actors_in_level` calls into a single multiplexed query with class
   and tag filters.

The `widget_edit` slot-property surface (alignment, fill, padding) is small
follow-up work if the consumer game needs it during smoke-testing.
