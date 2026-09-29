// SphereX: массовая расстановка инстансов из файла.
//
// Город — это сотни тысяч объектов. Отдельным актором на здание мы уже
// обожглись: сцена не открывалась, трассировка лучей переполнялась, а
// World Partition раскладывал всё в одну точку. Инстансы одного меша движок
// рисует пачкой, и такой размер ему по силам.
//
// Трансформы читаются ФАЙЛОМ: двести тысяч штук через текстовый протокол —
// это десятки мегабайт на запрос.
//
// Высота берётся у ландшафта прямо здесь (snapToLandscape): иначе её
// пришлось бы спрашивать отдельным запросом на каждую точку и гонять
// обратно.

#include "Core/Compatibility/McpVersionCompatibility.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Landscape.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Foundation/BridgeHelpers/McpAutomationBridgeHelpers.h"
#include "McpAutomationBridgeSubsystem.h"
#include "Foundation/HandlerUtils/McpHandlerUtils.h"

bool UMcpAutomationBridgeSubsystem::HandleSpawnInstancesFromFile(
    const FString &RequestId, const FString &Action,
    const TSharedPtr<FJsonObject> &Payload,
    TSharedPtr<FMcpBridgeWebSocket> RequestingSocket) {
  if (!Action.Equals(TEXT("spawn_instances_from_file"), ESearchCase::IgnoreCase)) {
    return false;
  }

#if WITH_EDITOR
  if (!GEditor || !Payload.IsValid()) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
    return true;
  }

  const FString FilePath = Payload->GetStringField(TEXT("filePath"));
  if (FilePath.IsEmpty() || !FPaths::FileExists(FilePath)) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("FILE_NOT_FOUND"), nullptr);
    return true;
  }

  FString Json;
  if (!FFileHelper::LoadFileToString(Json, *FilePath)) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("FILE_READ_FAILED"), nullptr);
    return true;
  }
  TSharedPtr<FJsonObject> Root;
  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
  if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("BAD_JSON"), nullptr);
    return true;
  }
  const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
  if (!Root->TryGetArrayField(TEXT("instances"), Items) || !Items) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("NO_INSTANCES"), nullptr);
    return true;
  }

  const FString MeshPath =
      Payload->HasField(TEXT("meshPath"))
          ? Payload->GetStringField(TEXT("meshPath"))
          : TEXT("/Engine/BasicShapes/Cube.Cube");
  UStaticMesh *Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
  if (!Mesh) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("MESH_NOT_FOUND"), nullptr);
    return true;
  }

  UWorld *World = GEditor->GetEditorWorldContext().World();
  if (!World) {
    SendAutomationResponse(RequestingSocket, RequestId, false, TEXT("NO_WORLD"),
                           nullptr);
    return true;
  }

  // к чему прижимать по высоте
  ALandscape *Landscape = nullptr;
  const bool bSnap = Payload->HasField(TEXT("snapToLandscape"))
                         ? Payload->GetBoolField(TEXT("snapToLandscape"))
                         : true;
  if (bSnap) {
    for (TActorIterator<ALandscape> It(World); It; ++It) {
      Landscape = *It;
      break;
    }
  }
  const double ZOffset = Payload->HasField(TEXT("zOffset"))
                             ? Payload->GetNumberField(TEXT("zOffset"))
                             : 0.0;
  // ⚠ Высота ТОЛЬКО В ЦЕНТРЕ годится лишь на ровном месте. Замерено на
  // Алматы: уклон под дорогой 2,33 %, перепад по концам плитки — медиана
  // 1,01 м при толщине полотна 0,2 м. У 86 % плиток один конец оказывался
  // закопан, другой висел, и дорога распадалась на куски.
  //   center — как было, земля под серединой;
  //   min    — САМЫЙ НИЗКИЙ из углов основания: дом садится на землю целиком
  //            и ни один угол не висит;
  //   slope  — концы по локальной оси X задают наклон: полотно ложится
  //            вдоль склона, а не поперёк него.
  const FString SnapMode = Payload->HasField(TEXT("snapMode"))
                               ? Payload->GetStringField(TEXT("snapMode"))
                               : TEXT("center");
  const bool bSnapMin = SnapMode.Equals(TEXT("min"), ESearchCase::IgnoreCase);
  const bool bSnapSlope = SnapMode.Equals(TEXT("slope"), ESearchCase::IgnoreCase);
  // насколько утопить основание: скрывает остаток ступеньки на переломах
  const double Sink = Payload->HasField(TEXT("sink"))
                          ? Payload->GetNumberField(TEXT("sink"))
                          : 0.0;

  const FString Name = Payload->HasField(TEXT("name"))
                           ? Payload->GetStringField(TEXT("name"))
                           : TEXT("SX_Instances");

  AActor *Holder = World->SpawnActor<AActor>();
  if (!Holder) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("SPAWN_FAILED"), nullptr);
    return true;
  }
  Holder->SetActorLabel(Name);
  UHierarchicalInstancedStaticMeshComponent *Hism =
      NewObject<UHierarchicalInstancedStaticMeshComponent>(Holder);
  Hism->SetStaticMesh(Mesh);
  // материал сразу здесь: иначе после каждой расстановки приходится делать
  // отдельный вызов, а без него город читается одной белой кашей
  const FString MaterialPath = Payload->HasField(TEXT("materialPath"))
                                   ? Payload->GetStringField(TEXT("materialPath"))
                                   : FString();
  UMaterialInterface *Mat = nullptr;
  if (!MaterialPath.IsEmpty()) {
    Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (Mat) {
      Hism->SetMaterial(0, Mat);
    }
  }
  Hism->SetMobility(EComponentMobility::Static);
  Hism->RegisterComponent();
  Holder->SetRootComponent(Hism);
  Holder->AddInstanceComponent(Hism);

  TArray<FTransform> Transforms;
  Transforms.Reserve(Items->Num());
  int32 NoGround = 0;
  for (const TSharedPtr<FJsonValue> &V : *Items) {
    const TSharedPtr<FJsonObject> *O = nullptr;
    if (!V.IsValid() || !V->TryGetObject(O) || !O || !O->IsValid()) {
      continue;
    }
    double X = 0, Y = 0, Z = 0, Yaw = 0, SX = 1, SY = 1, SZ = 1;
    (*O)->TryGetNumberField(TEXT("x"), X);
    (*O)->TryGetNumberField(TEXT("y"), Y);
    (*O)->TryGetNumberField(TEXT("z"), Z);
    (*O)->TryGetNumberField(TEXT("yaw"), Yaw);
    (*O)->TryGetNumberField(TEXT("sx"), SX);
    (*O)->TryGetNumberField(TEXT("sy"), SY);
    (*O)->TryGetNumberField(TEXT("sz"), SZ);

    double Pitch = 0.0;
    if (Landscape) {
      const double Rad = FMath::DegreesToRadians(Yaw);
      const double CosY = FMath::Cos(Rad);
      const double SinY = FMath::Sin(Rad);
      bool bGot = false;

      if (bSnapSlope) {
        // концы отрезка по локальной оси X; между ними и натягиваем плитку
        const double Half = SX * 50.0;
        const TOptional<float> A = Landscape->GetHeightAtLocation(
            FVector(X - CosY * Half, Y - SinY * Half, 0.0));
        const TOptional<float> B = Landscape->GetHeightAtLocation(
            FVector(X + CosY * Half, Y + SinY * Half, 0.0));
        if (A.IsSet() && B.IsSet()) {
          Z = 0.5 * (A.GetValue() + B.GetValue());
          const double Run = FMath::Max(1.0, 2.0 * Half);
          Pitch = FMath::RadiansToDegrees(
              FMath::Atan2(static_cast<double>(B.GetValue() - A.GetValue()), Run));
          bGot = true;
        }
      } else if (bSnapMin) {
        // четыре угла основания: садимся на САМЫЙ НИЗКИЙ, иначе угол висит
        const double HX = SX * 50.0;
        const double HY = SY * 50.0;
        static const double Corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        double Low = 0.0;
        int32 Hits = 0;
        for (int32 C = 0; C < 4; ++C) {
          const double DX = Corners[C][0] * HX;
          const double DY = Corners[C][1] * HY;
          const TOptional<float> G = Landscape->GetHeightAtLocation(
              FVector(X + DX * CosY - DY * SinY, Y + DX * SinY + DY * CosY, 0.0));
          if (G.IsSet()) {
            Low = (Hits == 0) ? G.GetValue() : FMath::Min(Low, static_cast<double>(G.GetValue()));
            ++Hits;
          }
        }
        if (Hits > 0) {
          Z = Low;
          bGot = true;
        }
      } else {
        const TOptional<float> G =
            Landscape->GetHeightAtLocation(FVector(X, Y, 0.0));
        if (G.IsSet()) {
          Z = G.GetValue();
          bGot = true;
        }
      }
      if (!bGot) {
        ++NoGround;
      }
    }
    // куб стоит центром, а нам надо основанием на земле
    Transforms.Add(FTransform(FRotator(Pitch, Yaw, 0.0),
                              FVector(X, Y, Z + SZ * 50.0 + ZOffset - Sink),
                              FVector(SX, SY, SZ)));
  }

  Hism->AddInstances(Transforms, false, true);
  Hism->MarkRenderStateDirty();
  Holder->MarkPackageDirty();

  TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("actor"), Holder->GetActorLabel());
  Result->SetStringField(TEXT("mesh"), MeshPath);
  Result->SetStringField(TEXT("material"), Mat ? MaterialPath : FString());
  Result->SetNumberField(TEXT("asked"), Items->Num());
  Result->SetNumberField(TEXT("placed"), Transforms.Num());
  Result->SetNumberField(TEXT("instances"), Hism->GetInstanceCount());
  Result->SetBoolField(TEXT("snappedToLandscape"), Landscape != nullptr);
  Result->SetStringField(TEXT("snapMode"), SnapMode);
  Result->SetNumberField(TEXT("withoutGround"), NoGround);
  SendAutomationResponse(RequestingSocket, RequestId, true,
                         TEXT("Instances spawned"), Result);
  return true;
#else
  SendAutomationResponse(RequestingSocket, RequestId, false,
                         TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
  return true;
#endif
}
