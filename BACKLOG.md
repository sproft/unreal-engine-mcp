# Sproft fork: hosted Flop tool backlog

Tracks the gap between the hosted Flop MCP tool surface (~58 tools, 50+ doc'd
in `README.md`) and what exists in the local Python server. The shipped fork
additions live next to this file; what remains is listed below with a one-line
spec lifted from the README and a difficulty estimate (small / medium / large).

All future work in this list must remain clean-room: derived from the public
UE5 API and the documented behaviour, never from the proprietary FlopAI plugin.

The most recent pass shipped one minimum-cut runtime tool plus two
small edit slices on existing read-only inspectors: `pie_test_scene`
(scene-state assertion harness with two assertion kinds answered
statically against the editor world without driving Play in
Editor: `actor_exists` for actor-name presence and
`actor_at_location` for distance-bounded location matches),
`behavior_tree` edit slice (`create_behavior_tree` for a new
UBehaviorTree at a `/Game/...` path with an optional linked
Blackboard plus `add_root_composite` to assign a Selector /
Sequence / SimpleParallel as the tree's RootNode in two
declarative calls), and `sequencer_edit` edit slice
(`create_level_sequence` for a new ULevelSequence with the default
tick / display rates through `ULevelSequence::Initialize` plus
`add_possessable` to bind a named editor-world actor through
`UMovieScene::AddPossessable` + `BindPossessableObject`). Edit-
side ops requiring a running PIE world (`var_equals`,
`actor_overlapping_tag` for overlap-driven gameplay assertions)
plus the heavier branches on each edit slice (BT child append /
decorator insertion / blackboard key edits, Sequencer track add /
section move / spawnable creation / camera-cut creation) stay on
this list.

The pass before that shipped three small wide-domain read-only
tools that round out the orientation surface the agent reaches
for at the start of a session: `project_context` (one-shot
project summary covering identity, engine version, enabled
plugins filtered to user-installed, source modules, top-level
Content folders with asset counts, and the active map +
GameMode + default pawn through both per-level override and
project-wide default), `animation_inspect` (class-keyed dump
for USkeletalMesh / UAnimSequence / UAnimMontage / UBlendSpace
/ UAnimBlueprint covering bones / sockets / notifies / sections
/ slot tracks / blend-space axes / state machines), and
`cpp_source` (read header + cpp pair through
`FSourceCodeNavigation::FindClassHeaderPath` /
`FindClassSourcePath` for class-driven lookup, or extension-swap
inference for direct disk-path lookup, with per-file truncation
caps and existence flags).

The pass before that shipped three read-only inspectors covering
broader-domain coverage that was previously locked behind
`python_execution`: `landscape_inspect` (every `ALandscape` actor's
component-grid configuration, materials, layers, bounds, and
heightmap / weightmap texture sets), `foliage_inspect` (every
`AInstancedFoliageActor`'s foliage type list with mesh / actor source
paths, density / radius / scale intervals, instance counts, and
optional sampled world locations), and the read-only first slice of
`sequencer_edit` for `ULevelSequence` (master tracks, sections,
possessables, spawnables, plus tick / display frame rates and
playback range).

The pass before that shipped `bp_export`, `behavior_tree` (read-only
slice), the `widget_edit` slot-property surface, and `gas_edit`
(read-only slice). Together they close the canonical Blueprint
snapshot for diffable round-trips, give the agent a structured AI
asset dump that pairs with `niagara_inspect`, and make UMG slot
authoring reflective without us spelling out every UPanelSlot
subclass. `gas_edit` reads UGameplayAbility / UGameplayEffect /
UAttributeSet assets so a caller can answer "what tags drive what
ability" alongside `tag_registry_edit`.

## Shipped in this fork

