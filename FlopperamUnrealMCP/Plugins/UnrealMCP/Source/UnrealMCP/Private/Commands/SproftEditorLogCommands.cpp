#include "Commands/SproftEditorLogCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "HAL/PlatformOutputDevices.h"
#include "Logging/LogVerbosity.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogSproftMCP, Log, All);

namespace
{
    /** Translate a string verbosity name to the engine enum. Defaults to Log. */
    ELogVerbosity::Type ParseVerbosity(const FString& InVerbosity)
    {
        const FString V = InVerbosity.ToLower().TrimStartAndEnd();
        if (V == TEXT("fatal")) return ELogVerbosity::Fatal;
        if (V == TEXT("error")) return ELogVerbosity::Error;
        if (V == TEXT("warning") || V == TEXT("warn")) return ELogVerbosity::Warning;
        if (V == TEXT("display")) return ELogVerbosity::Display;
        if (V == TEXT("log") || V.IsEmpty()) return ELogVerbosity::Log;
        if (V == TEXT("verbose")) return ELogVerbosity::Verbose;
        if (V == TEXT("veryverbose") || V == TEXT("very_verbose")) return ELogVerbosity::VeryVerbose;
        return ELogVerbosity::Log;
    }

    const TCHAR* VerbosityToString(ELogVerbosity::Type Verbosity)
    {
        switch (Verbosity)
        {
        case ELogVerbosity::Fatal:        return TEXT("Fatal");
        case ELogVerbosity::Error:        return TEXT("Error");
        case ELogVerbosity::Warning:      return TEXT("Warning");
        case ELogVerbosity::Display:      return TEXT("Display");
        case ELogVerbosity::Log:          return TEXT("Log");
        case ELogVerbosity::Verbose:      return TEXT("Verbose");
        case ELogVerbosity::VeryVerbose:  return TEXT("VeryVerbose");
        default:                          return TEXT("Log");
        }
    }

    /** Pull the verbosity tag out of a log line. UE writes "[time][frame]LogCat: Verbosity: message",
     *  with Log lines omitting the verbosity tag (so they look like "...LogCat: message"). Returns
     *  ELogVerbosity::Log if no tag is found. */
    ELogVerbosity::Type DetectLineVerbosity(const FString& Line)
    {
        // The verbosity tag, when present, sits between the first ": " after the
        // category prefix and the next ": ". We look for the explicit tags rather
        // than parsing the brackets, since those are easier to read robustly.
        if (Line.Contains(TEXT(": Fatal: "))) return ELogVerbosity::Fatal;
        if (Line.Contains(TEXT(": Error: "))) return ELogVerbosity::Error;
        if (Line.Contains(TEXT(": Warning: "))) return ELogVerbosity::Warning;
        if (Line.Contains(TEXT(": Display: "))) return ELogVerbosity::Display;
        if (Line.Contains(TEXT(": Verbose: "))) return ELogVerbosity::Verbose;
        if (Line.Contains(TEXT(": VeryVerbose: "))) return ELogVerbosity::VeryVerbose;
        return ELogVerbosity::Log;
    }

