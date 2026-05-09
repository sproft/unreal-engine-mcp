#include "Commands/SproftAssetReferencesCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/AssetRegistryInterface.h"
#include "UObject/Class.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectHash.h"

namespace
{
    enum class EDirection : uint8
    {
        HardReferencers,
        SoftReferencers,
        HardDependencies,
        SoftDependencies,
        AllReferencers,
        AllDependencies,
    };

    bool TryParseDirection(const FString& Token, EDirection& OutDir)
    {
        const FString T = Token.ToLower().Replace(TEXT(" "), TEXT(""));
        if (T == TEXT("hard_referencers") || T == TEXT("hardreferencers")
            || T == TEXT("hard_refs") || T == TEXT("referencers"))
        {
            OutDir = EDirection::HardReferencers;
            return true;
        }
        if (T == TEXT("soft_referencers") || T == TEXT("softreferencers")
            || T == TEXT("soft_refs"))
        {
            OutDir = EDirection::SoftReferencers;
            return true;
        }
        if (T == TEXT("hard_dependencies") || T == TEXT("harddependencies")
            || T == TEXT("hard_deps") || T == TEXT("dependencies"))
        {
            OutDir = EDirection::HardDependencies;
            return true;
        }
        if (T == TEXT("soft_dependencies") || T == TEXT("softdependencies")
            || T == TEXT("soft_deps"))
        {
            OutDir = EDirection::SoftDependencies;
            return true;
        }
        if (T == TEXT("all_referencers") || T == TEXT("allreferencers"))
        {
            OutDir = EDirection::AllReferencers;
            return true;
        }
        if (T == TEXT("all_dependencies") || T == TEXT("alldependencies"))
        {
            OutDir = EDirection::AllDependencies;
            return true;
        }
        return false;
    }

    UE::AssetRegistry::FDependencyQuery BuildQuery(EDirection Dir)
    {
        using namespace UE::AssetRegistry;
        FDependencyQuery Q;
        // For all_* directions we leave properties at None so every
        // package dep / ref comes back regardless of Hard / Soft.
        if (Dir == EDirection::HardReferencers || Dir == EDirection::HardDependencies)
        {
            Q.Required = EDependencyProperty::Hard;
        }
        else if (Dir == EDirection::SoftReferencers || Dir == EDirection::SoftDependencies)
        {
            Q.Excluded = EDependencyProperty::Hard;
        }
        return Q;
    }

    bool IsReferencerDirection(EDirection Dir)
    {
        return Dir == EDirection::HardReferencers
            || Dir == EDirection::SoftReferencers
            || Dir == EDirection::AllReferencers;
    }

    /** Strip "/Path/Pkg.AssetName" -> "/Path/Pkg" so the dependency
     *  walk lines up with FAssetData::PackageName. */
    FName NormalisePackageName(const FString& InAssetPath)
    {
        FString Trim = InAssetPath;
        Trim.TrimStartAndEndInline();
        // Cut off any sub-object suffix (e.g. ":Body").
        int32 ColonIdx = INDEX_NONE;
        if (Trim.FindChar(':', ColonIdx))
        {
            Trim = Trim.Left(ColonIdx);
        }
        // Cut off the asset-name suffix.
        int32 DotIdx = INDEX_NONE;
        if (Trim.FindChar('.', DotIdx))
        {
            Trim = Trim.Left(DotIdx);
        }
        return FName(*Trim);
    }

    /** Resolve a class token into an FTopLevelAssetPath for the
     *  optional class filter. Mirrors search_assets' resolver. */
    bool ResolveClassPath(const FString& Token, FTopLevelAssetPath& OutPath)
    {
        FString Trimmed = Token;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.IsEmpty())
        {
            return false;
        }
        if (Trimmed.StartsWith(TEXT("/Script/")))
        {
            int32 DotIdx = INDEX_NONE;
            if (Trimmed.FindChar('.', DotIdx))
            {
                OutPath = FTopLevelAssetPath(*Trimmed.Left(DotIdx), *Trimmed.Mid(DotIdx + 1));
                return true;
            }
            return false;
        }
        if (Trimmed.StartsWith(TEXT("/Game/")))
        {
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
        return false;
    }
}

FSproftAssetReferencesCommands::FSproftAssetReferencesCommands()
{
}