- `editor_actions` (small) — save / undo / redo / focus selection / play / stop play.
- `window_capture` (small) — synchronous PNG screenshot of the active viewport.
- `asset_factory` (small) — create DataTable / Enum / Struct / DataAsset /
  Enhanced Input Bundle assets. The Enum variant takes a list of entry
  names; the Struct variant takes a list of `{name, type}` field specs
  covering the standard scalar and small-struct types plus `/Game/`-
  rooted UScriptStruct paths; the DataAsset variant accepts a target
  UDataAsset class and an optional flat property dict applied through
  `FProperty::ImportText_InContainer`; the enhanced_input_bundle variant
  takes a list of action specs (name + value_type) plus a list of
  mapping rows (action + key + optional negate / swizzle) and produces
  one UInputMappingContext plus N UInputAction assets in a single call.
  Existing assets at the target paths are reused unless `overwrite`
  is set.
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
- `material_edit` (small) — six operations: `create_material` (with an
  optional `Constant3Vector` base-colour driver wired into `BaseColor`),
  `create_material_instance_constant` from a parent UMaterialInterface,
  `set_instance_parameter` for scalar / vector / texture overrides on a
  UMaterialInstanceConstant, plus the expression-graph trio
  `add_expression` (short-name resolver against the most-used
  UMaterialExpression* subclasses, optional `properties` dict applied
  through `FProperty::ImportText`, optional one-shot connection to a
  material attribute or another expression input),
  `connect_expressions` (source expression output -> destination
  expression input or material attribute through
  `UMaterialEditingLibrary::ConnectMaterialExpressions` /
  `ConnectMaterialProperty`), and `set_expression_property` (flat
  property dict applied to a named expression). Each expression-graph
  op recompiles + saves on success unless `recompile=false` or
  `save=false` is passed. Material Functions and Material Parameter
  Collections remain on the backlog.
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
- `search_assets` (small) — Content-Browser-style asset search backed
  by `IAssetRegistry::GetAssets(FARFilter, ...)`. Filters: class
  (single token, list, or substring pattern, plus optional
  `include_subclasses`), path (single prefix or list, recursive by
  default), name pattern, and package-tag pairs mapped onto
  `FARFilter::TagsAndValues`. Returns each row's path, name, class,
  class_path, package, and package_path. Optional `include_disk_size`
  pulls the package's on-disk byte size through
  `IAssetRegistry::TryGetAssetPackageData`. The response carries
  `count`, `matched_total`, and a `limit_hit` flag so a caller can
  paginate by tightening the filter.
- `asset_references` (small) — read-only dependency graph for one
  asset through `IAssetRegistry::GetReferencers` /
  `GetDependencies`. `direction` selects one of `hard_referencers`,
  `soft_referencers`, `hard_dependencies`, `soft_dependencies`, or
  the `all_*` variants. Walks transitively up to `depth` (default 1,
  cap 6). Each row carries name, path, class, class_path, package,
  and package_path. Optional `class_filter` drops rows whose asset
  class does not match. Returns `count`, `matched_total`,
  `limit_hit`, and `depth_reached`.
- `bp_commit` (small) — convenience wrapper that runs the standard
  end-of-edit Blueprint cycle in one call:
  `MarkBlueprintAsStructurallyModified` (or the lighter
  `MarkBlueprintAsModified` when `mark_structurally=false`) plus
  `CompileBlueprint` with a captured `FCompilerResultsLog` plus
  `SaveAsset`. Surfaces compiler errors / warnings / infos as separate
  string arrays. Skips save when the compile produced errors so a
  broken Blueprint does not get pinned to disk; `force_save=true`
  overrides for diagnostic snapshots. Becomes the canonical end-of-edit
  step for designers chaining `bp_nodes` -> `bp_wire` -> `bp_commit`.
- `bp_function_create` (small) — declarative one-call wrapper for
  laying down a new Blueprint function with its full typed signature.
  Wraps `FBlueprintEditorUtils::CreateNewGraph` +
  `AddFunctionGraph<UClass>` plus the FunctionEntry / FunctionResult
  pin authoring. Inputs accept the same wide type resolver as
  `bp_variable` (scalar tokens, built-in structs, `/Script/...`,
  `/Game/...` BP class refs auto-suffixed with `_C`, `struct:/...`
  UScriptStruct paths) plus per-entry `is_array` / `is_reference`.
  Optional `pure` flag, `category`, `keywords`, `tooltip`, and
  `call_in_editor` toggles land directly on the entry node's
  `FKismetUserDeclaredFunctionMetadata`. Compiles and saves on
  success unless `compile=false` / `save=false`.
- `material_edit` (bulk `add_expressions` op) — extends the
  expression-graph trio with a single-call form that takes a list of
  expression specs (each with `class`, optional `name` alias,
  `position`, `properties` dict) and an optional list of edge specs
  (each `{source, source_output?, dest, dest_input?}` between
  expressions or `{source, property}` to a material attribute). The
  per-spec `name` alias lets a downstream connection reference an
  expression created earlier in the same call without waiting for
  the engine's resolved FName. Recompiles + saves once after the
  whole batch unless overridden. Cuts the round-trip count for typical
  panner-driven UV chain or normal-map setup workflows.
- `niagara_inspect` (small, read-only) — structured dump of a
  UNiagaraSystem asset. Returns the system-level spawn / update
  script paths and an `emitters` array. Each emitter dict reports
  `name`, `enabled`, `sim_target` (cpu / gpu), `local_space`,
  `determinism`, the per-stage script list grouped by execution stage
  (emitter spawn / emitter update / particle spawn / particle update
  / particle gpu compute), the event-handler chain, the
  simulation-stage class list, and the renderer class list. The
  `parameters` array enumerates every entry in
  `UNiagaraSystem::GetExposedParameters()` with name + type + kind
  (primitive / data_interface / object). Edit-side ops
  (niagara_edit / niagara_script_edit) remain on the backlog. Adds
  Niagara to UnrealMCP's PublicDependencyModuleNames and the
  uplugin manifest.
