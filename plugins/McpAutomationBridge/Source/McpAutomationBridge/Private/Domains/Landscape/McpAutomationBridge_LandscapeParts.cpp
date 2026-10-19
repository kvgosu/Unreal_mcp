// SphereX (18.10, 19.10): части разбитого ландшафта (ALandscapeStreamingProxy).
//
// После partition_landscape у ландшафта своих компонентов нет — всё в частях,
// и метки у частей движковые («LandscapeStreamingProxy_0_3_0»), а не метка
// родителя. Поэтому снятие ландшафта по метке (restore_bloki, ostrova_import)
// либо отказывает, либо оставляет части без родителя: они продолжают рисовать
// землю там, где её уже заменили. Здесь — по GUID ландшафта, а не по метке.
//
// 19.10: выбор — по ОПИСАНИЯМ World Partition (FPartitionActorDesc::GridGuid
// = LandscapeGuid), а не по загруженным акторам. Первая версия требовала
// подгрузить части заранее; подгрузка «всего, что задевает рамки 54 островов»
// подняла 6908 акторов (части всех блоков КР) и уронила редактор и машину по
// памяти. Теперь подгружаются ТОЛЬКО части снимаемого ландшафта.
//
// subAction:
//   "audit"  — без подгрузки: ландшафтов и частей по описаниям, части-сироты
//              (GridGuid ни у одного ALandscape) — метка, guid, центр рамки;
//   "delete" — landscapeName (метка ALandscape, ровно одна): подгрузить его и
//              все части с тем же GridGuid, снять; в ответе найдено/снято —
//              расхождение = отказ;
//   "delete_orphans" — подгрузить и снять части-сироты.
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

#if WITH_EDITOR
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHandle.h"
#include "WorldPartition/ActorPartition/PartitionActorDesc.h"

