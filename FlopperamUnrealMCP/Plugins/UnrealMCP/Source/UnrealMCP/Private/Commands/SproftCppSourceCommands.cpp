#include "Commands/SproftCppSourceCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "GameFramework/Actor.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SourceCodeNavigation.h"
#include "UObject/Class.h"
#include "UObject/Object.h"

namespace
{
    /** Resolve a class identifier into a UClass. Accepts:
     *  - `/Script/Module.ClassName`
     *  - `/Game/...` Blueprint class path (auto-suffixed with `_C`)
     *  - short class name (e.g. `Actor`, `MyCharacter`).
     *
     *  Returns null when nothing resolves.
     */
    UClass* CppSource_ResolveClass(const FString& Input)
    {
        const FString Trimmed = Input.TrimStartAndEnd();
        if (Trimmed.IsEmpty())
        {
            return nullptr;
        }

        if (Trimmed.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *Trimmed))
            {
                return Loaded;
            }
        }
        if (Trimmed.StartsWith(TEXT("/Game/")))
        {
            FString WithSuffix = Trimmed;
            if (!WithSuffix.EndsWith(TEXT("_C")))
            {
                WithSuffix += TEXT("_C");
            }
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *WithSuffix))
            {
                return Loaded;
            }
        }

        // In-memory short-name probe with optional A/U/F prefix variants.
        TArray<FString> Candidates;
        Candidates.Add(Trimmed);
        if (!Trimmed.StartsWith(TEXT("A")) && !Trimmed.StartsWith(TEXT("U")))
        {
            Candidates.Add(TEXT("A") + Trimmed);
            Candidates.Add(TEXT("U") + Trimmed);
        }
        for (const FString& Candidate : Candidates)
        {
            if (UClass* Found = FindObject<UClass>(nullptr, *Candidate))
            {
                return Found;
            }
        }
        // Engine module fallback for common short names.
        for (const FString& Candidate : Candidates)
        {
            const FString EnginePath = FString::Printf(TEXT("/Script/Engine.%s"), *Candidate);
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *EnginePath))
            {
                return Loaded;
            }
        }
        return nullptr;
    }

    /** Read a file into a single FString, capping at MaxBytes. The cap
     *  is applied character-by-character on the loaded string so we
     *  do not have to do double work on the file size. Sets
     *  bOutTruncated when the cap fires. */
    bool LoadFileCapped(const FString& AbsolutePath, int32 MaxBytes,
                        FString& OutText, int32& OutOriginalLen, bool& bOutTruncated)
    {
        OutText.Reset();
        OutOriginalLen = 0;
        bOutTruncated = false;

        FString Whole;
        if (!FFileHelper::LoadFileToString(Whole, *AbsolutePath))
        {
            return false;
        }
        OutOriginalLen = Whole.Len();
        if (MaxBytes > 0 && Whole.Len() > MaxBytes)
        {
            OutText = Whole.Left(MaxBytes);
            bOutTruncated = true;
        }
        else
        {
            OutText = MoveTemp(Whole);
        }
        return true;
    }

    /** Swap a path's extension. Returns the same string when no `.`
     *  exists in the basename. */
    FString WithExtension(const FString& Path, const FString& NewExt)
    {
        FString Dir, Base, Ext;
        FPaths::Split(Path, Dir, Base, Ext);
        return FPaths::Combine(Dir, Base + NewExt);
    }
}

FSproftCppSourceCommands::FSproftCppSourceCommands()
{
}

