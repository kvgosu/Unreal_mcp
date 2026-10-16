// SphereX (12.10): разбить ландшафт на подгружаемые части World Partition.
//
// Ландшафт, созданный import_landscape_file, держит все компоненты в самом
// ALandscape, а ALandscape в World Partition грузится ВСЕГДА. Когда таких
// ландшафтов (участки блоков города и округи, острова рельефа) стало ~260,
// карта City_WP перестала открываться: при открытии требовалось >80 ГБ.
// Здесь повторяется то, что делает редактор в Landscape Mode -> Change
// Component Size / Grid: компоненты переезжают в ALandscapeStreamingProxy по
// сетке из N компонентов, и прокси подгружаются по месту, как обычные акторы.
//
// Параметры: landscapeName (метка актора), gridSizeInComponents (по умолчанию 4).
// Ответ: сколько стало частей. Сохранение — отдельным вызовом (как везде).

#include "Core/Compatibility/McpVersionCompatibility.h"

#include "Domains/Landscape/McpAutomationBridge_LandscapeLookup.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeStreamingProxy.h"
#include "LandscapeSubsystem.h"
#include "Foundation/BridgeHelpers/McpAutomationBridgeHelpers.h"
#include "McpAutomationBridgeSubsystem.h"
#include "Foundation/HandlerUtils/McpHandlerUtils.h"

bool UMcpAutomationBridgeSubsystem::HandlePartitionLandscape(
    const FString &RequestId, const FString &Action,
    const TSharedPtr<FJsonObject> &Payload,
    TSharedPtr<FMcpBridgeWebSocket> RequestingSocket) {
  if (!Action.Equals(TEXT("partition_landscape"), ESearchCase::IgnoreCase)) {
    return false;
  }

#if WITH_EDITOR
  if (!GEditor || !Payload.IsValid()) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
    return true;
  }
  const FString LandscapeName = Payload->HasField(TEXT("landscapeName"))
                                    ? Payload->GetStringField(TEXT("landscapeName"))
                                    : FString();
  const uint32 Grid = Payload->HasField(TEXT("gridSizeInComponents"))
                          ? static_cast<uint32>(Payload->GetNumberField(TEXT("gridSizeInComponents")))
                          : 4u;

  ALandscape *Landscape = McpLandscapeHandlers::FindLandscapeForEdit(FString(), LandscapeName);
  if (!Landscape) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           McpLandscapeHandlers::MakeLandscapeNotFoundMessage(FString(), LandscapeName),
                           nullptr);
    return true;
  }
  ULandscapeInfo *Info = Landscape->GetLandscapeInfo();
  UWorld *World = Landscape->GetWorld();
  ULandscapeSubsystem *Sub = World ? World->GetSubsystem<ULandscapeSubsystem>() : nullptr;
  if (!Info || !Sub) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("NO_LANDSCAPE_INFO_OR_SUBSYSTEM"), nullptr);
    return true;
  }

  const int32 Bylo = Info->GetSortedStreamingProxies().Num();
  const int32 KomponentovBylo = Landscape->LandscapeComponents.Num();
  Sub->ChangeGridSize(Info, Grid);
  const int32 Stalo = Info->GetSortedStreamingProxies().Num();

  TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("landscapeName"), Landscape->GetActorLabel());
  Result->SetNumberField(TEXT("gridSizeInComponents"), Grid);
  Result->SetNumberField(TEXT("proxiesBefore"), Bylo);
  Result->SetNumberField(TEXT("proxiesAfter"), Stalo);
  Result->SetNumberField(TEXT("componentsInLandscapeBefore"), KomponentovBylo);
  Result->SetNumberField(TEXT("componentsInLandscapeAfter"), Landscape->LandscapeComponents.Num());
  SendAutomationResponse(RequestingSocket, RequestId, Stalo > 0,
                         Stalo > 0 ? TEXT("Landscape partitioned") : TEXT("NO_PROXIES_CREATED"), Result);
  return true;
#else
  SendAutomationResponse(RequestingSocket, RequestId, false,
                         TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
  return true;
#endif
}
