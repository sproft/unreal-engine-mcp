#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: tag_registry_edit
 *
 * Read and edit the project's Gameplay Tag registry. Three operations:
 *
 *   - add_tag: writes a tag (and optional dev comment) into a chosen
 *     `Config/Default*Tags.ini` source through `IGameplayTagsEditorModule::
 *     AddNewGameplayTagToINI`. The editor module handles the ini write,
 *     the in-memory dictionary refresh, and the broadcast that tag pickers
 *     listen on.
 *
 *   - remove_tag: deletes a tag through `IGameplayTagsEditorModule::
 *     DeleteTagFromINI`, which also writes a redirector if children
 *     exist. The editor module manages the rebuild and broadcast.
 *
 *   - list_tags: read-only substring search against
 *     `UGameplayTagsManager::Get().RequestAllGameplayTags`. Returns each
 *     match's FName, owning source name, and (where the source is a
 *     `Config/Default*Tags.ini` file) the resolved on-disk path.
 *
 * The tool runs against the in-editor `IGameplayTagsEditorModule` and
 * therefore requires an editor session. It is documented as such in the
 * Python wrapper.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UGameplayTagsManager (RequestAllGameplayTags, FindTagNode,
 *     FindTagSource, GetTagSourceForTag, RequestGameplayTag).
 *   - IGameplayTagsEditorModule (AddNewGameplayTagToINI, DeleteTagFromINI).
 *   - FGameplayTagSource (SourceName, SourceType, ConfigFileName via the
 *     backing UGameplayTagsList object).
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftTagRegistryEditCommands
{
public:
    FSproftTagRegistryEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleTagRegistryEdit(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> AddTag(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> RemoveTag(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> ListTags(const TSharedPtr<FJsonObject>& Params);
};
