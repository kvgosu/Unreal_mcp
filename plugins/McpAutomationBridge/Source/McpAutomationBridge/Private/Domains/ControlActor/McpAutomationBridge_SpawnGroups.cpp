// SphereX: группы объектов из файла — ОДИН актор на группу.
//
// spawn_instances_from_file кладёт весь набор в один актор: двести тысяч
// коробок иначе не поставить. Но дом, который мы доводим до настоящего,
// обязан быть отдельным объектом: Rob 01.10 — «выбираю дом 13, а выделяется
// целая куча домов». Здесь каждая группа (дом) — свой актор с именем и
// папкой в Outliner, а внутри по набору инстансов на каждый меш (секции,
// торцы).
//
// Файл:
//   {"groups": [{"name": "5 мкр, 13", "folder": "Город/Хрущёвки",
//                "x": .., "y": .., "z": ..,
//                "parts": [{"mesh": "/Game/..", "x": .., "y": .., "z": .., "yaw": ..}]}]}
// Координаты частей — мировые, см. Высоты не считаются: их даёт вызывающий.

#include "Core/Compatibility/McpVersionCompatibility.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Foundation/BridgeHelpers/McpAutomationBridgeHelpers.h"
#include "McpAutomationBridgeSubsystem.h"
#include "Foundation/HandlerUtils/McpHandlerUtils.h"

