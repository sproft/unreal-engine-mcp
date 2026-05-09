#include "Commands/SproftSearchAssetsCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/Class.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectHash.h"

namespace
{
    /** Resolve a user-provided class token into an FTopLevelAssetPath.
     *
     *  Accepts:
     *    - "/Script/Module.ClassName" full path (used as-is).
     *    - "/Game/.../FooBP" Blueprint asset path (auto-suffixed with _C
     *      so the resulting class lives under the generated class).
     *    - Bare short name ("StaticMesh"). The short-name path tries the
     *      loaded class set through FindObjectChecked first, then
     *      FindFirstObjectSafe so the AssetRegistry filter can still
     *      enumerate even if the class was unloaded.
     */
    bool ResolveClassPath(const FString& Token, FTopLevelAssetPath& OutPath, FString& OutWarning)
    {
        FString Trimmed = Token;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.IsEmpty())
        {
            return false;
        }

        if (Trimmed.StartsWith(TEXT("/Script/")))
        {
            // /Script/Module.ClassName form; FTopLevelAssetPath wants
            // <PackageName>.<AssetName>, so split on the dot.
            int32 DotIdx = INDEX_NONE;
            if (Trimmed.FindChar('.', DotIdx))
            {
                const FString PackageName = Trimmed.Left(DotIdx);
                const FString AssetName = Trimmed.Mid(DotIdx + 1);
                OutPath = FTopLevelAssetPath(*PackageName, *AssetName);
                return true;
            }
            OutWarning = FString::Printf(TEXT("Class token '%s' missing dot between module and class"), *Trimmed);
            return false;
        }

        if (Trimmed.StartsWith(TEXT("/Game/")))
        {
            // Blueprint asset path -> generated class. Caller may or may
            // not have appended _C; normalise to "_C" so AssetRegistry
            // matches the generated class entry.
            FString PackagePath = Trimmed;
            int32 DotIdx = INDEX_NONE;
            if (PackagePath.FindChar('.', DotIdx))
            {
                PackagePath = PackagePath.Left(DotIdx);
            }
            FString AssetShortName;
            int32 LastSlash = INDEX_NONE;
            if (PackagePath.FindLastChar('/', LastSlash))
            {
                AssetShortName = PackagePath.Mid(LastSlash + 1);
            }
            else
            {
                AssetShortName = PackagePath;
            }
            if (!AssetShortName.EndsWith(TEXT("_C")))
            {
                AssetShortName.Append(TEXT("_C"));
            }
            OutPath = FTopLevelAssetPath(*PackagePath, *AssetShortName);
            return true;
        }

        // Bare short name. Walk the loaded class set and pick the first
        // matching UClass; the AssetRegistry will still filter purely by
        // path so this only works if the class is loaded. That is fine
        // for most editor-time queries.
        UClass* Resolved = nullptr;
        ForEachObjectOfClass(UClass::StaticClass(), [&Resolved, &Trimmed](UObject* Obj)
        {
            UClass* Cls = Cast<UClass>(Obj);
            if (Cls && Cls->GetName() == Trimmed)
            {
                Resolved = Cls;
            }
        });
        if (Resolved)
        {
            OutPath = Resolved->GetClassPathName();
            return true;
        }

