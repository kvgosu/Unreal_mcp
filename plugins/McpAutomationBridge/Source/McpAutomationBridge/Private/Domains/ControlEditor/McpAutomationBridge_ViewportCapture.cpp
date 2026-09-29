// SphereX: глаза моста — снимок вьюпорта редактора и управление его камерой.
//
// Без этого работа идёт вслепую: команды уходят, ответы приходят, а что на
// сцене видно на самом деле — неизвестно. Штатный HighResShot снимает не тот
// вьюпорт и не даёт поставить камеру, а BugItGo двигает игровую камеру, а не
// редакторскую: три кадра подряд вышли пустым небом при живом ландшафте.
//
// Две операции:
//   set_viewport_camera  — поставить камеру редактора (location + rotation)
//   capture_viewport     — снять активный вьюпорт в файл и вернуть путь
//
// Вместе они дают то, чего не хватало: посмотреть на сцену своими глазами и
// сверить с числами.

#include "Core/Compatibility/McpVersionCompatibility.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Foundation/BridgeHelpers/McpAutomationBridgeHelpers.h"
#include "McpAutomationBridgeSubsystem.h"
#include "Foundation/HandlerUtils/McpHandlerUtils.h"

#if WITH_EDITOR
#include "Editor.h"
#include "EditorViewportClient.h"
#include "HighResScreenshot.h"
#include "ImageUtils.h"
#include "UnrealEngine.h"
#include "LevelEditorViewport.h"
#include "RenderingThread.h"
#include "UnrealClient.h"
#endif

bool UMcpAutomationBridgeSubsystem::HandleViewportCamera(
    const FString &RequestId, const FString &Action,
    const TSharedPtr<FJsonObject> &Payload,
    TSharedPtr<FMcpBridgeWebSocket> RequestingSocket) {
  if (!Action.Equals(TEXT("set_viewport_camera"), ESearchCase::IgnoreCase)) {
    return false;
  }
#if WITH_EDITOR
  if (!GEditor) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
    return true;
  }

  // ⚠ GetAllViewportClients отдаёт ВСЕ вьюпорты редактора, включая превью
  // материалов и миниатюры. Первый перспективный из них — не тот, что видит
  // человек: три снимка подряд вышли одинаково чёрными. Берём вьюпорт УРОВНЯ.
  FEditorViewportClient *Client = GCurrentLevelEditingViewportClient;
  if (!Client) {
    for (FLevelEditorViewportClient *C : GEditor->GetLevelViewportClients()) {
      if (C && C->IsPerspective()) {
        Client = C;
        break;
      }
    }
  }
  if (!Client) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("NO_PERSPECTIVE_VIEWPORT"), nullptr);
    return true;
  }

  FVector Loc = Client->GetViewLocation();
  FRotator Rot = Client->GetViewRotation();

  const TSharedPtr<FJsonObject> *LocObj = nullptr;
  if (Payload.IsValid() && Payload->TryGetObjectField(TEXT("location"), LocObj) &&
      LocObj && LocObj->IsValid()) {
    double v = 0.0;
    if ((*LocObj)->TryGetNumberField(TEXT("x"), v)) Loc.X = v;
    if ((*LocObj)->TryGetNumberField(TEXT("y"), v)) Loc.Y = v;
    if ((*LocObj)->TryGetNumberField(TEXT("z"), v)) Loc.Z = v;
  }
  const TSharedPtr<FJsonObject> *RotObj = nullptr;
  if (Payload.IsValid() && Payload->TryGetObjectField(TEXT("rotation"), RotObj) &&
      RotObj && RotObj->IsValid()) {
    double v = 0.0;
    if ((*RotObj)->TryGetNumberField(TEXT("pitch"), v)) Rot.Pitch = v;
    if ((*RotObj)->TryGetNumberField(TEXT("yaw"), v)) Rot.Yaw = v;
    if ((*RotObj)->TryGetNumberField(TEXT("roll"), v)) Rot.Roll = v;
  }

  // навестись на точку, если она задана: избавляет от счёта углов вручную
  const TSharedPtr<FJsonObject> *LookObj = nullptr;
  if (Payload.IsValid() && Payload->TryGetObjectField(TEXT("lookAt"), LookObj) &&
      LookObj && LookObj->IsValid()) {
    FVector Target(0, 0, 0);
    double v = 0.0;
    if ((*LookObj)->TryGetNumberField(TEXT("x"), v)) Target.X = v;
    if ((*LookObj)->TryGetNumberField(TEXT("y"), v)) Target.Y = v;
    if ((*LookObj)->TryGetNumberField(TEXT("z"), v)) Target.Z = v;
    Rot = (Target - Loc).Rotation();
  }

  Client->SetViewLocation(Loc);
  Client->SetViewRotation(Rot);
  if (Payload.IsValid()) {
    double Fov = 0.0;
    if (Payload->TryGetNumberField(TEXT("fov"), Fov) && Fov > 5.0 && Fov < 170.0) {
      Client->ViewFOV = Fov;
    }
  }
  Client->Invalidate();

  TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
  TSharedPtr<FJsonObject> L = MakeShared<FJsonObject>();
  L->SetNumberField(TEXT("x"), Loc.X);
  L->SetNumberField(TEXT("y"), Loc.Y);
  L->SetNumberField(TEXT("z"), Loc.Z);
  Result->SetObjectField(TEXT("location"), L);
  TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
  R->SetNumberField(TEXT("pitch"), Rot.Pitch);
  R->SetNumberField(TEXT("yaw"), Rot.Yaw);
  R->SetNumberField(TEXT("roll"), Rot.Roll);
  Result->SetObjectField(TEXT("rotation"), R);
  Result->SetNumberField(TEXT("fov"), Client->ViewFOV);
  SendAutomationResponse(RequestingSocket, RequestId, true,
                         TEXT("Viewport camera set"), Result);
  return true;
