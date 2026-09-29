#include "Core/Compatibility/McpVersionCompatibility.h"

#include "Dom/JsonObject.h"
#include "McpAutomationBridgeSubsystem.h"

bool UMcpAutomationBridgeSubsystem::HandleEditLandscape(
    const FString &RequestId, const FString &Action,
    const TSharedPtr<FJsonObject> &Payload,
    TSharedPtr<FMcpBridgeWebSocket> RequestingSocket) {
  if (HandleModifyHeightmap(RequestId, Action, Payload, RequestingSocket))
    return true;
  // SphereX: карта высот файлом — один вызов вместо сотен
  if (HandleImportHeightmapFile(RequestId, Action, Payload, RequestingSocket))
    return true;
  if (HandlePaintLandscapeLayer(RequestId, Action, Payload, RequestingSocket))
    return true;
  if (HandleSculptLandscape(RequestId, Action, Payload, RequestingSocket))
    return true;
  if (HandleSetLandscapeMaterial(RequestId, Action, Payload, RequestingSocket))
    return true;
  return false;
}
