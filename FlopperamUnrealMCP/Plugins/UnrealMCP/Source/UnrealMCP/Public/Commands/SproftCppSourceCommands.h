#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: cpp_source (read-only)
 *
 * Read C++ source by class path or by full file path on disk. Useful
 * for verifying that an MCP-driven authoring session ended up
 * referencing the C++ class we expect, or for asking "what does the
 * UClass behind this Blueprint actually look like".
 *
 * Operation: single op (`read`, default).
 *
 * Required input (one of):
 *   - `class`: a class path, accepting either
 *     `/Script/Module.ClassName`, a `/Game/...` Blueprint class path,
 *     or a short class name resolved against the loaded class set.
 *     Header + cpp paths come from
 *     `FSourceCodeNavigation::FindClassHeaderPath` /
 *     `FSourceCodeNavigation::FindClassSourcePath`.
 *   - `header_path`: a full disk path to a .h file. The cpp follow-on
 *     is inferred by replacing the extension with `.cpp` when the
 *     sibling exists.
 *   - `source_path`: a full disk path to a .cpp file. The header
 *     follow-on is inferred by replacing the extension with `.h` when
 *     the sibling exists.
 *
 * Optional inputs:
 *   - `include_header`: emit the header text. Default True.
 *   - `include_source`: emit the cpp text. Default True.
 *   - `max_bytes`: cap on each emitted file's text length. Default
 *     262144 (256 KiB). When the cap fires the text is truncated and
 *     the corresponding `*_truncated` flag is set.
 *
 * Returns a structured payload with:
 *   - `class` (full `/Script/Module.ClassName` path), `class_short`,
 *     `module` (when resolvable through `IPluginManager` /
 *     `FModuleManager`), `module_dir`.
 *   - `header_path` (absolute disk path), `header_text`,
 *     `header_text_bytes`, `header_truncated`.
 *   - `source_path` (absolute disk path), `source_text`,
 *     `source_text_bytes`, `source_truncated`.
 *   - `header_exists` / `source_exists` flags so a caller can tell
 *     "no .cpp yet, the class is header-only" from "no source code at
 *     all, this is a Blueprint-defined class".
 *
 * Read-only. We never write to disk.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - FSourceCodeNavigation::FindClassHeaderPath /
 *     FindClassSourcePath / FindModulePath / FindClassModuleName.
 *   - FFileHelper::LoadFileToString for the file read.
 *   - StaticLoadClass / FindObject<UClass> for class resolution.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftCppSourceCommands
{
public:
    FSproftCppSourceCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleCppSource(const TSharedPtr<FJsonObject>& Params);
};
