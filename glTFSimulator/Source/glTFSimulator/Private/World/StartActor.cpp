// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/StartActor.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "System/FileFunctionLibrary.h"
#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UnrealType.h"
#include "World/WorldData.h"

namespace
{
    static const FName NAME_DefaultStartActorWidgetProperty(TEXT("StartWorld"));
    static const FName NAME_DefaultWorldSelectionInitFunction(TEXT("Init"));
}

AStartActor::AStartActor()
{
    PrimaryActorTick.bCanEverTick = false;
    StartActorWidgetPropertyName = NAME_DefaultStartActorWidgetProperty;
    WorldSelectionInitFunctionName = NAME_DefaultWorldSelectionInitFunction;

    // These are only safe defaults. Blueprint children can override both class variables.
    static ConstructorHelpers::FClassFinder<UUserWidget> DefaultStartMenuWidgetFinder(TEXT("/Game/Blueprints/StartWorld/WBP_StartWorld"));
    if (DefaultStartMenuWidgetFinder.Succeeded())
    {
        StartMenuWidgetClass = DefaultStartMenuWidgetFinder.Class;
    }

    static ConstructorHelpers::FClassFinder<UUserWidget> DefaultWorldSelectionWidgetFinder(TEXT("/Game/Blueprints/StartWorld/WBP_LevelMenu"));
    if (DefaultWorldSelectionWidgetFinder.Succeeded())
    {
        WorldSelectionWidgetClass = DefaultWorldSelectionWidgetFinder.Class;
    }
}

void AStartActor::BeginPlay()
{
    Super::BeginPlay();

    // Rebuild the level list before any UI asks for it.
    BuildLevelFolderNameMap();

    // The old Blueprint graph creates widgets after calling the parent BeginPlay.
    // Running the C++ UI setup on the next tick lets this class remove that legacy output
    // and guarantees that the native flow owns the final visible menu.
    GetWorldTimerManager().SetTimerForNextTick(
        FTimerDelegate::CreateWeakLambda(this, [this]()
        {
            InitializeStartScreenAfterBlueprintBeginPlay();
        }));
}

void AStartActor::Destroyed()
{
    RemoveTrackedMenuWidgets();

    // Match the old BP_StartWorld cleanup behavior without depending on the Blueprint graph.
    UKismetSystemLibrary::CollectGarbage();

    Super::Destroyed();
}

void AStartActor::InitializeStartScreenAfterBlueprintBeginPlay()
{
    bool bOpenWorldSelection = false;

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        // A gameplay world can only be fully collected after the destination menu level has finished loading.
        if (GameManager->HasPendingMainWorldRuntimePurge())
        {
            GameManager->ReleaseMainWorldRuntimeMemory(true);
        }

        bOpenWorldSelection = GameManager->ConsumeWorldSelectionMenuRequest();
    }

    if (bOpenWorldSelection)
    {
        ShowWorldSelectionMenu();
    }
    else
    {
        ShowStartMenu();
    }
}

void AStartActor::StartGame()
{
    ShowWorldSelectionMenu();
}

void AStartActor::ReturnToMainMenuFromWorldSelection()
{
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    ShowStartMenu();
}

void AStartActor::ShowStartMenu()
{
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->ClearWorldSelectionMenuRequest();
    }

    RemoveAllMenuWidgets();

    StartMenuWidget = CreateAndAddMenuWidget(StartMenuWidgetClass, TEXT("StartMenuWidgetClass"));
    AssignStartActorReference(StartMenuWidget.Get());
    ApplyMenuInputMode(StartMenuWidget.Get());
}

void AStartActor::ShowWorldSelectionMenu()
{
    BuildLevelFolderNameMap();
    RemoveAllMenuWidgets();

    WorldSelectionWidget = CreateAndAddMenuWidget(WorldSelectionWidgetClass, TEXT("WorldSelectionWidgetClass"));
    AssignStartActorReference(WorldSelectionWidget.Get());
    InvokeWorldSelectionInit(WorldSelectionWidget.Get());
    ApplyMenuInputMode(WorldSelectionWidget.Get());
}

void AStartActor::RefreshWorldFolderNameMap()
{
    BuildLevelFolderNameMap();
}

UUserWidget* AStartActor::CreateAndAddMenuWidget(TSubclassOf<UUserWidget> WidgetClass, const TCHAR* DebugWidgetName)
{
    if (!WidgetClass)
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor cannot create %s because the widget class is not assigned. Set it in the Blueprint child."),
               DebugWidgetName ? DebugWidgetName : TEXT("MenuWidgetClass"));
        return nullptr;
    }

    UUserWidget* Widget = nullptr;
    if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
    {
        Widget = CreateWidget<UUserWidget>(PlayerController, WidgetClass);
    }
    else if (UWorld* World = GetWorld())
    {
        Widget = CreateWidget<UUserWidget>(World, WidgetClass);
    }

    if (!IsValid(Widget))
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor failed to create %s."), DebugWidgetName ? DebugWidgetName : TEXT("MenuWidget"));
        return nullptr;
    }

    Widget->AddToViewport(MenuZOrder);
    return Widget;
}

