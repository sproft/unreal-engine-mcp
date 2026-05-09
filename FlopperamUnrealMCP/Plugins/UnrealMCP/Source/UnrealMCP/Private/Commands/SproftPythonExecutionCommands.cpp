#include "Commands/SproftPythonExecutionCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "HAL/PlatformFileManager.h"
#include "IPythonScriptPlugin.h"
#include "Misc/Paths.h"
#include "PythonScriptTypes.h"

namespace
{
    /** Map a short mode name to EPythonCommandExecutionMode. Returns false
     *  with an empty out value if the supplied text does not match. */
    bool TryParseExecutionMode(const FString& InText, EPythonCommandExecutionMode& OutMode)
    {
        const FString T = InText.ToLower().TrimStartAndEnd();
        if (T == TEXT("execute_file") || T == TEXT("file"))
        {
            OutMode = EPythonCommandExecutionMode::ExecuteFile;
            return true;
        }
        if (T == TEXT("execute_statement") || T == TEXT("statement") || T == TEXT("exec"))
        {
            OutMode = EPythonCommandExecutionMode::ExecuteStatement;
            return true;
        }
        if (T == TEXT("evaluate_statement") || T == TEXT("evaluate") || T == TEXT("eval"))
        {
            OutMode = EPythonCommandExecutionMode::EvaluateStatement;
            return true;
        }
        return false;
    }

    FString ExecutionModeToString(EPythonCommandExecutionMode InMode)
    {
        switch (InMode)
        {
            case EPythonCommandExecutionMode::ExecuteFile:      return TEXT("execute_file");
            case EPythonCommandExecutionMode::ExecuteStatement: return TEXT("execute_statement");
            case EPythonCommandExecutionMode::EvaluateStatement:return TEXT("evaluate_statement");
        }
        return TEXT("unknown");
    }

    FString LogTypeToString(EPythonLogOutputType InType)
    {
        switch (InType)
        {
            case EPythonLogOutputType::Info:    return TEXT("info");
            case EPythonLogOutputType::Warning: return TEXT("warning");
            case EPythonLogOutputType::Error:   return TEXT("error");
        }
        return TEXT("unknown");
    }

    /** Cap a string at MaxLen with a trailing ellipsis. Cheap belt-and-braces
     *  guard against a runaway script flooding the JSON channel. */
    FString TruncateForResponse(const FString& In, int32 MaxLen)
    {
        if (MaxLen <= 0 || In.Len() <= MaxLen)
        {
            return In;
        }
        return In.Left(FMath::Max(0, MaxLen - 3)) + TEXT("...");
    }
}

FSproftPythonExecutionCommands::FSproftPythonExecutionCommands()
{
}

