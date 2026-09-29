// SphereX: импорт карты высот ФАЙЛОМ, а не массивом в запросе.
//
// Штатный modify_heightmap принимает высоты массивом в теле запроса. Для
// настоящего мира это неподъёмно: наша карта 8129x8129 — это 66 миллионов
// значений, то есть сотни запросов и десятки мегабайт JSON на каждый.
//
// Здесь путь к файлу передаётся строкой, а PNG читает сам движок. Один
// вызов вместо сотен, и данные не проходят через текстовый протокол.
//
// Формат: 16-битный PNG в оттенках серого — то, что отдаёт наш
// heightmap_from_srtm.py и что понимает штатный импорт Landscape.

#include "Core/Compatibility/McpVersionCompatibility.h"

#include "Domains/Landscape/McpAutomationBridge_LandscapeLookup.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Foundation/BridgeHelpers/McpAutomationBridgeHelpers.h"
#include "McpAutomationBridgeSubsystem.h"
#include "Foundation/HandlerUtils/McpHandlerUtils.h"

bool UMcpAutomationBridgeSubsystem::HandleImportHeightmapFile(
    const FString &RequestId, const FString &Action,
    const TSharedPtr<FJsonObject> &Payload,
    TSharedPtr<FMcpBridgeWebSocket> RequestingSocket) {
  if (!Action.Equals(TEXT("import_heightmap_file"), ESearchCase::IgnoreCase)) {
    return false;
  }

#if WITH_EDITOR
  if (!Payload.IsValid()) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("INVALID_PAYLOAD"), nullptr);
    return true;
  }

  const FString FilePath = Payload->GetStringField(TEXT("filePath"));
  const FString LandscapeName = Payload->HasField(TEXT("landscapeName"))
                                    ? Payload->GetStringField(TEXT("landscapeName"))
                                    : FString();
  // Blender пишет картинку снизу вверх, Unreal читает сверху вниз. Если
  // рельеф войдёт зеркальным, застройка разойдётся с землёй незаметно,
  // поэтому переворот — явный параметр, а не догадка.
  const bool bFlipY = Payload->HasField(TEXT("flipY"))
                          ? Payload->GetBoolField(TEXT("flipY"))
                          : false;

  if (FilePath.IsEmpty() || !FPaths::FileExists(FilePath)) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("FILE_NOT_FOUND"), nullptr);
    return true;
  }

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
  ULandscapeInfo *Info = Landscape->GetLandscapeInfo();
  if (!Info) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("INVALID_LANDSCAPE"), nullptr);
    return true;
  }

  TArray<uint8> Raw;
  if (!FFileHelper::LoadFileToArray(Raw, *FilePath)) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("FILE_READ_FAILED"), nullptr);
    return true;
  }

  IImageWrapperModule &ImageModule =
      FModuleManager::LoadModuleChecked<IImageWrapperModule>(
          FName("ImageWrapper"));
  TSharedPtr<IImageWrapper> Wrapper =
      ImageModule.CreateImageWrapper(EImageFormat::PNG);
  if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Raw.GetData(), Raw.Num())) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("NOT_A_PNG"), nullptr);
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

  // область самого ландшафта: карта обязана совпасть с ней по размеру,
  // иначе высоты лягут со сдвигом, и это будет видно только в кадре
  int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
  if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY)) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("NO_LANDSCAPE_EXTENT"), nullptr);
    return true;
  }
  const int32 SizeX = MaxX - MinX + 1;
  const int32 SizeY = MaxY - MinY + 1;

  TArray<uint16> Heights;
  Heights.SetNumUninitialized(SizeX * SizeY);
  const uint16 *Src = reinterpret_cast<const uint16 *>(Bytes.GetData());
  for (int32 y = 0; y < SizeY; ++y) {
    // выборка ближайшего: размеры обычно совпадают, а когда нет —
    // растягиваем, чтобы не падать на единице расхождения
    const int32 sy0 = ImgH == SizeY ? y : (y * ImgH) / SizeY;
    const int32 sy = bFlipY ? (ImgH - 1 - sy0) : sy0;
    for (int32 x = 0; x < SizeX; ++x) {
      const int32 sx = ImgW == SizeX ? x : (x * ImgW) / SizeX;
      Heights[y * SizeX + x] = Src[sy * ImgW + sx];
    }
  }

  FLandscapeEditDataInterface Edit(Info, false);
  Edit.SetHeightData(MinX, MinY, MaxX, MaxY, Heights.GetData(), SizeX, true);
  Edit.Flush();
  Landscape->MarkPackageDirty();

  TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("landscape"), Landscape->GetName());
  Result->SetStringField(TEXT("file"), FilePath);
  Result->SetNumberField(TEXT("imageWidth"), ImgW);
  Result->SetNumberField(TEXT("imageHeight"), ImgH);
  Result->SetNumberField(TEXT("landscapeSizeX"), SizeX);
  Result->SetNumberField(TEXT("landscapeSizeY"), SizeY);
  Result->SetBoolField(TEXT("sizeMatched"), ImgW == SizeX && ImgH == SizeY);
  Result->SetBoolField(TEXT("flippedY"), bFlipY);
  SendAutomationResponse(RequestingSocket, RequestId, true,
                         TEXT("Heightmap imported from file"), Result);
  return true;
#else
  SendAutomationResponse(RequestingSocket, RequestId, false,
                         TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
  return true;
#endif
}
