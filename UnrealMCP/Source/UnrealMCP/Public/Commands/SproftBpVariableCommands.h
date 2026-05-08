#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: bp_variable
 *
 * Declarative Blueprint variable management. Sits next to the local
 * `create_variable` and `set_blueprint_variable_properties` helpers, but
 * with two big additions:
 *
 *   - A single multi-op tool that lets the agent batch declarations,
 *     defaults, flag tweaks, removals, and reads behind one call site.
 *   - A wider type resolver that accepts the standard scalar names plus
 *     full `/Script/Module.ClassName` paths for object refs and
 *     `/Game/...` Blueprint class paths for `_C` references.
 *
 * Operations (keyed by `op` plus a required `blueprint` resolver):
 *   - "list": dump every Blueprint variable with type, current default,
 *     category, friendly name, and the user-visible flag set.
 *   - "add": declare a new variable. Inputs:
 *       - name: FName.
 *       - type: scalar token (bool / int / int64 / float / double / string
 *         / name / text / byte / vector / vector2d / rotator / transform
 *         / color / linear_color), `/Script/Module.ClassName` path for an
 *         object ref, `/Game/...` Blueprint class path for a `_C` ref, or
 *         a `struct:/Game/...` prefix for a UScriptStruct.
 *       - default: optional default value, applied through Variable.DefaultValue.
 *       - category, friendly_name, tooltip: optional metadata.
 *       - editable, blueprint_read_only, expose_on_spawn, replicated,
 *         private: optional flag toggles.
 *       - container: "single" (default), "array", "set", or "map". The
 *         "map" variant takes an extra `value_type` token resolved with
 *         the same resolver as `type`.
 *   - "remove": delete a variable by name.
 *   - "set_default": update Variable.DefaultValue on an existing variable
 *     using a JSON value (string / number / bool / array / object).
 *   - "set_flags": mutate the editor-visible flag set on an existing
 *     variable. Recognised keys: editable, blueprint_read_only,
 *     blueprint_writable, expose_on_spawn, replicated, private,
 *     instance_editable, expose_to_cinematics. Unknown keys land in a
 *     `skipped` list.
 *
 * Each mutating op compiles and saves the Blueprint on success unless
 * `compile=false` or `save=false` is passed. Compile-only and save-only
 * combinations are also supported.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - FBlueprintEditorUtils::AddMemberVariable / RemoveMemberVariable.
 *   - FBPVariableDescription field surface (PropertyFlags, MetaData,
 *     DefaultValue, Category, FriendlyName, ReplicationCondition).
 *   - FEdGraphPinType built from UEdGraphSchema_K2 PC_* constants and
 *     TBaseStructure<>::Get() for built-in structs.
 *   - LoadObject<UClass> / LoadObject<UScriptStruct> for path-keyed types.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftBpVariableCommands
{
public:
    FSproftBpVariableCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleBpVariable(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> ListVariables(class UBlueprint* Blueprint);
    TSharedPtr<FJsonObject> AddVariable(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> RemoveVariable(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetDefault(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> SetFlags(class UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params);
};