- `bp_export` (small, read-only) — canonical Blueprint snapshot.
  Returns name + path + parent class + blueprint type, the
  variables array (with type, default, friendly name, category,
  flag set), the SCS components array (with relative transform +
  optional `FProperty::ExportText` defaults dump), the implemented-
  interfaces array, and a `graphs` array covering every event /
  function / macro / interface graph. Each graph carries name +
  kind + node count + a node list (class, title, position, GUID,
  optional event / custom-event signature, capped pin list with
  default value / default object / link count) plus a flat edge
  list (source / target node + pin name + is_exec). Per-node pin
  output is capped at `max_pins_per_node` (default 64) and tagged
  `pins_truncated` when the cap fires. Sits next to the read-only
  `bp_brief` / `bp_inspect` / `bp_graph` triad and is the primary
  diff-able payload for verifying that an MCP-driven authoring
  session left a Blueprint in the expected state.
- `behavior_tree` (small) — multi-op tool keyed by `op`. The
  `inspect` op (default) is the read-only structured dump shipped
  earlier: full tree (composite root + recursive children +
  per-child decorator chain + per-composite service chain), the
  tree-level RootDecorators, plus the linked Blackboard (path,
  parent path, key list with name + type token + inner BaseClass /
  EnumType / Struct path for typed Object / Class / Enum / Struct
  keys, instance-sync flag, parent-inherited flag). SimpleParallel
  composites also report their FinishMode (immediate / delayed).
  The recursive walk caps at `max_depth` (default 32) and flips
  `children_truncated` on the offending composite.
  The new edit ops are `create_behavior_tree` (NewObject's a
  UBehaviorTree at a `/Game/...` path with an optional linked
  UBlackboardData and the standard `overwrite` escape hatch for
  an existing asset) and `add_root_composite` (NewObject's a
  Selector / Sequence / SimpleParallel composite under the tree
  and assigns it as `RootNode`; `replace=true` overrides the
  existing-RootNode guard). Both edit ops save by default. Pairs
  with `niagara_inspect` for AI assets. Heavier edit ops (append
  child task / composite, insert decorator, append service,
  blackboard key edits) remain on the backlog. Adds AIModule to
  PublicDependencyModuleNames.
- `widget_edit` slot-property surface — a third op `set_slot_property`
  on the existing `widget_edit` tool. Takes a target widget FName plus
  a flat property dict and applies the dict to the widget's UPanelSlot
  through `FProperty::ImportText_InContainer`. Covers UCanvasPanelSlot
  anchors / offsets / size / ZOrder, UVerticalBoxSlot /
  UHorizontalBoxSlot padding / fill / alignment, UOverlaySlot /
  UGridSlot, and any other UPanelSlot-derived class without us
  spelling out each property by name. Each entry that fails to
  resolve as a UPROPERTY or refuses ImportText is reported under
  `skipped` with a reason. After applying the dict the slot's
  `SynchronizeProperties()` runs so a re-layout tick picks the change
  up.
- `landscape_inspect` (small, read-only) — structured dump of every
  `ALandscape` actor in the editor world. Per-actor record covers
  name + label + transform + owning level + landscape GUID, the
  section-grid tuple (ComponentSizeQuads / SubsectionSizeQuads /
  NumSubsections / component_count), the proxy material driver and
  any hole-material override, world-space proxy bounds (min / max /
  size), the editor-only XY component-space extent rectangle when
  ULandscapeInfo is registered, the registered layer list (each
  entry with `layer_name`, `layer_info_object_path`,
  `phys_material`, `blend_method` enum byte, `is_no_blend`,
  `is_visibility_layer`), heightmap / weightmap texture
  deduplication counts plus opt-in package-path arrays, and an
  optional per-component records array (each with `name`,
  `section_base`, `weightmap_count`, `weightmap_layer_allocation_count`,
  `heightmap_path`). Filters: `name_pattern` (substring on actor
  name + label) and `level_filter` (substring on owning ULevel
  name). Adds `Landscape` to PublicDependencyModuleNames. Pairs
  with `foliage_inspect` for terrain reasoning. Edit-side ops
  (sculpting, paint layers, heightmap import / export) remain on
  the backlog.
- `foliage_inspect` (small, read-only) — structured dump of every
  `AInstancedFoliageActor` in the editor world. Walks the public
  `AInstancedFoliageActor::GetFoliageInfos` accessor and emits a
  per-IFA actor block (name, label, transform, level, foliage type
  count, total instance count) plus a per-type `foliage_types`
  array. Each foliage_type carries the type asset path, source-mesh
  or actor-class path with a `source_kind`
  (`static_mesh` / `actor` / `unknown`) discriminator, density,
  density adjustment factor, radius, per-axis scale interval
  (ScaleX / ScaleY / ScaleZ min and max), instance counts (placed
  and total), and the editor-only approximated-bounds box of all
  its instances. Optional `sample_locations` draws a deterministic
  seeded reservoir of N world-space instance locations per type
  (capped at 1024) so the agent can probe density without us
  shipping the full instance dump. Filters: `name_pattern` and
  `level_filter`. Adds `Foliage` to PublicDependencyModuleNames.
  Edit-side ops (paint, scatter, remove instances) remain on the
  backlog.