        OutWarning = FString::Printf(TEXT("Could not resolve class token '%s'. Use a full /Script/Module.ClassName path."), *Trimmed);
        return false;
    }

    /** Append a class token (or array of class tokens) to the filter.
     *  Appends warnings to OutWarnings; never throws. */
    void AppendClassPaths(const TSharedPtr<FJsonObject>& Params, const FString& Field, FARFilter& Filter, TArray<FString>& OutWarnings)
    {
        if (!Params->HasField(Field))
        {
            return;
        }
        const TSharedPtr<FJsonValue> Val = Params->TryGetField(Field);
        if (!Val.IsValid())
        {
            return;
        }
        TArray<FString> Tokens;
        if (Val->Type == EJson::String)
        {
            Tokens.Add(Val->AsString());
        }
        else if (Val->Type == EJson::Array)
        {
            for (const TSharedPtr<FJsonValue>& Entry : Val->AsArray())
            {
                if (Entry.IsValid() && Entry->Type == EJson::String)
                {
                    Tokens.Add(Entry->AsString());
                }
            }
        }
        for (const FString& Token : Tokens)
        {
            FTopLevelAssetPath ClassPath;
            FString Warning;
            if (ResolveClassPath(Token, ClassPath, Warning))
            {
                Filter.ClassPaths.AddUnique(ClassPath);
            }
            else if (!Warning.IsEmpty())
            {
                OutWarnings.Add(Warning);
            }
        }
    }

    /** Append a path token (or array of path tokens) to the filter. */
    void AppendPackagePaths(const TSharedPtr<FJsonObject>& Params, const FString& Field, FARFilter& Filter)
    {
        if (!Params->HasField(Field))
        {
            return;
        }
        const TSharedPtr<FJsonValue> Val = Params->TryGetField(Field);
        if (!Val.IsValid())
        {
            return;
        }
        auto AddOne = [&Filter](const FString& InPath)
        {
            FString Path = InPath;
            Path.TrimStartAndEndInline();
            if (Path.IsEmpty())
            {
                return;
            }
            // FARFilter::PackagePaths wants no trailing slash and a
            // leading slash, e.g. "/Game/Crafting".
            if (!Path.StartsWith(TEXT("/")))
            {
                Path = FString(TEXT("/")) + Path;
            }
            if (Path.EndsWith(TEXT("/")) && Path.Len() > 1)
            {
                Path.LeftChopInline(1);
            }
            Filter.PackagePaths.AddUnique(*Path);
        };
        if (Val->Type == EJson::String)
        {
            AddOne(Val->AsString());
        }
        else if (Val->Type == EJson::Array)
        {
            for (const TSharedPtr<FJsonValue>& Entry : Val->AsArray())
            {
                if (Entry.IsValid() && Entry->Type == EJson::String)
                {
                    AddOne(Entry->AsString());
                }
            }
        }
    }

    /** Append package-tag filters (object or array of objects). */
    void AppendTagsAndValues(const TSharedPtr<FJsonObject>& Params, FARFilter& Filter)
    {
        if (!Params->HasField(TEXT("tag")) && !Params->HasField(TEXT("tags")))
        {
            return;
        }
        const TSharedPtr<FJsonValue> Val = Params->HasField(TEXT("tag"))
            ? Params->TryGetField(TEXT("tag"))
            : Params->TryGetField(TEXT("tags"));
        if (!Val.IsValid())
        {
            return;
        }

        auto AddOne = [&Filter](const TSharedPtr<FJsonObject>& Obj)
        {
            if (!Obj.IsValid())
            {
                return;
            }
            FString Name;
            if (!Obj->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
            {
                return;
            }
            FString Value;
            if (Obj->TryGetStringField(TEXT("value"), Value))
            {
                Filter.TagsAndValues.Add(FName(*Name), TOptional<FString>(Value));
            }
            else
            {
                Filter.TagsAndValues.Add(FName(*Name), TOptional<FString>());
            }
        };

        if (Val->Type == EJson::Object)
        {
            AddOne(Val->AsObject());
        }
        else if (Val->Type == EJson::Array)
        {
            for (const TSharedPtr<FJsonValue>& Entry : Val->AsArray())
            {
                if (Entry.IsValid() && Entry->Type == EJson::Object)
                {
                    AddOne(Entry->AsObject());
                }
            }
        }
    }
}

FSproftSearchAssetsCommands::FSproftSearchAssetsCommands()
{
}