#else
  SendAutomationResponse(RequestingSocket, RequestId, false,
                         TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
  return true;
#endif
}

bool UMcpAutomationBridgeSubsystem::HandleCaptureViewport(
    const FString &RequestId, const FString &Action,
    const TSharedPtr<FJsonObject> &Payload,
    TSharedPtr<FMcpBridgeWebSocket> RequestingSocket) {
  if (!Action.Equals(TEXT("capture_viewport"), ESearchCase::IgnoreCase)) {
    return false;
  }
#if WITH_EDITOR
  if (!GEditor) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
    return true;
  }

  // ⚠ GetAllViewportClients отдаёт ВСЕ вьюпорты редактора, включая превью
  // материалов и миниатюры. Первый перспективный из них — не тот, что видит
  // человек: три снимка подряд вышли одинаково чёрными. Берём вьюпорт УРОВНЯ.
  FEditorViewportClient *Client = GCurrentLevelEditingViewportClient;
  if (!Client) {
    for (FLevelEditorViewportClient *C : GEditor->GetLevelViewportClients()) {
      if (C && C->IsPerspective()) {
        Client = C;
        break;
      }
    }
  }
  if (!Client || !Client->Viewport) {
    SendAutomationResponse(RequestingSocket, RequestId, false,
                           TEXT("NO_PERSPECTIVE_VIEWPORT"), nullptr);
    return true;
  }

  FString OutPath = Payload.IsValid() ? Payload->GetStringField(TEXT("filePath"))
                                      : FString();
  if (OutPath.IsEmpty()) {
    OutPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCP"),
                              TEXT("viewport.png"));
  }
  const FString Dir = FPaths::GetPath(OutPath);
  IFileManager::Get().MakeDirectory(*Dir, true);

  // ⛔ ReadPixels с вьюпорта редактора отдаёт ЧЁРНОЕ, сколько ни жди поток
  // рендера: проверено двумя сборками, кадр не менялся ни на байт, хотя
  // оверлей с осями в нём виден. Берём штатный механизм скриншотов — тот
  // самый, что работает из консоли, — и просто указываем ему наш файл.
  FScreenshotRequest::RequestScreenshot(OutPath, false, false);
  Client->Viewport->Invalidate();
  Client->Viewport->Draw();
  FlushRenderingCommands();
  // механизм пишет файл в следующем кадре, поэтому рисуем ещё раз
  Client->Viewport->Draw();
  FlushRenderingCommands();

  const FString Written = FScreenshotRequest::GetFilename();
  const bool bExists = !Written.IsEmpty() && FPaths::FileExists(Written);

  const FVector Loc = Client->GetViewLocation();
  const FRotator Rot = Client->GetViewRotation();
  TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("filePath"), bExists ? Written : OutPath);
  Result->SetBoolField(TEXT("fileWritten"), bExists);
  Result->SetNumberField(TEXT("width"), Client->Viewport->GetSizeXY().X);
  Result->SetNumberField(TEXT("height"), Client->Viewport->GetSizeXY().Y);
  Result->SetNumberField(
      TEXT("bytes"), bExists ? IFileManager::Get().FileSize(*Written) : 0);
  Result->SetStringField(TEXT("cameraLocation"),
                         FString::Printf(TEXT("%.0f, %.0f, %.0f"), Loc.X, Loc.Y, Loc.Z));
  Result->SetStringField(TEXT("cameraRotation"),
                         FString::Printf(TEXT("%.1f, %.1f, %.1f"), Rot.Pitch,
                                         Rot.Yaw, Rot.Roll));
  SendAutomationResponse(RequestingSocket, RequestId, true,
                         TEXT("Viewport captured"), Result);
  return true;
#else
  SendAutomationResponse(RequestingSocket, RequestId, false,
                         TEXT("EDITOR_NOT_AVAILABLE"), nullptr);
  return true;
#endif
}