- `cpp_source` (small, read-only) — read C++ source by class path
  or by full file path on disk. Three input modes (class +
  header_path + source_path), all routed through
  `FSourceCodeNavigation::FindClassHeaderPath` /
  `FindClassSourcePath` for the class-driven path or extension-swap
  inference for the file-driven paths. Returns header / cpp text +
  byte size + truncation flag per file plus per-file existence
  flags so a caller can tell "header-only class" from
  "Blueprint-defined class with no C++ at all". Pairs with
  `bp_brief` for the "what does this Blueprint's parent C++ class
  look like" question. The `module` + `module_dir` metadata
  through `FindClassModuleName` + `FindModulePath` lets a caller
  jump straight to the .Build.cs without a second tool call.
- `pie_test_scene` (small, minimum cut) — scene-state assertion
  harness. Runs against the editor world without driving Play in
  Editor. Two assertion kinds in this slice:
  - `actor_exists`: `target` is an actor name (matched against
    `GetName()` first and Outliner label second). Pass = an
    actor with that name or label is present in the current
    editor world.
  - `actor_at_location`: `target` is an actor name, `expected`
    is a `[x, y, z]` world-space location, and `tolerance`
    (default 1.0 cm) is the pass radius. Pass = the resolved
    actor's `GetActorLocation` is within `tolerance` of
    `expected`.
  Per-assertion the response carries `index`, `kind`, `target`,
  `passed` flag, optional `actual` / `expected` / `delta` /
  `tolerance` for distance-based kinds, and a human-readable
  `message`. Aggregate counts (`total`, `passed`, `failed`,
  `unsupported`, `all_passed`) sit at the top of the response.
  Open follow-ons: PIE-driven kinds (`var_equals` against a
  Blueprint instance variable, `actor_overlapping_tag` for
  overlap-driven gameplay assertions), per-assertion timeout for
  PIE-driven kinds, and an over-PIE harness option for callers
  who want the same surface but inside a running PIE world.
- `behavior_tree` edit slice (small) — adds two edit ops to the
  existing read-only inspector through a new `op` discriminator
  (default stays `inspect`):
  - `create_behavior_tree`: NewObject's a UBehaviorTree at a
    `/Game/...` package path with an optional `/Game/...`
    UBlackboardData linked through `BlackboardAsset`.
    `overwrite=true` is the standard escape hatch for an existing
    asset at the path.
  - `add_root_composite`: NewObject's a Selector / Sequence /
    SimpleParallel composite (case-insensitive token, with a
    UClass-path escape hatch for any UBTCompositeNode subclass)
    under the tree as outer and assigns it to `RootNode`. We
    refuse to overwrite an existing RootNode unless `replace=true`.
  Both ops save by default. Together the two ops produce a usable
  empty tree in two declarative calls. Open follow-ons: append
  child task / composite to a chosen parent, insert a decorator
  on a chosen child slot, append a service on a composite, and
  the full Blackboard key edit surface (add / remove / rename /
  type change / sync flag toggle).
- `sequencer_edit` edit slice (small) — adds two edit ops to the
  existing read-only inspector through the same `op` discriminator
  (default stays `inspect`):
  - `create_level_sequence`: NewObject's a ULevelSequence at a
    `/Game/...` package path and runs `ULevelSequence::Initialize`
    so the new asset has a fresh UMovieScene with the project's
    default tick / display rates and clock source. Without
    Initialize the sequence opens but every Sequencer panel call
    hits a null MovieScene path, so we run it unconditionally
    before AssetCreated and the optional save.
  - `add_possessable`: resolves a target sequence by short name or
    `/Game/...` path and a target actor by `GetName()` (first) /
    Outliner label (second), then runs
    `UMovieScene::AddPossessable` + `UMovieSceneSequence::BindPossessableObject`
    so Sequencer's runtime can map the binding GUID back to the
    editor-world actor. `binding_name` defaults to the actor's
    `GetActorLabel()` so the Sequencer Outliner shows a
    designer-readable row name.
  Both ops save by default. Open follow-ons: track add (a chosen
  UMovieSceneTrack subclass), section add (with an explicit frame
  range), section move, spawnable creation, and camera-cut track
  creation.