bool UMcpAutomationBridgeSubsystem::HandleSpawnGroupsFromFile(
    const FString &RequestId, const FString &Action,
    const TSharedPtr<FJsonObject> &Payload,
    TSharedPtr<FMcpBridgeWebSocket> RequestingSocket) {
  if (!Action.Equals(TEXT("spawn_groups_from_file"), ESearchCase::IgnoreCase)) {
    return false;
  }

#if WITH_EDITOR
  if (!GEditor || !Payload.IsValid()) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
    return true;
  }
  const FString FilePath = Payload->GetStringField(TEXT("filePath"));
  FString Json;
  if (FilePath.IsEmpty() || !FPaths::FileExists(FilePath) ||
      !FFileHelper::LoadFileToString(Json, *FilePath)) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("FILE_NOT_FOUND"), nullptr);
    return true;
  }
  TSharedPtr<FJsonObject> Root;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
  const TArray<TSharedPtr<FJsonValue>> *Groups = nullptr;
  if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() ||
      !Root->TryGetArrayField(TEXT("groups"), Groups) || !Groups) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("BAD_JSON"), nullptr);
    return true;
  }
  UWorld *World = GEditor->GetEditorWorldContext().World();
  if (!World) {
    SendAutomationResponse(RequestingSocket, RequestId, false, TEXT("NO_WORLD"),
                           nullptr);
    return true;
  }

  // повторный прогон ничего не плодит: актор с тем же именем В ТОЙ ЖЕ папке —
  // долой. Только имя сравнивать нельзя: «дом 13» есть в каждом микрорайоне
  TSet<FString> Names;
  for (const TSharedPtr<FJsonValue> &V : *Groups) {
    const TSharedPtr<FJsonObject> *G = nullptr;
    if (V.IsValid() && V->TryGetObject(G) && G && G->IsValid()) {
      FString F;
      (*G)->TryGetStringField(TEXT("folder"), F);
      Names.Add(F + TEXT("|") + (*G)->GetStringField(TEXT("name")));
    }
  }
  // и всё в папках, которые просят очистить (прежнее поколение под другими именами)
  TSet<FName> ClearFolders;
  const TArray<TSharedPtr<FJsonValue>> *Clear = nullptr;
  if (Payload->TryGetArrayField(TEXT("clearFolders"), Clear) && Clear) {
    for (const TSharedPtr<FJsonValue> &C : *Clear) {
      ClearFolders.Add(FName(*C->AsString()));
    }
  }
  int32 Replaced = 0;
  {
    TArray<AActor *> Old;
    for (TActorIterator<AActor> It(World); It; ++It) {
      if (Names.Contains(It->GetFolderPath().ToString() + TEXT("|") + It->GetActorLabel()) ||
          ClearFolders.Contains(It->GetFolderPath())) {
        Old.Add(*It);
      }
    }
    for (AActor *A : Old) {
      World->EditorDestroyActor(A, true);
      ++Replaced;
    }
  }

  TMap<FString, UStaticMesh *> MeshCache;
  int32 Actors = 0, Instances = 0, MissingMesh = 0;
  for (const TSharedPtr<FJsonValue> &V : *Groups) {
    const TSharedPtr<FJsonObject> *G = nullptr;
    if (!V.IsValid() || !V->TryGetObject(G) || !G || !G->IsValid()) {
      continue;
    }
    const FString Name = (*G)->GetStringField(TEXT("name"));
    FString Folder;
    (*G)->TryGetStringField(TEXT("folder"), Folder);
    double GX = 0, GY = 0, GZ = 0;
    (*G)->TryGetNumberField(TEXT("x"), GX);
    (*G)->TryGetNumberField(TEXT("y"), GY);
    (*G)->TryGetNumberField(TEXT("z"), GZ);

    AActor *Holder = World->SpawnActor<AActor>();
    if (!Holder) {
      continue;
    }
    USceneComponent *RootComp = NewObject<USceneComponent>(Holder, TEXT("Root"));
    RootComp->SetMobility(EComponentMobility::Static);
    Holder->SetRootComponent(RootComp);
    RootComp->RegisterComponent();
    Holder->AddInstanceComponent(RootComp);
    Holder->SetActorLocation(FVector(GX, GY, GZ));

    TMap<FString, UInstancedStaticMeshComponent *> ByMesh;
    const TArray<TSharedPtr<FJsonValue>> *Parts = nullptr;
    if ((*G)->TryGetArrayField(TEXT("parts"), Parts) && Parts) {
      for (const TSharedPtr<FJsonValue> &PV : *Parts) {
        const TSharedPtr<FJsonObject> *P = nullptr;
        if (!PV.IsValid() || !PV->TryGetObject(P) || !P || !P->IsValid()) {
          continue;
        }
        const FString MeshPath = (*P)->GetStringField(TEXT("mesh"));
        UInstancedStaticMeshComponent **Found = ByMesh.Find(MeshPath);
        UInstancedStaticMeshComponent *Ism = Found ? *Found : nullptr;
        if (!Ism) {
          UStaticMesh **Cached = MeshCache.Find(MeshPath);
          UStaticMesh *Mesh = Cached ? *Cached : LoadObject<UStaticMesh>(nullptr, *MeshPath);
          MeshCache.Add(MeshPath, Mesh);
          if (!Mesh) {
            ++MissingMesh;
            continue;
          }
          Ism = NewObject<UInstancedStaticMeshComponent>(Holder);
          Ism->SetStaticMesh(Mesh);
          // материал части: коробки «отдельного проекта» иначе белые
          FString MatPath;
          if ((*P)->TryGetStringField(TEXT("material"), MatPath) && !MatPath.IsEmpty()) {
            if (UMaterialInterface *Mat = LoadObject<UMaterialInterface>(nullptr, *MatPath)) {
              Ism->SetMaterial(0, Mat);
            }
          }
          Ism->SetMobility(EComponentMobility::Static);
          Ism->AttachToComponent(RootComp, FAttachmentTransformRules::KeepRelativeTransform);
          Ism->RegisterComponent();
          Holder->AddInstanceComponent(Ism);
          ByMesh.Add(MeshPath, Ism);
        }
        double X = 0, Y = 0, Z = 0, Yaw = 0, SX = 1, SY = 1, SZ = 1;
        (*P)->TryGetNumberField(TEXT("x"), X);
        (*P)->TryGetNumberField(TEXT("y"), Y);
        (*P)->TryGetNumberField(TEXT("z"), Z);
        (*P)->TryGetNumberField(TEXT("yaw"), Yaw);
        (*P)->TryGetNumberField(TEXT("sx"), SX);
        (*P)->TryGetNumberField(TEXT("sy"), SY);
        (*P)->TryGetNumberField(TEXT("sz"), SZ);
        Ism->AddInstance(FTransform(FRotator(0.0, Yaw, 0.0), FVector(X, Y, Z),
                                    FVector(SX, SY, SZ)),
                         /*bWorldSpace=*/true);
        ++Instances;
      }
    }
    Holder->SetActorLabel(Name);
    if (!Folder.IsEmpty()) {
      Holder->SetFolderPath(FName(*Folder));
    }
    Holder->MarkPackageDirty();
    ++Actors;
  }

  TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetNumberField(TEXT("groups"), Groups->Num());
  Result->SetNumberField(TEXT("actors"), Actors);
  Result->SetNumberField(TEXT("instances"), Instances);
  Result->SetNumberField(TEXT("missingMesh"), MissingMesh);
  Result->SetNumberField(TEXT("replaced"), Replaced);
  SendAutomationResponse(RequestingSocket, RequestId, true,
                         TEXT("Groups spawned"), Result);
  return true;
#else
  SendAutomationResponse(RequestingSocket, RequestId, false,
                         TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
  return true;
#endif
}