void AStartActor::RemoveTrackedMenuWidgets()
{
    if (IsValid(StartMenuWidget))
    {
        StartMenuWidget->RemoveFromParent();
    }
    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->RemoveFromParent();
    }

    StartMenuWidget = nullptr;
    WorldSelectionWidget = nullptr;
}

void AStartActor::RemoveAllMenuWidgets()
{
    UWidgetLayoutLibrary::RemoveAllWidgets(this);
}

void AStartActor::ApplyMenuInputMode(UUserWidget* FocusWidget) const
{
    if (!bApplyMenuInputMode)
    {
        return;
    }

    APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
    if (!PlayerController)
    {
        return;
    }

    PlayerController->bShowMouseCursor = true;

    FInputModeUIOnly InputMode;
    InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    if (IsValid(FocusWidget))
    {
        InputMode.SetWidgetToFocus(FocusWidget->TakeWidget());
    }
    PlayerController->SetInputMode(InputMode);
}

void AStartActor::AssignStartActorReference(UUserWidget* Widget) const
{
    if (!IsValid(Widget) || StartActorWidgetPropertyName.IsNone())
    {
        return;
    }

    FProperty* Property = Widget->GetClass()->FindPropertyByName(StartActorWidgetPropertyName);
    FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
    if (!ObjectProperty)
    {
        return;
    }

    if (!ObjectProperty->PropertyClass || IsA(ObjectProperty->PropertyClass))
    {
        ObjectProperty->SetObjectPropertyValue_InContainer(Widget, const_cast<AStartActor*>(this));
    }
}

bool AStartActor::HasCompatibleInitFunction(const UFunction* Function) const
{
    if (!Function)
    {
        return false;
    }

    int32 NonReturnParamCount = 0;
    bool bHasCompatibleMapParam = false;

    for (TFieldIterator<FProperty> It(Function); It; ++It)
    {
        const FProperty* Property = *It;
        if (!Property || !Property->HasAnyPropertyFlags(CPF_Parm) || Property->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            continue;
        }

        ++NonReturnParamCount;
        const FMapProperty* MapProperty = CastField<FMapProperty>(Property);
        if (!MapProperty)
        {
            continue;
        }

        const FStrProperty* KeyProperty = CastField<FStrProperty>(MapProperty->KeyProp);
        const FStrProperty* ValueProperty = CastField<FStrProperty>(MapProperty->ValueProp);
        bHasCompatibleMapParam = KeyProperty && ValueProperty;
    }

    return NonReturnParamCount == 1 && bHasCompatibleMapParam;
}

void AStartActor::InvokeWorldSelectionInit(UUserWidget* Widget) const
{
    if (!IsValid(Widget) || WorldSelectionInitFunctionName.IsNone())
    {
        return;
    }

    UFunction* InitFunction = Widget->FindFunction(WorldSelectionInitFunctionName);
    if (!InitFunction)
    {
        UE_LOG(LogTemp, Verbose, TEXT("World-selection widget has no %s function."), *WorldSelectionInitFunctionName.ToString());
        return;
    }

    if (!HasCompatibleInitFunction(InitFunction))
    {
        UE_LOG(LogTemp, Warning, TEXT("World-selection widget function %s must take exactly one TMap<FString, FString> parameter."),
               *WorldSelectionInitFunctionName.ToString());
        return;
    }

    struct FLevelMenuInitParams
    {
        TMap<FString, FString> Values;
    };

    FLevelMenuInitParams Params;
    Params.Values = FolderNameMap;
    Widget->ProcessEvent(InitFunction, &Params);
}

void AStartActor::BuildLevelFolderNameMap()
{
    FolderNameMap.Empty();

    // Build absolute paths relative to the project Content directory.
    const FString RootPath = PATH_ROOT;
    TArray<FString> SubFolders;
    if (!UFileFunctionLibrary::GetSubFolders(RootPath, SubFolders))
    {
        UE_LOG(LogTemp, Warning, TEXT("No subfolders found in %s"), *RootPath);
        return;
    }

    for (const FString& SubFolderName : SubFolders)
    {
        const FString FullSubFolderPath = RootPath / SubFolderName;
        if (!FPaths::DirectoryExists(FullSubFolderPath))
        {
            continue;
        }

        const FString LevelJsonPath = FullSubFolderPath / LEVEL_FILE_NAME;
        FString NameValue;
        if (UFileFunctionLibrary::LoadJsonStringValue(LevelJsonPath, LEVELNAME, NameValue))
        {
            FolderNameMap.Add(SubFolderName, NameValue);
            UE_LOG(LogTemp, Log, TEXT("Loaded Level: Folder=%s, Name=%s"), *SubFolderName, *NameValue);
        }
    }
}
