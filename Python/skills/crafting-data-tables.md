# Crafting Data Tables

When to use this skill: any time a UE5 project needs a designer-edited
table of structured rows that runtime code can look up by row name.
Reach for it on crafting recipes, item definitions, dialogue lines,
loot drops, weapon stats, enemy archetypes, and any other data that
should ship as authored content rather than baked into a Blueprint
default. For a fixed-shape config that never changes after compile
time, a `UDeveloperSettings` subclass is leaner.

The canonical UE5 path: define a `USTRUCT(BlueprintType)` derived
from `FTableRowBase` (e.g. `FCraftingRecipe { FName Output;
TArray<FName> Inputs; float CraftTime; }`), then create a
`UDataTable` asset in the editor and pick the struct as its
`RowStruct`. Each row is keyed by FName. Runtime code calls
`UDataTable::FindRow<FCraftingRecipe>(RowName, ContextString)` (or
the Blueprint `Get DataTable Row` node) and gets a pointer to the
typed row. CSV / JSON import lands through the asset's
`ReimportAsset` / drag-and-drop path, which serialises through
`UDataTable::CreateTableFromCSVString`. For a table that needs to
combine entries from multiple ini sources, the `UCompositeDataTable`
subclass aggregates parent tables and resolves overrides by row name.

Our wrappers in this fork: `asset_factory` covers the table-creation
side. The `data_table` variant takes a target row-struct path
(`/Script/Module.FFooStruct` or `/Game/...` for a UScriptStruct
asset path) and produces a `UDataTable` asset; `struct` produces a
new UScriptStruct asset with a typed field list (each
`{name, type}` covering scalar, built-in struct, or
`/Game/...`-rooted UScriptStruct paths) so the row struct itself
can come from the same MCP session. The `data_asset` variant covers
single-instance config (one `UDataAsset` subclass + a flat property
dict applied through `FProperty::ImportText_InContainer`). For
declarative scene wiring after the table is in place, `bp_variable`
exposes a TMap default-value path that can pre-populate a Blueprint
field with row references. `python_execution` handles bulk row
loading (CSV import, programmatic row writes) when the editor
panel is too clicky.

Gotchas: row FNames are case-insensitive but not case-preserving on
some import paths; treat them as lowercase for any external lookup.
Changing a row struct's field set after rows exist requires a
re-import or hand-edit; the table will load but the changed fields
silently default to zero. CSV imports treat empty cells as default-
constructed, so a missing optional column does not surface as an
error. UDataTable references hold a soft pointer to the row struct;
deleting the struct asset before the table breaks the table's
serialization. Always cook the row struct in the same module as the
runtime code that calls `FindRow`, otherwise the cooked client
fails to resolve `RowStruct` on load.
