// SphereX: высота ландшафта в точке.
//
// Нужно для двух вещей сразу. Во-первых, сверка: высоты обязаны совпасть с
// настоящими, и проверять это надо числом, а не глазами по кадру. Во-вторых,
// постановка объектов: без высоты под точкой здание повиснет в воздухе или
// утонет, а узнать её иначе неоткуда — у ландшафта нет коллизии в
// командном режиме, и трассировка лучом не годится.
//
// Принимает список точек, возвращает высоты. Список, а не одну точку:
// на городе таких запросов десятки тысяч, и по одному это были бы часы.

#include "Core/Compatibility/McpVersionCompatibility.h"

#include "Domains/Landscape/McpAutomationBridge_LandscapeLookup.h"

#include "Dom/JsonObject.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "Foundation/BridgeHelpers/McpAutomationBridgeHelpers.h"
#include "McpAutomationBridgeSubsystem.h"
#include "Foundation/HandlerUtils/McpHandlerUtils.h"

bool UMcpAutomationBridgeSubsystem::HandleSampleLandscapeHeight(
    const FString &RequestId, const FString &Action,
    const TSharedPtr<FJsonObject> &Payload,
    TSharedPtr<FMcpBridgeWebSocket> RequestingSocket) {
  if (!Action.Equals(TEXT("sample_landscape_height"), ESearchCase::IgnoreCase)) {
    return false;
  }

#if WITH_EDITOR
  if (!Payload.IsValid()) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("INVALID_PAYLOAD"), nullptr);
    return true;
  }

  const FString LandscapeName = Payload->HasField(TEXT("landscapeName"))
                                    ? Payload->GetStringField(TEXT("landscapeName"))
                                    : FString();
  const FString LandscapePath = Payload->HasField(TEXT("landscapePath"))
                                    ? Payload->GetStringField(TEXT("landscapePath"))
                                    : FString();
  ALandscape *Landscape =
      McpLandscapeHandlers::FindLandscapeForEdit(LandscapePath, LandscapeName);
  if (!Landscape) {
    SendAutomationResponse(
        RequestingSocket, RequestId, false,
        McpLandscapeHandlers::MakeLandscapeNotFoundMessage(LandscapePath,
                                                           LandscapeName),
        nullptr);
    return true;
  }

  const TArray<TSharedPtr<FJsonValue>> *Points = nullptr;
  if (!Payload->TryGetArrayField(TEXT("points"), Points) || !Points) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("POINTS_REQUIRED"), nullptr);
    return true;
  }

  TArray<TSharedPtr<FJsonValue>> Out;
  int32 Hits = 0;
  int32 Misses = 0;
  for (const TSharedPtr<FJsonValue> &V : *Points) {
    const TSharedPtr<FJsonObject> *P = nullptr;
    if (!V.IsValid() || !V->TryGetObject(P) || !P || !P->IsValid()) {
      continue;
    }
    double X = 0.0, Y = 0.0;
    (*P)->TryGetNumberField(TEXT("x"), X);
    (*P)->TryGetNumberField(TEXT("y"), Y);

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("x"), X);
    R->SetNumberField(TEXT("y"), Y);

    // высоту спрашиваем у самого ландшафта: он знает её точно и без коллизии
    const TOptional<float> Z =
        Landscape->GetHeightAtLocation(FVector(X, Y, 0.0));
    if (Z.IsSet())    {
      R->SetNumberField(TEXT("z"), Z.GetValue());
      R->SetBoolField(TEXT("hit"), true);
      ++Hits;
    } else {
      R->SetBoolField(TEXT("hit"), false);
      ++Misses;
    }
    Out.Add(MakeShared<FJsonValueObject>(R));
  }

  TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("landscape"), Landscape->GetActorLabel());
  Result->SetNumberField(TEXT("asked"), Points->Num());
  Result->SetNumberField(TEXT("hit"), Hits);
  Result->SetNumberField(TEXT("missed"), Misses);
  Result->SetArrayField(TEXT("points"), Out);
  SendAutomationResponse(RequestingSocket, RequestId, true,
                         TEXT("Landscape heights sampled"), Result);
  return true;
#else
  SendAutomationResponse(RequestingSocket, RequestId, false,
                         TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
  return true;
#endif
}
