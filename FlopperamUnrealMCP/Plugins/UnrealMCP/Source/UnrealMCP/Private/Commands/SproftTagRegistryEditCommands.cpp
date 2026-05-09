#include "Commands/SproftTagRegistryEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsModule.h"
#include "GameplayTagsSettings.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"

#if WITH_EDITOR
#include "GameplayTagsEditorModule.h"
#endif

namespace
{
    /** Resolve the on-disk config path for a tag source if its backing
     *  UGameplayTagsList object exposes one. Returns the empty string for
     *  Native / DataTable / unsaved sources. */
    FString ResolveSourceIniPath(const FGameplayTagSource* Source)
    {
        if (!Source)
        {
            return FString();
        }
        if (Source->SourceTagList && !Source->SourceTagList->ConfigFileName.IsEmpty())
        {
            return Source->SourceTagList->ConfigFileName;
        }
        if (Source->SourceRestrictedTagList && !Source->SourceRestrictedTagList->ConfigFileName.IsEmpty())
        {
            return Source->SourceRestrictedTagList->ConfigFileName;
        }
        return FString();
    }

    /** Stable string label for a tag source enum value. */
    FString SourceTypeLabel(EGameplayTagSourceType Type)
    {
        switch (Type)
        {
            case EGameplayTagSourceType::Native:           return TEXT("Native");
            case EGameplayTagSourceType::DefaultTagList:   return TEXT("DefaultTagList");
            case EGameplayTagSourceType::TagList:          return TEXT("TagList");
            case EGameplayTagSourceType::RestrictedTagList:return TEXT("RestrictedTagList");
            case EGameplayTagSourceType::DataTable:        return TEXT("DataTable");
            default:                                       return TEXT("Invalid");
        }
    }
}

FSproftTagRegistryEditCommands::FSproftTagRegistryEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftTagRegistryEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("tag_registry_edit"))
    {
        return HandleTagRegistryEdit(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown tag_registry_edit command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftTagRegistryEditCommands::HandleTagRegistryEdit(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Operation;
    if (!Params->TryGetStringField(TEXT("operation"), Operation))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'operation' parameter"));
    }
    Operation = Operation.ToLower();

    if (Operation == TEXT("add_tag") || Operation == TEXT("add"))
    {
        return AddTag(Params);
    }
    if (Operation == TEXT("remove_tag") || Operation == TEXT("delete_tag") || Operation == TEXT("remove"))
    {
        return RemoveTag(Params);
    }
    if (Operation == TEXT("list_tags") || Operation == TEXT("list") || Operation == TEXT("query"))
    {
        return ListTags(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported tag_registry_edit operation '%s'. Supported: add_tag, remove_tag, list_tags"), *Operation));
}

TSharedPtr<FJsonObject> FSproftTagRegistryEditCommands::AddTag(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_EDITOR
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("tag_registry_edit add_tag requires the editor"));
#else
    FString TagName;
    if (!Params->TryGetStringField(TEXT("tag"), TagName)
        && !Params->TryGetStringField(TEXT("name"), TagName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'tag' parameter"));
    }
    TagName.TrimStartAndEndInline();
    if (TagName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'tag' parameter is empty"));
    }

    FString Comment;
    Params->TryGetStringField(TEXT("comment"), Comment);
    if (Comment.IsEmpty())
    {
        Params->TryGetStringField(TEXT("dev_comment"), Comment);
    }

    // Default the source to the standard project ini, matching what the
    // Project Settings widget would do when no specific source is picked.
    FString SourceString;
    Params->TryGetStringField(TEXT("source"), SourceString);
    FName SourceName = SourceString.IsEmpty()
        ? FName(TEXT("DefaultGameplayTags.ini"))
        : FName(*SourceString);

    bool bIsRestrictedTag = false;
    Params->TryGetBoolField(TEXT("restricted"), bIsRestrictedTag);

    bool bAllowNonRestrictedChildren = true;
    Params->TryGetBoolField(TEXT("allow_non_restricted_children"), bAllowNonRestrictedChildren);

    if (!IGameplayTagsEditorModule::IsAvailable())
    {
        FModuleManager::Get().LoadModule(TEXT("GameplayTagsEditor"));
    }
    if (!IGameplayTagsEditorModule::IsAvailable())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("GameplayTagsEditor module is not available"));
    }
    IGameplayTagsEditorModule& EditorModule = IGameplayTagsEditorModule::Get();

    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    const bool bAlreadyExists = Manager.RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/false).IsValid();

    const bool bWritten = EditorModule.AddNewGameplayTagToINI(
        TagName,
        Comment,
        SourceName,
        bIsRestrictedTag,
        bAllowNonRestrictedChildren);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_tag"));
    Result->SetStringField(TEXT("tag"), TagName);
    Result->SetStringField(TEXT("source"), SourceName.ToString());
    if (!Comment.IsEmpty())
    {
        Result->SetStringField(TEXT("comment"), Comment);
    }
    Result->SetBoolField(TEXT("already_existed"), bAlreadyExists);
    Result->SetBoolField(TEXT("written"), bWritten);
    Result->SetBoolField(TEXT("restricted"), bIsRestrictedTag);

    // The editor module rebuilds the dictionary and broadcasts after a
    // successful write. We surface that as a hint so callers know the live
    // tag pickers should refresh on their own.
    Result->SetBoolField(TEXT("requires_reload"), false);

    if (const FGameplayTagSource* Source = Manager.FindTagSource(SourceName))
    {
        const FString IniPath = ResolveSourceIniPath(Source);
        if (!IniPath.IsEmpty())
        {
            Result->SetStringField(TEXT("source_ini_path"), IniPath);
        }
        Result->SetStringField(TEXT("source_type"), SourceTypeLabel(Source->SourceType));
    }
    return Result;
#endif // WITH_EDITOR
}