- `animation_inspect` (small, read-only) — structured dump for
  animation assets. Resolves the asset by short name or
  `/Game/...` path and branches by class:
  - USkeletalMesh: skeleton path + LOD count + bone list (each
    `{name, parent_index, parent_name}`) + socket list (mesh-level
    sockets first, then non-overridden skeleton-level sockets,
    each with `relative_location` / `relative_rotation` /
    `relative_scale` plus a `source` discriminator).
  - UAnimSequence: play length + rate scale + sampling frame rate
    (numerator / denominator / approx_fps) + sampled key count +
    additive anim type token + notify list. Each notify carries
    name + absolute time + duration + track index + trigger
    chance + linked UAnimNotify / UAnimNotifyState class path
    plus a `notify_kind` discriminator (`instant` / `state` /
    `event`).
  - UAnimMontage: play length + rate scale + composite sections
    (name + time + next-section) + slot tracks (slot_name +
    animation count) + notifies through the same shape.
  - UBlendSpace (and UBlendSpace1D): per-axis FBlendParameter
    (display_name + min + max + grid_num + snap_to_grid +
    wrap_input) plus axis_count and sample count. Reads through
    the public `UBlendSpace::GetBlendParameter(int32)` so we
    avoid the fixed-size FBlendParameter[3] reflection dance.
  - UAnimBlueprint: parent class + target skeleton path +
    template flag + variable count + state-machine list (each
    machine's name + state count + transition count + initial
    state index + initial state name). The state-machine list
    reads off the cached UAnimBlueprintGeneratedClass through
    the `BakedStateMachines` array so we do not need the
    editor-only AnimGraph module.
  Pairs with `bp_brief` for AnimBP-specific orientation. Edit-side
  ops (sequence / montage authoring, AnimBP state-machine
  authoring, IK rig / retargeting) remain on the backlog.
- `project_context` (small, read-only) — one-shot designer summary
  of the loaded project. Returns project name + uproject path +
  project dir + content dir, the .uproject metadata (description,
  category, EngineAssociation, enterprise flag), the full engine
  version strings plus per-component major / minor / patch /
  changelist / branch / licensee flag, the current editor level
  (name + path), the per-level GameMode override + default pawn
  through `AWorldSettings::DefaultGameMode`, the project-wide
  GameMapsSettings surface (`default_game_mode_class_project`,
  `default_game_map`, `transition_map`, `editor_startup_map`,
  `game_instance_class`), an `enabled_plugins` array filtered by
  default to project / external / mod / enterprise plugins (each
  entry with name + friendly_name + type + location + version +
  version_name + category + description + created_by +
  engine_version + can_contain_content + is_beta + is_experimental
  + base_dir; engine plugins opt-in through
  `include_engine_plugins=true`), a `source_modules` array from
  the .uproject (name + type + loading_phase), and a
  `content_roots` array of every immediate `/Game/*` subfolder
  with a recursive asset count through `IAssetRegistry::GetAssets`.
  Pairs with `scene_brief` for the orientation pass: one tells
  the agent what project it is in, the other tells it what level
  it is in. Adds EngineSettings to PublicDependencyModuleNames.
