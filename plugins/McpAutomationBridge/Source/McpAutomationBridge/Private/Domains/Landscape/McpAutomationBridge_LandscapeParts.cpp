// SphereX (18.10): части разбитого ландшафта (ALandscapeStreamingProxy).
//
// После partition_landscape у ландшафта своих компонентов нет — всё в частях,
// и метки у частей движковые («LandscapeStreamingProxy_0_3_0»), а не метка
// родителя. Поэтому снятие ландшафта по метке (restore_bloki, ostrova_import)
// либо отказывает, либо оставляет части без родителя: они продолжают рисовать
// землю там, где её уже заменили. Здесь — по GUID ландшафта, а не по метке.
//
// subAction:
//   "audit"  — по ЗАГРУЖЕННЫМ частям: сколько частей у каждого ландшафта и какие
//              части-сироты (нет ALandscape с их LandscapeGuid); до вызова части
//              района надо подгрузить (WorldPartitionBlueprintLibrary.load_actors);
//   "delete" — landscapeName: снять ЗАГРУЖЕННЫЕ части с тем же LandscapeGuid и сам
//              ландшафт; части подгружать заранее (по рамке ландшафта), иначе
//              незагруженные останутся сиротами — в ответе число снятых частей,
//              сверять его с описанием;
//   "delete_orphans" — снять загруженные части-сироты (после "audit").
// Сохранение — отдельным вызовом (как везде).

#include "Core/Compatibility/McpVersionCompatibility.h"

#include "Domains/Landscape/McpAutomationBridge_LandscapeLookup.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeStreamingProxy.h"
#include "Foundation/BridgeHelpers/McpAutomationBridgeHelpers.h"
#include "McpAutomationBridgeSubsystem.h"
#include "Foundation/HandlerUtils/McpHandlerUtils.h"

bool UMcpAutomationBridgeSubsystem::HandleLandscapeParts(
    const FString &RequestId, const FString &Action,
    const TSharedPtr<FJsonObject> &Payload,
    TSharedPtr<FMcpBridgeWebSocket> RequestingSocket) {
  if (!Action.Equals(TEXT("landscape_parts"), ESearchCase::IgnoreCase)) {
    return false;
  }

#if WITH_EDITOR
  if (!GEditor || !Payload.IsValid()) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
    return true;
  }
  UWorld *World = GEditor->GetEditorWorldContext().World();
  if (!World) {
    SendAutomationResponse(RequestingSocket, RequestId, false, TEXT("NO_EDITOR_WORLD"), nullptr);
    return true;
  }
  const FString Sub = Payload->HasField(TEXT("subAction")) ? Payload->GetStringField(TEXT("subAction"))
                                                           : FString(TEXT("audit"));

  TMap<FGuid, ALandscape *> PoGuid;
  for (TActorIterator<ALandscape> It(World); It; ++It) {
    PoGuid.Add(It->GetLandscapeGuid(), *It);
  }
  TArray<ALandscapeStreamingProxy *> Chasti;
  for (TActorIterator<ALandscapeStreamingProxy> It(World); It; ++It) {
    Chasti.Add(*It);
  }

  TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetNumberField(TEXT("landscapes"), PoGuid.Num());
  Result->SetNumberField(TEXT("partsLoaded"), Chasti.Num());

  if (Sub.Equals(TEXT("audit"), ESearchCase::IgnoreCase) ||
      Sub.Equals(TEXT("delete_orphans"), ESearchCase::IgnoreCase)) {
    const bool Snimat = Sub.Equals(TEXT("delete_orphans"), ESearchCase::IgnoreCase);
    TMap<FString, int32> PoLandshaftu;
    TArray<TSharedPtr<FJsonValue>> Siroty;
    int32 Snyato = 0;
    for (ALandscapeStreamingProxy *P : Chasti) {
      ALandscape **L = PoGuid.Find(P->GetLandscapeGuid());
      if (L && *L) {
        PoLandshaftu.FindOrAdd((*L)->GetActorLabel())++;
        continue;
      }
      TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
      S->SetStringField(TEXT("label"), P->GetActorLabel());
      S->SetStringField(TEXT("guid"), P->GetLandscapeGuid().ToString());
      const FVector C = P->GetActorLocation();
      S->SetNumberField(TEXT("x"), C.X);
      S->SetNumberField(TEXT("y"), C.Y);
      Siroty.Add(MakeShared<FJsonValueObject>(S));
      if (Snimat && World->EditorDestroyActor(P, true)) {
        Snyato++;
      }
    }
    TSharedPtr<FJsonObject> Po = MakeShared<FJsonObject>();
    for (const TPair<FString, int32> &KV : PoLandshaftu) {
      Po->SetNumberField(KV.Key, KV.Value);
    }
    Result->SetObjectField(TEXT("partsPerLandscape"), Po);
    Result->SetNumberField(TEXT("orphans"), Siroty.Num());
    Result->SetArrayField(TEXT("orphanList"), Siroty);
    Result->SetNumberField(TEXT("orphansDeleted"), Snyato);
    SendAutomationResponse(RequestingSocket, RequestId, true, TEXT("Landscape parts audited"), Result);
    return true;
  }

  if (Sub.Equals(TEXT("delete"), ESearchCase::IgnoreCase)) {
    const FString Imya = Payload->HasField(TEXT("landscapeName")) ? Payload->GetStringField(TEXT("landscapeName"))
                                                                  : FString();
    ALandscape *Landscape = nullptr;
    int32 Odnoimennyh = 0;
    for (const TPair<FGuid, ALandscape *> &KV : PoGuid) {
      if (KV.Value && KV.Value->GetActorLabel() == Imya) {
        Landscape = KV.Value;
        Odnoimennyh++;
      }
    }
    if (!Landscape || Odnoimennyh != 1) {
      Result->SetNumberField(TEXT("sameLabel"), Odnoimennyh);
      SendAutomationResponse(RequestingSocket, RequestId, false,
                             Odnoimennyh ? TEXT("LANDSCAPE_LABEL_NOT_UNIQUE") : TEXT("LANDSCAPE_NOT_FOUND"), Result);
      return true;
    }
    const FGuid G = Landscape->GetLandscapeGuid();
    int32 Snyato = 0;
    for (ALandscapeStreamingProxy *P : Chasti) {
      if (P->GetLandscapeGuid() == G && World->EditorDestroyActor(P, true)) {
        Snyato++;
      }
    }
    const bool Ok = World->EditorDestroyActor(Landscape, true);
    Result->SetStringField(TEXT("landscapeName"), Imya);
    Result->SetNumberField(TEXT("partsDeleted"), Snyato);
    Result->SetBoolField(TEXT("landscapeDeleted"), Ok);
    SendAutomationResponse(RequestingSocket, RequestId, Ok,
                           Ok ? TEXT("Landscape and parts deleted") : TEXT("LANDSCAPE_NOT_DELETED"), Result);
    return true;
  }

  SendAutomationResponse(RequestingSocket, RequestId, false, TEXT("UNKNOWN_SUBACTION"), Result);
  return true;
#else
  SendAutomationResponse(RequestingSocket, RequestId, false,
                         TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
  return true;
#endif
}