namespace {
struct FOpisChasti {
  FGuid Actor;
  FGuid Grid;
  FString Label;
  FVector Centr;
  bool Roditel;
};

void SobratOpisaniya(UWorldPartition *WP, TArray<FOpisChasti> &Out) {
  FWorldPartitionHelpers::ForEachActorDescInstance<ALandscapeProxy>(
      WP, [&Out](const FWorldPartitionActorDescInstance *D) {
        const FPartitionActorDesc *P = static_cast<const FPartitionActorDesc *>(D->GetActorDesc());
        UClass *K = D->GetActorNativeClass();
        FOpisChasti O;
        O.Actor = D->GetGuid();
        O.Grid = P->GridGuid;
        O.Label = D->GetActorLabel().ToString();
        O.Centr = D->GetEditorBounds().GetCenter();
        O.Roditel = K && K->IsChildOf(ALandscape::StaticClass());
        Out.Add(O);
        return true;
      });
}

// подгрузить и снять акторы по их guid; вернуть число снятых
// (UWorldPartitionBlueprintLibrary::LoadActors наружу не экспортирован — LNK2019;
//  грузим жёсткими ссылками World Partition, как это делают загрузчики движка)
int32 PodgruzitISnyat(UWorld *World, UWorldPartition *WP, const TArray<FGuid> &Guidy) {
  if (Guidy.Num() == 0) {
    return 0;
  }
  TArray<FWorldPartitionReference> Refy;
  for (const FGuid &G : Guidy) {
    Refy.Emplace(WP, G);
  }
  TSet<FGuid> Nado(Guidy);
  TArray<AActor *> Snimat;
  for (TActorIterator<ALandscapeProxy> It(World); It; ++It) {
    if (Nado.Contains(It->GetActorGuid())) {
      Snimat.Add(*It);
    }
  }
  int32 N = 0;
  for (AActor *A : Snimat) {
    if (World->EditorDestroyActor(A, true)) {
      N++;
    }
  }
  return N;
}
} // namespace
#endif

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
  UWorldPartition *WP = World ? World->GetWorldPartition() : nullptr;
  if (!World || !WP) {
    SendAutomationResponse(RequestingSocket, RequestId, false, TEXT("NO_WORLD_PARTITION"), nullptr);
    return true;
  }
  const FString Sub = Payload->HasField(TEXT("subAction")) ? Payload->GetStringField(TEXT("subAction"))
                                                           : FString(TEXT("audit"));

  TArray<FOpisChasti> Vse;
  SobratOpisaniya(WP, Vse);
  TSet<FGuid> Gridy;
  for (const FOpisChasti &O : Vse) {
    if (O.Roditel) {
      Gridy.Add(O.Grid);
    }
  }

  TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetNumberField(TEXT("landscapes"), Gridy.Num());
  Result->SetNumberField(TEXT("partsInDescs"), Vse.Num() - Gridy.Num());

  if (Sub.Equals(TEXT("audit"), ESearchCase::IgnoreCase) ||
      Sub.Equals(TEXT("delete_orphans"), ESearchCase::IgnoreCase)) {
    TArray<TSharedPtr<FJsonValue>> Siroty;
    TArray<FGuid> SirotyGuid;
    for (const FOpisChasti &O : Vse) {
      if (O.Roditel || Gridy.Contains(O.Grid)) {
        continue;
      }
      TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
      S->SetStringField(TEXT("label"), O.Label);
      S->SetStringField(TEXT("guid"), O.Grid.ToString());
      S->SetNumberField(TEXT("x"), O.Centr.X);
      S->SetNumberField(TEXT("y"), O.Centr.Y);
      Siroty.Add(MakeShared<FJsonValueObject>(S));
      SirotyGuid.Add(O.Actor);
    }
    int32 Snyato = 0;
    if (Sub.Equals(TEXT("delete_orphans"), ESearchCase::IgnoreCase)) {
      Snyato = PodgruzitISnyat(World, WP, SirotyGuid);
    }
    Result->SetNumberField(TEXT("orphans"), Siroty.Num());
    Result->SetArrayField(TEXT("orphanList"), Siroty);
    Result->SetNumberField(TEXT("orphansDeleted"), Snyato);
    const bool Ok = Sub.Equals(TEXT("audit"), ESearchCase::IgnoreCase) || Snyato == SirotyGuid.Num();
    SendAutomationResponse(RequestingSocket, RequestId, Ok,
                           Ok ? TEXT("Landscape parts audited") : TEXT("ORPHANS_NOT_ALL_DELETED"), Result);
    return true;
  }

  if (Sub.Equals(TEXT("delete"), ESearchCase::IgnoreCase)) {
    const FString Imya = Payload->HasField(TEXT("landscapeName")) ? Payload->GetStringField(TEXT("landscapeName"))
                                                                  : FString();
    int32 Odnoimennyh = 0;
    FGuid G;
    for (const FOpisChasti &O : Vse) {
      if (O.Roditel && O.Label == Imya) {
        G = O.Grid;
        Odnoimennyh++;
      }
    }
    Result->SetStringField(TEXT("landscapeName"), Imya);
    if (Odnoimennyh != 1) {
      Result->SetNumberField(TEXT("sameLabel"), Odnoimennyh);
      SendAutomationResponse(RequestingSocket, RequestId, false,
                             Odnoimennyh ? TEXT("LANDSCAPE_LABEL_NOT_UNIQUE") : TEXT("LANDSCAPE_NOT_FOUND"), Result);
      return true;
    }
    TArray<FGuid> Guidy;
    for (const FOpisChasti &O : Vse) {
      if (O.Grid == G) {
        Guidy.Add(O.Actor);
      }
    }
    const int32 Snyato = PodgruzitISnyat(World, WP, Guidy);
    const bool Ok = Snyato == Guidy.Num();
    Result->SetNumberField(TEXT("actorsFound"), Guidy.Num());
    Result->SetNumberField(TEXT("partsDeleted"), Snyato - 1);
    Result->SetNumberField(TEXT("actorsDeleted"), Snyato);
    SendAutomationResponse(RequestingSocket, RequestId, Ok,
                           Ok ? TEXT("Landscape and parts deleted") : TEXT("NOT_ALL_DELETED"), Result);
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
