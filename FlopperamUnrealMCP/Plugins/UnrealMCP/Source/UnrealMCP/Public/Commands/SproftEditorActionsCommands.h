#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: editor_actions
 *
 * A single multiplexed handler that exposes the small set of editor lifecycle
 * verbs we need from the MCP client: save, undo, redo, focus selection, and
 * play / stop play in editor. The "action" parameter selects the verb, so the
 * Python side can ship a single tool with a clear enum.
 *
 * This is a clean-room implementation derived from the public UE5 API
 * (FEditorFileUtils, GEditor undo/redo + RequestPlaySession, and viewport
 * focus helpers). It does not link against or copy any FlopAI sources.
 */
class UNREALMCP_API FSproftEditorActionsCommands
{
public:
    FSproftEditorActionsCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleEditorActions(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> ActionSaveAll(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> ActionSaveCurrentLevel();
    TSharedPtr<FJsonObject> ActionSaveAsset(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> ActionUndo();
    TSharedPtr<FJsonObject> ActionRedo();
    TSharedPtr<FJsonObject> ActionFocusSelection();
    TSharedPtr<FJsonObject> ActionPlayInEditor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> ActionStopPlayInEditor();
};
