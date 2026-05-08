#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: editor_log
 *
 * Two operations cover the documented behaviour of the hosted Flop
 * "editor_log" tool to the extent we can reach without proprietary plumbing:
 *   - "tail": read the last N lines of the project's log file, with optional
 *     category and minimum-verbosity filters.
 *   - "write": emit a single log line at a chosen verbosity through GLog so
 *     it lands in the Output Log and the on-disk log file.
 *
 * Reading the live in-editor SOutputLog widget would require a hook into the
 * OutputLog module's history buffer, which is private. The on-disk log is
 * authoritative and stays in sync with the editor's Output Log, so that path
 * is what we expose.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - FPlatformOutputDevices::GetAbsoluteLogFilename for the on-disk path
 *   - FFileHelper for line-by-line reads
 *   - UE_LOG with a custom Sproft MCP log category for writes
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftEditorLogCommands
{
public:
    FSproftEditorLogCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleEditorLog(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> TailLog(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> WriteLog(const TSharedPtr<FJsonObject>& Params);
};