- `sequencer_edit` (small) — multi-op tool keyed by `op`. The
  `inspect` op (default) is the read-only structured dump shipped
  earlier: resolves a target `ULevelSequence` (or any
  UMovieSceneSequence subclass) and returns asset name + path +
  class plus the linked UMovieScene's tick / display frame rates
  (each as `{numerator, denominator, approx_fps}`), the playback
  range as a start / end / duration triple with
  `playback_has_start` / `playback_has_end` flags, the master
  tracks array (each track with name + editor-only display_name +
  class + class_path + section_count and an optional sections
  array reporting `class` + `class_path` +
  `inclusive_start_frame` / `exclusive_end_frame` /
  `duration_frames` plus the `has_start_frame` / `has_end_frame`
  bound flags), the optional camera-cut track stub when present,
  the possessables array (binding GUID + name + possessed-class +
  parent_guid), and the spawnables array (binding GUID + name +
  spawn-template class). Per-track section emission caps at
  `max_sections_per_track` (default 64), with `sections_truncated`
  set on the offending track when the cap fires.
  The new edit ops are `create_level_sequence` (NewObject's a
  ULevelSequence at a `/Game/...` path and runs
  `ULevelSequence::Initialize` so the asset has a fresh UMovieScene
  with the project's default tick / display rates and clock
  source) and `add_possessable` (resolves a target sequence and a
  named editor-world actor, runs `UMovieScene::AddPossessable` +
  `UMovieSceneSequence::BindPossessableObject` so Sequencer's
  runtime can map the binding GUID back to the actor;
  `binding_name` defaults to the actor's `GetActorLabel()`). Both
  edit ops save by default. Adds `MovieScene` and `LevelSequence`
  to PublicDependencyModuleNames. Heavier edit ops (track add,
  section move, spawnable creation, camera-cut creation, per-row
  edits) remain on the backlog.
- `gas_edit` (small, read-only) — Gameplay Ability System dump for
  three asset shapes:
  - UGameplayAbility (or a Blueprint with a UGameplayAbility CDO):
    ability tags + cancel / block / activation owned / required /
    blocked tags + source / target required / blocked tags, cost +
    cooldown gameplay-effect class paths, AbilityTriggers.
  - UGameplayEffect (or a Blueprint with a UGameplayEffect CDO):
    DurationPolicy + DurationMagnitude / MaxDurationMagnitude when
    Has-Duration, modifier list (each with attribute name + owning
    AttributeSet class + ModifierOp + literal magnitude when
    scalable), executions list with calculation classes,
    GameplayCues with tag set + level range + magnitude attribute,
    plus the cached asset / granted / blocked-ability tag
    containers through the public accessors that the GE component
    model migrated to in 5.3+. Includes stack limit + stack
    expiration policy.
  - UAttributeSet (or a Blueprint with a UAttributeSet CDO): walks
    the CDO's FProperty list filtering on
    `FGameplayAttribute::IsSupportedProperty` and dumps each
    attribute's name, CPP type, base / current default value, and
    storage mode (legacy float vs. FGameplayAttributeData).
  Pairs with `tag_registry_edit` so a caller can answer "what tags
  drive what ability" in two read-only calls. Edit-side ops (tag
  mutation, modifier add / remove, cost / cooldown rebind, attribute
  default override) remain on the backlog. Adds GameplayAbilities to
  PublicDependencyModuleNames.

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
- `bp_commit` (small variant ships in this fork) — `MarkBlueprintAsStructurallyModified` + `CompileBlueprint` with captured `FCompilerResultsLog` + `SaveAsset`. Surfaces error / warning / info lines as separate string arrays and refuses to save a broken Blueprint unless `force_save=true`. Open follow-ons: include the structural diff against the previous compiled state (added / removed function signatures and variable types), and a per-call timing breakdown.
- `bp_function_create` (small variant ships in this fork) — declarative `CreateNewGraph` + `AddFunctionGraph<UClass>` + typed FunctionEntry / FunctionResult pins in one call. Open follow-ons: add a Local Variables array to the new function in the same call, add Latent flag handling + UObject return-pin glue beyond the current FUNC_BlueprintPure toggle, and a `from_interface` mode that fills the signature from a Blueprint Interface method's parameter list.
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
- `bp_export` (small variant ships in this fork) — canonical
  Blueprint-to-JSON snapshot covering every graph plus components,
  variables, defaults, interfaces, and pin-level edges. Open
  follow-ons: a deeper component default-overrides-only mode
  (compare against the parent CDO and only emit deltas), a graph
  filter param so a caller can restrict the dump to a single named
  graph, and a `compact` mode that returns the snapshot as a
  graphviz-style edge dump for one-shot rendering.

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
- `search_assets` (small variant ships in this fork) — Content
  Browser search through `IAssetRegistry`. Open follow-ons: persisted
  saved-search definitions and result paging across multiple calls
  through an opaque cursor.
- `asset_references` (small variant ships in this fork) — dependency
  graph for an asset through `IAssetRegistry::GetReferencers` /
  `GetDependencies`. Open follow-ons: SearchableName / Manage
  category support beyond the current package-only walk, return-by-
  level grouping when the seed package is a level, and a `dot_graph`
  output mode for one-shot rendering of the dependency closure.