TSharedPtr<FJsonObject> FSproftAssetReferencesCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("asset_references"))
    {
        return HandleAssetReferences(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown asset_references command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftAssetReferencesCommands::HandleAssetReferences(const TSharedPtr<FJsonObject>& Params)
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

    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'asset' parameter (path to a content-browser asset)"));
    }

    FString DirToken = TEXT("hard_referencers");
    Params->TryGetStringField(TEXT("direction"), DirToken);
    EDirection Direction;
    if (!TryParseDirection(DirToken, Direction))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown direction '%s'. Valid: hard_referencers, soft_referencers, hard_dependencies, soft_dependencies, all_referencers, all_dependencies"), *DirToken));
    }

    int32 Depth = 1;
    int32 DepthFromJson = 0;
    if (Params->TryGetNumberField(TEXT("depth"), DepthFromJson) && DepthFromJson > 0)
    {
        Depth = DepthFromJson;
    }
    if (Depth > 6) { Depth = 6; }

    int32 Limit = 1024;
    int32 LimitFromJson = 0;
    if (Params->TryGetNumberField(TEXT("limit"), LimitFromJson) && LimitFromJson > 0)
    {
        Limit = LimitFromJson;
    }
    if (Limit > 50000) { Limit = 50000; }

    FString ClassFilterToken;
    Params->TryGetStringField(TEXT("class_filter"), ClassFilterToken);
    FTopLevelAssetPath ClassFilter;
    bool bHasClassFilter = false;
    if (!ClassFilterToken.IsEmpty())
    {
        bHasClassFilter = ResolveClassPath(ClassFilterToken, ClassFilter);
    }

    const FName SeedPackage = NormalisePackageName(AssetPath);
    if (SeedPackage.IsNone())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not derive a package name from '%s'"), *AssetPath));
    }

    const UE::AssetRegistry::FDependencyQuery Query = BuildQuery(Direction);
    const UE::AssetRegistry::EDependencyCategory PackageCategory = UE::AssetRegistry::EDependencyCategory::Package;

    // BFS over the dependency / referencer graph, capped by depth.
    TSet<FName> Visited;
    Visited.Add(SeedPackage);
    TArray<FName> CurrentLayer;
    CurrentLayer.Add(SeedPackage);
    TArray<FName> AggregatedHits;
    int32 DepthReached = 0;
    for (int32 LayerIdx = 0; LayerIdx < Depth && CurrentLayer.Num() > 0; ++LayerIdx)
    {
        TArray<FName> NextLayer;
        for (const FName& Pkg : CurrentLayer)
        {
            TArray<FName> StepHits;
            if (IsReferencerDirection(Direction))
            {
                AssetRegistry->GetReferencers(Pkg, StepHits, PackageCategory, Query);
            }
            else
            {
                AssetRegistry->GetDependencies(Pkg, StepHits, PackageCategory, Query);
            }
            for (const FName& Hit : StepHits)
            {
                if (Hit.IsNone() || Hit == SeedPackage) { continue; }
                if (Visited.Contains(Hit)) { continue; }
                Visited.Add(Hit);
                AggregatedHits.Add(Hit);
                NextLayer.Add(Hit);
            }
        }
        DepthReached = LayerIdx + 1;
        CurrentLayer = MoveTemp(NextLayer);
    }

    // For each hit package, look up its asset rows. Most packages have
    // exactly one row but level / cooked content can have several. We
    // emit one JSON entry per asset, scoped to the AssetRegistry's
    // PackageName so the output is deterministic.
    TArray<TSharedPtr<FJsonValue>> Rows;
    int32 PostFilteredCount = 0;
    bool bLimitHit = false;
    for (const FName& PackageName : AggregatedHits)
    {
        TArray<FAssetData> Datas;
        AssetRegistry->GetAssetsByPackageName(PackageName, Datas, /*bIncludeOnlyOnDiskAssets=*/false, /*bSkipARFilteredAssets=*/true);
        if (Datas.Num() == 0)
        {
            // Even if the registry has no FAssetData (e.g. a package
            // that holds only redirectors or a non-asset class), keep
            // the package listed so the caller still sees it.
            if (bHasClassFilter)
            {
                continue; // we cannot satisfy the class filter without data
            }
            ++PostFilteredCount;
            if (Rows.Num() >= Limit)
            {
                bLimitHit = true;
                continue;
            }
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("package"), PackageName.ToString());
            Row->SetStringField(TEXT("class"), TEXT(""));
            Row->SetStringField(TEXT("class_path"), TEXT(""));
            Row->SetStringField(TEXT("name"), TEXT(""));
            Row->SetStringField(TEXT("path"), PackageName.ToString());
            Rows.Add(MakeShared<FJsonValueObject>(Row));
            continue;
        }

        for (const FAssetData& Data : Datas)
        {
            if (bHasClassFilter && Data.AssetClassPath != ClassFilter)
            {
                continue;
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
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("asset_references"));
    ResultObj->SetStringField(TEXT("seed_asset"), AssetPath);
    ResultObj->SetStringField(TEXT("seed_package"), SeedPackage.ToString());
    ResultObj->SetStringField(TEXT("direction"), DirToken);
    ResultObj->SetNumberField(TEXT("depth_requested"), Depth);
    ResultObj->SetNumberField(TEXT("depth_reached"), DepthReached);
    ResultObj->SetArrayField(TEXT("assets"), Rows);
    ResultObj->SetNumberField(TEXT("count"), Rows.Num());
    ResultObj->SetNumberField(TEXT("matched_total"), PostFilteredCount);
    ResultObj->SetBoolField(TEXT("limit_hit"), bLimitHit);
    ResultObj->SetNumberField(TEXT("limit"), Limit);
    return ResultObj;
}
