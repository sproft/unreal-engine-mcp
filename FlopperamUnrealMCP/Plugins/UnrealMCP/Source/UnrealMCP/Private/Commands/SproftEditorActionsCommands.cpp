#include "Commands/SproftEditorActionsCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "FileHelpers.h"
#include "PlayInEditorDataTypes.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"

FSproftEditorActionsCommands::FSproftEditorActionsCommands()
{
}

TSharedPtr<FJsonObject> FSproftEditorActionsCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("editor_actions"))
    {
        return HandleEditorActions(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown editor actions command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftEditorActionsCommands::HandleEditorActions(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Action;
    if (!Params->TryGetStringField(TEXT("action"), Action))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'action' parameter"));
    }

    Action = Action.ToLower();

    if (Action == TEXT("save_all"))
    {
        return ActionSaveAll(Params);
    }
    else if (Action == TEXT("save_current_level"))
    {
        return ActionSaveCurrentLevel();
    }
    else if (Action == TEXT("save_asset"))
    {
        return ActionSaveAsset(Params);
    }
    else if (Action == TEXT("undo"))
    {
        return ActionUndo();
    }
    else if (Action == TEXT("redo"))
    {
        return ActionRedo();
    }
    else if (Action == TEXT("focus_selection") || Action == TEXT("focus"))
    {
        return ActionFocusSelection();
    }
    else if (Action == TEXT("play"))
    {
        return ActionPlayInEditor(Params);
    }
    else if (Action == TEXT("stop_play"))
    {
        return ActionStopPlayInEditor();
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown editor_actions action: %s"), *Action));
}

TSharedPtr<FJsonObject> FSproftEditorActionsCommands::ActionSaveAll(const TSharedPtr<FJsonObject>& Params)
{
    bool bSaveMaps = true;
    bool bSaveContent = true;
    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("save_maps"), bSaveMaps);
        Params->TryGetBoolField(TEXT("save_content"), bSaveContent);
    }

    const bool bOk = UEditorLoadingAndSavingUtils::SaveDirtyPackages(bSaveMaps, bSaveContent);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action"), TEXT("save_all"));
    ResultObj->SetBoolField(TEXT("saved"), bOk);
    ResultObj->SetBoolField(TEXT("save_maps"), bSaveMaps);
    ResultObj->SetBoolField(TEXT("save_content"), bSaveContent);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftEditorActionsCommands::ActionSaveCurrentLevel()
{
    const bool bOk = UEditorLoadingAndSavingUtils::SaveCurrentLevel();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action"), TEXT("save_current_level"));
    ResultObj->SetBoolField(TEXT("saved"), bOk);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftEditorActionsCommands::ActionSaveAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("save_asset requires 'asset_path'"));
    }

    bool bOnlyIfDirty = true;
    Params->TryGetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);

    if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset does not exist: %s"), *AssetPath));
    }

    const bool bOk = UEditorAssetLibrary::SaveAsset(AssetPath, bOnlyIfDirty);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action"), TEXT("save_asset"));
    ResultObj->SetStringField(TEXT("asset_path"), AssetPath);
    ResultObj->SetBoolField(TEXT("saved"), bOk);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftEditorActionsCommands::ActionUndo()
{
    if (!GEditor)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("GEditor is null"));
    }

    const bool bOk = GEditor->UndoTransaction(/*bCanRedo=*/true);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action"), TEXT("undo"));
    ResultObj->SetBoolField(TEXT("performed"), bOk);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftEditorActionsCommands::ActionRedo()
{
    if (!GEditor)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("GEditor is null"));
    }

    const bool bOk = GEditor->RedoTransaction();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action"), TEXT("redo"));
    ResultObj->SetBoolField(TEXT("performed"), bOk);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftEditorActionsCommands::ActionFocusSelection()
{
    if (!GEditor)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("GEditor is null"));
    }

    USelection* Selection = GEditor->GetSelectedActors();
    if (!Selection)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No actor selection available"));
    }

    TArray<AActor*> Selected;
    Selection->GetSelectedObjects<AActor>(Selected);

    if (Selected.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Nothing selected to focus on"));
    }

    GEditor->MoveViewportCamerasToActor(Selected, /*bActiveViewportOnly=*/true);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action"), TEXT("focus_selection"));
    ResultObj->SetNumberField(TEXT("selected_count"), Selected.Num());
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftEditorActionsCommands::ActionPlayInEditor(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("GEditor is null"));
    }

    if (GEditor->IsPlaySessionInProgress() || GEditor->IsPlayingSessionInEditor())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("A play session is already in progress"));
    }

    FRequestPlaySessionParams PlayParams;
    PlayParams.SessionDestination = EPlaySessionDestinationType::InProcess;
    PlayParams.WorldType = EPlaySessionWorldType::PlayInEditor;

    if (Params.IsValid() && Params->HasField(TEXT("start_location")))
    {
        const FVector Loc = FEpicUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("start_location"));
        PlayParams.StartLocation = Loc;
        if (Params->HasField(TEXT("start_rotation")))
        {
            PlayParams.StartRotation = FEpicUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("start_rotation"));
        }
    }

    GEditor->RequestPlaySession(PlayParams);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action"), TEXT("play"));
    ResultObj->SetBoolField(TEXT("requested"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftEditorActionsCommands::ActionStopPlayInEditor()
{
    if (!GEditor)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("GEditor is null"));
    }

    const bool bSessionRunning = GEditor->IsPlaySessionInProgress() || GEditor->IsPlayingSessionInEditor();
    if (bSessionRunning)
    {
        GEditor->RequestEndPlayMap();
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action"), TEXT("stop_play"));
    ResultObj->SetBoolField(TEXT("was_running"), bSessionRunning);
    return ResultObj;
}
