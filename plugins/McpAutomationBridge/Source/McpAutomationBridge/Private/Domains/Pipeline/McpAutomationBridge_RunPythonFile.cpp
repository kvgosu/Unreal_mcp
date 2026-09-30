// SphereX: run_python_file -- execute a Python FILE that lives inside the project.
//
// Why this exists at all: the MetaHuman surface Epic actually documents is
// Python. The conform recipe ships with the engine as
// Content/Python/examples/example_conform_from_custom_mesh.py, and there is no
// C++ equivalent we could reach reflectively without reimplementing the whole
// solver setup. Reaching it through `py <text>` is what the console policy
// blocks, and rightly so: that is arbitrary code from the wire.
//
// So this is a NARROW door, not a hole in the wall:
//   * a FILE PATH only -- never command text, so nothing executable travels
//     over the socket;
//   * the file must resolve INSIDE the project directory, checked the same way
//     asset import checks its source path (normalise to full, compare prefix);
//   * the extension must be .py, so this cannot be turned into a launcher;
//   * an explicit acknowledge field is required, so no accidental invocation.
//
// The console-command policy is untouched: `py "print(1)"` stays refused.

#include "Core/Compatibility/McpVersionCompatibility.h"

#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Foundation/BridgeHelpers/McpAutomationBridgeHelpers.h"
#include "McpAutomationBridgeSubsystem.h"
#include "Foundation/HandlerUtils/McpHandlerUtils.h"

#if WITH_EDITOR
#include "IPythonScriptPlugin.h"
#endif

bool UMcpAutomationBridgeSubsystem::HandleRunPythonFile(
    const FString &RequestId, const FString &Action,
    const TSharedPtr<FJsonObject> &Payload,
    TSharedPtr<FMcpBridgeWebSocket> RequestingSocket) {
  if (!Action.Equals(TEXT("run_python_file"), ESearchCase::IgnoreCase)) {
    return false;
  }

#if WITH_EDITOR
  if (!Payload.IsValid()) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("INVALID_PAYLOAD"), nullptr);
    return true;
  }

  // ⚠ Подтверждение обязательно: случайный вызов исключён.
  FString Ack;
  Payload->TryGetStringField(TEXT("acknowledge"), Ack);
  if (!Ack.Equals(TEXT("run-project-python"))) {
    SendAutomationResponse(
        RequestingSocket, RequestId, false,
        TEXT("run_python_file requires acknowledge:\"run-project-python\"."),
        nullptr, TEXT("PRECONDITION_FAILED"));
    return true;
  }

  FString FilePath;
  Payload->TryGetStringField(TEXT("filePath"), FilePath);
  if (FilePath.IsEmpty()) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("filePath is required."), nullptr,
                           TEXT("MISSING_PARAMETER"));
    return true;
  }

  FString Full = FPaths::ConvertRelativePathToFull(FilePath);
  FPaths::NormalizeFilename(Full);

  if (!Full.EndsWith(TEXT(".py"), ESearchCase::IgnoreCase)) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("Only .py files may be run."), nullptr,
                           TEXT("SECURITY_VIOLATION"));
    return true;
  }

  FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
  FPaths::NormalizeDirectoryName(ProjectDir);
  if (!ProjectDir.EndsWith(TEXT("/"))) {
    ProjectDir += TEXT("/");
  }
  if (!Full.StartsWith(ProjectDir, ESearchCase::IgnoreCase)) {
    SendAutomationResponse(
        RequestingSocket, RequestId, false,
        TEXT("Script path escapes the project directory."), nullptr,
        TEXT("SECURITY_VIOLATION"));
    return true;
  }

  if (!FPaths::FileExists(Full)) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("Script file not found."), nullptr,
                           TEXT("FILE_NOT_FOUND"));
    return true;
  }

  IPythonScriptPlugin *Py = IPythonScriptPlugin::Get();
  if (!Py || !Py->IsPythonAvailable()) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("PythonScriptPlugin is not available."), nullptr,
                           TEXT("NOT_IMPLEMENTED"));
    return true;
  }

  // ⚠ ExecPythonCommandEx отдаёт то, что печатает скрипт, и сообщение об
  // ошибке. Без этого «успех» означал бы только «файл запущен», а не
  // «отработал» — ровно та подмена, на которой мы уже горели.
  FPythonCommandEx Cmd;
  Cmd.Command = Full;
  Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
  Cmd.FileExecutionScope = EPythonFileExecutionScope::Public;
  Cmd.Flags = EPythonCommandFlags::None;
  const bool bOk = Py->ExecPythonCommandEx(Cmd);

  TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
  Result->SetStringField(TEXT("filePath"), Full);
  Result->SetStringField(TEXT("result"), Cmd.CommandResult);
  TArray<TSharedPtr<FJsonValue>> Lines;
  for (const FPythonLogOutputEntry &E : Cmd.LogOutput) {
    Lines.Add(MakeShared<FJsonValueString>(E.Output));
  }
  Result->SetArrayField(TEXT("output"), Lines);
  Result->SetNumberField(TEXT("outputLines"), Lines.Num());

  SendAutomationResponse(RequestingSocket, RequestId, bOk,
                         bOk ? TEXT("Python file executed")
                             : TEXT("Python file reported an error"),
                         Result, bOk ? FString() : TEXT("OPERATION_FAILED"));
  return true;
#else
  SendAutomationResponse(RequestingSocket, RequestId, false,
                         TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
  return true;
#endif
}
