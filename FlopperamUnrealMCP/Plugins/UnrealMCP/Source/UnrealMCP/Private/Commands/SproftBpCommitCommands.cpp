#include "Commands/SproftBpCommitCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"

namespace
{
    UBlueprint* BpCommit_ResolveBlueprintParam(const TSharedPtr<FJsonObject>& Params)
    {
        FString Input;
        if (!Params->TryGetStringField(TEXT("blueprint"), Input)
            && !Params->TryGetStringField(TEXT("blueprint_path"), Input)
            && !Params->TryGetStringField(TEXT("blueprint_name"), Input))
        {
            return nullptr;
        }
        UBlueprint* BP = FEpicUnrealMCPCommonUtils::FindBlueprint(Input);
        if (!BP && Input.StartsWith(TEXT("/")))
        {
            BP = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(Input));
        }
        return BP;
    }

    /** Convert a single FCompilerResultsLog message into a flat string. */
    FString FormatCompilerMessage(const TSharedRef<FTokenizedMessage>& Message)
    {
        return Message->ToText().ToString();
    }
}

FSproftBpCommitCommands::FSproftBpCommitCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpCommitCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_commit"))
    {
        return HandleBpCommit(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_commit command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpCommitCommands::HandleBpCommit(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    UBlueprint* Blueprint = BpCommit_ResolveBlueprintParam(Params);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Blueprint not found or 'blueprint' parameter missing"));
    }

    // Optional op accepted for symmetry with the other multi-op tools, but
    // the only meaningful value is "commit". We accept the omitted form.
    FString Operation;
    if (Params->TryGetStringField(TEXT("op"), Operation)
        || Params->TryGetStringField(TEXT("operation"), Operation))
    {
        Operation = Operation.ToLower();
        if (!Operation.IsEmpty() && Operation != TEXT("commit"))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unsupported bp_commit op '%s'. Only 'commit' is supported."), *Operation));
        }
    }

    bool bMarkStructurally = true;
    Params->TryGetBoolField(TEXT("mark_structurally"), bMarkStructurally);
    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    if (bMarkStructurally)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    }
    else
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }

    TArray<FString> Errors;
    TArray<FString> Warnings;
    TArray<FString> Infos;
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    bool bCompileSucceeded = true;

    if (bCompile)
    {
        FCompilerResultsLog ResultsLog;
        ResultsLog.bSilentMode = true;
        ResultsLog.SetSourcePath(Blueprint->GetPathName());

        FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);

        ErrorCount = ResultsLog.NumErrors;
        WarningCount = ResultsLog.NumWarnings;
        bCompileSucceeded = (ResultsLog.NumErrors == 0);

        for (const TSharedRef<FTokenizedMessage>& Message : ResultsLog.Messages)
        {
            const FString Line = FormatCompilerMessage(Message);
            switch (Message->GetSeverity())
            {
            case EMessageSeverity::Error:
                Errors.Add(Line);
                break;
            case EMessageSeverity::Warning:
            case EMessageSeverity::PerformanceWarning:
                Warnings.Add(Line);
                break;
            case EMessageSeverity::Info:
                Infos.Add(Line);
                break;
            default:
                Infos.Add(Line);
                break;
            }
        }
    }

    // Skip save when the compile produced errors so we do not pin a
    // broken Blueprint to disk. Caller can pass `force_save=true` to
    // override (e.g. snapshot a half-finished asset for diagnostics).
    bool bForceSave = false;
    Params->TryGetBoolField(TEXT("force_save"), bForceSave);
    bool bSavedOk = false;
    if (bSave && (bCompileSucceeded || bForceSave))
    {
        bSavedOk = UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    auto MakeStringArray = [](const TArray<FString>& Items)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& S : Items)
        {
            Arr.Add(MakeShared<FJsonValueString>(S));
        }
        return Arr;
    };

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("commit"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetBoolField(TEXT("mark_structurally"), bMarkStructurally);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("compile_success"), bCompileSucceeded);
    Result->SetNumberField(TEXT("error_count"), ErrorCount);
    Result->SetNumberField(TEXT("warning_count"), WarningCount);
    Result->SetArrayField(TEXT("errors"), MakeStringArray(Errors));
    Result->SetArrayField(TEXT("warnings"), MakeStringArray(Warnings));
    Result->SetArrayField(TEXT("infos"), MakeStringArray(Infos));
    Result->SetBoolField(TEXT("save_requested"), bSave);
    Result->SetBoolField(TEXT("force_save"), bForceSave);
    Result->SetBoolField(TEXT("saved"), bSavedOk);
    // The bridge layer reads `success` to decide whether to wrap the
    // payload in a status:error envelope. We surface compile failure as
    // success=false so the caller's existing error-handling path fires.
    Result->SetBoolField(TEXT("success"), bCompile ? bCompileSucceeded : true);
    if (bCompile && !bCompileSucceeded)
    {
        Result->SetStringField(TEXT("error"),
            FString::Printf(TEXT("Blueprint compile failed with %d error(s)"), ErrorCount));
    }
    return Result;
}
