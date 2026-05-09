#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: landscape_edit (small variant)
 *
 * Two ops on `ALandscape` actors in the active editor world, keyed by
 * `op`:
 *   - `set_landscape_material`: writes the proxy's master
 *     `LandscapeMaterial` UPROPERTY to a chosen
 *     `UMaterialInterface` (UMaterial or UMaterialInstance) and runs
 *     the same `PostEditChangeProperty` rebroadcast that the
 *     editor's BlueprintSetter uses, so component MICs rebuild on
 *     the next tick. The hole-material override (`LandscapeHoleMaterial`)
 *     is not in this slice.
 *   - `import_heightmap_png`: decodes a 16-bit grayscale PNG file off
 *     disk through `IImageWrapperModule::CreateImageWrapper` plus
 *     `IImageWrapper::SetCompressed` / `GetRaw`, walks the resolved
 *     `ULandscapeInfo` extent, and writes the height samples through
 *     `FLandscapeEditDataInterface::SetHeightData` so every component,
 *     heightmap texture, and collision mip lands in one pass. PNG dimensions must match the landscape's extent (the
 *     standard "components * (CompSize) + 1" inclusive grid). Smaller
 *     PNGs are rejected; larger PNGs centre-crop is a follow-on. 8-bit
 *     PNGs land widened to the 16-bit landscape range; floating-point
 *     PNGs are rejected.
 *
 * The wider sculpt-by-brush / paint-layer-by-stroke surface stays in
 * BACKLOG.
 *
 * Inputs (op-dependent):
 *   - `actor`: `ALandscape` actor name (matched by `GetName()` first
 *     and Outliner label second). Required for both ops.
 *   - `material`: `/Game/...` path to a UMaterialInterface, or short
 *     name resolved through the asset registry. `set_landscape_material`
 *     only.
 *   - `path`: absolute path to a 16-bit grayscale PNG on disk.
 *     `import_heightmap_png` only.
 *   - `save`: when true (default) saves the persistent level after
 *     the edit so the heightmap textures land on disk.
 *
 * Returns a payload with `operation`, the resolved actor's
 * `actor_name` / `actor_label` / `level`, and op-specific fields:
 *   - `set_landscape_material`: `material_path` + the previous
 *     material path under `previous_material_path`.
 *   - `import_heightmap_png`: `width` / `height` of the decoded PNG,
 *     the inclusive landscape extent (`min_x` / `min_y` / `max_x` /
 *     `max_y`), the bit depth, and `samples_written`.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `ALandscapeProxy::LandscapeMaterial` UPROPERTY on
 *     LandscapeProxy.h plus the two-line PostEditChangeProperty
 *     rebroadcast that mirrors `EditorSetLandscapeMaterial`.
 *   - `ULandscapeInfo::GetLandscapeExtent` for the destination
 *     coordinate rectangle.
 *   - `FLandscapeEditDataInterface` from
 *     `Runtime/Landscape/Public/LandscapeEdit.h` plus its
 *     `SetHeightData` write side.
 *   - `IImageWrapperModule::CreateImageWrapper(EImageFormat::PNG)` plus
 *     `IImageWrapper::SetCompressed` / `GetRaw` for the PNG decode.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftLandscapeEditCommands
{
public:
    FSproftLandscapeEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleSetLandscapeMaterial(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleImportHeightmapPng(const TSharedPtr<FJsonObject>& Params);
};