TSharedPtr<FJsonObject> FSproftPythonExecutionCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("python_execution"))
    {
        return HandlePythonExecution(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown python_execution command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftPythonExecutionCommands::HandlePythonExecution(const TSharedPtr<FJsonObject>& Params)
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

    // The two documented operations forward to the same engine entry point.
    // execute_string is a convenience that takes a literal `code` payload;
    // execute_file takes a `path` to a .py file on disk.
    bool bExecuteString = false;
    bool bExecuteFile = false;
    if (Operation == TEXT("execute_string") || Operation == TEXT("exec_string")
        || Operation == TEXT("string"))
    {
        bExecuteString = true;
    }
    else if (Operation == TEXT("execute_file") || Operation == TEXT("exec_file")
        || Operation == TEXT("file"))
    {
        bExecuteFile = true;
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported python_execution operation '%s'. Supported: execute_string, execute_file"), *Operation));
    }

    // Resolve the actual command string we hand off to the interpreter.
    FString Command;
    if (bExecuteString)
    {
        if (!Params->TryGetStringField(TEXT("code"), Command)
            && !Params->TryGetStringField(TEXT("source"), Command)
            && !Params->TryGetStringField(TEXT("command"), Command))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'code' parameter for execute_string"));
        }
    }
    else // bExecuteFile
    {
        FString Path;
        if (!Params->TryGetStringField(TEXT("path"), Path)
            && !Params->TryGetStringField(TEXT("file"), Path))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'path' parameter for execute_file"));
        }

        // Reject up-front when the file is missing. ExecPythonCommandEx would
        // surface the same error through Python output, but a clean structured
        // error is friendlier for callers.
        if (!FPaths::FileExists(Path))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Python file does not exist: %s"), *Path));
        }

        Command = Path;

        // Optional positional args: the engine's "file (with optional
        // arguments)" convention is whitespace-joined onto the file path.
        const TArray<TSharedPtr<FJsonValue>>* ArgsArr = nullptr;
        if (Params->TryGetArrayField(TEXT("args"), ArgsArr) && ArgsArr)
        {
            FString Joined;
            for (const TSharedPtr<FJsonValue>& V : *ArgsArr)
            {
                if (!V.IsValid() || V->Type != EJson::String)
                {
                    continue;
                }
                Joined += TEXT(" ");
                Joined += V->AsString();
            }
            Command += Joined;
        }
    }

    if (Command.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Command is empty"));
    }

    // Optional execution mode override. execute_string defaults to file mode
    // because the engine's statement and evaluate modes do not accept
    // multi-line programs; file mode handles both single-statement and
    // multi-statement payloads.
    EPythonCommandExecutionMode Mode = bExecuteString
        ? EPythonCommandExecutionMode::ExecuteFile
        : EPythonCommandExecutionMode::ExecuteFile;

    FString ModeText;
    if (Params->TryGetStringField(TEXT("mode"), ModeText) && !ModeText.IsEmpty())
    {
        if (!TryParseExecutionMode(ModeText, Mode))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unsupported mode '%s'. Supported: execute_file, execute_statement, evaluate_statement"), *ModeText));
        }
    }

    bool bUnattended = true;
    Params->TryGetBoolField(TEXT("unattended"), bUnattended);

    int32 MaxResponseChars = 64 * 1024;
    if (Params->HasField(TEXT("max_response_chars")))
    {
        MaxResponseChars = static_cast<int32>(Params->GetNumberField(TEXT("max_response_chars")));
        if (MaxResponseChars < 256)
        {
            MaxResponseChars = 256;
        }
    }

    IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
    if (!PythonPlugin)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("PythonScriptPlugin module is not loaded. Enable the 'Python Editor Script Plugin' in the project's plugin settings and restart the editor."));
    }
    if (!PythonPlugin->IsPythonAvailable())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("PythonScriptPlugin reports Python is not available in this editor build."));
    }

    FPythonCommandEx PyCommand;
    PyCommand.Command = Command;
    PyCommand.ExecutionMode = Mode;
    PyCommand.FileExecutionScope = EPythonFileExecutionScope::Private;
    PyCommand.Flags = bUnattended ? EPythonCommandFlags::Unattended : EPythonCommandFlags::None;

    const bool bOk = PythonPlugin->ExecPythonCommandEx(PyCommand);

    // Split the engine's log output into stdout (Info) and stderr (Warning,
    // Error) channels for friendlier consumer code.
    FString StdoutText;
    FString StderrText;
    TArray<TSharedPtr<FJsonValue>> LogEntries;

    for (const FPythonLogOutputEntry& Entry : PyCommand.LogOutput)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("type"), LogTypeToString(Entry.Type));
        Item->SetStringField(TEXT("text"), Entry.Output);
        LogEntries.Add(MakeShared<FJsonValueObject>(Item));

        if (Entry.Type == EPythonLogOutputType::Info)
        {
            StdoutText += Entry.Output;
        }
        else
        {
            StderrText += Entry.Output;
        }
    }

    StdoutText = TruncateForResponse(StdoutText, MaxResponseChars);
    StderrText = TruncateForResponse(StderrText, MaxResponseChars);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), bExecuteString ? TEXT("execute_string") : TEXT("execute_file"));
    Result->SetStringField(TEXT("mode"), ExecutionModeToString(Mode));
    Result->SetBoolField(TEXT("ok"), bOk);
    Result->SetStringField(TEXT("stdout"), StdoutText);
    Result->SetStringField(TEXT("stderr"), StderrText);
    Result->SetStringField(TEXT("command_result"), TruncateForResponse(PyCommand.CommandResult, MaxResponseChars));
    Result->SetArrayField(TEXT("log"), LogEntries);

    if (!bOk)
    {
        // Surface a short single-line error so the JSON status flag reflects
        // the failure even when no Python exception text bubbled up.
        Result->SetBoolField(TEXT("success"), false);
        Result->SetStringField(TEXT("error"),
            PyCommand.CommandResult.IsEmpty()
                ? TEXT("Python command failed (see stderr / log for details)")
                : TruncateForResponse(PyCommand.CommandResult, 1024));
    }

    return Result;
}
