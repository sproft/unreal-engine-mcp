#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: project_context (read-only)
 *
 * One-shot designer-readable summary of the loaded Unreal project. The
 * hosted Flop tool surface listed `project_context` as the "what kind of
 * project am I in" call so an agent could orient before chasing assets.
 * Clean-room implementation reading through the public UE5 + project APIs.
 *
 * Operation: single op (`context`, default).
 *
 * Optional inputs:
 *   - `include_plugins`: emit the enabled-plugins array. Default True.
 *   - `include_modules`: emit the source-module list. Default True.
 *   - `include_content_roots`: emit the top-level Content folder array.
 *     Default True.
 *   - `include_engine_plugins`: include built-in / engine-shipped plugins
 *     in the plugins array. Default False; we filter to "user-installed
 *     and project-local" plugins by default.
 *   - `max_content_roots`: cap on top-level Content folder emission.
 *     Default 64.
 *   - `content_roots_recursive_count`: count assets recursively (under
 *     each top-level folder) instead of only direct children. Default
 *     True.
 *
 * Returns a structured payload with:
 *   - `project_name`, `uproject_path` (absolute disk path).
 *   - `project_description`, `project_category` from the .uproject
 *     metadata. The .uproject does not carry company / homepage /
 *     support fields; those live in the .uplugin metadata for plugins
 *     and we surface them per-plugin in `enabled_plugins`.
 *   - `is_enterprise_project`.
 *   - `engine_version` (full e.g. `5.7.0-29314046+++UE5+Release-5.7`),
 *     `engine_compatible_version`, `engine_branch`,
 *     `engine_major` / `engine_minor` / `engine_patch`,
 *     `engine_changelist`, `engine_is_licensee`.
 *   - `engine_association` (the .uproject's EngineAssociation token).
 *   - `current_level_path` and `current_level_name` from the editor world.
 *   - `default_game_mode_class` and `default_pawn_class` from the
 *     persistent level's WorldSettings (override) plus
 *     `default_game_mode_class_project` from the project's
 *     `/Script/EngineSettings.GameMapsSettings`.
 *   - `default_game_map`, `transition_map`, `server_default_map` from
 *     `UGameMapsSettings`.
 *   - `enabled_plugins`: array of `{name, version, version_name,
 *     category, description, friendly_name, created_by, type, location}`
 *     for every enabled plugin (filtered by default to `Project` /
 *     `External` location, opt-in `Engine` / `EngineProject`).
 *   - `source_modules`: array of `{name, type, loading_phase}` from the
 *     .uproject Modules array. Lives on the host project, not the plugin.
 *   - `content_roots`: array of `{name, asset_count}` for each top-level
 *     folder under `/Game/`.
 *
 * Read-only. We never write to disk.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - IProjectManager / FProjectDescriptor for .uproject metadata.
 *   - FApp::GetProjectName / FPaths::GetProjectFilePath for paths.
 *   - FEngineVersion::Current / FEngineVersion::CompatibleWith for
 *     engine version surface.
 *   - IPluginManager::GetEnabledPlugins for the plugin list.
 *   - UGameMapsSettings::GetGameInstanceClass / GetGlobalDefaultGameMode
 *     for the project-level GameMode override and the default map(s).
 *   - GEditor->GetEditorWorldContext / UWorld::PersistentLevel for the
 *     current map.
 *   - IAssetRegistry::GetAssetsByPath for content root counts.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftProjectContextCommands
{
public:
    FSproftProjectContextCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleProjectContext(const TSharedPtr<FJsonObject>& Params);
};