    /** Pull the category name (without the leading "Log") from the engine line.
     *  Returns an empty string if none is found. */
    FString DetectLineCategory(const FString& Line)
    {
        // Engine log lines start with bracketed prefixes like "[2025.05.08-18.55.06:123][  0]",
        // followed by "LogTemp: ...". Find the first non-bracket segment.
        int32 Cursor = 0;
        while (Cursor < Line.Len() && Line[Cursor] == '[')
        {
            const int32 CloseBracket = Line.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
            if (CloseBracket == INDEX_NONE)
            {
                return FString();
            }
            Cursor = CloseBracket + 1;
        }

        const int32 ColonIdx = Line.Find(TEXT(": "), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
        if (ColonIdx == INDEX_NONE)
        {
            return FString();
        }

        return Line.Mid(Cursor, ColonIdx - Cursor).TrimStartAndEnd();
    }
}

FSproftEditorLogCommands::FSproftEditorLogCommands()
{
}

TSharedPtr<FJsonObject> FSproftEditorLogCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("editor_log"))
    {
        return HandleEditorLog(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown editor log command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftEditorLogCommands::HandleEditorLog(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Operation;
    Params->TryGetStringField(TEXT("operation"), Operation);
    Operation = Operation.ToLower();
    if (Operation.IsEmpty())
    {
        // Default behaviour is a tail read, since that is the more common use.
        Operation = TEXT("tail");
    }

    if (Operation == TEXT("tail") || Operation == TEXT("read"))
    {
        return TailLog(Params);
    }
    if (Operation == TEXT("write"))
    {
        return WriteLog(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported editor_log operation '%s'. Supported: tail, write"), *Operation));
}

TSharedPtr<FJsonObject> FSproftEditorLogCommands::TailLog(const TSharedPtr<FJsonObject>& Params)
{
    int32 LineCount = 200;
    {
        int32 RequestedLines = 0;
        if (Params->TryGetNumberField(TEXT("lines"), RequestedLines) && RequestedLines > 0)
        {
            LineCount = FMath::Min(RequestedLines, 5000);
        }
    }

    FString CategoryFilter;
    Params->TryGetStringField(TEXT("category"), CategoryFilter);
    CategoryFilter = CategoryFilter.TrimStartAndEnd();

    FString MinVerbosityName;
    Params->TryGetStringField(TEXT("min_verbosity"), MinVerbosityName);
    const ELogVerbosity::Type MinVerbosity = ParseVerbosity(MinVerbosityName);

    FString FilePath;
    Params->TryGetStringField(TEXT("log_path"), FilePath);
    if (FilePath.IsEmpty())
    {
        FilePath = FPlatformOutputDevices::GetAbsoluteLogFilename();
    }
    FilePath = FPaths::ConvertRelativePathToFull(FilePath);

    if (!FPaths::FileExists(FilePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Log file does not exist: %s"), *FilePath));
    }

    TArray<FString> AllLines;
    if (!FFileHelper::LoadFileToStringArray(AllLines, *FilePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to read log file: %s"), *FilePath));
    }

    // Filter pass.
    TArray<FString> Filtered;
    Filtered.Reserve(AllLines.Num());
    for (const FString& Line : AllLines)
    {
        if (!CategoryFilter.IsEmpty())
        {
            const FString DetectedCategory = DetectLineCategory(Line);
            if (!DetectedCategory.Equals(CategoryFilter, ESearchCase::IgnoreCase))
            {
                continue;
            }
        }

        if (!MinVerbosityName.IsEmpty())
        {
            const ELogVerbosity::Type LineVerbosity = DetectLineVerbosity(Line);
            // Lower numeric verbosity == higher severity in the engine enum.
            if ((int32)LineVerbosity > (int32)MinVerbosity)
            {
                continue;
            }
        }

        Filtered.Add(Line);
    }

    const int32 StartIdx = FMath::Max(0, Filtered.Num() - LineCount);
    TArray<TSharedPtr<FJsonValue>> LinesJson;
    LinesJson.Reserve(Filtered.Num() - StartIdx);
    for (int32 Idx = StartIdx; Idx < Filtered.Num(); ++Idx)
    {
        LinesJson.Add(MakeShared<FJsonValueString>(Filtered[Idx]));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("tail"));
    ResultObj->SetStringField(TEXT("log_path"), FilePath);
    ResultObj->SetNumberField(TEXT("lines_total"), AllLines.Num());
    ResultObj->SetNumberField(TEXT("lines_after_filter"), Filtered.Num());
    ResultObj->SetNumberField(TEXT("lines_returned"), LinesJson.Num());
    if (!CategoryFilter.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("category"), CategoryFilter);
    }
    if (!MinVerbosityName.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("min_verbosity"), VerbosityToString(MinVerbosity));
    }
    ResultObj->SetArrayField(TEXT("lines"), LinesJson);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftEditorLogCommands::WriteLog(const TSharedPtr<FJsonObject>& Params)
{
    FString Message;
    if (!Params->TryGetStringField(TEXT("message"), Message))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'message' parameter"));
    }

    FString VerbosityName;
    Params->TryGetStringField(TEXT("verbosity"), VerbosityName);
    const ELogVerbosity::Type Verbosity = ParseVerbosity(VerbosityName);

    // Route through a single category so external readers can grep "LogSproftMCP:".
    switch (Verbosity)
    {
    case ELogVerbosity::Error:
        UE_LOG(LogSproftMCP, Error, TEXT("%s"), *Message);
        break;
    case ELogVerbosity::Warning:
        UE_LOG(LogSproftMCP, Warning, TEXT("%s"), *Message);
        break;
    case ELogVerbosity::Display:
        UE_LOG(LogSproftMCP, Display, TEXT("%s"), *Message);
        break;
    case ELogVerbosity::Verbose:
        UE_LOG(LogSproftMCP, Verbose, TEXT("%s"), *Message);
        break;
    case ELogVerbosity::VeryVerbose:
        UE_LOG(LogSproftMCP, VeryVerbose, TEXT("%s"), *Message);
        break;
    case ELogVerbosity::Fatal:
        // We deliberately do not call UE_LOG with Fatal: it will check() and crash
        // the editor. Demote to Error.
        UE_LOG(LogSproftMCP, Error, TEXT("[fatal-demoted] %s"), *Message);
        break;
    case ELogVerbosity::Log:
    default:
        UE_LOG(LogSproftMCP, Log, TEXT("%s"), *Message);
        break;
    }

    if (GLog)
    {
        GLog->Flush();
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("write"));
    ResultObj->SetStringField(TEXT("category"), TEXT("LogSproftMCP"));
    ResultObj->SetStringField(TEXT("verbosity"), VerbosityToString(Verbosity));
    ResultObj->SetStringField(TEXT("message"), Message);
    return ResultObj;
}