TSharedPtr<FJsonObject> FSproftCppSourceCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("cpp_source"))
    {
        return HandleCppSource(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown cpp_source command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftCppSourceCommands::HandleCppSource(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
    }

    bool bIncludeHeader = true;
    bool bIncludeSource = true;
    int32 MaxBytes = 262144;

    bool TempBool = false;
    if (Params->TryGetBoolField(TEXT("include_header"), TempBool)) bIncludeHeader = TempBool;
    if (Params->TryGetBoolField(TEXT("include_source"), TempBool)) bIncludeSource = TempBool;
    double TempNum = 0.0;
    if (Params->TryGetNumberField(TEXT("max_bytes"), TempNum))
    {
        MaxBytes = FMath::Max(0, static_cast<int32>(TempNum));
    }

    FString HeaderPath;
    FString SourcePath;
    UClass* ResolvedClass = nullptr;

    // Path-driven branch first; class-driven branch second. The two are
    // mutually exclusive so we surface the resolved path arrays plus
    // optional class metadata.
    FString HeaderPathParam;
    FString SourcePathParam;
    FString ClassParam;

    Params->TryGetStringField(TEXT("header_path"), HeaderPathParam);
    Params->TryGetStringField(TEXT("source_path"), SourcePathParam);
    Params->TryGetStringField(TEXT("class"), ClassParam);

    if (!HeaderPathParam.IsEmpty())
    {
        HeaderPath = FPaths::ConvertRelativePathToFull(HeaderPathParam);
        // Sibling cpp inference: replace .h with .cpp.
        const FString InferredCpp = WithExtension(HeaderPath, TEXT(".cpp"));
        if (FPaths::FileExists(InferredCpp))
        {
            SourcePath = InferredCpp;
        }
    }
    else if (!SourcePathParam.IsEmpty())
    {
        SourcePath = FPaths::ConvertRelativePathToFull(SourcePathParam);
        const FString InferredHeader = WithExtension(SourcePath, TEXT(".h"));
        if (FPaths::FileExists(InferredHeader))
        {
            HeaderPath = InferredHeader;
        }
    }
    else if (!ClassParam.IsEmpty())
    {
        ResolvedClass = CppSource_ResolveClass(ClassParam);
        if (!ResolvedClass)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Failed to resolve class: %s"), *ClassParam));
        }
        FString FoundHeader;
        FString FoundSource;
        if (FSourceCodeNavigation::FindClassHeaderPath(ResolvedClass, FoundHeader))
        {
            HeaderPath = FPaths::ConvertRelativePathToFull(FoundHeader);
        }
        if (FSourceCodeNavigation::FindClassSourcePath(ResolvedClass, FoundSource))
        {
            SourcePath = FPaths::ConvertRelativePathToFull(FoundSource);
        }
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("One of 'class', 'header_path', or 'source_path' is required"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    if (ResolvedClass)
    {
        Result->SetStringField(TEXT("class"), ResolvedClass->GetPathName());
        Result->SetStringField(TEXT("class_short"), ResolvedClass->GetName());

        // Module name + dir lookup. FindClassModuleName returns the
        // friendly module name; FindModulePath gives the absolute
        // module dir on disk.
        FString ModuleName;
        if (FSourceCodeNavigation::FindClassModuleName(ResolvedClass, ModuleName))
        {
            Result->SetStringField(TEXT("module"), ModuleName);
            FString ModuleDir;
            if (FSourceCodeNavigation::FindModulePath(ModuleName, ModuleDir))
            {
                Result->SetStringField(TEXT("module_dir"), FPaths::ConvertRelativePathToFull(ModuleDir));
            }
        }
    }

    // Header readback.
    if (!HeaderPath.IsEmpty())
    {
        const bool bExists = FPaths::FileExists(HeaderPath);
        Result->SetStringField(TEXT("header_path"), HeaderPath);
        Result->SetBoolField(TEXT("header_exists"), bExists);
        if (bIncludeHeader && bExists)
        {
            FString Text;
            int32 OriginalLen = 0;
            bool bTruncated = false;
            if (LoadFileCapped(HeaderPath, MaxBytes, Text, OriginalLen, bTruncated))
            {
                Result->SetStringField(TEXT("header_text"), Text);
                Result->SetNumberField(TEXT("header_text_bytes"), OriginalLen);
                Result->SetBoolField(TEXT("header_truncated"), bTruncated);
            }
            else
            {
                Result->SetBoolField(TEXT("header_load_failed"), true);
            }
        }
    }
    else
    {
        Result->SetBoolField(TEXT("header_exists"), false);
    }

    // Source readback (the .cpp). Same shape as the header.
    if (!SourcePath.IsEmpty())
    {
        const bool bExists = FPaths::FileExists(SourcePath);
        Result->SetStringField(TEXT("source_path"), SourcePath);
        Result->SetBoolField(TEXT("source_exists"), bExists);
        if (bIncludeSource && bExists)
        {
            FString Text;
            int32 OriginalLen = 0;
            bool bTruncated = false;
            if (LoadFileCapped(SourcePath, MaxBytes, Text, OriginalLen, bTruncated))
            {
                Result->SetStringField(TEXT("source_text"), Text);
                Result->SetNumberField(TEXT("source_text_bytes"), OriginalLen);
                Result->SetBoolField(TEXT("source_truncated"), bTruncated);
            }
            else
            {
                Result->SetBoolField(TEXT("source_load_failed"), true);
            }
        }
    }
    else
    {
        Result->SetBoolField(TEXT("source_exists"), false);
    }

    return Result;
}
