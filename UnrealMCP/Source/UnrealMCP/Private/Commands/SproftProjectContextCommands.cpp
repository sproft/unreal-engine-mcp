#include "Commands/SproftProjectContextCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "GameMapsSettings.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "ModuleDescriptor.h"
#include "PluginDescriptor.h"
#include "ProjectDescriptor.h"
#include "UObject/SoftObjectPath.h"

namespace
{
    /** Resolve the location of a plugin against EPluginType: most callers
     *  want to know whether the plugin lives in the project, the engine,
     *  or an external directory. Map onto a small string token. */
    FString PluginTypeToString(EPluginType Type)
    {
        switch (Type)
        {
        case EPluginType::Engine:     return TEXT("engine");
        case EPluginType::Enterprise: return TEXT("enterprise");
        case EPluginType::Project:    return TEXT("project");
        case EPluginType::External:   return TEXT("external");
        case EPluginType::Mod:        return TEXT("mod");
        default:                      return TEXT("unknown");
        }
    }

    /** Map EPluginLoadedFrom to a single-word token. */
    FString PluginLoadedFromToString(EPluginLoadedFrom From)
    {
        switch (From)
        {
        case EPluginLoadedFrom::Engine:  return TEXT("engine");
        case EPluginLoadedFrom::Project: return TEXT("project");
        default:                         return TEXT("unknown");
        }
    }

    /** True for plugins the project's authors actually opted in to: the
     *  Project type or anything in External (additional plugin
     *  directories or command-line). The engine ships with hundreds of
     *  always-on plugins; emitting all of them swamps the response. */
    bool IsUserPlugin(const TSharedRef<IPlugin>& Plugin)
    {
        const EPluginType Type = Plugin->GetType();
        return Type == EPluginType::Project ||
               Type == EPluginType::External ||
               Type == EPluginType::Mod ||
               Type == EPluginType::Enterprise;
    }

    /** Read a UClass path from a FSoftClassPath; returns an empty string
     *  when the path is empty so the caller can omit the field. */
    FString SoftClassPathString(const FSoftClassPath& Path)
    {
        return Path.IsValid() ? Path.ToString() : FString();
    }

    /** Read a soft object path; same behaviour. */
    FString SoftObjectPathString(const FSoftObjectPath& Path)
    {
        return Path.IsValid() ? Path.ToString() : FString();
    }
}

FSproftProjectContextCommands::FSproftProjectContextCommands()
{
}

