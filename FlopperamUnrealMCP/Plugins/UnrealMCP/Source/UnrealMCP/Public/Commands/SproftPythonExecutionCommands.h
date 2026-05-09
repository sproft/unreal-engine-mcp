#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: python_execution
 *
 * Run Python in the editor's interpreter. Two operations:
 *
 *   - "execute_string": run a string of Python source. Defaults to file
 *     mode so multi-statement programs and `import` work.
 *   - "execute_file":   run a Python file on disk. The path may include
 *     positional arguments per the engine's documented behaviour
 *     (everything after the file path is forwarded as `sys.argv[1:]`).
 *
 * Returns the captured stdout / stderr lines, the boolean success flag,
 * and `command_result` which is the repr of the last evaluated statement
 * for evaluate-statement mode (None otherwise). Output longer than
 * MaxResponseChars is truncated tail-first to keep transports happy.
 *
 * Requires the `PythonScriptPlugin` plugin to be enabled in the consumer
 * project. The implementation goes through the public IPythonScriptPlugin
 * interface (`IPythonScriptPlugin::Get()->ExecPythonCommandEx`) and never
 * touches the proprietary FlopAI plugin or any GPL / LGPL source.
 */
class UNREALMCP_API FSproftPythonExecutionCommands
{
public:
    FSproftPythonExecutionCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandlePythonExecution(const TSharedPtr<FJsonObject>& Params);
};