- `project_context` (small variant ships in this fork) — one-shot
  read-only project summary covering identity, engine version,
  enabled plugins (filtered to user-installed by default), source
  modules from the .uproject, top-level Content folders with
  recursive asset counts, and the active map + GameMode +
  default pawn (per-level override and project-wide). Open
  follow-ons: per-plugin module list (descriptor's `Modules` array)
  beyond the project's modules, project-wide tag categories /
  feature packs surface, and an `additional_plugin_directories`
  array exposing the .uproject's external plugin search paths.

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
- `material_edit` (small variant ships in this fork: create material
  with a Constant3Vector base colour, create material instance
  constant, set scalar / vector / texture parameters on an instance,
  the expression-graph trio `add_expression` / `connect_expressions`
  / `set_expression_property`, plus the bulk `add_expressions` op
  that lays a small graph down in one call from a list of expression
  specs and a list of edge specs). Pending: Material Functions and
  Material Parameter Collections, plus a `set_attribute_blendable`
  op for the override-surface chain on an instance.

## VFX (large each)

- `niagara_inspect` (small read-only variant ships in this fork) — dump
  emitters, per-stage scripts, event-handler chain, simulation-stage
  class list, renderer class list, and the user-exposed parameter
  store entries. Open follow-ons: per-emitter renderer property
  readback (lit-sprite / mesh / ribbon settings), data-interface
  configuration dump beyond the type kind, and per-script binding
  list for the rapid-iteration parameter set.
- `niagara_edit` — Niagara particle system editing.
- `niagara_script_edit` — reusable Niagara module authoring.
- `chaos_edit` — Geometry Collection destruction setup.

## Animation (large each)

- `animation_inspect` (small read-only variant ships in this fork) —
  branch by class for USkeletalMesh / UAnimSequence / UAnimMontage /
  UBlendSpace / UAnimBlueprint. Open follow-ons: per-LOD bone-name
  delta against the skeleton, virtual-bone surface (USkeleton::GetVirtualBones)
  beyond the basic bone list, AnimComposite + AimOffsetBlendSpace
  + UAnimSequence's TrackToSkeletonMapTable readback, Anim Notify
  Track readout (Tracks array on UAnimSequenceBase) so the agent
  can ask "which track is this notify on", FCompositeSection's
  `MetaData` array dump on UAnimMontage, and an `include_anim_graph_dump`
  toggle on UAnimBlueprint that walks the EventGraph + AnimGraph
  pages and emits the K2 nodes' titles + classes (matches
  `bp_inspect`'s shape).
- `animation_edit` — create / modify the same.
- `animation_graph_edit` — AnimBP state machines, transitions, blend nodes.
- `ik_rig_edit` — IK rig setup.
- `ik_retarget` — retarget animations.

## UMG / Widgets (medium)

- `widget_inspect` — the small variant ships in this fork. The remaining
  hosted-Flop scope (style readback, MVVM binding readback) is still
  outstanding.
- `widget_edit` — the small variant plus the slot-property surface
  ship in this fork. `set_slot_property` covers UCanvasPanelSlot /
  UVerticalBoxSlot / UHorizontalBoxSlot / UOverlaySlot etc. without
  spelling out each subclass. The remaining hosted-Flop scope
  (animations, MVVM bindings, advanced styles, event binding) is
  still on the table.

## AI & abilities (large each)

- `behavior_tree` (small read + edit slice ships in this fork) —
  read-only `inspect` plus the two edit ops `create_behavior_tree`
  (asset creation with optional linked Blackboard) and
  `add_root_composite` (Selector / Sequence / SimpleParallel root
  assignment). Open follow-ons: append child task / composite to
  a chosen parent, insert a decorator on a chosen child slot,
  append a service on a composite, full Blackboard key edits
  (add / remove / rename / type change / sync flag toggle), and an
  AI Controller / EQS slice with the same read-only structure for
  the Run Behavior Tree -> Make Decision flow. AIModule is already
  pulled in.
- `gas_edit` (small read-only slice ships in this fork) —
  UGameplayAbility / UGameplayEffect / UAttributeSet dump (tags,
  modifiers, attribute defaults). Open follow-ons: tag-container
  mutation through GE components, GE modifier add / remove,
  ability cost / cooldown class rebind, and an attribute-default
  override path that writes through the CDO and recompiles the
  Blueprint. Native AbilityTask classes and GameplayCue assets
  remain unscoped. GameplayAbilities is already pulled in.
- `tag_registry_edit` (small variant ships in this fork) — `add_tag`,
  `remove_tag`, `list_tags` through `IGameplayTagsEditorModule`. Open
  follow-ons: rename through `RenameTagInINI`, restricted-tag source
  creation through `AddNewGameplayTagSource`, and per-tag-asset usage
  search over the AssetRegistry for "where is this tag referenced".

## Landscape & foliage (large each)

- `landscape_inspect` (small read-only variant ships in this fork) —
  per-actor dump for every `ALandscape`: section-grid configuration,
  proxy material, registered layers, world-space bounds, deduplicated
  heightmap / weightmap texture sets, optional per-component records.
  Open follow-ons: per-edit-layer dump (each ULandscapeEditLayer's
  Guid + name + visibility + paint / sculpt flags), spline component
  dump for actors implementing ILandscapeSplineInterface, and a
  height / weight sample at a chosen world location through
  `ULandscapeInfo::GetLayerWeightAtLocation`.
- `landscape_edit` — sculpting, paint layers, heightmap import /
  export.
- `foliage_inspect` (small read-only variant ships in this fork) —
  per-IFA dump with foliage type list (mesh / actor source path,
  density, radius, per-axis scale interval, instance counts) plus an
  optional sampled world-location reservoir per type. Open follow-
  ons: per-instance base-component lookup (which static mesh /
  landscape component does each instance attach to), procedural
  foliage spawner + procedural foliage volume readout, and a
  spatial-bound query that returns "every foliage instance whose
  origin is inside this FBox / FSphere".
- `foliage_edit` — paint, scatter, remove instances.

## Cinematics & audio (large each)

- `sequencer_edit` (small read + edit slice ships in this fork) —
  read-only `inspect` plus the two edit ops `create_level_sequence`
  (NewObject + `ULevelSequence::Initialize` for a fresh empty
  asset) and `add_possessable` (`UMovieScene::AddPossessable` +
  `BindPossessableObject` for actor-world binding). Open follow-
  ons: per-track row dump for multi-row tracks
  (UMovieSceneNameableTrack subclasses), per-channel key dump for
  the standard transform / float / bool / enum tracks through
  `UMovieSceneSection::GetChannelProxy`, asset-bound resolution
  for possessables (which actor in the current editor world is
  bound to a possessable GUID), and the heavier edit ops (track
  add, section add with explicit frame range, section move,
  spawnable creation, camera-cut creation).
- `metasound_edit` — MetaSound graphs.
- `sound_asset_edit` — SoundCue graphs.

## Procedural (large)

- `pcg_graph_edit` — PCG graph authoring.

## Data assets (small to medium each, on the asset_factory umbrella)

- `asset_factory` (DataAsset) — shipped in this fork.
- `asset_factory` (Enhanced Input bundle) — shipped in this fork. One
  call accepts a list of action specs (name + value_type) and a list
  of mapping rows (action + key + optional negate / swizzle) and
  produces an IMC plus N UInputActions. The dedicated `bp_input` tool
  still covers per-asset creation and the action-event-node wiring
  side; the bundle variant is the right shortcut when the caller can
  describe the entire input layer declaratively.

## Editor & diagnostics (medium each)

- `editor_log` — the on-disk-log tail variant ships in this fork. A future
  pass could attach a buffering FOutputDevice to GLog so the tool can read
  log entries that arrived after the editor started without re-parsing the
  full file. We can also expose the in-editor SOutputLog widget filter
  helpers if a hook is added to the OutputLog module.
- `performance_audit` — gather perf stats and run a basic audit.
- `cpp_source` (small read-only variant ships in this fork) — read
  header / cpp pair through `FSourceCodeNavigation::FindClassHeaderPath`
  / `FindClassSourcePath`, or by direct .h / .cpp disk path with
  sibling-inference. Open follow-ons: write side (mutate header /
  cpp text + run `ICompilerResultsLog` Live Coding patch) and
  per-class symbol dump (functions + properties from the
  reflection database with their declared file + line through
  `FProperty::GetMetaData("MetaSource")`).

## Runtime verification (large each)

- `pie_test_bp` — Blueprint test harness with assertions during PIE.
- `pie_test_scene` (small minimum cut ships in this fork) — scene-
  state assertion harness with the two assertion kinds we can
  answer statically against the editor world: `actor_exists` for
  actor-name presence and `actor_at_location` for distance-bounded
  location matches. Open follow-ons: PIE-driven kinds
  (`var_equals` against a Blueprint instance variable,
  `actor_overlapping_tag` for overlap-driven gameplay assertions),
  per-assertion timeout for PIE-driven kinds, and an over-PIE
  harness option for callers who want the same surface but inside
  a running PIE world.

## Execution (large each)

- `python_execution` (small variant ships in this fork) — broader hosted
  scope still pending: persistent shared interpreter scope across calls,
  output streaming for long-running scripts, and richer typed result
  marshalling beyond the current stdout / stderr / repr capture.
- `unreal_api` — query the 15,000+ entry API surface.
- `skills` — fetch on-demand workflow docs.

## Suggested next-pass shortlist for a single-player game project

After the latest pass (`pie_test_scene` minimum cut, `behavior_tree`
edit slice, `sequencer_edit` edit slice), the next set should pick
up:

1. `pie_test_scene` PIE-driven kinds — `var_equals` against a
   Blueprint instance variable in a running PIE world, and
   `actor_overlapping_tag` for overlap-driven gameplay assertions.
   Adds the live-PIE harness path that the minimum cut deferred so
   we could ship the static-world half of the tool first.
2. `behavior_tree` heavier edit ops — append a child task / composite
   to a chosen parent, insert a decorator on a chosen child slot,
   append a service on a composite, and the full Blackboard key
   edit surface. Reuses the AIModule dep already pulled in.
3. `sequencer_edit` heavier edit ops — track add (a chosen
   UMovieSceneTrack subclass), section add (with an explicit frame
   range), section move, spawnable creation, and camera-cut track
   creation. Reuses the MovieScene + LevelSequence deps.
4. `niagara_edit` (small variant) — `create_niagara_system` for an
   empty UNiagaraSystem asset (the implicit `PostInitProperties` flow
   sets up SystemSpawnScript / SystemUpdateScript so no per-call
   scripting is needed), then `add_emitter` once the parent emitter
   surface is scoped clean-room. Pairs with `niagara_inspect`.
5. `gas_edit` edit slice — append a modifier to a UGameplayEffect,
   set DurationPolicy / DurationMagnitude through CDO writes +
   recompile, rebind cost / cooldown classes on a UGameplayAbility.
   Reuses the existing GameplayAbilities dep.
6. `animation_edit` (small variant) — set montage section timings,
   add notify entries on a UAnimSequence / UAnimMontage through the
   public Notifies array, and toggle additive flags through the
   CDO. Pairs with `animation_inspect`.

`python_execution` still covers any operation we have not wrapped
natively; prefer wrapping the high-frequency calls as dedicated tools
so the agent does not need to author Python every time.
