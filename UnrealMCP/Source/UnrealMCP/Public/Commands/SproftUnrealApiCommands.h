#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: unreal_api (read-only)
 *
 * Reflection-driven query of the live UE5 type database. Where the
 * hosted Flop tool is documented as "15 K+ API lookup", the local
 * variant trades the offline reference table for live `UClass` /
 * `FProperty` / `UFunction` walks against whichever modules have
 * already loaded into the editor. The actual designer use case
 * ("what can this class do, what methods can I call, what UPROPERTY
 * fields does it expose") is fully answered out of the in-process
 * reflection database.
 *
 * One op (`describe`, default), keyed by `op`:
 *   - `describe` returns the full surface of one class: parent class,
 *     direct child classes, implemented interfaces, all UPROPERTY
 *     fields with type / flags / tooltip, all UFUNCTION methods with
 *     signature / flags / tooltip, plus the class's flag set.
 *   - `find_property` is a substring search across one class's
 *     property list. Returns the same property record shape but only
 *     for matches.
 *   - `find_function` is a substring search across one class's
 *     function list. Returns the same function record shape but only
 *     for matches.
 *
 * Inputs:
 *   - `class`: required for every op. Accepts:
 *       * `/Script/Module.ClassName` (the canonical form)
 *       * a `/Game/...` Blueprint class path (auto-suffixed with `_C`)
 *       * a short class name probed against the loaded class set with
 *         A / U prefix variants and a `/Script/Engine.<Name>` fallback
 *   - `pattern`: required for `find_property` / `find_function`.
 *     Case-insensitive substring matched against the property /
 *     function FName.
 *   - `include_inherited`: walk parent properties + functions too.
 *     Default true.
 *   - `include_children`: include the direct-child class list under
 *     `describe`. Default true. The walk is non-recursive, with a
 *     `child_count_total` field that reports the recursive count.
 *   - `max_children`: cap on the direct-child class list. Default 256.
 *   - `max_properties`: cap on the property list (or match list).
 *     Default 512.
 *   - `max_functions`: cap on the function list (or match list).
 *     Default 512.
 *
 * Returns:
 *   - `class`, `class_short`, `class_path`, `is_native`, `is_abstract`,
 *     `is_blueprint`, `class_flags` (string array), and `tooltip`.
 *   - `parent` (when present): `{class, class_path, is_native}`.
 *   - `interfaces`: each `{class, class_path, is_native}`.
 *   - `properties`: each `{name, cpp_type, container, flags, tooltip,
 *     category, owner_class}` plus a `inherited` flag.
 *   - `functions`: each `{name, return, params, flags, tooltip,
 *     category, owner_class, is_pure, is_blueprint_callable}` plus a
 *     `inherited` flag. `params` is an array of
 *     `{name, cpp_type, container, is_const, is_reference, is_out, is_return}`.
 *   - `children`: each `{class, class_path, is_native}`. Only on
 *     `describe`.
 *   - `child_count`, `child_count_total`, `properties_total`,
 *     `properties_truncated`, `functions_total`, `functions_truncated`.
 *
 * Read-only. Never writes the asset registry.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `UClass` / `UClass::GetSuperClass` / `UClass::Interfaces`.
 *   - `TFieldIterator<FProperty>` / `TFieldIterator<UFunction>`.
 *   - `FProperty::GetCPPType` / `GetMetaData("ToolTip")`.
 *   - `UFunction::FunctionFlags` / property iteration for the
 *     parameter list.
 *   - `GetDerivedClasses` from `UObjectHash.h`.
 *   - `StaticLoadClass` / `FindObject<UClass>` for class resolution.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftUnrealApiCommands
{
public:
    FSproftUnrealApiCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleDescribe(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFindProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFindFunction(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListClasses(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFindInSubclasses(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleClassDiff(const TSharedPtr<FJsonObject>& Params);
};
