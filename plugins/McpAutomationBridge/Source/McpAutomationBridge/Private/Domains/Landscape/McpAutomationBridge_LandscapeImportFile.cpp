// SphereX: импорт ландшафта из файла — то же, что делает пункт меню
// Landscape Mode -> Manage -> Import from File.
//
// Штатный create_landscape в мосте собирает актор и компоненты, но НЕ
// вызывает ALandscape::Import, поэтому у ландшафта нет данных, которые
// движок рисует: актор есть, компонентов 4096, координаты верные, материал
// назначен — а в кадре пусто при любой камере и любом масштабе.
//
// Здесь повторяется путь самого редактора: читаем 16-битный PNG, спавним
// ландшафт, отдаём высоты в Import и регистрируем компоненты.

#include "Core/Compatibility/McpVersionCompatibility.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Foundation/BridgeHelpers/McpAutomationBridgeHelpers.h"
#include "McpAutomationBridgeSubsystem.h"
#include "Foundation/HandlerUtils/McpHandlerUtils.h"

bool UMcpAutomationBridgeSubsystem::HandleImportLandscapeFile(
    const FString &RequestId, const FString &Action,
    const TSharedPtr<FJsonObject> &Payload,
    TSharedPtr<FMcpBridgeWebSocket> RequestingSocket) {
  if (!Action.Equals(TEXT("import_landscape_file"), ESearchCase::IgnoreCase)) {
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

  // ---- читаем карту высот ----
  TArray<uint8> Raw;
  if (!FFileHelper::LoadFileToArray(Raw, *FilePath)) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("FILE_READ_FAILED"), nullptr);
    return true;
  }
  IImageWrapperModule &ImageModule =
      FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
  TSharedPtr<IImageWrapper> Wrapper =
      ImageModule.CreateImageWrapper(EImageFormat::PNG);
  if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Raw.GetData(), Raw.Num())) {
    SendAutomationResponse(RequestingSocket, RequestId, false, TEXT("NOT_A_PNG"),
                           nullptr);
    return true;
  }
  const int32 ImgW = Wrapper->GetWidth();
  const int32 ImgH = Wrapper->GetHeight();
  TArray<uint8> Bytes;
  if (!Wrapper->GetRaw(ERGBFormat::Gray, 16, Bytes)) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("NOT_16BIT_GRAY"), nullptr);
    return true;
  }

  // ---- разбиение на компоненты ----
  // Размер обязан быть вида (компонентов * квадов + 1): движок другого не
  // примет, а расхождение всплывёт уже пустым ландшафтом.
  const int32 QuadsPerSection =
      Payload->HasField(TEXT("quadsPerSection"))
          ? static_cast<int32>(Payload->GetNumberField(TEXT("quadsPerSection")))
          : 127;
  const int32 SectionsPerComponent =
      Payload->HasField(TEXT("sectionsPerComponent"))
          ? static_cast<int32>(
                Payload->GetNumberField(TEXT("sectionsPerComponent")))
          : 1;
  const int32 QuadsPerComponent = QuadsPerSection * SectionsPerComponent;
  const int32 ComponentsX = (ImgW - 1) / QuadsPerComponent;
  const int32 ComponentsY = (ImgH - 1) / QuadsPerComponent;
  if (ComponentsX <= 0 || ComponentsY <= 0 ||
      ComponentsX * QuadsPerComponent + 1 != ImgW ||
      ComponentsY * QuadsPerComponent + 1 != ImgH) {
    TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
    Err->SetNumberField(TEXT("imageWidth"), ImgW);
    Err->SetNumberField(TEXT("imageHeight"), ImgH);
    Err->SetNumberField(TEXT("quadsPerComponent"), QuadsPerComponent);
    Err->SetStringField(
        TEXT("hint"),
        TEXT("size must be components * quadsPerComponent + 1 (e.g. 64*127+1 = 8129)"));
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("SIZE_NOT_DIVISIBLE"), Err);
    return true;
  }

  TArray<uint16> Heights;
  Heights.SetNumUninitialized(ImgW * ImgH);
  const uint16 *Src = reinterpret_cast<const uint16 *>(Bytes.GetData());
  const bool bFlipY = Payload->HasField(TEXT("flipY"))
                          ? Payload->GetBoolField(TEXT("flipY"))
                          : false;
  for (int32 y = 0; y < ImgH; ++y) {
    const int32 sy = bFlipY ? (ImgH - 1 - y) : y;
    FMemory::Memcpy(&Heights[y * ImgW], &Src[sy * ImgW], ImgW * sizeof(uint16));
  }

  // ---- где и какого масштаба ----
  FVector Location(0, 0, 0);
  const TSharedPtr<FJsonObject> *LocObj = nullptr;
  if (Payload->TryGetObjectField(TEXT("location"), LocObj) && LocObj &&
      LocObj->IsValid()) {
    double v = 0.0;
    if ((*LocObj)->TryGetNumberField(TEXT("x"), v)) Location.X = v;
    if ((*LocObj)->TryGetNumberField(TEXT("y"), v)) Location.Y = v;
    if ((*LocObj)->TryGetNumberField(TEXT("z"), v)) Location.Z = v;
  }
  FVector Scale(100, 100, 100);
  const TSharedPtr<FJsonObject> *ScaleObj = nullptr;
  if (Payload->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj &&
      ScaleObj->IsValid()) {
    double v = 0.0;
    if ((*ScaleObj)->TryGetNumberField(TEXT("x"), v)) Scale.X = v;
    if ((*ScaleObj)->TryGetNumberField(TEXT("y"), v)) Scale.Y = v;
    if ((*ScaleObj)->TryGetNumberField(TEXT("z"), v)) Scale.Z = v;
  }

  UWorld *World = GEditor->GetEditorWorldContext().World();
  if (!World) {
    SendAutomationResponse(RequestingSocket, RequestId, false, TEXT("NO_WORLD"),
                           nullptr);
    return true;
  }

  FActorSpawnParameters Params;
  ALandscape *Landscape = World->SpawnActor<ALandscape>(Params);
  if (!Landscape) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("SPAWN_FAILED"), nullptr);
    return true;
  }
  const FString Name = Payload->HasField(TEXT("name"))
                           ? Payload->GetStringField(TEXT("name"))
                           : TEXT("SX_Landscape");
  Landscape->SetActorLabel(Name);
  Landscape->SetActorTransform(
      FTransform(FRotator::ZeroRotator, Location, Scale));
  Landscape->SetLandscapeGuid(FGuid::NewGuid());

  // ⭐ ВОТ ЧЕГО НЕ ХВАТАЛО: сам импорт данных. Без него актор есть, а
  // рисовать нечего.
  TMap<FGuid, TArray<uint16>> HeightDataPerLayer;
  HeightDataPerLayer.Add(FGuid(), MoveTemp(Heights));
  TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayer;
  MaterialLayerDataPerLayer.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

  // ⚠ В 5.8 у Import ДВЕНАДЦАТЬ аргументов: последним идёт список слоёв
  // правки. Пустой — значит обычный ландшафт без слоёв редактирования.
  const TArray<FLandscapeLayer> EmptyLayers;
  Landscape->Import(Landscape->GetLandscapeGuid(), 0, 0, ImgW - 1, ImgH - 1,
                    SectionsPerComponent, QuadsPerSection, HeightDataPerLayer,
                    nullptr, MaterialLayerDataPerLayer,
                    ELandscapeImportAlphamapType::Additive,
                    TArrayView<const FLandscapeLayer>(EmptyLayers));

  ULandscapeInfo *Info = Landscape->GetLandscapeInfo();
  if (Info) {
    Info->UpdateLayerInfoMap(Landscape);
  }
  Landscape->RegisterAllComponents();
  Landscape->MarkPackageDirty();

  const FString MaterialPath =
      Payload->HasField(TEXT("materialPath"))
          ? Payload->GetStringField(TEXT("materialPath"))
          : FString();
  if (!MaterialPath.IsEmpty()) {
    if (UMaterialInterface *Mat = LoadObject<UMaterialInterface>(
            nullptr, *MaterialPath)) {
      Landscape->LandscapeMaterial = Mat;
      Landscape->PostEditChange();
    }
  }

  TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("landscapeName"), Landscape->GetActorLabel());
  Result->SetStringField(TEXT("landscapePath"), Landscape->GetPathName());
  Result->SetNumberField(TEXT("imageWidth"), ImgW);
  Result->SetNumberField(TEXT("imageHeight"), ImgH);
  Result->SetNumberField(TEXT("componentsX"), ComponentsX);
  Result->SetNumberField(TEXT("componentsY"), ComponentsY);
  Result->SetNumberField(TEXT("quadsPerSection"), QuadsPerSection);
  Result->SetNumberField(TEXT("sectionsPerComponent"), SectionsPerComponent);
  Result->SetNumberField(TEXT("components"),
                         Landscape->LandscapeComponents.Num());
  SendAutomationResponse(RequestingSocket, RequestId, true,
                         TEXT("Landscape imported from file"), Result);
  return true;
#else
  SendAutomationResponse(RequestingSocket, RequestId, false,
                         TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
  return true;
#endif
}