TSharedPtr<FJsonObject> FSproftProjectContextCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("project_context"))
    {
        return HandleProjectContext(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown project_context command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftProjectContextCommands::HandleProjectContext(const TSharedPtr<FJsonObject>& Params)
{
    // Toggle defaults. Read each up-front so we do not branch on the
    // optional Params dict in the body.
    bool bIncludePlugins        = true;
    bool bIncludeModules        = true;
    bool bIncludeContentRoots   = true;
    bool bIncludeEnginePlugins  = false;
    bool bContentRootsRecursive = true;
    int32 MaxContentRoots       = 64;

    if (Params.IsValid())
    {
        bool TempBool = false;
        if (Params->TryGetBoolField(TEXT("include_plugins"), TempBool))         bIncludePlugins        = TempBool;
        if (Params->TryGetBoolField(TEXT("include_modules"), TempBool))         bIncludeModules        = TempBool;
        if (Params->TryGetBoolField(TEXT("include_content_roots"), TempBool))   bIncludeContentRoots   = TempBool;
        if (Params->TryGetBoolField(TEXT("include_engine_plugins"), TempBool))  bIncludeEnginePlugins  = TempBool;
        if (Params->TryGetBoolField(TEXT("content_roots_recursive_count"), TempBool)) bContentRootsRecursive = TempBool;

        double TempNum = 0.0;
        if (Params->TryGetNumberField(TEXT("max_content_roots"), TempNum))
        {
            MaxContentRoots = FMath::Max(0, static_cast<int32>(TempNum));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    // Project identity. FApp::GetProjectName is reliable for every
    // editor build; FPaths::GetProjectFilePath returns either the
    // .uproject path or the project directory + name when the path is
    // implicit.
    Result->SetStringField(TEXT("project_name"), FApp::GetProjectName());

    const FString UProjectPath = FPaths::GetProjectFilePath();
    if (!UProjectPath.IsEmpty())
    {
        Result->SetStringField(TEXT("uproject_path"), FPaths::ConvertRelativePathToFull(UProjectPath));
    }
    Result->SetStringField(TEXT("project_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
    Result->SetStringField(TEXT("project_content_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));

    // .uproject metadata read through the project manager.
    if (const FProjectDescriptor* ProjectDesc = IProjectManager::Get().GetCurrentProject())
    {
        if (!ProjectDesc->Description.IsEmpty())
        {
            Result->SetStringField(TEXT("project_description"), ProjectDesc->Description);
        }
        if (!ProjectDesc->Category.IsEmpty())
        {
            Result->SetStringField(TEXT("project_category"), ProjectDesc->Category);
        }
        if (!ProjectDesc->EngineAssociation.IsEmpty())
        {
            Result->SetStringField(TEXT("engine_association"), ProjectDesc->EngineAssociation);
        }
        Result->SetBoolField(TEXT("is_enterprise_project"), ProjectDesc->bIsEnterpriseProject);
    }

    // Engine version. Print the full descriptor and the major/minor/patch
    // helpers so a caller can branch on them without re-parsing the
    // ToString output.
    {
        const FEngineVersion& Current = FEngineVersion::Current();
        Result->SetStringField(TEXT("engine_version"), Current.ToString());
        Result->SetStringField(TEXT("engine_compatible_version"), FEngineVersion::CompatibleWith().ToString());
        Result->SetNumberField(TEXT("engine_major"), Current.GetMajor());
        Result->SetNumberField(TEXT("engine_minor"), Current.GetMinor());
        Result->SetNumberField(TEXT("engine_patch"), Current.GetPatch());
        Result->SetNumberField(TEXT("engine_changelist"), static_cast<double>(Current.GetChangelist()));
        Result->SetStringField(TEXT("engine_branch"), Current.GetBranch());
        Result->SetBoolField(TEXT("engine_is_licensee"), Current.IsLicenseeVersion());
    }

    // Current level + GameMode override + default pawn from the editor world.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (World)
    {
        Result->SetStringField(TEXT("current_level_name"), World->GetMapName());
        if (UPackage* WorldPackage = World->GetOutermost())
        {
            Result->SetStringField(TEXT("current_level_path"), WorldPackage->GetName());
        }

        if (AWorldSettings* WorldSettings = World->GetWorldSettings(false, false))
        {
            if (UClass* GameModeOverride = WorldSettings->DefaultGameMode)
            {
                Result->SetStringField(TEXT("default_game_mode_class"), GameModeOverride->GetPathName());
                if (AGameModeBase* CDO = GameModeOverride->GetDefaultObject<AGameModeBase>())
                {
                    if (UClass* Pawn = CDO->DefaultPawnClass)
                    {
                        Result->SetStringField(TEXT("default_pawn_class"), Pawn->GetPathName());
                    }
                }
            }
        }
    }

    // Project-level UGameMapsSettings: maps + the GlobalDefaultGameMode
    // soft path. We read the soft paths directly so we do not need to
    // load the classes; the caller is just asking what the .ini says.
    if (UGameMapsSettings* MapsSettings = UGameMapsSettings::GetGameMapsSettings())
    {
        // GlobalDefaultGameMode + GlobalDefaultServerGameMode are private
        // members; the ToString helpers under the public surface read
        // them through the static accessors.
        const FString ProjectGameMode = UGameMapsSettings::GetGlobalDefaultGameMode();
        if (!ProjectGameMode.IsEmpty())
        {
            Result->SetStringField(TEXT("default_game_mode_class_project"), ProjectGameMode);
        }

        const FString DefaultMap = UGameMapsSettings::GetGameDefaultMap();
        if (!DefaultMap.IsEmpty())
        {
            Result->SetStringField(TEXT("default_game_map"), DefaultMap);
        }

        const FString TransitionMap = SoftObjectPathString(MapsSettings->TransitionMap);
        if (!TransitionMap.IsEmpty())
        {
            Result->SetStringField(TEXT("transition_map"), TransitionMap);
        }

#if WITH_EDITORONLY_DATA
        const FString EditorStartupMap = SoftObjectPathString(MapsSettings->EditorStartupMap);
        if (!EditorStartupMap.IsEmpty())
        {
            Result->SetStringField(TEXT("editor_startup_map"), EditorStartupMap);
        }
#endif

        const FString GameInstance = SoftClassPathString(MapsSettings->GameInstanceClass);
        if (!GameInstance.IsEmpty())
        {
            Result->SetStringField(TEXT("game_instance_class"), GameInstance);
        }
    }

    // Enabled plugins. Default-filtered to user plugins; engine plugins
    // are opt-in.
    if (bIncludePlugins)
    {
        TArray<TSharedPtr<FJsonValue>> PluginArr;

        IPluginManager& PluginMgr = IPluginManager::Get();
        TArray<TSharedRef<IPlugin>> EnabledPlugins = PluginMgr.GetEnabledPlugins();
        // Sort by name for predictable output.
        EnabledPlugins.Sort([](const TSharedRef<IPlugin>& A, const TSharedRef<IPlugin>& B)
        {
            return A->GetName() < B->GetName();
        });

        int32 EngineFilteredCount = 0;
        for (const TSharedRef<IPlugin>& Plugin : EnabledPlugins)
        {
            const bool bIsUser = IsUserPlugin(Plugin);
            if (!bIncludeEnginePlugins && !bIsUser)
            {
                ++EngineFilteredCount;
                continue;
            }

            const FPluginDescriptor& Desc = Plugin->GetDescriptor();
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Plugin->GetName());
            Entry->SetStringField(TEXT("friendly_name"), Plugin->GetFriendlyName());
            Entry->SetStringField(TEXT("type"), PluginTypeToString(Plugin->GetType()));
            Entry->SetStringField(TEXT("location"), PluginLoadedFromToString(Plugin->GetLoadedFrom()));
            Entry->SetNumberField(TEXT("version"), Desc.Version);
            if (!Desc.VersionName.IsEmpty())
            {
                Entry->SetStringField(TEXT("version_name"), Desc.VersionName);
            }
            if (!Desc.Category.IsEmpty())
            {
                Entry->SetStringField(TEXT("category"), Desc.Category);
            }
            if (!Desc.Description.IsEmpty())
            {
                Entry->SetStringField(TEXT("description"), Desc.Description);
            }
            if (!Desc.CreatedBy.IsEmpty())
            {
                Entry->SetStringField(TEXT("created_by"), Desc.CreatedBy);
            }
            if (!Desc.EngineVersion.IsEmpty())
            {
                Entry->SetStringField(TEXT("engine_version"), Desc.EngineVersion);
            }
            Entry->SetBoolField(TEXT("can_contain_content"), Desc.bCanContainContent);
            Entry->SetBoolField(TEXT("is_beta"), Desc.bIsBetaVersion);
            Entry->SetBoolField(TEXT("is_experimental"), Desc.bIsExperimentalVersion);
            Entry->SetStringField(TEXT("base_dir"), FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()));

            PluginArr.Add(MakeShared<FJsonValueObject>(Entry));
        }

        Result->SetArrayField(TEXT("enabled_plugins"), PluginArr);
        Result->SetNumberField(TEXT("enabled_plugin_count"), PluginArr.Num());
        if (!bIncludeEnginePlugins)
        {
            Result->SetNumberField(TEXT("engine_plugins_filtered_count"), EngineFilteredCount);
        }
    }

    // Source modules. Read off the .uproject so the caller sees what
    // the project author opted in to, not the firehose of every loaded
    // engine module.
    if (bIncludeModules)
    {
        TArray<TSharedPtr<FJsonValue>> ModuleArr;
        if (const FProjectDescriptor* ProjectDesc = IProjectManager::Get().GetCurrentProject())
        {
            for (const FModuleDescriptor& Module : ProjectDesc->Modules)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Module.Name.ToString());
                Entry->SetStringField(TEXT("type"), EHostType::ToString(Module.Type));
                Entry->SetStringField(TEXT("loading_phase"), ELoadingPhase::ToString(Module.LoadingPhase));
                ModuleArr.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
        Result->SetArrayField(TEXT("source_modules"), ModuleArr);
    }

    // Content roots: every immediate subfolder under /Game/ with an
    // asset count. Recursive count by default; this is the more useful
    // designer signal ("how big is /Game/Crafting") and the asset
    // registry already keeps the index hot.
    if (bIncludeContentRoots)
    {
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        // The asset registry exposes both the assets-by-path and the
        // sub-paths walk; we use the sub-paths walk so we do not
        // accidentally hit assets that live directly under /Game/.
        TArray<FString> SubPaths;
        AssetRegistry.GetSubPaths(TEXT("/Game"), SubPaths, /*bRecurse=*/false);
        SubPaths.Sort();

        TArray<TSharedPtr<FJsonValue>> ContentRootArr;
        const int32 EmitCount = MaxContentRoots > 0 ? FMath::Min(SubPaths.Num(), MaxContentRoots) : SubPaths.Num();
        for (int32 i = 0; i < EmitCount; ++i)
        {
            const FString& SubPath = SubPaths[i];

            FARFilter Filter;
            Filter.PackagePaths.Add(*SubPath);
            Filter.bRecursivePaths = bContentRootsRecursive;

            TArray<FAssetData> AssetData;
            AssetRegistry.GetAssets(Filter, AssetData);

            // Folder name is the last path segment.
            FString FolderName = SubPath;
            const int32 LastSlash = FolderName.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
            if (LastSlash != INDEX_NONE)
            {
                FolderName = FolderName.RightChop(LastSlash + 1);
            }

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), FolderName);
            Entry->SetStringField(TEXT("path"), SubPath);
            Entry->SetNumberField(TEXT("asset_count"), AssetData.Num());
            ContentRootArr.Add(MakeShared<FJsonValueObject>(Entry));
        }

        Result->SetArrayField(TEXT("content_roots"), ContentRootArr);
        Result->SetNumberField(TEXT("content_root_count"), SubPaths.Num());
        if (SubPaths.Num() > EmitCount)
        {
            Result->SetBoolField(TEXT("content_roots_truncated"), true);
        }
        Result->SetBoolField(TEXT("content_roots_recursive_count"), bContentRootsRecursive);
    }

    return Result;
}
