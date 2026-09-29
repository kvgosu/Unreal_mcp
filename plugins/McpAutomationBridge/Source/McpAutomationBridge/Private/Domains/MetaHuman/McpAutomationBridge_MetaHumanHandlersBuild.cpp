#include "Domains/MetaHuman/McpAutomationBridge_MetaHumanHandlers.h"

#if WITH_EDITOR
#include "Safety/McpSafeOperations.h"

namespace McpMetaHumanHandlers
{
// SphereX: metahuman_textures -- synthesise the character's texture sources.
//
// Without this the build refuses with "The Character is missing textures, use
// Download Texture Sources to create them before assembling", and that reason
// appears ONLY in the editor log: CanBuildMetaHuman just answers false, so from
// the outside it looks exactly like an unrigged character. We lost a rig cycle
// to that before adding this.
//
// Synthesis is local (Optional/TextureSynthesis in Core Data), not a cloud call,
// so blocking is the sane default here -- unlike auto-rigging.
bool HandleMetaHumanTextures(UMcpAutomationBridgeSubsystem* Self, const FString& RequestId,
    const TSharedPtr<FJsonObject>& Payload, FMetaHumanSocket Socket)
{
    UObject* Subsystem = nullptr;
    UObject* Character = RequireEditableCharacter(Self, RequestId, Payload, Socket, Subsystem);
    if (!Character)
    {
        return true;
    }

    // Field names are the UPROPERTY names on FMetaHumanCharacterTextureRequestParams.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetBoolField(TEXT("bReportProgress"), GetJsonBoolField(Payload, TEXT("reportProgress"), false));
    Params->SetBoolField(TEXT("bBlocking"), GetJsonBoolField(Payload, TEXT("blocking"), true));

    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetObjectField(TEXT("InParams"), Params);

    TSharedPtr<FJsonObject> Results;
    FString Error;
    if (!InvokeMetaHumanFunction(Subsystem, TEXT("RequestTextureSources"), Args, Character, Results, Error))
    {
        Self->SendAutomationError(Socket, RequestId, Error, TEXT("OPERATION_FAILED"));
        return true;
    }

    // The request answers through delegates, so report build readiness rather
    // than claiming success.
    bool bCanBuild = false;
    TSharedPtr<FJsonObject> CanBuildArgs = MakeShared<FJsonObject>();
    CanBuildArgs->SetBoolField(TEXT("bInLogError"), true);
    TSharedPtr<FJsonObject> CanBuildResults;
    FString CanBuildError;
    if (InvokeMetaHumanFunction(Subsystem, TEXT("CanBuildMetaHuman"), CanBuildArgs, Character, CanBuildResults, CanBuildError)
        && CanBuildResults.IsValid())
    {
        CanBuildResults->TryGetBoolField(TEXT("ReturnValue"), bCanBuild);
    }

    TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
    Result->SetStringField(TEXT("characterPath"), GetJsonStringField(Payload, TEXT("characterPath")));
    Result->SetBoolField(TEXT("canBuild"), bCanBuild);
    Self->SendAutomationResponse(Socket, RequestId, true,
        bCanBuild ? TEXT("Texture sources ready; character can be built")
                  : TEXT("Texture sources requested; character still not buildable"),
        Result, FString());
    return true;
}

// SphereX: spawn_metahuman -- put the assembled character into the level.
//
// Without this there is no way to LOOK at the result: build reports success and
// writes an asset, but an asset is not a frame. The function falls back to a
// default actor type if the pipeline's own actor fails, so it always returns
// something -- which means the returned path is the only proof the spawn took.
//
// bKeepTransient=false by default: a transient actor vanishes on save and the
// level would look empty next session.
bool HandleSpawnMetaHuman(UMcpAutomationBridgeSubsystem* Self, const FString& RequestId,
    const TSharedPtr<FJsonObject>& Payload, FMetaHumanSocket Socket)
{
    UObject* Subsystem = nullptr;
    UObject* Character = RequireEditableCharacter(Self, RequestId, Payload, Socket, Subsystem);
    if (!Character)
    {
        return true;
    }

    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetObjectField(TEXT("InCharacter"), nullptr);
    Args->SetBoolField(TEXT("bKeepTransient"), GetJsonBoolField(Payload, TEXT("keepTransient"), false));

    TSharedPtr<FJsonObject> Results;
    FString Error;
    if (!InvokeMetaHumanFunction(Subsystem, TEXT("SpawnMetaHumanActor"), Args, Character, Results, Error))
    {
        Self->SendAutomationError(Socket, RequestId, Error, TEXT("OPERATION_FAILED"));
        return true;
    }

    FString ActorPath;
    if (Results.IsValid())
    {
        Results->TryGetStringField(TEXT("ReturnValue"), ActorPath);
    }

    TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
    Result->SetStringField(TEXT("characterPath"), GetJsonStringField(Payload, TEXT("characterPath")));
    Result->SetStringField(TEXT("actorPath"), ActorPath);
    Result->SetBoolField(TEXT("spawned"), !ActorPath.IsEmpty());
    Self->SendAutomationResponse(Socket, RequestId, !ActorPath.IsEmpty(),
        ActorPath.IsEmpty() ? TEXT("Spawn returned no actor") : TEXT("MetaHuman actor spawned"),
        Result, ActorPath.IsEmpty() ? TEXT("OPERATION_FAILED") : FString());
    return true;
}

// rig_metahuman -- request auto-rigging for a character.
//
// This is the step that gates assembly: CanBuildMetaHuman refuses an unrigged
// character. Auto-rigging is an Epic CLOUD service (FAutoRigServiceRequest), so
// it needs the editor signed in to an Epic account and can fail with an
// authorization error that has nothing to do with the character.
bool HandleRigMetaHuman(UMcpAutomationBridgeSubsystem* Self, const FString& RequestId,
    const TSharedPtr<FJsonObject>& Payload, FMetaHumanSocket Socket)
{
    UObject* Subsystem = nullptr;
    UObject* Character = RequireEditableCharacter(Self, RequestId, Payload, Socket, Subsystem);
    if (!Character)
    {
        return true;
    }

    const FString RigType = GetJsonStringField(Payload, TEXT("rigType"), TEXT("JointsAndBlendShapes"));
    if (RigType != TEXT("JointsOnly") && RigType != TEXT("JointsAndBlendShapes"))
    {
        Self->SendAutomationError(Socket, RequestId,
            FString::Printf(TEXT("Unknown rigType '%s'. Use 'JointsOnly' or 'JointsAndBlendShapes'."), *RigType),
            TEXT("INVALID_ARGUMENT"));
        return true;
    }

    // Field names are the UPROPERTY names on FMetaHumanCharacterAutoRiggingRequestParams;
    // the reflected binder converts the enum from its string name.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    // Non-blocking by default: the blocking mode parks the game thread on an
    // Epic cloud round-trip, which freezes the editor and silences every
    // transport heartbeat for as long as the service takes. The caller polls
    // metahuman_status for canBuild instead, or passes blocking:true to wait.
    const bool bBlocking = GetJsonBoolField(Payload, TEXT("blocking"), false);
    Params->SetStringField(TEXT("RigType"), RigType);
    Params->SetBoolField(TEXT("bBlocking"), bBlocking);
    Params->SetBoolField(TEXT("bReportProgress"), GetJsonBoolField(Payload, TEXT("reportProgress"), false));

    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetObjectField(TEXT("InParams"), Params);

    TSharedPtr<FJsonObject> Results;
    FString Error;
    if (!InvokeMetaHumanFunction(Subsystem, TEXT("RequestAutoRigging"), Args, Character, Results, Error))
    {
        Self->SendAutomationError(Socket, RequestId, Error, TEXT("OPERATION_FAILED"));
        return true;
    }

    // The request reports its own outcome through delegates, not a return value,
    // so re-read build readiness rather than claiming the rig succeeded.
    bool bCanBuild = false;
    TSharedPtr<FJsonObject> CanBuildArgs = MakeShared<FJsonObject>();
    CanBuildArgs->SetBoolField(TEXT("bInLogError"), false);
    TSharedPtr<FJsonObject> CanBuildResults;
    FString CanBuildError;
    if (InvokeMetaHumanFunction(Subsystem, TEXT("CanBuildMetaHuman"), CanBuildArgs, Character, CanBuildResults, CanBuildError)
        && CanBuildResults.IsValid())
    {
        CanBuildResults->TryGetBoolField(TEXT("ReturnValue"), bCanBuild);
    }

    McpSafeAssetSave(Character);

    TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
    Result->SetStringField(TEXT("characterPath"), GetJsonStringField(Payload, TEXT("characterPath")));
    Result->SetStringField(TEXT("rigType"), RigType);
    Result->SetBoolField(TEXT("canBuild"), bCanBuild);
    Result->SetBoolField(TEXT("blocking"), bBlocking);
    if (!bCanBuild && bBlocking)
    {
        Result->SetStringField(TEXT("detail"),
            TEXT("Auto-rigging did not leave the character buildable. Auto-rigging runs on Epic's ")
            TEXT("cloud service: confirm the editor is signed in to an Epic account, then re-run."));
    }
    else if (!bCanBuild)
    {
        Result->SetStringField(TEXT("detail"),
            TEXT("Auto-rigging is running on Epic's cloud service. Poll manage_character ")
            TEXT("metahuman_status until canBuild is true, then run build_metahuman. Pass ")
            TEXT("blocking:true to wait here instead; the editor is unresponsive while it waits."));
    }
    McpHandlerUtils::AddVerification(Result, Character);
    const TCHAR* Message = bCanBuild
        ? TEXT("MetaHuman rigged")
        : (bBlocking ? TEXT("Auto-rigging requested but the character is still not buildable")
                     : TEXT("Auto-rigging requested; poll metahuman_status for canBuild"));
    Self->SendAutomationResponse(Socket, RequestId, true, Message, Result);
    return true;
}

// build_metahuman -- run the assembly pipeline, producing the usable assets.
bool HandleBuildMetaHuman(UMcpAutomationBridgeSubsystem* Self, const FString& RequestId,
    const TSharedPtr<FJsonObject>& Payload, FMetaHumanSocket Socket)
{
    UObject* Subsystem = nullptr;
    UObject* Character = RequireEditableCharacter(Self, RequestId, Payload, Socket, Subsystem);
    if (!Character)
    {
        return true;
    }

    // Refuse up front rather than letting BuildMetaHuman fail with only a log
    // line and report success to the caller.
    TSharedPtr<FJsonObject> CanBuildArgs = MakeShared<FJsonObject>();
    CanBuildArgs->SetBoolField(TEXT("bInLogError"), true);
    TSharedPtr<FJsonObject> CanBuildResults;
    FString CanBuildError;
    bool bCanBuild = false;
    if (InvokeMetaHumanFunction(Subsystem, TEXT("CanBuildMetaHuman"), CanBuildArgs, Character, CanBuildResults, CanBuildError)
        && CanBuildResults.IsValid())
    {
        CanBuildResults->TryGetBoolField(TEXT("ReturnValue"), bCanBuild);
    }
    if (!bCanBuild)
    {
        Self->SendAutomationError(Socket, RequestId,
            TEXT("The character is not ready to build (most often: not rigged). ")
            TEXT("Run rig_metahuman first, and use metahuman_status to see every blocker."),
            TEXT("PRECONDITION_FAILED"));
        return true;
    }

    const FString PipelineType = GetJsonStringField(Payload, TEXT("pipelineType"), TEXT("Cinematic"));
    const FString Quality = GetJsonStringField(Payload, TEXT("pipelineQuality"), TEXT("Cinematic"));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("PipelineType"), PipelineType);
    Params->SetStringField(TEXT("PipelineQuality"), Quality);
    const FString BuildPath = GetJsonStringField(Payload, TEXT("buildPath"));
    if (!BuildPath.IsEmpty()) { Params->SetStringField(TEXT("AbsoluteBuildPath"), BuildPath); }
    const FString CommonPath = GetJsonStringField(Payload, TEXT("commonFolderPath"));
    if (!CommonPath.IsEmpty()) { Params->SetStringField(TEXT("CommonFolderPath"), CommonPath); }
    const FString NameOverride = GetJsonStringField(Payload, TEXT("nameOverride"));
    if (!NameOverride.IsEmpty()) { Params->SetStringField(TEXT("NameOverride"), NameOverride); }

    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetObjectField(TEXT("InParams"), Params);

    TSharedPtr<FJsonObject> Results;
    FString Error;
    if (!InvokeMetaHumanFunction(Subsystem, TEXT("BuildMetaHuman"), Args, Character, Results, Error))
    {
        Self->SendAutomationError(Socket, RequestId, Error, TEXT("OPERATION_FAILED"));
        return true;
    }

    TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
    Result->SetStringField(TEXT("characterPath"), GetJsonStringField(Payload, TEXT("characterPath")));
    Result->SetStringField(TEXT("pipelineType"), PipelineType);
    Result->SetStringField(TEXT("pipelineQuality"), Quality);
    if (!BuildPath.IsEmpty()) { Result->SetStringField(TEXT("buildPath"), BuildPath); }
    McpHandlerUtils::AddVerification(Result, Character);
    Self->SendAutomationResponse(Socket, RequestId, true, TEXT("MetaHuman assembled"), Result);
    return true;
}
}
#endif
