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
- `asset_factory` (DataTable variant only, small) — create a DataTable asset
  with a configurable row UScriptStruct.

## Blueprint authoring (medium to large each)

- `bp_create` — create Actor / Pawn / Character / GameMode / etc. Blueprints with a parent class.
- `bp_class` — read or change the parent class on an existing Blueprint.
- `bp_variable` — declare typed variables, expose as instance editable, set defaults.
- `bp_component` — add components (StaticMesh / Skeletal / Camera / Spring Arm / custom).
- `bp_graph` — create or fetch event / function graphs by name.
- `bp_nodes` — batched node creation across an event graph (event nodes, branch, sequence, casts).
- `bp_wire` — connect / disconnect named pins between nodes.
- `bp_input` — bind Enhanced Input action and axis events.
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

- `widget_inspect` — read widget tree, named slots, styles, MVVM bindings.
- `widget_edit` — create Widget Blueprint with named child widgets, basic
  styling, animations, event bindings. A "create widget BP + add Text /
  Button / Image with named slots" cut is small to medium.

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

- `asset_factory` (Enum) — create UEnum assets with named entries.
- `asset_factory` (Struct) — create UScriptStruct assets with typed members.
- `asset_factory` (DataAsset) — create UPrimaryDataAsset subclasses.
- `asset_factory` (Enhanced Input bundle) — create InputAction + InputMappingContext + IA_Lookup.

## Editor & diagnostics (medium each)

- `editor_log` — read the Output Log filtered by category and verbosity.
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

These are the next 3 to 5 we should pick up after the user smoke-tests the
current batch:

1. `widget_edit` (small variant: create Widget Blueprint with named child
   widgets) — directly unblocks the Phase 4 crafting menu.
2. `material_edit` (small variant: create material instance + set scalar /
   vector parameters) — supports basic look development.
3. `bp_input` (small) — Enhanced Input bindings are needed for any new gameplay
   feature.
4. `editor_log` (small) — easy win, useful for every workflow.
5. `asset_factory` Enum + Struct — needed once we extend the crafting system.