TSharedPtr<FJsonObject> FSproftSearchAssetsCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("search_assets"))
    {
        return HandleSearchAssets(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown search_assets command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftSearchAssetsCommands::HandleSearchAssets(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
    if (!AssetRegistry)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("AssetRegistry module is not available"));
    }

    FARFilter Filter;
    TArray<FString> Warnings;

    // Class filter (single + list).
    AppendClassPaths(Params, TEXT("class"), Filter, Warnings);
    AppendClassPaths(Params, TEXT("class_list"), Filter, Warnings);
    bool bRecursiveClasses = false;
    Params->TryGetBoolField(TEXT("include_subclasses"), bRecursiveClasses);
    Filter.bRecursiveClasses = bRecursiveClasses;

    // Path filter (single + list).
    AppendPackagePaths(Params, TEXT("path"), Filter);
    AppendPackagePaths(Params, TEXT("path_list"), Filter);
    bool bRecursivePaths = true;
    Params->TryGetBoolField(TEXT("recursive_paths"), bRecursivePaths);
    Filter.bRecursivePaths = bRecursivePaths;

    // Package tags.
    AppendTagsAndValues(Params, Filter);

    // Post-filter strings.
    FString NamePattern;
    Params->TryGetStringField(TEXT("name_pattern"), NamePattern);
    FString ClassPattern;
    Params->TryGetStringField(TEXT("class_pattern"), ClassPattern);

    int32 Limit = 256;
    int32 LimitFromJson = 0;
    if (Params->TryGetNumberField(TEXT("limit"), LimitFromJson) && LimitFromJson > 0)
    {
        Limit = LimitFromJson;
    }
    if (Limit > 50000) { Limit = 50000; }

    bool bIncludeDiskSize = false;
    Params->TryGetBoolField(TEXT("include_disk_size"), bIncludeDiskSize);

    // Special-case the "no filter at all" path. FARFilter::IsEmpty would
    // make AssetRegistry skip the lookup. The hosted Flop tool's documented
    // behaviour is to require at least one constraint; we accept it but
    // surface a warning so callers do not pull the entire content tree by
    // accident.
    if (Filter.IsEmpty() && NamePattern.IsEmpty() && ClassPattern.IsEmpty())
    {
        Warnings.Add(TEXT("search_assets called with no filters; supply at least one of class / path / name_pattern."));
    }

    TArray<FAssetData> Hits;
    if (!Filter.IsEmpty())
    {
        AssetRegistry->GetAssets(Filter, Hits);
    }
    else
    {
        // Fall back to enumerating /Game so we still respect the
        // name / class post-filter.
        FARFilter EveryGameAsset;
        EveryGameAsset.bRecursivePaths = true;
        EveryGameAsset.PackagePaths.Add(FName(TEXT("/Game")));
        AssetRegistry->GetAssets(EveryGameAsset, Hits);
    }

    // Apply post-filters and collect rows up to the limit.
    TArray<TSharedPtr<FJsonValue>> Rows;
    int32 PostFilteredCount = 0;
    bool bLimitHit = false;
    for (const FAssetData& Data : Hits)
    {
        if (!NamePattern.IsEmpty())
        {
            if (!Data.AssetName.ToString().Contains(NamePattern, ESearchCase::IgnoreCase))
            {
                continue;
            }
        }
        if (!ClassPattern.IsEmpty())
        {
            const FString ShortClass = Data.AssetClassPath.GetAssetName().ToString();
            if (!ShortClass.Contains(ClassPattern, ESearchCase::IgnoreCase))
            {
                continue;
            }
        }
        ++PostFilteredCount;
        if (Rows.Num() >= Limit)
        {
            bLimitHit = true;
            continue;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Data.AssetName.ToString());
        Row->SetStringField(TEXT("path"), Data.GetSoftObjectPath().ToString());
        Row->SetStringField(TEXT("class"), Data.AssetClassPath.GetAssetName().ToString());
        Row->SetStringField(TEXT("class_path"), Data.AssetClassPath.ToString());
        Row->SetStringField(TEXT("package"), Data.PackageName.ToString());
        Row->SetStringField(TEXT("package_path"), Data.PackagePath.ToString());

        if (bIncludeDiskSize)
        {
            FAssetPackageData PackageData;
            const UE::AssetRegistry::EExists Exists = AssetRegistry->TryGetAssetPackageData(Data.PackageName, PackageData);
            if (Exists == UE::AssetRegistry::EExists::Exists)
            {
                Row->SetNumberField(TEXT("disk_size"), static_cast<double>(PackageData.DiskSize));
            }
            else
            {
                Row->SetNumberField(TEXT("disk_size"), -1.0);
            }
        }

        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("search_assets"));
    ResultObj->SetArrayField(TEXT("assets"), Rows);
    ResultObj->SetNumberField(TEXT("count"), Rows.Num());
    ResultObj->SetNumberField(TEXT("matched_total"), PostFilteredCount);
    ResultObj->SetBoolField(TEXT("limit_hit"), bLimitHit);
    ResultObj->SetNumberField(TEXT("limit"), Limit);
    if (!Warnings.IsEmpty())
    {
        TArray<TSharedPtr<FJsonValue>> WarningArr;
        for (const FString& W : Warnings)
        {
            WarningArr.Add(MakeShared<FJsonValueString>(W));
        }
        ResultObj->SetArrayField(TEXT("warnings"), WarningArr);
    }
    return ResultObj;
}