TSharedPtr<FJsonObject> FSproftTagRegistryEditCommands::RemoveTag(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_EDITOR
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("tag_registry_edit remove_tag requires the editor"));
#else
    FString TagName;
    if (!Params->TryGetStringField(TEXT("tag"), TagName)
        && !Params->TryGetStringField(TEXT("name"), TagName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'tag' parameter"));
    }
    TagName.TrimStartAndEndInline();
    if (TagName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'tag' parameter is empty"));
    }

    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    TSharedPtr<FGameplayTagNode> Node = Manager.FindTagNode(FName(*TagName));
    if (!Node.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Tag '%s' is not registered"), *TagName));
    }

    FName SourceName = NAME_None;
#if WITH_EDITORONLY_DATA
    SourceName = Node->GetFirstSourceName();
#endif

    if (!IGameplayTagsEditorModule::IsAvailable())
    {
        FModuleManager::Get().LoadModule(TEXT("GameplayTagsEditor"));
    }
    if (!IGameplayTagsEditorModule::IsAvailable())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("GameplayTagsEditor module is not available"));
    }
    IGameplayTagsEditorModule& EditorModule = IGameplayTagsEditorModule::Get();

    const bool bDeleted = EditorModule.DeleteTagFromINI(Node);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("remove_tag"));
    Result->SetStringField(TEXT("tag"), TagName);
    Result->SetBoolField(TEXT("deleted"), bDeleted);
    Result->SetBoolField(TEXT("requires_reload"), false);
    if (!SourceName.IsNone())
    {
        Result->SetStringField(TEXT("source"), SourceName.ToString());
        if (const FGameplayTagSource* Source = Manager.FindTagSource(SourceName))
        {
            const FString IniPath = ResolveSourceIniPath(Source);
            if (!IniPath.IsEmpty())
            {
                Result->SetStringField(TEXT("source_ini_path"), IniPath);
            }
            Result->SetStringField(TEXT("source_type"), SourceTypeLabel(Source->SourceType));
        }
    }
    return Result;
#endif // WITH_EDITOR
}

TSharedPtr<FJsonObject> FSproftTagRegistryEditCommands::ListTags(const TSharedPtr<FJsonObject>& Params)
{
    FString Pattern;
    Params->TryGetStringField(TEXT("pattern"), Pattern);
    if (Pattern.IsEmpty())
    {
        Params->TryGetStringField(TEXT("query"), Pattern);
    }
    Pattern = Pattern.ToLower();

    bool bOnlyDictionaryTags = false;
    Params->TryGetBoolField(TEXT("only_dictionary_tags"), bOnlyDictionaryTags);

    int32 Limit = 256;
    int32 ParsedLimit = 0;
    if (Params->TryGetNumberField(TEXT("limit"), ParsedLimit) && ParsedLimit > 0)
    {
        Limit = ParsedLimit;
    }

    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    FGameplayTagContainer Container;
    Manager.RequestAllGameplayTags(Container, bOnlyDictionaryTags);

    int32 TotalScanned = 0;
    int32 TotalMatches = 0;
    TArray<TSharedPtr<FJsonValue>> Tags;
    for (const FGameplayTag& Tag : Container)
    {
        ++TotalScanned;
        const FString TagString = Tag.ToString();
        if (!Pattern.IsEmpty() && !TagString.ToLower().Contains(Pattern))
        {
            continue;
        }
        ++TotalMatches;
        if (Tags.Num() >= Limit)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), TagString);

        TSharedPtr<FGameplayTagNode> Node = Manager.FindTagNode(Tag);
        if (Node.IsValid())
        {
#if WITH_EDITORONLY_DATA
            const FName FirstSource = Node->GetFirstSourceName();
            if (!FirstSource.IsNone())
            {
                Entry->SetStringField(TEXT("source"), FirstSource.ToString());
                if (const FGameplayTagSource* Src = Manager.FindTagSource(FirstSource))
                {
                    const FString IniPath = ResolveSourceIniPath(Src);
                    if (!IniPath.IsEmpty())
                    {
                        Entry->SetStringField(TEXT("source_ini_path"), IniPath);
                    }
                    Entry->SetStringField(TEXT("source_type"), SourceTypeLabel(Src->SourceType));
                }
            }
            const FString DevComment = Node->GetDevComment();
            if (!DevComment.IsEmpty())
            {
                Entry->SetStringField(TEXT("comment"), DevComment);
            }
            Entry->SetBoolField(TEXT("explicit"), Node->IsExplicitTag());
            Entry->SetBoolField(TEXT("restricted"), Node->IsRestrictedGameplayTag());
#endif
        }
        Tags.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("list_tags"));
    if (!Pattern.IsEmpty())
    {
        Result->SetStringField(TEXT("pattern"), Pattern);
    }
    Result->SetBoolField(TEXT("only_dictionary_tags"), bOnlyDictionaryTags);
    Result->SetNumberField(TEXT("scanned"), TotalScanned);
    Result->SetNumberField(TEXT("total_matches"), TotalMatches);
    Result->SetNumberField(TEXT("returned"), Tags.Num());
    Result->SetBoolField(TEXT("truncated"), Tags.Num() < TotalMatches);
    Result->SetNumberField(TEXT("limit"), Limit);
    Result->SetArrayField(TEXT("tags"), Tags);
    return Result;
}
