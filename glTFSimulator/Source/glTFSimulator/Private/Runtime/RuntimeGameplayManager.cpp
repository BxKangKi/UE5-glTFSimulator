// Copyright © 2026 BxKangKi. Licensed under the MIT License.

// Runtime gameplay note: dense comments are intentional here because these Blueprint-facing functions define the creator-mode interaction contract.
// Runtime gameplay note: center-crosshair placement, click-vs-hold vertex editing, and vertex merge behavior are all coordinated in this file.

#include "Runtime/RuntimeGameplayManager.h"
#include "Runtime/RuntimeEditableMeshActor.h"
#include "Runtime/RuntimeGLTFSaveLibrary.h"
#include "Runtime/RuntimePrefabActor.h"
#include "Runtime/RuntimeVehiclePawn.h"
#include "Runtime/RuntimeWeaponActor.h"
#include "Character/CharacterController.h"
#include "Character/CharacterComponent.h"
#include "Character/PlayerCharacterController.h"
#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

static constexpr int32 RuntimeToolbarSlotCount = 7;

ARuntimeGameplayManager::ARuntimeGameplayManager()
{
    PrimaryActorTick.bCanEverTick = true;

    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(Root);

    PlacementGridComponent = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PlacementGrid"));
    PlacementGridComponent->SetupAttachment(Root);
    PlacementGridComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    PlacementGridComponent->SetGenerateOverlapEvents(false);
    PlacementGridComponent->SetCastShadow(false);
    PlacementGridComponent->SetHiddenInGame(true);

    static ConstructorHelpers::FObjectFinder<UMaterialInterface> GridMaterialFinder(TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial"));
    if (GridMaterialFinder.Succeeded())
    {
        PlacementGridMaterial = GridMaterialFinder.Object;
    }

    PrefabActorClass = ARuntimePrefabActor::StaticClass();
    EditableMeshActorClass = ARuntimeEditableMeshActor::StaticClass();
    VehiclePawnClass = ARuntimeVehiclePawn::StaticClass();
    WeaponActorClass = ARuntimeWeaponActor::StaticClass();
}

void ARuntimeGameplayManager::BeginPlay()
{
    Super::BeginPlay();
    EnsureRuntimeFolders();
    ScanRuntimeFolders();
    BuildAvailableItems();
    InitializeToolbarSlotsIfNeeded();
    ApplySelectedToolbarItem(false);
    UE_LOG(LogTemp, Display, TEXT("[RuntimeGameplay] RuntimeGameplayManager active: %s Class=%s"),
        *GetNameSafe(this),
        *GetNameSafe(GetClass()));
    NotifyToolbarChanged();
    NotifyRuntimeStateChanged();
    GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
    {
        LoadSavedRuntimeScene();
    }));

    if (bAutoSaveRuntimeScene && RuntimeSceneAutoSaveIntervalSeconds >= 5.0f)
    {
        GetWorldTimerManager().SetTimer(
            RuntimeSceneAutoSaveTimerHandle,
            this,
            &ARuntimeGameplayManager::AutoSaveRuntimeScene,
            RuntimeSceneAutoSaveIntervalSeconds,
            true);
    }
}

void ARuntimeGameplayManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(RuntimeSceneAutoSaveTimerHandle);
    }

    if (bSaveRuntimeSceneOnEndPlay && EndPlayReason != EEndPlayReason::Destroyed)
    {
        SaveRuntimeScene();
    }

    Super::EndPlay(EndPlayReason);
}

void ARuntimeGameplayManager::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    if (RuntimePlayMode != ERuntimePlayMode::Creator)
    {
        DestroyPendingEmptyObjectPreview();
        ClearPlacementGridMesh();
        return;
    }

    const bool bNeedsPlacementTick = CurrentMode != ERuntimeToolMode::None
        || IsValid(CurrentEditableActor)
        || bPrimaryVertexPressActive
        || IsSelectedToolbarItemObjectCreation();
    if (!bNeedsPlacementTick)
    {
        DestroyPendingEmptyObjectPreview();
        ClearPlacementGridMesh();
        return;
    }

    FHitResult Hit;
    FVector Preview;
    if (TracePlacementLocation(Preview, Hit))
    {
        LastPreviewLocation = ApplyGridSnap(Preview);
    }

    UpdateObjectCreationPreview();
    UpdateEditableVertexPreviewAndSelection();
    UpdatePlacementGrid();
}

void ARuntimeGameplayManager::EnsureRuntimeFolders() const
{
    IFileManager::Get().MakeDirectory(*GetPrefabDirectory(), true);
    IFileManager::Get().MakeDirectory(*GetItemsDirectory(), true);
    IFileManager::Get().MakeDirectory(*FPaths::Combine(GetWorldRootPath(), TEXT("generated")), true);
}

FString ARuntimeGameplayManager::GetWorldRootPath() const
{
    FString WorldName = TEXT("New World");
    if (UGameManagerSubSystem* SubSystem = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        if (!SubSystem->GetCurrentWorldName().IsEmpty())
        {
            WorldName = SubSystem->GetCurrentWorldName();
        }
    }
    return FPaths::Combine(PATH_ROOT, WorldName);
}

FString ARuntimeGameplayManager::GetPrefabDirectory() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("prefab"));
}

FString ARuntimeGameplayManager::GetItemsDirectory() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("items"));
}

FString ARuntimeGameplayManager::GetManifestPath() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("entities.json"));
}

FString ARuntimeGameplayManager::GetLegacyManifestPath() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("runtime_installed.json"));
}

FString ARuntimeGameplayManager::GetLegacyGltfScenePath() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("runtime_installed.gltf"));
}

void ARuntimeGameplayManager::ScanRuntimeFolders()
{
    PrefabFiles.Empty();
    VehicleFiles.Empty();
    WeaponFiles.Empty();

    IFileManager& FileManager = IFileManager::Get();
    const FString WorldName = FPaths::GetCleanFilename(GetWorldRootPath());

    TArray<FString> PrefabDirectories;
    PrefabDirectories.Add(GetPrefabDirectory());
    PrefabDirectories.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("World"), TEXT("prefab")));
    PrefabDirectories.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("World"), WorldName, TEXT("prefab")));

    TArray<FString> ItemDirectories;
    ItemDirectories.Add(GetItemsDirectory());
    ItemDirectories.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("World"), TEXT("items")));
    ItemDirectories.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("World"), WorldName, TEXT("items")));

    auto AppendGltfFiles = [&FileManager](const TArray<FString>& Directories, TArray<FString>& OutFiles)
    {
        for (const FString& Directory : Directories)
        {
            TArray<FString> Glb;
            TArray<FString> Gltf;
            FileManager.FindFilesRecursive(Glb, *Directory, TEXT("*.glb"), true, false, false);
            FileManager.FindFilesRecursive(Gltf, *Directory, TEXT("*.gltf"), true, false, false);
            for (FString& Path : Glb)
            {
                FPaths::NormalizeFilename(Path);
                OutFiles.AddUnique(Path);
            }
            for (FString& Path : Gltf)
            {
                FPaths::NormalizeFilename(Path);
                OutFiles.AddUnique(Path);
            }
        }
    };

    AppendGltfFiles(PrefabDirectories, PrefabFiles);
    AppendGltfFiles(ItemDirectories, WeaponFiles);

    // A glTF/GLB prefab that contains a mesh/node name ending with ;WHEL is a driveable vehicle asset.
    // Keep it out of the normal prefab list so the Vehicle placement path can load it into ARuntimeVehiclePawn.
    for (int32 Index = PrefabFiles.Num() - 1; Index >= 0; --Index)
    {
        if (DoesRuntimeAssetFileContainWheelTag(PrefabFiles[Index]))
        {
            VehicleFiles.AddUnique(PrefabFiles[Index]);
            PrefabFiles.RemoveAt(Index);
        }
    }

    PrefabFiles.Sort();
    VehicleFiles.Sort();
    WeaponFiles.Sort();

    CurrentPrefabIndex = PrefabFiles.Num() > 0 ? FMath::Clamp(CurrentPrefabIndex, 0, PrefabFiles.Num() - 1) : 0;
    CurrentWeaponIndex = WeaponFiles.Num() > 0 ? FMath::Clamp(CurrentWeaponIndex, 0, WeaponFiles.Num() - 1) : 0;
}

bool ARuntimeGameplayManager::DoesRuntimeAssetFileContainWheelTag(const FString& FilePath) const
{
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FilePath) || Bytes.Num() <= 0)
    {
        return false;
    }

    auto ToUpperAscii = [](uint8 Value) -> uint8
    {
        return Value >= 'a' && Value <= 'z' ? Value - ('a' - 'A') : Value;
    };

    auto ContainsTag = [&Bytes, &ToUpperAscii](const ANSICHAR* Tag) -> bool
    {
        const int32 TagLen = FCStringAnsi::Strlen(Tag);
        if (TagLen <= 0 || Bytes.Num() < TagLen)
        {
            return false;
        }

        for (int32 Index = 0; Index <= Bytes.Num() - TagLen; ++Index)
        {
            bool bMatches = true;
            for (int32 TagIndex = 0; TagIndex < TagLen; ++TagIndex)
            {
                if (ToUpperAscii(Bytes[Index + TagIndex]) != static_cast<uint8>(Tag[TagIndex]))
                {
                    bMatches = false;
                    break;
                }
            }

            if (bMatches)
            {
                return true;
            }
        }

        return false;
    };

    return ContainsTag(";WHEL") || ContainsTag(";WHEEL");
}

FString ARuntimeGameplayManager::GetRuntimeAssetDisplayName(const FString& AssetPath) const
{
    const FString JsonPath = FPaths::ChangeExtension(AssetPath, TEXT("json"));
    FString JsonString;
    if (FFileHelper::LoadFileToString(JsonString, *JsonPath))
    {
        TSharedPtr<FJsonObject> RootObject;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
        if (FJsonSerializer::Deserialize(Reader, RootObject) && RootObject.IsValid())
        {
            FString DisplayName;
            if (RootObject->TryGetStringField(TEXT("DisplayName"), DisplayName) && !DisplayName.IsEmpty())
            {
                return DisplayName;
            }
            if (RootObject->TryGetStringField(TEXT("Name"), DisplayName) && !DisplayName.IsEmpty())
            {
                return DisplayName;
            }
        }
    }

    return FPaths::GetBaseFilename(AssetPath);
}

FRuntimeToolbarItem ARuntimeGameplayManager::MakeToolbarItem(ERuntimeToolbarItemKind Kind, const FString& DisplayName, const FString& SourcePath, int32 SourceIndex) const
{
    FRuntimeToolbarItem Item;
    Item.Kind = Kind;
    Item.DisplayName = DisplayName;
    Item.SourcePath = SourcePath;
    Item.SourceIndex = SourceIndex;
    Item.bAvailable = Kind != ERuntimeToolbarItemKind::None;
    return Item;
}

void ARuntimeGameplayManager::BuildAvailableItems()
{
    AvailableItems.Empty();

    if (RuntimePlayMode == ERuntimePlayMode::Creator)
    {
        if (bEnableObjectVertexCreation)
        {
            AvailableItems.Add(MakeToolbarItem(ERuntimeToolbarItemKind::CreateObject, TEXT("오브젝트 만들기")));
        }

        AvailableItems.Add(MakeToolbarItem(ERuntimeToolbarItemKind::Vehicle, TEXT("기본 차량 만들기")));

        for (int32 Index = 0; Index < VehicleFiles.Num(); ++Index)
        {
            AvailableItems.Add(MakeToolbarItem(ERuntimeToolbarItemKind::Vehicle, GetRuntimeAssetDisplayName(VehicleFiles[Index]), VehicleFiles[Index], Index));
        }

        for (int32 Index = 0; Index < PrefabFiles.Num(); ++Index)
        {
            AvailableItems.Add(MakeToolbarItem(ERuntimeToolbarItemKind::Prefab, GetRuntimeAssetDisplayName(PrefabFiles[Index]), PrefabFiles[Index], Index));
        }

        for (int32 Index = 0; Index < WeaponFiles.Num(); ++Index)
        {
            AvailableItems.Add(MakeToolbarItem(ERuntimeToolbarItemKind::Weapon, GetRuntimeAssetDisplayName(WeaponFiles[Index]), WeaponFiles[Index], Index));
        }
    }

    ReconcileToolbarSlotsWithAvailableItems();
}

void ARuntimeGameplayManager::InitializeToolbarSlotsIfNeeded()
{
    if (ToolbarSlots.Num() != RuntimeToolbarSlotCount)
    {
        ToolbarSlots.SetNum(RuntimeToolbarSlotCount);
    }

    if (bToolbarInitialized)
    {
        ReconcileToolbarSlotsWithAvailableItems();
        return;
    }

    for (int32 Slot = 0; Slot < RuntimeToolbarSlotCount; ++Slot)
    {
        ToolbarSlots[Slot] = AvailableItems.IsValidIndex(Slot) ? AvailableItems[Slot] : FRuntimeToolbarItem();
    }

    SelectedToolbarSlotIndex = 0;
    bToolbarInitialized = true;
    ReconcileToolbarSlotsWithAvailableItems();
}

int32 ARuntimeGameplayManager::FindAvailableItemIndexMatching(const FRuntimeToolbarItem& Item) const
{
    for (int32 Index = 0; Index < AvailableItems.Num(); ++Index)
    {
        const FRuntimeToolbarItem& Candidate = AvailableItems[Index];
        if (Candidate.Kind != Item.Kind)
        {
            continue;
        }

        if (Item.Kind == ERuntimeToolbarItemKind::CreateObject)
        {
            return Index;
        }

        if (Item.Kind == ERuntimeToolbarItemKind::Vehicle)
        {
            if (Item.SourcePath.IsEmpty() && Candidate.SourcePath.IsEmpty())
            {
                return Index;
            }
            if (!Item.SourcePath.IsEmpty() && Candidate.SourcePath.Equals(Item.SourcePath, ESearchCase::IgnoreCase))
            {
                return Index;
            }
            continue;
        }

        if (!Item.SourcePath.IsEmpty() && Candidate.SourcePath.Equals(Item.SourcePath, ESearchCase::IgnoreCase))
        {
            return Index;
        }
    }

    return INDEX_NONE;
}

void ARuntimeGameplayManager::ReconcileToolbarSlotsWithAvailableItems()
{
    if (ToolbarSlots.Num() != RuntimeToolbarSlotCount)
    {
        ToolbarSlots.SetNum(RuntimeToolbarSlotCount);
    }

    for (int32 Slot = 0; Slot < RuntimeToolbarSlotCount; ++Slot)
    {
        FRuntimeToolbarItem& SlotItem = ToolbarSlots[Slot];
        const int32 MatchingIndex = FindAvailableItemIndexMatching(SlotItem);
        if (AvailableItems.IsValidIndex(MatchingIndex))
        {
            SlotItem = AvailableItems[MatchingIndex];
        }
        else if (SlotItem.Kind == ERuntimeToolbarItemKind::None && AvailableItems.IsValidIndex(Slot))
        {
            SlotItem = AvailableItems[Slot];
        }
        else if (SlotItem.Kind != ERuntimeToolbarItemKind::None)
        {
            SlotItem.bAvailable = false;
        }
    }

    SelectedToolbarSlotIndex = FMath::Clamp(SelectedToolbarSlotIndex, 0, RuntimeToolbarSlotCount - 1);
}

void ARuntimeGameplayManager::NotifyToolbarChanged()
{
    OnRuntimeToolbarChanged.Broadcast();
}

void ARuntimeGameplayManager::NotifyRuntimeStateChanged()
{
    OnRuntimeStateChanged.Broadcast();
    OnRuntimeMessageChanged.Broadcast(LastSaveMessage);
}

bool ARuntimeGameplayManager::IsObjectCreationItem(const FRuntimeToolbarItem& Item) const
{
    return bEnableObjectVertexCreation
        && RuntimePlayMode == ERuntimePlayMode::Creator
        && Item.Kind == ERuntimeToolbarItemKind::CreateObject
        && Item.bAvailable;
}

bool ARuntimeGameplayManager::IsSelectedToolbarItemObjectCreation() const
{
    return IsObjectCreationItem(GetSelectedToolbarItem());
}

FRuntimeToolbarItem ARuntimeGameplayManager::GetToolbarItemAtSlot(int32 SlotIndex) const
{
    return ToolbarSlots.IsValidIndex(SlotIndex) ? ToolbarSlots[SlotIndex] : FRuntimeToolbarItem();
}

FRuntimeToolbarItem ARuntimeGameplayManager::GetSelectedToolbarItem() const
{
    return GetToolbarItemAtSlot(SelectedToolbarSlotIndex);
}

FRuntimeToolbarItem ARuntimeGameplayManager::GetAvailableItemAtIndex(int32 Index) const
{
    return AvailableItems.IsValidIndex(Index) ? AvailableItems[Index] : FRuntimeToolbarItem();
}

bool ARuntimeGameplayManager::SelectToolbarSlot(int32 SlotIndex)
{
    if (SlotIndex < 0 || SlotIndex >= RuntimeToolbarSlotCount)
    {
        return false;
    }

    SelectedToolbarSlotIndex = SlotIndex;
    ApplySelectedToolbarItem(true);
    return true;
}

void ARuntimeGameplayManager::ScrollToolbarSelection(float ScrollValue)
{
    if (FMath::IsNearlyZero(ScrollValue) || RuntimeToolbarSlotCount <= 0)
    {
        return;
    }

    const int32 Direction = ScrollValue > 0.0f ? -1 : 1;
    SelectedToolbarSlotIndex = (SelectedToolbarSlotIndex + Direction + RuntimeToolbarSlotCount) % RuntimeToolbarSlotCount;
    ApplySelectedToolbarItem(true);
}

bool ARuntimeGameplayManager::SetToolbarSlotFromAvailableItem(int32 SlotIndex, int32 AvailableItemIndex)
{
    if (!ToolbarSlots.IsValidIndex(SlotIndex) || !AvailableItems.IsValidIndex(AvailableItemIndex))
    {
        LastSaveMessage = TEXT("툴바에 넣을 아이템 인덱스가 유효하지 않습니다.");
        NotifyRuntimeStateChanged();
        return false;
    }

    ToolbarSlots[SlotIndex] = AvailableItems[AvailableItemIndex];
    SelectedToolbarSlotIndex = SlotIndex;
    ApplySelectedToolbarItem(true);
    return true;
}

bool ARuntimeGameplayManager::SelectAvailableItemForCurrentToolbarSlot(int32 AvailableItemIndex, bool bCloseItemList)
{
    if (!SetToolbarSlotFromAvailableItem(SelectedToolbarSlotIndex, AvailableItemIndex))
    {
        return false;
    }

    if (bCloseItemList)
    {
        SetItemListWindowOpen(false);
    }
    return true;
}

void ARuntimeGameplayManager::ToggleItemListWindow()
{
    SetItemListWindowOpen(!bItemListWindowOpen);
}

void ARuntimeGameplayManager::SetItemListWindowOpen(bool bOpen)
{
    if (bItemListWindowOpen == bOpen)
    {
        return;
    }

    bItemListWindowOpen = bOpen;
    LastSaveMessage = bItemListWindowOpen ? TEXT("전체 아이템 목록 열림") : TEXT("전체 아이템 목록 닫힘");

    if (APlayerCharacterController* RuntimePC = Cast<APlayerCharacterController>(GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr))
    {
        if (bItemListWindowOpen)
        {
            RuntimePC->ApplyUIInputMode(nullptr);
        }
        else
        {
            RuntimePC->ApplyGameInputMode();
        }
    }

    OnRuntimeItemListWindowChanged.Broadcast(bItemListWindowOpen);
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::SetRuntimePlayMode(ERuntimePlayMode NewMode)
{
    if (RuntimePlayMode == NewMode)
    {
        return;
    }

    RuntimePlayMode = NewMode;
    ScanRuntimeFolders();
    BuildAvailableItems();
    InitializeToolbarSlotsIfNeeded();
    ApplySelectedToolbarItem(false);
    LastSaveMessage = RuntimePlayMode == ERuntimePlayMode::Creator ? TEXT("Creator Mode") : TEXT("Real Life Mode");
    NotifyToolbarChanged();
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::ApplySelectedToolbarItem(bool bBroadcastChange)
{
    // Cache the selected toolbar item once so the rest of the function uses a stable target even if finishing an edit broadcasts UI events.
    const FRuntimeToolbarItem Item = GetSelectedToolbarItem();

    // A toolbar change is a hard mode change, so any live mesh edit must be resolved before the cursor preview swaps tools.
    // Valid meshes are finalized; meshes with only one/two dangling vertices are canceled or reverted by FinishCurrentEditableMesh().
    CloseCurrentEditableMeshForToolChange();

    // Remove the old object-creation cursor actor before the new toolbar item decides whether it needs a new preview.
    DestroyPendingEmptyObjectPreview();

    // Clear old pending locations so a previous tool cannot leave a detached cursor behind after the item changes.
    ClearPendingPlacementSelection();

    switch (Item.Kind)
    {
    case ERuntimeToolbarItemKind::CreateObject:
        if (!bEnableObjectVertexCreation)
        {
            CurrentMode = ERuntimeToolMode::None;
            LastSaveMessage = TEXT("오브젝트 정점 생성 기능은 현재 임시 비활성화되어 있습니다.");
            break;
        }
        CurrentMode = ERuntimeToolMode::PlaceEmptyObject;
        LastSaveMessage = TEXT("오브젝트 만들기: 중앙 십자가 위치에 프리뷰가 표시됩니다. 좌클릭 또는 우클릭=새 오브젝트/기존 메시 편집");
        break;
    case ERuntimeToolbarItemKind::Prefab:
        if (Item.bAvailable && PrefabFiles.IsValidIndex(Item.SourceIndex))
        {
            CurrentPrefabIndex = Item.SourceIndex;
        }
        CurrentMode = ERuntimeToolMode::PlacePrefab;
        LastSaveMessage = FString::Printf(TEXT("Prefab 선택: %s"), *GetCurrentPrefabName());
        break;
    case ERuntimeToolbarItemKind::Vehicle:
        CurrentMode = ERuntimeToolMode::PlaceVehicle;
        LastSaveMessage = TEXT("차량 만들기: 중앙 십자가 위치에 좌클릭으로 차량을 설치합니다.");
        break;
    case ERuntimeToolbarItemKind::Weapon:
        if (Item.bAvailable && WeaponFiles.IsValidIndex(Item.SourceIndex))
        {
            CurrentWeaponIndex = Item.SourceIndex;
        }
        EquipCurrentWeapon();
        if (bBroadcastChange)
        {
            NotifyToolbarChanged();
        }
        return;
    case ERuntimeToolbarItemKind::None:
    default:
        CurrentMode = ERuntimeToolMode::None;
        LastSaveMessage = TEXT("툴바 슬롯이 비어 있습니다.");
        break;
    }

    if (bBroadcastChange)
    {
        NotifyToolbarChanged();
    }
    NotifyRuntimeStateChanged();
}

ARuntimeGameplayManager* ARuntimeGameplayManager::FindRuntimeGameplayManager(const UObject* WorldContextObject)
{
    if (!WorldContextObject)
    {
        return nullptr;
    }

    UWorld* World = WorldContextObject->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    ARuntimeGameplayManager* FirstNativeManager = nullptr;
    for (TActorIterator<ARuntimeGameplayManager> It(World); It; ++It)
    {
        ARuntimeGameplayManager* Manager = *It;
        if (!IsValid(Manager))
        {
            continue;
        }

        if (Manager->GetClass() != ARuntimeGameplayManager::StaticClass())
        {
            return Manager;
        }

        if (!FirstNativeManager)
        {
            FirstNativeManager = Manager;
        }
    }

    return FirstNativeManager;
}

FVector ARuntimeGameplayManager::GetPendingPlacementSelection() const
{
    if (bHasPendingVertexLocation)
    {
        return PendingVertexLocation;
    }

    if (bHasPendingEmptyObjectLocation)
    {
        return PendingEmptyObjectLocation;
    }

    return LastPreviewLocation;
}

void ARuntimeGameplayManager::ClearPendingPlacementSelection()
{
    bHasPendingEmptyObjectLocation = false;
    bHasPendingVertexLocation = false;
    PendingEmptyObjectLocation = FVector::ZeroVector;
    PendingVertexLocation = FVector::ZeroVector;
    if (IsValid(CurrentEditableActor))
    {
        CurrentEditableActor->ClearPreviewVertex();
    }
}

AActor* ARuntimeGameplayManager::GetCrosshairHitActor() const
{
    return bLastTraceBlockingHit ? LastTraceHit.GetActor() : nullptr;
}

FString ARuntimeGameplayManager::GetCurrentPrefabName() const
{
    return PrefabFiles.IsValidIndex(CurrentPrefabIndex) ? FPaths::GetBaseFilename(PrefabFiles[CurrentPrefabIndex]) : TEXT("없음");
}

FString ARuntimeGameplayManager::GetCurrentWeaponName() const
{
    return WeaponFiles.IsValidIndex(CurrentWeaponIndex) ? FPaths::GetBaseFilename(WeaponFiles[CurrentWeaponIndex]) : TEXT("없음");
}

void ARuntimeGameplayManager::SelectPreviousPrefab()
{
    ScanRuntimeFolders();
    BuildAvailableItems();
    if (PrefabFiles.Num() > 0)
    {
        CurrentPrefabIndex = (CurrentPrefabIndex - 1 + PrefabFiles.Num()) % PrefabFiles.Num();
        LastSaveMessage = FString::Printf(TEXT("Prefab 선택: %s"), *GetCurrentPrefabName());
    }
    else
    {
        LastSaveMessage = TEXT("prefab/ 폴더에 gltf 또는 glb가 없습니다.");
    }
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::SelectNextPrefab()
{
    ScanRuntimeFolders();
    BuildAvailableItems();
    if (PrefabFiles.Num() > 0)
    {
        CurrentPrefabIndex = (CurrentPrefabIndex + 1) % PrefabFiles.Num();
        LastSaveMessage = FString::Printf(TEXT("Prefab 선택: %s"), *GetCurrentPrefabName());
    }
    else
    {
        LastSaveMessage = TEXT("prefab/ 폴더에 gltf 또는 glb가 없습니다.");
    }
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::SelectPrefabPlacementTool()
{
    // Direct Blueprint buttons bypass the toolbar path, so close any in-progress mesh edit here as well.
    CloseCurrentEditableMeshForToolChange();

    ScanRuntimeFolders();
    BuildAvailableItems();
    CurrentMode = ERuntimeToolMode::PlacePrefab;
    LastSaveMessage = TEXT("Prefab 도구: 중앙 십자가 위치에 좌클릭으로 현재 Prefab을 설치합니다.");
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::SelectEmptyObjectTool()
{
    if (!bEnableObjectVertexCreation)
    {
        CloseCurrentEditableMeshForToolChange();
        DestroyPendingEmptyObjectPreview();
        CurrentMode = ERuntimeToolMode::None;
        LastSaveMessage = TEXT("오브젝트 정점 생성 기능은 현재 임시 비활성화되어 있습니다.");
        NotifyRuntimeStateChanged();
        return;
    }

    // Selecting the object tool explicitly means "leave edit mode and return to placement".
    CloseCurrentEditableMeshForToolChange();

    CurrentMode = ERuntimeToolMode::PlaceEmptyObject;
    LastSaveMessage = TEXT("오브젝트 만들기 도구: 좌클릭으로 새 오브젝트를 만들고 자동으로 정점 편집에 들어갑니다.");
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::SelectVertexTool()
{
    if (!bEnableObjectVertexCreation)
    {
        LastSaveMessage = TEXT("오브젝트 정점 생성 기능은 현재 임시 비활성화되어 있습니다.");
        NotifyRuntimeStateChanged();
        return;
    }

    if (IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized())
    {
        CurrentMode = ERuntimeToolMode::EditVertices;
        LastSaveMessage = TEXT("정점 편집: 중앙 십자가 위치에 정점 프리뷰가 표시됩니다. 좌클릭=정점 추가/선택 정점 이동, 우클릭=완료");
    }
    else
    {
        LastSaveMessage = TEXT("편집 중인 오브젝트가 없습니다. 오브젝트 만들기 아이템을 선택하고 좌클릭하세요.");
    }
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::SelectVehicleTool()
{
    // Direct Blueprint buttons should not leave a half-edited mesh actor owning the runtime cursor.
    CloseCurrentEditableMeshForToolChange();

    CurrentMode = ERuntimeToolMode::PlaceVehicle;
    LastSaveMessage = TEXT("차량 도구: 중앙 십자가 위치에 좌클릭으로 차량을 설치합니다.");
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::SelectPreviousWeapon()
{
    ScanRuntimeFolders();
    BuildAvailableItems();
    if (WeaponFiles.Num() > 0)
    {
        CurrentWeaponIndex = (CurrentWeaponIndex - 1 + WeaponFiles.Num()) % WeaponFiles.Num();
        LastSaveMessage = FString::Printf(TEXT("무기 선택: %s"), *GetCurrentWeaponName());
    }
    else
    {
        LastSaveMessage = TEXT("items/ 폴더에 gltf 또는 glb 무기가 없습니다.");
    }
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::SelectNextWeapon()
{
    ScanRuntimeFolders();
    BuildAvailableItems();
    if (WeaponFiles.Num() > 0)
    {
        CurrentWeaponIndex = (CurrentWeaponIndex + 1) % WeaponFiles.Num();
        LastSaveMessage = FString::Printf(TEXT("무기 선택: %s"), *GetCurrentWeaponName());
    }
    else
    {
        LastSaveMessage = TEXT("items/ 폴더에 gltf 또는 glb 무기가 없습니다.");
    }
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::EquipCurrentWeapon()
{
    ScanRuntimeFolders();
    if (!WeaponFiles.IsValidIndex(CurrentWeaponIndex))
    {
        LastSaveMessage = TEXT("items/ 폴더에 gltf 또는 glb 무기가 없습니다.");
        NotifyRuntimeStateChanged();
        return;
    }

    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (!IsValid(PC))
    {
        LastSaveMessage = TEXT("PlayerController를 찾을 수 없습니다.");
        NotifyRuntimeStateChanged();
        return;
    }

    if (IsValid(EquippedWeapon))
    {
        EquippedWeapon->Destroy();
        EquippedWeapon = nullptr;
    }

    UCameraComponent* Camera = nullptr;
    if (UGameManagerSubSystem* SubSystem = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        Camera = SubSystem->GetCameraComponent<UCameraComponent>();
    }

    FActorSpawnParameters Params;
    Params.Owner = this;
    UClass* WeaponSpawnClass = WeaponActorClass ? WeaponActorClass.Get() : ARuntimeWeaponActor::StaticClass();
    ARuntimeWeaponActor* Weapon = GetWorld()->SpawnActor<ARuntimeWeaponActor>(WeaponSpawnClass, FTransform::Identity, Params);
    if (IsValid(Weapon) && Weapon->EquipFromFile(WeaponFiles[CurrentWeaponIndex], Camera))
    {
        EquippedWeapon = Weapon;
        CurrentMode = ERuntimeToolMode::Weapon;
        LastSaveMessage = FString::Printf(TEXT("무기 장착: %s"), *GetCurrentWeaponName());
    }
    else if (IsValid(Weapon))
    {
        Weapon->Destroy();
        LastSaveMessage = TEXT("무기 로드 실패");
    }
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::ToggleSnap()
{
    bSnapToGrid = !bSnapToGrid;
    LastSaveMessage = bSnapToGrid ? TEXT("Grid Snap 켜짐") : TEXT("Grid Snap 꺼짐");
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::SetSnapEnabled(bool bEnabled)
{
    bSnapToGrid = bEnabled;
    LastSaveMessage = bSnapToGrid ? TEXT("Grid Snap 켜짐") : TEXT("Grid Snap 꺼짐");
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::SetGridSize(float NewGridSize)
{
    GridSize = FMath::Max(1.0f, NewGridSize);
    LastSaveMessage = FString::Printf(TEXT("Grid Size: %.0f cm"), GridSize);
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::ToggleFirstPerson()
{
    bFirstPerson = !bFirstPerson;
    if (UGameManagerSubSystem* SubSystem = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        if (ACharacterController* Character = SubSystem->GetPlayerActor<ACharacterController>())
        {
            Character->SetFirstPersonEnabled(bFirstPerson);
        }
    }
    LastSaveMessage = bFirstPerson ? TEXT("1인칭 모드 켜짐") : TEXT("1인칭 모드 꺼짐");
    NotifyRuntimeStateChanged();
}

bool ARuntimeGameplayManager::TracePlacementLocation(FVector& OutLocation, FHitResult& OutHit)
{
    // Resolve the world and the local player controller because the cursor ray must come from the active camera.
    UWorld* World = GetWorld();
    APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
    if (!IsValid(World) || !IsValid(PC))
    {
        // Fall back to the manager actor location only so callers never read an uninitialized vector.
        OutLocation = GetActorLocation();
        // Reset the cached ray to a safe default when there is no player camera.
        LastTraceStart = GetActorLocation();
        LastTraceDirection = FVector::ForwardVector;
        // Clear the hit result because no collision query was actually possible.
        OutHit = FHitResult();
        LastTraceHit = FHitResult();
        // Mark both collision and placement as unavailable in this exceptional case.
        bLastTraceBlockingHit = false;
        bLastTraceHasPlacementLocation = false;
        bLastTraceUsedFreeSpace = false;
        return false;
    }

    // Build query parameters for the short collision probe under the center crosshair.
    FCollisionQueryParams Params(SCENE_QUERY_STAT(RuntimePlacementTrace), true, this);
    // Ignore the controlled pawn so first-person cameras do not immediately hit the player capsule.
    if (APawn* Pawn = PC->GetPawn())
    {
        Params.AddIgnoredActor(Pawn);
    }
    // Ignore the equipped weapon so firing/held weapon meshes do not block placement directly in front of the camera.
    if (IsValid(EquippedWeapon))
    {
        Params.AddIgnoredActor(EquippedWeapon);
    }
    // Ignore the translucent object-creation preview so the cursor can move through its own preview mesh.
    if (IsValid(PendingEmptyObjectPreviewActor))
    {
        Params.AddIgnoredActor(PendingEmptyObjectPreviewActor);
    }

    // Start with a harmless fallback ray; it is overwritten by deprojection or camera viewpoint below.
    FVector Start = FVector::ZeroVector;
    FVector Direction = FVector::ForwardVector;
    // Query the viewport size so the center-crosshair ray is independent from the OS mouse cursor position.
    int32 ViewportX = 0;
    int32 ViewportY = 0;
    PC->GetViewportSize(ViewportX, ViewportY);
    if (ViewportX > 0 && ViewportY > 0)
    {
        // Deproject the exact center of the viewport, matching the Minecraft-like crosshair interaction model.
        PC->DeprojectScreenPositionToWorld(static_cast<float>(ViewportX) * 0.5f, static_cast<float>(ViewportY) * 0.5f, Start, Direction);
    }
    else
    {
        // Dedicated/serverless or unusual viewport states can still use the camera viewpoint as a ray source.
        FRotator ViewRotation;
        PC->GetPlayerViewPoint(Start, ViewRotation);
        Direction = ViewRotation.Vector();
    }

    // Normalize the ray direction before multiplying it by centimeter distances.
    Direction = Direction.GetSafeNormal();
    if (Direction.IsNearlyZero())
    {
        // Avoid NaNs and zero-length line traces if the camera returned an invalid direction.
        Direction = FVector::ForwardVector;
    }

    // Cache the raw ray for vertex-nearest-to-ray selection, even when no collision is hit.
    LastTraceStart = Start;
    LastTraceDirection = Direction;
    // Clear last-frame collision state before evaluating the new frame.
    OutHit = FHitResult();
    LastTraceHit = FHitResult();
    bLastTraceBlockingHit = false;
    bLastTraceHasPlacementLocation = false;
    bLastTraceUsedFreeSpace = false;

    // Clamp the free-space point to a hard maximum distance from the camera; 1000 cm equals 10 meters.
    const float SafeFreeSpaceDistance = FMath::Max(1.0f, FreeSpacePlacementDistance);
    // Collision probing is intentionally short and is never allowed to exceed the free-space placement cap.
    const float SafeCollisionDistance = FMath::Clamp(CrosshairCollisionTraceDistance, 0.0f, SafeFreeSpaceDistance);

    // Only perform the expensive/physical blocking test inside the configured short range.
    bool bHit = false;
    if (SafeCollisionDistance > KINDA_SMALL_NUMBER)
    {
        // Anything beyond this end point is treated as empty air, even if a far-away wall exists behind it.
        const FVector CollisionEnd = Start + Direction * SafeCollisionDistance;
        bHit = World->LineTraceSingleByChannel(OutHit, Start, CollisionEnd, ECC_Visibility, Params);
    }

    if (bHit)
    {
        // A physical hit wins over free-space placement and places slightly above the blocking surface.
        const FVector SurfaceOffset = OutHit.ImpactNormal.GetSafeNormal() * SurfacePlacementOffset;
        OutLocation = OutHit.ImpactPoint + SurfaceOffset;
        // Cache the hit so Blueprint UI and existing-mesh editing can identify the object under the crosshair.
        LastTraceHit = OutHit;
        bLastTraceBlockingHit = true;
        bLastTraceHasPlacementLocation = true;
        bLastTraceUsedFreeSpace = false;
        return true;
    }

    // No nearby blocking object was hit; resolve the cursor to a free-space point if air placement is enabled.
    OutLocation = Start + Direction * SafeFreeSpaceDistance;
    // Keep LastTraceHit empty because there is no actor/component under the cursor in air placement mode.
    LastTraceHit = FHitResult();
    bLastTraceBlockingHit = false;
    bLastTraceUsedFreeSpace = true;
    bLastTraceHasPlacementLocation = bAllowFreeSpacePlacement;

    // Returning true means callers may create objects/vertices at the air point.
    return bLastTraceHasPlacementLocation;
}

FVector ARuntimeGameplayManager::ApplyGridSnap(const FVector& Location) const
{
    if (!bSnapToGrid || GridSize <= KINDA_SMALL_NUMBER)
    {
        return Location;
    }
    return FVector(
        FMath::GridSnap(Location.X, static_cast<double>(GridSize)),
        FMath::GridSnap(Location.Y, static_cast<double>(GridSize)),
        FMath::GridSnap(Location.Z, static_cast<double>(GridSize)));
}

bool ARuntimeGameplayManager::ShouldShowPlacementGrid() const
{
    if (RuntimePlayMode != ERuntimePlayMode::Creator || !bLastTraceHasPlacementLocation)
    {
        return false;
    }

    const FRuntimeToolbarItem SelectedItem = GetSelectedToolbarItem();
    if (CurrentMode == ERuntimeToolMode::PlaceVehicle || SelectedItem.Kind == ERuntimeToolbarItemKind::Vehicle)
    {
        return false;
    }

    if (CurrentMode == ERuntimeToolMode::PlacePrefab)
    {
        return true;
    }

    if (bEnableObjectVertexCreation && (CurrentMode == ERuntimeToolMode::PlaceEmptyObject || CurrentMode == ERuntimeToolMode::EditVertices))
    {
        return true;
    }

    return IsObjectCreationItem(SelectedItem);
}

void ARuntimeGameplayManager::UpdatePlacementGrid()
{
    if (!IsValid(PlacementGridComponent))
    {
        return;
    }

    if (!ShouldShowPlacementGrid())
    {
        ClearPlacementGridMesh();
        return;
    }

    const float Spacing = FMath::Max(1.0f, PlacementGridSpacing);
    // Keep the placement guide extremely small: three 1m cells from the cursor, axes only.
    const float Radius = Spacing * 3.0f;
    const FVector Center(
        FMath::GridSnap(LastPreviewLocation.X, static_cast<double>(Spacing)),
        FMath::GridSnap(LastPreviewLocation.Y, static_cast<double>(Spacing)),
        FMath::GridSnap(LastPreviewLocation.Z, static_cast<double>(Spacing)));

    const float RebuildMoveThreshold = Spacing * 0.5f;
    const bool bNeedsRebuild = !bPlacementGridBuilt
        || FVector::DistSquared(Center, CachedPlacementGridCenter) > FMath::Square(RebuildMoveThreshold)
        || !FMath::IsNearlyEqual(Radius, CachedPlacementGridRadius, 1.0f);

    if (bNeedsRebuild)
    {
        RebuildPlacementGridMesh(Center, Radius);
    }

    PlacementGridComponent->SetHiddenInGame(false);
    PlacementGridComponent->SetVisibility(true, true);
}

void ARuntimeGameplayManager::ClearPlacementGridMesh()
{
    if (IsValid(PlacementGridComponent))
    {
        PlacementGridComponent->ClearAllMeshSections();
        PlacementGridComponent->SetHiddenInGame(true);
        PlacementGridComponent->SetVisibility(false, true);
    }

    bPlacementGridBuilt = false;
    CachedPlacementGridCenter = FVector::ZeroVector;
    CachedPlacementGridRadius = 0.0f;
}

static void AppendPlacementGridLine(const FVector& Start, const FVector& End, float Thickness, const FColor& LineColor, TArray<FVector>& Vertices, TArray<int32>& Triangles, TArray<FVector>& Normals, TArray<FVector2D>& UV0, TArray<FColor>& VertexColors, TArray<FProcMeshTangent>& Tangents)
{
    const FVector Axis = End - Start;
    const float Length = Axis.Size();
    if (Length <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    const FVector Direction = Axis / Length;
    const FVector Reference = FMath::Abs(Direction.Z) < 0.95f ? FVector::UpVector : FVector::RightVector;
    const FVector Side = FVector::CrossProduct(Direction, Reference).GetSafeNormal() * Thickness;
    const FVector Up = FVector::CrossProduct(Side.GetSafeNormal(), Direction).GetSafeNormal() * Thickness;

    const int32 BaseIndex = Vertices.Num();
    Vertices.Add(Start - Side - Up);
    Vertices.Add(Start + Side - Up);
    Vertices.Add(Start + Side + Up);
    Vertices.Add(Start - Side + Up);
    Vertices.Add(End - Side - Up);
    Vertices.Add(End + Side - Up);
    Vertices.Add(End + Side + Up);
    Vertices.Add(End - Side + Up);

    const int32 FaceIndices[] =
    {
        0, 1, 5, 0, 5, 4,
        1, 2, 6, 1, 6, 5,
        2, 3, 7, 2, 7, 6,
        3, 0, 4, 3, 4, 7,
        0, 3, 2, 0, 2, 1,
        4, 5, 6, 4, 6, 7
    };
    for (int32 Index : FaceIndices)
    {
        Triangles.Add(BaseIndex + Index);
    }

    for (int32 VertexIndex = 0; VertexIndex < 8; ++VertexIndex)
    {
        Normals.Add(FVector::UpVector);
        UV0.Add(FVector2D::ZeroVector);
        VertexColors.Add(LineColor);
        Tangents.Add(FProcMeshTangent(Direction, false));
    }
}

void ARuntimeGameplayManager::RebuildPlacementGridMesh(const FVector& Center, float Radius)
{
    if (!IsValid(PlacementGridComponent))
    {
        return;
    }

    const float Spacing = FMath::Max(1.0f, PlacementGridSpacing);
    const float BaseThickness = FMath::Max(0.25f, PlacementGridLineThickness * 0.65f);

    // Drastically simplified 3D placement guide: only the three center axes plus short 1m tick marks.
    // This preserves scale/orientation without the visually noisy volumetric/plane grid.
    const int32 FadeCells = 3;
    const float SafeRadius = FMath::Min(FMath::Max(Radius, Spacing), Spacing * static_cast<float>(FadeCells));
    const float FadeRadius = FMath::Max(Spacing, SafeRadius);

    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector> Normals;
    TArray<FVector2D> UV0;
    TArray<FColor> VertexColors;
    TArray<FProcMeshTangent> Tangents;

    constexpr int32 EstimatedLines = 27;
    Vertices.Reserve(EstimatedLines * 8);
    Triangles.Reserve(EstimatedLines * 36);
    Normals.Reserve(EstimatedLines * 8);
    UV0.Reserve(EstimatedLines * 8);
    VertexColors.Reserve(EstimatedLines * 8);
    Tangents.Reserve(EstimatedLines * 8);

    const FTransform ToLocal = PlacementGridComponent->GetComponentTransform().Inverse();
    auto ToLocalPosition = [&ToLocal](const FVector& WorldPosition)
    {
        return ToLocal.TransformPosition(WorldPosition);
    };

    auto AlphaForDistance = [FadeRadius](float Distance)
    {
        const float T = FMath::Clamp(Distance / FMath::Max(1.0f, FadeRadius), 0.0f, 1.0f);
        const float SmoothT = T * T * (3.0f - 2.0f * T);
        return FMath::Clamp(1.0f - SmoothT, 0.0f, 1.0f);
    };

    auto MakeColor = [](float Alpha, bool bAxis)
    {
        const float Brightness = bAxis ? 1.0f : FMath::Square(Alpha);
        FLinearColor LinearColor(
            0.02f + 0.18f * Brightness,
            0.04f + 0.28f * Brightness,
            0.05f + 0.35f * Brightness,
            Alpha);
        FColor Color = LinearColor.ToFColor(true);
        Color.A = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(255.0f * Alpha), 0, 255));
        return Color;
    };

    auto AppendGuideLine = [&](const FVector& WorldStart, const FVector& WorldEnd, float Distance, bool bAxis)
    {
        const float Alpha = bAxis ? 0.95f : AlphaForDistance(Distance) * 0.55f;
        if (Alpha <= 0.05f)
        {
            return;
        }

        const float Thickness = BaseThickness * (bAxis ? 1.3f : FMath::Lerp(0.45f, 0.8f, Alpha));
        AppendPlacementGridLine(ToLocalPosition(WorldStart), ToLocalPosition(WorldEnd), Thickness, MakeColor(Alpha, bAxis), Vertices, Triangles, Normals, UV0, VertexColors, Tangents);
    };

    const FVector AxisX(SafeRadius, 0.0f, 0.0f);
    const FVector AxisY(0.0f, SafeRadius, 0.0f);
    const FVector AxisZ(0.0f, 0.0f, SafeRadius);

    AppendGuideLine(Center - AxisX, Center + AxisX, 0.0f, true);
    AppendGuideLine(Center - AxisY, Center + AxisY, 0.0f, true);
    AppendGuideLine(Center - AxisZ, Center + AxisZ, 0.0f, true);

    const float TickHalfLength = Spacing * 0.075f;
    for (int32 Cell = -FadeCells; Cell <= FadeCells; ++Cell)
    {
        if (Cell == 0)
        {
            continue;
        }

        const float Offset = static_cast<float>(Cell) * Spacing;
        const float Distance = FMath::Abs(Offset);
        if (Distance > SafeRadius + KINDA_SMALL_NUMBER)
        {
            continue;
        }

        const FVector XTickCenter = Center + FVector(Offset, 0.0f, 0.0f);
        const FVector YTickCenter = Center + FVector(0.0f, Offset, 0.0f);
        const FVector ZTickCenter = Center + FVector(0.0f, 0.0f, Offset);

        AppendGuideLine(XTickCenter - FVector(0.0f, TickHalfLength, 0.0f), XTickCenter + FVector(0.0f, TickHalfLength, 0.0f), Distance, false);
        AppendGuideLine(YTickCenter - FVector(TickHalfLength, 0.0f, 0.0f), YTickCenter + FVector(TickHalfLength, 0.0f, 0.0f), Distance, false);
        AppendGuideLine(ZTickCenter - FVector(TickHalfLength, 0.0f, 0.0f), ZTickCenter + FVector(TickHalfLength, 0.0f, 0.0f), Distance, false);
    }

    PlacementGridComponent->ClearAllMeshSections();
    if (Vertices.Num() > 0)
    {
        PlacementGridComponent->CreateMeshSection(0, Vertices, Triangles, Normals, UV0, VertexColors, Tangents, false);
        if (PlacementGridMaterial)
        {
            PlacementGridComponent->SetMaterial(0, PlacementGridMaterial);
        }
    }

    CachedPlacementGridCenter = Center;
    CachedPlacementGridRadius = SafeRadius;
    bPlacementGridBuilt = true;
}


void ARuntimeGameplayManager::AutoSaveRuntimeScene()
{
    if (!bAutoSaveRuntimeScene || bIsSavingRuntimeScene)
    {
        return;
    }

    SaveRuntimeScene();
}

int32 ARuntimeGameplayManager::CountExistingBaseName(const FString& BaseName, ERuntimePlacedObjectKind Kind) const
{
    int32 Count = 0;
    if (Kind == ERuntimePlacedObjectKind::Prefab)
    {
        for (const ARuntimePrefabActor* Prefab : SpawnedPrefabs)
        {
            if (IsValid(Prefab) && Prefab->GetBaseName().Equals(BaseName, ESearchCase::IgnoreCase))
            {
                Count++;
            }
        }
    }
    else if (Kind == ERuntimePlacedObjectKind::GeneratedMesh)
    {
        for (const ARuntimeEditableMeshActor* Mesh : SpawnedGeneratedMeshes)
        {
            if (IsValid(Mesh) && Mesh->GetRuntimeName().StartsWith(BaseName))
            {
                Count++;
            }
        }
        if (IsValid(CurrentEditableActor) && CurrentEditableActor->GetRuntimeName().StartsWith(BaseName))
        {
            Count++;
        }
    }
    else if (Kind == ERuntimePlacedObjectKind::Vehicle)
    {
        for (const ARuntimeVehiclePawn* Vehicle : SpawnedVehicles)
        {
            if (IsValid(Vehicle))
            {
                Count++;
            }
        }
    }
    return Count;
}

FString ARuntimeGameplayManager::MakeRuntimeObjectName(const FString& BaseName, ERuntimePlacedObjectKind Kind) const
{
    const FString SafeBaseName = BaseName.IsEmpty() ? TEXT("RuntimeEntity") : BaseName;
    const int32 ExistingCount = CountExistingBaseName(SafeBaseName, Kind);
    if (ExistingCount <= 0)
    {
        return SafeBaseName;
    }
    if (ExistingCount == 1)
    {
        return SafeBaseName + TEXT(";INST");
    }
    return FString::Printf(TEXT("%s;INST_%d"), *SafeBaseName, ExistingCount);
}
void ARuntimeGameplayManager::InputPrimaryAction()
{
    InputPrimaryPressed();
    InputPrimaryReleased();
}

void ARuntimeGameplayManager::InputPrimaryPressed()
{
    if (bItemListWindowOpen)
    {
        return;
    }

    FHitResult Hit;
    FVector Location;
    const bool bHasPlacementLocation = TracePlacementLocation(Location, Hit);
    const FRuntimeToolbarItem Item = GetSelectedToolbarItem();
    const bool bVehiclePlacementRequest = CurrentMode != ERuntimeToolMode::EditVertices
        && (CurrentMode == ERuntimeToolMode::PlaceVehicle || Item.Kind == ERuntimeToolbarItemKind::Vehicle);
    if (!bVehiclePlacementRequest)
    {
        Location = ApplyGridSnap(Location);
    }

    if (CurrentMode == ERuntimeToolMode::EditVertices && IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized())
    {
        if (HighlightedEditableVertexIndex != INDEX_NONE)
        {
            BeginEditableVertexPrimaryPress(HighlightedEditableVertexIndex);
        }
        else
        {
            if (!bHasPlacementLocation)
            {
                LastSaveMessage = TEXT("정점을 찍을 중앙 십자가 위치를 계산할 수 없습니다.");
                NotifyRuntimeStateChanged();
                return;
            }
            AddVertexToEditableObject(Location);
        }
        NotifyRuntimeStateChanged();
        return;
    }

    switch (Item.Kind)
    {
    case ERuntimeToolbarItemKind::CreateObject:
        // Object-creation item activation is shared by LMB and RMB so either click enters vertex edit in one action.
        TryUseObjectCreationItemAtCrosshair();
        break;
    case ERuntimeToolbarItemKind::Prefab:
        if (bHasPlacementLocation)
        {
            PlaceCurrentPrefab(Location);
        }
        else
        {
            LastSaveMessage = TEXT("Prefab을 설치할 중앙 십자가 위치를 계산할 수 없습니다.");
        }
        break;
    case ERuntimeToolbarItemKind::Vehicle:
        if (bHasPlacementLocation)
        {
            PlaceVehicle(Location, Item.SourcePath);
        }
        else
        {
            LastSaveMessage = TEXT("차량을 설치할 중앙 십자가 위치를 계산할 수 없습니다.");
        }
        break;
    case ERuntimeToolbarItemKind::Weapon:
        if (IsValid(EquippedWeapon))
        {
            APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
            EquippedWeapon->Fire(PC);
        }
        else
        {
            EquipCurrentWeapon();
        }
        break;
    case ERuntimeToolbarItemKind::None:
    default:
        if (CurrentMode == ERuntimeToolMode::PlacePrefab)
        {
            if (bHasPlacementLocation)
            {
                PlaceCurrentPrefab(Location);
            }
            else
            {
                LastSaveMessage = TEXT("Prefab을 설치할 중앙 십자가 위치를 계산할 수 없습니다.");
            }
        }
        else if (CurrentMode == ERuntimeToolMode::PlaceVehicle)
        {
            if (bHasPlacementLocation)
            {
                PlaceVehicle(Location, GetSelectedToolbarItem().SourcePath);
            }
            else
            {
                LastSaveMessage = TEXT("차량을 설치할 중앙 십자가 위치를 계산할 수 없습니다.");
            }
        }
        else if (IsValid(EquippedWeapon))
        {
            APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
            EquippedWeapon->Fire(PC);
        }
        break;
    }

    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::InputPrimaryReleased()
{
    if (CurrentMode == ERuntimeToolMode::EditVertices && bPrimaryVertexPressActive)
    {
        EndEditableVertexPrimaryPress();
        NotifyRuntimeStateChanged();
    }
}

void ARuntimeGameplayManager::InputSecondaryAction()
{
    // Do not let gameplay clicks leak through while the Blueprint item list is open.
    if (bItemListWindowOpen)
    {
        return;
    }

    // If a live editable mesh exists, RMB is always the one-click finish/cancel command, even if a stale mode flag says otherwise.
    if (IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized())
    {
        FinishOrCancelCurrentVertexEditing();
        NotifyRuntimeStateChanged();
        return;
    }

    // In object-placement mode, RMB now mirrors LMB so placing the object and entering vertex mode takes exactly one click.
    if (CurrentMode == ERuntimeToolMode::PlaceEmptyObject && IsSelectedToolbarItemObjectCreation())
    {
        TryUseObjectCreationItemAtCrosshair();
        NotifyRuntimeStateChanged();
        return;
    }

    // Other tools currently do not use secondary input, but still broadcast so UI status text can refresh consistently.
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::InputInteractAction()
{
    TryEnterOrExitVehicle();
}

void ARuntimeGameplayManager::InputToggleFirstPersonAction()
{
    ToggleFirstPerson();
}

void ARuntimeGameplayManager::InputToolbarScrollAction(float ScrollValue)
{
    ScrollToolbarSelection(ScrollValue);
}

void ARuntimeGameplayManager::InputToggleItemListAction()
{
    ToggleItemListWindow();
}

void ARuntimeGameplayManager::InputToggleSnapModeAction()
{
    ToggleSnap();
}

void ARuntimeGameplayManager::InputVehicleMoveAction(const FVector2D& MoveValue)
{
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (ARuntimeVehiclePawn* Vehicle = PC ? Cast<ARuntimeVehiclePawn>(PC->GetPawn()) : nullptr)
    {
        Vehicle->SetDriveInput(MoveValue.Y, MoveValue.X);
    }
}

void ARuntimeGameplayManager::InputVehicleThrottleAction(float Throttle)
{
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (ARuntimeVehiclePawn* Vehicle = PC ? Cast<ARuntimeVehiclePawn>(PC->GetPawn()) : nullptr)
    {
        Vehicle->SetThrottleInput(Throttle);
    }
}

void ARuntimeGameplayManager::InputVehicleSteeringAction(float Steering)
{
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (ARuntimeVehiclePawn* Vehicle = PC ? Cast<ARuntimeVehiclePawn>(PC->GetPawn()) : nullptr)
    {
        Vehicle->SetSteeringInput(Steering);
    }
}

void ARuntimeGameplayManager::SelectCurrentTraceLocation()
{
    InputPrimaryAction();
}

void ARuntimeGameplayManager::ConfirmCurrentPendingLocation()
{
    InputSecondaryAction();
}

bool ARuntimeGameplayManager::TryUseObjectCreationItemAtCrosshair()
{
    if (!bEnableObjectVertexCreation)
    {
        DestroyPendingEmptyObjectPreview();
        CurrentMode = ERuntimeToolMode::None;
        LastSaveMessage = TEXT("오브젝트 정점 생성 기능은 현재 임시 비활성화되어 있습니다.");
        return false;
    }

    // Compute the current center-crosshair placement point at the exact moment the click happens.
    FHitResult Hit;

    // This location can be either a short collision hit surface or the configured free-space point.
    FVector Location = FVector::ZeroVector;

    // TracePlacementLocation also refreshes LastTraceHit, LastTraceStart, and LastTraceDirection for selection feedback.
    const bool bHasPlacementLocation = TracePlacementLocation(Location, Hit);

    // Grid snapping applies equally to surface placement and air placement.
    Location = ApplyGridSnap(Location);

    // Looking at an existing finalized runtime mesh with the object item means "edit this mesh" instead of "spawn a new one".
    if (ARuntimeEditableMeshActor* ExistingMesh = GetEditableMeshFromHit(Hit))
    {
        // BeginEditingExistingMesh stores a rollback copy, removes the mesh from the finalized list, and switches to EditVertices.
        BeginEditingExistingMesh(ExistingMesh);

        // The click was consumed successfully.
        return true;
    }

    // If neither a surface nor a free-space point exists, no object can be created safely.
    if (!bHasPlacementLocation)
    {
        // Keep this message Blueprint-readable for the custom toolbar/status widget.
        LastSaveMessage = TEXT("오브젝트를 만들 중앙 십자가 위치를 계산할 수 없습니다.");

        // Nothing was placed.
        return false;
    }

    // Reuse the current translucent object-creation preview actor as the real editable object when possible.
    ARuntimeEditableMeshActor* PreviewActor = PendingEmptyObjectPreviewActor.Get();

    // Detach the pointer before PlaceEmptyObject converts/destroys/owns the actor so the cursor preview cannot become a stale ghost.
    PendingEmptyObjectPreviewActor = nullptr;

    // Spawn or convert the object and make it the active editable mesh.
    PlaceEmptyObject(Location, PreviewActor);

    // Force the expected one-click state: after object creation, the next click must add/edit vertices immediately.
    if (IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized())
    {
        // The mode assignment is duplicated intentionally to protect against stale mode changes caused by UI event order.
        CurrentMode = ERuntimeToolMode::EditVertices;

        // Keep the cached preview location in sync with the spawn location so the first vertex preview appears immediately.
        LastPreviewLocation = Location;

        // Make the newly active actor show the center-crosshair vertex preview without waiting for a second click/tick.
        UpdateEditableVertexPreviewAndSelection();

        // Tell UI code that the one-click transition succeeded.
        LastSaveMessage += TEXT(" 정점 편집 모드로 즉시 전환되었습니다.");

        // Successfully entered edit mode.
        return true;
    }

    // PlaceEmptyObject failed to create a valid actor.
    return false;
}

bool ARuntimeGameplayManager::CloseCurrentEditableMeshForToolChange()
{
    // No live actor means there is nothing for the mode change to resolve.
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized())
    {
        // Clear stale click/drag flags anyway so a previous input cannot affect the next tool.
        ClearEditableVertexMoveState();

        // The toolbar switch may proceed normally.
        return true;
    }

    // FinishCurrentEditableMesh finalizes valid meshes and cancels/reverts invalid or one/two-vertex edits.
    const bool bClosedAsValidMesh = FinishCurrentEditableMesh();

    // Always clear press/drag/source flags after a tool switch so the cursor cannot remain attached to an old edit actor.
    ClearEditableVertexMoveState();

    // Return whether the edit became a valid finalized mesh; callers currently use the cleanup side effect either way.
    return bClosedAsValidMesh;
}

void ARuntimeGameplayManager::PlaceCurrentPrefab(const FVector& Location)
{
    ScanRuntimeFolders();
    if (!PrefabFiles.IsValidIndex(CurrentPrefabIndex))
    {
        LastSaveMessage = TEXT("prefab/ 폴더에 gltf 또는 glb가 없습니다.");
        return;
    }

    const FString SourceFile = PrefabFiles[CurrentPrefabIndex];
    const FString BaseName = FPaths::GetBaseFilename(SourceFile);
    const FString RuntimeName = MakeRuntimeObjectName(BaseName, ERuntimePlacedObjectKind::Prefab);

    FActorSpawnParameters Params;
    Params.Owner = this;
    const FRotator SpawnRot = FRotator(0.0f, GetWorld()->GetFirstPlayerController() ? GetWorld()->GetFirstPlayerController()->GetControlRotation().Yaw : 0.0f, 0.0f);
    UClass* PrefabSpawnClass = PrefabActorClass ? PrefabActorClass.Get() : ARuntimePrefabActor::StaticClass();
    ARuntimePrefabActor* Actor = GetWorld()->SpawnActor<ARuntimePrefabActor>(PrefabSpawnClass, FTransform(SpawnRot, Location), Params);
    if (IsValid(Actor) && Actor->LoadPrefab(SourceFile, RuntimeName))
    {
        SpawnedPrefabs.Add(Actor);
        LastSaveMessage = FString::Printf(TEXT("설치됨: %s"), *RuntimeName);
    }
    else if (IsValid(Actor))
    {
        Actor->Destroy();
        LastSaveMessage = TEXT("Prefab 로드 실패");
    }
}

void ARuntimeGameplayManager::PlaceEmptyObject(const FVector& Location, ARuntimeEditableMeshActor* ExistingPreviewActor)
{
    if (IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized())
    {
        LastSaveMessage = TEXT("이미 편집 중인 오브젝트가 있습니다. 우클릭으로 먼저 편집을 끝내세요.");
        return;
    }

    const FString RuntimeName = MakeRuntimeObjectName(TEXT("GeneratedMesh"), ERuntimePlacedObjectKind::GeneratedMesh);
    ARuntimeEditableMeshActor* Actor = ExistingPreviewActor;
    if (!IsValid(Actor))
    {
        FActorSpawnParameters Params;
        Params.Owner = this;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        UClass* EditableSpawnClass = EditableMeshActorClass ? EditableMeshActorClass.Get() : ARuntimeEditableMeshActor::StaticClass();
        Actor = GetWorld()->SpawnActor<ARuntimeEditableMeshActor>(EditableSpawnClass, FTransform(Location), Params);
    }
    else
    {
        Actor->SetActorLocation(Location);
    }

    if (IsValid(Actor))
    {
        Actor->BeginObject(RuntimeName);
        Actor->SetActorHiddenInGame(false);
        Actor->SetActorEnableCollision(true);
        CurrentEditableActor = Actor;
        CurrentMode = ERuntimeToolMode::EditVertices;
        bCurrentEditableActorWasExisting = false;
        bHasOriginalEditableMeshRecord = false;
        ClearEditableVertexMoveState();
        // Make the actor display a first candidate vertex at the object spawn/crosshair point immediately.
        Actor->SetPreviewVertexWorld(Location);
        LastSaveMessage = FString::Printf(TEXT("오브젝트 생성: %s. 빈 공간 좌클릭=정점 추가, 기존 정점 짧은 클릭=연결 정점 생성 모드, 기존 정점 누른 채 이동=위치 편집, 우클릭=완료/취소"), *RuntimeName);
    }
}

ARuntimeEditableMeshActor* ARuntimeGameplayManager::GetEditableMeshFromHit(const FHitResult& Hit) const
{
    ARuntimeEditableMeshActor* MeshActor = Cast<ARuntimeEditableMeshActor>(Hit.GetActor());
    if (!IsValid(MeshActor) && Hit.GetComponent())
    {
        MeshActor = Cast<ARuntimeEditableMeshActor>(Hit.GetComponent()->GetOwner());
    }

    if (IsValid(MeshActor) && MeshActor != PendingEmptyObjectPreviewActor.Get() && MeshActor->IsFinalized())
    {
        return MeshActor;
    }
    return nullptr;
}

void ARuntimeGameplayManager::BeginEditingExistingMesh(ARuntimeEditableMeshActor* MeshActor)
{
    if (!IsValid(MeshActor))
    {
        return;
    }

    DestroyPendingEmptyObjectPreview();
    if (IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized() && CurrentEditableActor != MeshActor)
    {
        FinishOrCancelCurrentVertexEditing();
    }

    CurrentEditableActor = MeshActor;
    OriginalEditableMeshRecord = MeshActor->ToGeneratedMeshRecord();
    bHasOriginalEditableMeshRecord = true;
    bCurrentEditableActorWasExisting = true;
    SpawnedGeneratedMeshes.Remove(MeshActor);
    MeshActor->BeginEditingExistingObject();
    CurrentMode = ERuntimeToolMode::EditVertices;
    ClearEditableVertexMoveState();
    LastSaveMessage = FString::Printf(TEXT("기존 메시 편집 시작: %s. 빈 공간 좌클릭=정점 추가, 기존 정점 짧은 클릭=연결 정점 생성 모드, 기존 정점 누른 채 이동=위치 편집, 우클릭=완료"), *MeshActor->GetRuntimeName());
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::AddVertexToEditableObject(const FVector& Location)
{
    // Guard against clicks when no runtime mesh is open for editing.
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized())
    {
        // Store a BP-readable message instead of silently ignoring the click.
        LastSaveMessage = TEXT("편집할 오브젝트가 없습니다.");

        // Stop because there is no actor that can receive a new vertex.
        return;
    }

    // If gameplay state has a connected source but the actor does not, resync the actor before adding the next vertex.
    if (ConnectedEditableVertexSourceIndex != INDEX_NONE && CurrentEditableActor->GetConnectedVertexSourceIndex() != ConnectedEditableVertexSourceIndex)
    {
        // Ask the actor to seed its active polygon chain from the selected source vertex.
        if (!CurrentEditableActor->StartConnectedVertexFromIndex(ConnectedEditableVertexSourceIndex))
        {
            // Drop stale source indices that no longer exist on the actor.
            ConnectedEditableVertexSourceIndex = INDEX_NONE;
        }
    }

    // Remember the source before adding, so the user message can say what this segment extended from.
    const int32 PreviousConnectedSourceIndex = ConnectedEditableVertexSourceIndex;

    // Add a brand-new vertex at the center-crosshair hit location and let the actor extend the current n-gon.
    const int32 NewIndex = CurrentEditableActor->AddVertexWorld(Location);

    // Pull the actor's new source index back into manager state; this normally becomes the newly added vertex.
    ConnectedEditableVertexSourceIndex = CurrentEditableActor->GetConnectedVertexSourceIndex();

    // Highlight the new point so feedback follows the actual chain end.
    HighlightedEditableVertexIndex = NewIndex;

    // Adding a vertex is not a drag operation.
    bMovingHighlightedEditableVertex = false;

    // A committed click ends any click-vs-hold classification that may have been active.
    bPrimaryVertexPressActive = false;

    // A committed click also cancels drag mode.
    bPrimaryVertexDragActive = false;

    // No vertex is currently being pressed after a committed add.
    PressedEditableVertexIndex = INDEX_NONE;

    // Read topology after the actor has rebuilt its fan triangles.
    const bool bTopologyValid = CurrentEditableActor->IsTopologyValid();

    // Connected-source messages are more useful when the user is extending from an existing point.
    if (PreviousConnectedSourceIndex != INDEX_NONE)
    {
        // Tell the user that the new point continued the chain from a specific source vertex.
        LastSaveMessage = FString::Printf(TEXT("정점 %d 추가: 정점 %d에서 이어지는 n-gon 선분입니다. 현재 위상: %s."), NewIndex, PreviousConnectedSourceIndex, bTopologyValid ? TEXT("정상(초록)") : TEXT("미완성/이상(빨강)"));
    }
    else
    {
        // Ensure the actor highlight matches the new vertex instead of clearing all feedback.
        CurrentEditableActor->SetHighlightedVertexIndex(NewIndex, false);

        // Tell the user that the polygon remains open for additional points beyond triangles/quads.
        LastSaveMessage = FString::Printf(TEXT("정점 %d 추가. 계속 좌클릭하면 삼각형/사각형 이후에도 같은 면에 정점이 이어집니다. 현재 위상: %s."), NewIndex, bTopologyValid ? TEXT("정상(초록)") : TEXT("미완성/이상(빨강)"));
    }
}

bool ARuntimeGameplayManager::AddExistingVertexToEditableObject(int32 ExistingVertexIndex)
{
    // Guard against merge clicks when no editable mesh is active.
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized())
    {
        // Store a clear message for Blueprint UI status text.
        LastSaveMessage = TEXT("병합할 편집 중 오브젝트가 없습니다.");

        // Stop because the merge target has nowhere to go.
        return false;
    }

    // Remember the source before merging so the status message can describe the new segment.
    const int32 PreviousConnectedSourceIndex = ConnectedEditableVertexSourceIndex;

    // Ask the actor to append the existing vertex index to the active polygon without duplicating the vertex.
    if (!CurrentEditableActor->AddExistingVertexToCurrentFace(ExistingVertexIndex))
    {
        // Invalid merges include same-vertex edges or repeated non-first vertices inside the active n-gon.
        LastSaveMessage = FString::Printf(TEXT("정점 %d 병합 실패: 같은 선분 또는 중복 정점으로 인해 위상이 꼬일 수 있습니다."), ExistingVertexIndex);

        // Keep current highlights unchanged so the user can see the rejected target.
        return false;
    }

    // Mirror the actor's connected source; after a merge this becomes the merged target vertex.
    ConnectedEditableVertexSourceIndex = CurrentEditableActor->GetConnectedVertexSourceIndex();

    // Keep manager selection state aligned with the merged target.
    HighlightedEditableVertexIndex = ExistingVertexIndex;

    // A merge click is a short click, not a drag move.
    bMovingHighlightedEditableVertex = false;

    // The press classification has completed by the time this merge is called.
    bPrimaryVertexPressActive = false;

    // A successful merge cannot simultaneously be a drag.
    bPrimaryVertexDragActive = false;

    // Clear the pressed vertex because no button is currently held.
    PressedEditableVertexIndex = INDEX_NONE;

    // Check the rebuilt face fan so the message matches the red/green preview.
    const bool bTopologyValid = CurrentEditableActor->IsTopologyValid();

    // Use a source-aware message when the segment came from a previously selected vertex.
    if (PreviousConnectedSourceIndex != INDEX_NONE && PreviousConnectedSourceIndex != ExistingVertexIndex)
    {
        // Tell the user that no duplicate point was created; the existing target was reused.
        LastSaveMessage = FString::Printf(TEXT("정점 %d → 정점 %d 병합 연결 완료. 기존 정점을 재사용했습니다. 현재 위상: %s."), PreviousConnectedSourceIndex, ExistingVertexIndex, bTopologyValid ? TEXT("정상(초록)") : TEXT("미완성/이상(빨강)"));
    }
    else
    {
        // Cover the case where the first vertex is selected as the starting/closing vertex.
        LastSaveMessage = FString::Printf(TEXT("정점 %d을 현재 n-gon 체인에 병합했습니다. 현재 위상: %s."), ExistingVertexIndex, bTopologyValid ? TEXT("정상(초록)") : TEXT("미완성/이상(빨강)"));
    }

    // True lets the release handler know that it does not need to fall back to source-selection mode.
    return true;
}

void ARuntimeGameplayManager::BeginEditableVertexPrimaryPress(int32 VertexIndex)
{
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized())
    {
        return;
    }

    bPrimaryVertexPressActive = true;
    bPrimaryVertexDragActive = false;
    bMovingHighlightedEditableVertex = false;
    PressedEditableVertexIndex = VertexIndex;
    HighlightedEditableVertexIndex = VertexIndex;
    PrimaryVertexPressStartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
    PrimaryVertexPressStartLocation = CurrentEditableActor->GetVertexWorldLocation(VertexIndex);
    CurrentEditableActor->SetHighlightedVertexIndex(VertexIndex, false);
    CurrentEditableActor->ClearPreviewVertex();
    LastSaveMessage = FString::Printf(TEXT("정점 %d 선택: 짧게 떼면 연결 정점 생성 모드, 누른 채 중앙 십자가를 움직이면 정점 위치 편집."), VertexIndex);
}

void ARuntimeGameplayManager::UpdateEditableVertexPrimaryPressDrag()
{
    if (!bPrimaryVertexPressActive || !IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized() || PressedEditableVertexIndex == INDEX_NONE)
    {
        return;
    }

    const double NowSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : PrimaryVertexPressStartTime;
    const double HeldSeconds = NowSeconds - PrimaryVertexPressStartTime;
    const float MoveDistance = bLastTraceHasPlacementLocation ? FVector::Dist(LastPreviewLocation, PrimaryVertexPressStartLocation) : 0.0f;

    if (!bPrimaryVertexDragActive && HeldSeconds >= FMath::Max(0.0f, VertexDragHoldSeconds) && MoveDistance >= FMath::Max(1.0f, VertexDragStartDistance))
    {
        bPrimaryVertexDragActive = true;
        bMovingHighlightedEditableVertex = true;
        ClearConnectedVertexCreationState();
        LastSaveMessage = FString::Printf(TEXT("정점 %d 위치 편집 중: 버튼을 떼면 현재 위치로 확정됩니다."), PressedEditableVertexIndex);
        NotifyRuntimeStateChanged();
    }

    if (bPrimaryVertexDragActive)
    {
        if (bLastTraceHasPlacementLocation)
        {
            CurrentEditableActor->MoveVertexWorld(PressedEditableVertexIndex, LastPreviewLocation);
        }
        CurrentEditableActor->SetHighlightedVertexIndex(PressedEditableVertexIndex, true);
    }
    else
    {
        CurrentEditableActor->SetHighlightedVertexIndex(PressedEditableVertexIndex, false);
        CurrentEditableActor->ClearPreviewVertex();
    }
}

void ARuntimeGameplayManager::EndEditableVertexPrimaryPress()
{
    if (!bPrimaryVertexPressActive)
    {
        return;
    }

    const int32 ReleasedVertexIndex = PressedEditableVertexIndex;
    const bool bWasDragging = bPrimaryVertexDragActive;
    bPrimaryVertexPressActive = false;
    bPrimaryVertexDragActive = false;
    bMovingHighlightedEditableVertex = false;
    PressedEditableVertexIndex = INDEX_NONE;

    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized() || ReleasedVertexIndex == INDEX_NONE)
    {
        ClearEditableVertexMoveState();
        return;
    }

    if (bWasDragging)
    {
        // A hold-and-move gesture edits the selected vertex position.
        if (bLastTraceHasPlacementLocation)
        {
            // Commit the dragged vertex to the current center-crosshair location, which can be a surface hit or a 10m air point.
            CurrentEditableActor->MoveVertexWorld(ReleasedVertexIndex, LastPreviewLocation);
        }

        // Return the vertex highlight to the normal selected color after release.
        CurrentEditableActor->SetHighlightedVertexIndex(ReleasedVertexIndex, false);

        // Keep the moved vertex selected for immediate visual confirmation.
        HighlightedEditableVertexIndex = ReleasedVertexIndex;

        // Moving a vertex does not change the edge source unless the actor had already set one.
        ConnectedEditableVertexSourceIndex = CurrentEditableActor->GetConnectedVertexSourceIndex();

        // Store a user-visible status message for Blueprint UI.
        LastSaveMessage = FString::Printf(TEXT("정점 %d 위치 변경 완료"), ReleasedVertexIndex);
    }
    else if (ConnectedEditableVertexSourceIndex != INDEX_NONE && CurrentEditableActor->GetCurrentFaceVertexCount() > 0)
    {
        // A short click on an existing vertex while a chain is active first attempts to merge/connect to that existing vertex.
        if (!AddExistingVertexToEditableObject(ReleasedVertexIndex))
        {
            // If the vertex was already part of the active face, reinterpret the click as choosing a new source vertex.
            BeginConnectedVertexCreationFromIndex(ReleasedVertexIndex);
        }
    }
    else
    {
        // A short click with no active chain means this vertex becomes the source for a new connected segment.
        BeginConnectedVertexCreationFromIndex(ReleasedVertexIndex);
    }
}

void ARuntimeGameplayManager::BeginConnectedVertexCreationFromIndex(int32 VertexIndex)
{
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized())
    {
        return;
    }

    // Ask the mesh actor to reset its active polygon chain so this vertex becomes the first point.
    if (CurrentEditableActor->StartConnectedVertexFromIndex(VertexIndex))
    {
        // Mirror the source index in manager state for input decisions and status text.
        ConnectedEditableVertexSourceIndex = VertexIndex;

        // Keep the clicked vertex selected.
        HighlightedEditableVertexIndex = VertexIndex;

        // Short-click source selection is not a move operation.
        bMovingHighlightedEditableVertex = false;

        // Tell the user they can either add a new point in space or merge into another highlighted vertex.
        LastSaveMessage = FString::Printf(TEXT("정점 %d 연결 생성 모드: 빈 곳 좌클릭=새 정점, 기존 정점에 맞춰 좌클릭=병합 연결."), VertexIndex);
    }
}

void ARuntimeGameplayManager::ClearConnectedVertexCreationState()
{
    ConnectedEditableVertexSourceIndex = INDEX_NONE;
    if (IsValid(CurrentEditableActor))
    {
        CurrentEditableActor->ClearConnectedVertexSource();
    }
}

bool ARuntimeGameplayManager::FinishOrCancelCurrentVertexEditing()
{
    return FinishCurrentEditableMesh();
}

bool ARuntimeGameplayManager::FinishCurrentEditableMesh()
{
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized())
    {
        LastSaveMessage = TEXT("완성할 편집 중 메시가 없습니다.");
        NotifyRuntimeStateChanged();
        return false;
    }

    // Track whether the failure is specifically the user-requested point/line cancellation case before optional cleanup.
    const bool bPointOrLineOnly = CurrentEditableActor->GetVertices().Num() > 0 && CurrentEditableActor->GetVertices().Num() <= 2;

    // Track unfinished connected edits such as "existing vertex + one new vertex" that would otherwise leave an unusable cursor/line.
    const bool bDanglingPointOrLineEdit = CurrentEditableActor->HasDanglingPointOrLineEdit();

    // If a valid mesh already exists but the user leaves a one/two-vertex branch unfinished, discard only that branch.
    if (bDanglingPointOrLineEdit && CurrentEditableActor->HasAnyTriangle() && CurrentEditableActor->IsTopologyValid())
    {
        // This keeps completed faces while preventing orphan vertices from becoming part of the finalized object.
        CurrentEditableActor->DiscardDanglingPointOrLineEdit();
    }

    // A real finalized object needs at least one triangle, valid triangle topology, and no remaining dangling one/two-vertex edit chain.
    const bool bHasValidFace = CurrentEditableActor->CanFinalizeAsObject();

    if (!bHasValidFace)
    {
        // Give a clearer message when the edit contains only one/two vertices or an unfinished one/two-vertex branch.
        const bool bCanceledBecausePointOrLine = bPointOrLineOnly || bDanglingPointOrLineEdit;

        if (bCurrentEditableActorWasExisting && bHasOriginalEditableMeshRecord)
        {
            CurrentEditableActor->LoadFromGeneratedMeshRecord(OriginalEditableMeshRecord);
            CurrentEditableActor->SetActorHiddenInGame(false);
            SpawnedGeneratedMeshes.AddUnique(CurrentEditableActor);
            LastSaveMessage = bCanceledBecausePointOrLine
                ? TEXT("정점 1~2개짜리 미완성 편집이어서 기존 메시 편집을 되돌렸습니다.")
                : TEXT("면이 형성되지 않았거나 위상이 올바르지 않아 기존 메시 편집을 되돌렸습니다.");
            CurrentEditableActor = nullptr;
        }
        else
        {
            ARuntimeEditableMeshActor* ActorToDestroy = CurrentEditableActor.Get();
            CurrentEditableActor = nullptr;
            if (IsValid(ActorToDestroy))
            {
                ActorToDestroy->Destroy();
            }
            LastSaveMessage = bCanceledBecausePointOrLine
                ? TEXT("정점 1~2개만 있는 오브젝트는 생성하지 않고 취소했습니다.")
                : TEXT("면이 형성되지 않았거나 위상이 올바르지 않아 오브젝트 생성을 취소했습니다.");
        }

        CurrentMode = IsSelectedToolbarItemObjectCreation() ? ERuntimeToolMode::PlaceEmptyObject : ERuntimeToolMode::None;
        ClearEditableVertexMoveState();
        bCurrentEditableActorWasExisting = false;
        bHasOriginalEditableMeshRecord = false;
        NotifyRuntimeStateChanged();
        return false;
    }

    CurrentEditableActor->ClearPreviewVertex();
    CurrentEditableActor->SetHighlightedVertexIndex(INDEX_NONE, false);
    CurrentEditableActor->FinalizeObject();
    CurrentEditableActor->SetActorHiddenInGame(false);
    SpawnedGeneratedMeshes.AddUnique(CurrentEditableActor);
    CurrentEditableActor = nullptr;
    CurrentMode = IsSelectedToolbarItemObjectCreation() ? ERuntimeToolMode::PlaceEmptyObject : ERuntimeToolMode::None;
    ClearEditableVertexMoveState();
    bCurrentEditableActorWasExisting = false;
    bHasOriginalEditableMeshRecord = false;
    LastSaveMessage = TEXT("메시 편집 완료: 완성된 메시가 월드에 노출됩니다. 오브젝트 만들기 아이템으로 다시 좌클릭하면 편집할 수 있습니다.");
    NotifyRuntimeStateChanged();
    return true;
}

void ARuntimeGameplayManager::CancelCurrentEditableMesh(bool bDestroyActor)
{
    DestroyPendingEmptyObjectPreview();
    if (IsValid(CurrentEditableActor))
    {
        if (bCurrentEditableActorWasExisting && bHasOriginalEditableMeshRecord)
        {
            CurrentEditableActor->LoadFromGeneratedMeshRecord(OriginalEditableMeshRecord);
            SpawnedGeneratedMeshes.AddUnique(CurrentEditableActor);
        }
        else if (bDestroyActor)
        {
            CurrentEditableActor->Destroy();
        }
        CurrentEditableActor = nullptr;
    }
    CurrentMode = IsSelectedToolbarItemObjectCreation() ? ERuntimeToolMode::PlaceEmptyObject : ERuntimeToolMode::None;
    ClearEditableVertexMoveState();
    bCurrentEditableActorWasExisting = false;
    bHasOriginalEditableMeshRecord = false;
    LastSaveMessage = bDestroyActor ? TEXT("편집 중 메시를 취소했습니다.") : TEXT("편집 중 메시를 종료했습니다.");
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::PlaceVehicle(const FVector& Location, const FString& SourceFile)
{
    FActorSpawnParameters Params;
    Params.Owner = this;
    const FRotator SpawnRot = FRotator(0.0f, GetWorld()->GetFirstPlayerController() ? GetWorld()->GetFirstPlayerController()->GetControlRotation().Yaw : 0.0f, 0.0f);
    UClass* VehicleSpawnClass = VehiclePawnClass ? VehiclePawnClass.Get() : ARuntimeVehiclePawn::StaticClass();
    const FVector SpawnLocation = Location + FVector(0.0f, 0.0f, 220.0f);
    ARuntimeVehiclePawn* Vehicle = GetWorld()->SpawnActor<ARuntimeVehiclePawn>(VehicleSpawnClass, FTransform(SpawnRot, SpawnLocation), Params);
    if (IsValid(Vehicle))
    {
        if (!SourceFile.IsEmpty())
        {
            Vehicle->LoadVehicleModel(SourceFile, FPaths::GetBaseFilename(SourceFile));
        }
        Vehicle->ResetVehiclePoseAboveGround();
        SpawnedVehicles.Add(Vehicle);
        LastSaveMessage = SourceFile.IsEmpty() ? TEXT("기본 자동차 설치됨. F키로 탑승하세요.") : TEXT("glTF 자동차 설치됨. F키로 탑승하세요.");
    }
}

void ARuntimeGameplayManager::TryEnterOrExitVehicle()
{
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (!IsValid(PC))
    {
        return;
    }

    if (ARuntimeVehiclePawn* CurrentVehicle = Cast<ARuntimeVehiclePawn>(PC->GetPawn()))
    {
        CurrentVehicle->ExitVehicle();
        return;
    }

    APawn* CurrentPawn = PC->GetPawn();
    if (ACharacterController* CharacterPawn = Cast<ACharacterController>(CurrentPawn))
    {
        if (UCharacterComponent* CharacterState = CharacterPawn->GetCharacterComponent())
        {
            if (CharacterState->IsRagdollActive() || CharacterState->IsGettingUp())
            {
                LastSaveMessage = TEXT("레그돌 상태에서는 차량에 탑승할 수 없습니다.");
                NotifyRuntimeStateChanged();
                return;
            }
        }
    }

    const FVector Origin = IsValid(CurrentPawn) ? CurrentPawn->GetActorLocation() : PC->PlayerCameraManager->GetCameraLocation();
    ARuntimeVehiclePawn* BestVehicle = nullptr;
    float BestDistSq = FMath::Square(VehicleEnterDistance);

    for (ARuntimeVehiclePawn* Vehicle : SpawnedVehicles)
    {
        if (!IsValid(Vehicle) || Vehicle->IsOccupied())
        {
            continue;
        }
        const float DistSq = FVector::DistSquared(Origin, Vehicle->GetActorLocation());
        if (DistSq < BestDistSq)
        {
            BestDistSq = DistSq;
            BestVehicle = Vehicle;
        }
    }

    if (IsValid(BestVehicle) && !BestVehicle->EnterVehicle(PC, CurrentPawn))
    {
        LastSaveMessage = TEXT("현재 상태에서는 차량에 탑승할 수 없습니다.");
        NotifyRuntimeStateChanged();
    }
}

void ARuntimeGameplayManager::CollectSceneRecords(TArray<FRuntimePlacedObjectRecord>& OutPlaced, TArray<FRuntimeGeneratedMeshRecord>& OutMeshes) const
{
    OutPlaced.Empty();
    OutMeshes.Empty();

    for (const ARuntimePrefabActor* Prefab : SpawnedPrefabs)
    {
        if (IsValid(Prefab))
        {
            OutPlaced.Add(Prefab->ToPlacementRecord());
        }
    }

    int32 VehicleRecordIndex = 0;
    for (const ARuntimeVehiclePawn* Vehicle : SpawnedVehicles)
    {
        if (IsValid(Vehicle))
        {
            OutPlaced.Add(Vehicle->ToPlacementRecord(VehicleRecordIndex));
            VehicleRecordIndex++;
        }
    }

    if (IsValid(CurrentEditableActor) && CurrentEditableActor->IsFinalized())
    {
        OutMeshes.Add(CurrentEditableActor->ToGeneratedMeshRecord());
    }
    for (const ARuntimeEditableMeshActor* Mesh : SpawnedGeneratedMeshes)
    {
        if (IsValid(Mesh) && Mesh->IsFinalized())
        {
            OutMeshes.Add(Mesh->ToGeneratedMeshRecord());
        }
    }
}

bool ARuntimeGameplayManager::SaveRuntimeScene()
{
    if (bIsSavingRuntimeScene)
    {
        return false;
    }
    bIsSavingRuntimeScene = true;

    bool bSkippedIncompleteEditableMesh = false;
    if (IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized())
    {
        // Saving uses the same one/two-vertex cleanup rule as right-click completion.
        if (CurrentEditableActor->HasDanglingPointOrLineEdit() && CurrentEditableActor->HasAnyTriangle() && CurrentEditableActor->IsTopologyValid())
        {
            // Keep completed faces but drop an unfinished dangling point/line before finalizing for save.
            CurrentEditableActor->DiscardDanglingPointOrLineEdit();
        }

        // Only meshes that can become real world objects are finalized and exported.
        if (CurrentEditableActor->CanFinalizeAsObject())
        {
            // Remove edit-only preview geometry.
            CurrentEditableActor->ClearPreviewVertex();

            // Convert the procedural mesh into its finalized/collidable state.
            CurrentEditableActor->FinalizeObject();

            // Make sure the finalized mesh remains visible in the world.
            CurrentEditableActor->SetActorHiddenInGame(false);

            // Track the generated mesh for future save/export passes.
            SpawnedGeneratedMeshes.AddUnique(CurrentEditableActor);

            // Clear the active edit pointer because the mesh is now a finalized world object.
            CurrentEditableActor = nullptr;

            // Return to object-placement mode only when the object-creation item is still selected.
            CurrentMode = IsSelectedToolbarItemObjectCreation() ? ERuntimeToolMode::PlaceEmptyObject : ERuntimeToolMode::None;
        }
        else
        {
            // Leave the active actor out of the save when it is still just a point/line or invalid topology.
            bSkippedIncompleteEditableMesh = true;
        }
    }

    TArray<FRuntimePlacedObjectRecord> Placed;
    TArray<FRuntimeGeneratedMeshRecord> Meshes;
    CollectSceneRecords(Placed, Meshes);
    const FString ManifestPath = GetManifestPath();
    const bool bSaved = URuntimeGLTFSaveLibrary::SaveRuntimeScene(this, Placed, Meshes, ManifestPath, FString());
    if (bSaved)
    {
        UE_LOG(LogTemp, Verbose, TEXT("Runtime scene saved: %s%s"), *ManifestPath, bSkippedIncompleteEditableMesh ? TEXT(" (skipped incomplete editable mesh)") : TEXT(""));
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("Runtime scene save failed."));
    }
    NotifyRuntimeStateChanged();
    bIsSavingRuntimeScene = false;
    return bSaved;
}

bool ARuntimeGameplayManager::LoadSavedRuntimeScene()
{
    if (bSavedSceneLoaded)
    {
        return false;
    }
    bSavedSceneLoaded = true;

    TArray<FRuntimePlacedObjectRecord> Placed;
    TArray<FRuntimeGeneratedMeshRecord> Meshes;
    FString LoadedManifestPath = GetManifestPath();
    if (!URuntimeGLTFSaveLibrary::LoadRuntimeScene(LoadedManifestPath, Placed, Meshes))
    {
        LoadedManifestPath = GetLegacyManifestPath();
        if (!URuntimeGLTFSaveLibrary::LoadRuntimeScene(LoadedManifestPath, Placed, Meshes))
        {
            return false;
        }
    }

    FActorSpawnParameters Params;
    Params.Owner = this;
    for (const FRuntimeGeneratedMeshRecord& MeshRecord : Meshes)
    {
        UClass* EditableSpawnClass = EditableMeshActorClass ? EditableMeshActorClass.Get() : ARuntimeEditableMeshActor::StaticClass();
        ARuntimeEditableMeshActor* MeshActor = GetWorld()->SpawnActor<ARuntimeEditableMeshActor>(EditableSpawnClass, MeshRecord.Transform, Params);
        if (IsValid(MeshActor))
        {
            MeshActor->LoadFromGeneratedMeshRecord(MeshRecord);
            SpawnedGeneratedMeshes.Add(MeshActor);
        }
    }

    for (const FRuntimePlacedObjectRecord& Record : Placed)
    {
        if (Record.Kind == ERuntimePlacedObjectKind::Prefab)
        {
            UClass* PrefabSpawnClass = PrefabActorClass ? PrefabActorClass.Get() : ARuntimePrefabActor::StaticClass();
            ARuntimePrefabActor* Prefab = GetWorld()->SpawnActor<ARuntimePrefabActor>(PrefabSpawnClass, Record.Transform, Params);
            if (IsValid(Prefab) && Prefab->LoadPrefab(Record.SourceFile, Record.RuntimeName))
            {
                SpawnedPrefabs.Add(Prefab);
            }
            else if (IsValid(Prefab))
            {
                Prefab->Destroy();
            }
        }
        else if (Record.Kind == ERuntimePlacedObjectKind::Vehicle)
        {
            UClass* VehicleSpawnClass = VehiclePawnClass ? VehiclePawnClass.Get() : ARuntimeVehiclePawn::StaticClass();
            ARuntimeVehiclePawn* Vehicle = GetWorld()->SpawnActor<ARuntimeVehiclePawn>(VehicleSpawnClass, Record.Transform, Params);
            if (IsValid(Vehicle))
            {
                if (!Record.SourceFile.IsEmpty())
                {
                    Vehicle->LoadVehicleModel(Record.SourceFile, Record.RuntimeName);
                }
                Vehicle->ResetVehiclePoseAboveGround();
                SpawnedVehicles.Add(Vehicle);
            }
        }
    }
    UE_LOG(LogTemp, Verbose, TEXT("Loaded runtime entity manifest: %s"), *LoadedManifestPath);
    NotifyRuntimeStateChanged();
    return true;
}

void ARuntimeGameplayManager::RefreshRuntimeAssetLists()
{
    ScanRuntimeFolders();
    BuildAvailableItems();
    InitializeToolbarSlotsIfNeeded();
    LastSaveMessage = TEXT("Runtime asset lists refreshed.");
    NotifyToolbarChanged();
    NotifyRuntimeStateChanged();
}

void ARuntimeGameplayManager::SetCurrentToolMode(ERuntimeToolMode NewMode)
{
    switch (NewMode)
    {
    case ERuntimeToolMode::PlacePrefab:
        SelectPrefabPlacementTool();
        break;
    case ERuntimeToolMode::PlaceEmptyObject:
        SelectEmptyObjectTool();
        break;
    case ERuntimeToolMode::EditVertices:
        SelectVertexTool();
        break;
    case ERuntimeToolMode::PlaceVehicle:
        SelectVehicleTool();
        break;
    case ERuntimeToolMode::Weapon:
        EquipCurrentWeapon();
        break;
    case ERuntimeToolMode::None:
    default:
        CurrentMode = ERuntimeToolMode::None;
        DestroyPendingEmptyObjectPreview();
        LastSaveMessage = TEXT("Runtime tool cleared.");
        NotifyRuntimeStateChanged();
        break;
    }
}

bool ARuntimeGameplayManager::SetCurrentPrefabIndex(int32 NewIndex)
{
    if (PrefabFiles.Num() == 0)
    {
        ScanRuntimeFolders();
        BuildAvailableItems();
    }
    if (!PrefabFiles.IsValidIndex(NewIndex))
    {
        LastSaveMessage = TEXT("Invalid prefab index.");
        NotifyRuntimeStateChanged();
        return false;
    }

    CurrentPrefabIndex = NewIndex;
    LastSaveMessage = FString::Printf(TEXT("Prefab 선택: %s"), *GetCurrentPrefabName());
    NotifyRuntimeStateChanged();
    return true;
}

bool ARuntimeGameplayManager::SetCurrentWeaponIndex(int32 NewIndex)
{
    if (WeaponFiles.Num() == 0)
    {
        ScanRuntimeFolders();
        BuildAvailableItems();
    }
    if (!WeaponFiles.IsValidIndex(NewIndex))
    {
        LastSaveMessage = TEXT("Invalid weapon index.");
        NotifyRuntimeStateChanged();
        return false;
    }

    CurrentWeaponIndex = NewIndex;
    LastSaveMessage = FString::Printf(TEXT("무기 선택: %s"), *GetCurrentWeaponName());
    NotifyRuntimeStateChanged();
    return true;
}

FString ARuntimeGameplayManager::GetPrefabNameAtIndex(int32 Index) const
{
    return PrefabFiles.IsValidIndex(Index) ? FPaths::GetBaseFilename(PrefabFiles[Index]) : FString();
}

FString ARuntimeGameplayManager::GetWeaponNameAtIndex(int32 Index) const
{
    return WeaponFiles.IsValidIndex(Index) ? FPaths::GetBaseFilename(WeaponFiles[Index]) : FString();
}

FString ARuntimeGameplayManager::GetPrefabPathAtIndex(int32 Index) const
{
    return PrefabFiles.IsValidIndex(Index) ? PrefabFiles[Index] : FString();
}

FString ARuntimeGameplayManager::GetWeaponPathAtIndex(int32 Index) const
{
    return WeaponFiles.IsValidIndex(Index) ? WeaponFiles[Index] : FString();
}

bool ARuntimeGameplayManager::IsEditingGeneratedMesh() const
{
    return IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized();
}

int32 ARuntimeGameplayManager::GetCurrentEditableMeshVertexCount() const
{
    return IsValid(CurrentEditableActor) ? CurrentEditableActor->GetVertices().Num() : 0;
}

int32 ARuntimeGameplayManager::GetCurrentEditableMeshTriangleCount() const
{
    return IsValid(CurrentEditableActor) ? CurrentEditableActor->GetTriangles().Num() / 3 : 0;
}

bool ARuntimeGameplayManager::IsCurrentEditableMeshTopologyValid() const
{
    return IsValid(CurrentEditableActor) && CurrentEditableActor->IsTopologyValid();
}

void ARuntimeGameplayManager::GetSpawnedGeneratedMeshActors(TArray<ARuntimeEditableMeshActor*>& OutActors) const
{
    OutActors.Empty();
    for (ARuntimeEditableMeshActor* Actor : SpawnedGeneratedMeshes)
    {
        if (IsValid(Actor))
        {
            OutActors.Add(Actor);
        }
    }
}

void ARuntimeGameplayManager::UpdatePendingEmptyObjectPreview(const FVector& Location)
{
    if (!IsValid(PendingEmptyObjectPreviewActor))
    {
        FActorSpawnParameters Params;
        Params.Owner = this;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        UClass* EditableSpawnClass = EditableMeshActorClass ? EditableMeshActorClass.Get() : ARuntimeEditableMeshActor::StaticClass();
        PendingEmptyObjectPreviewActor = GetWorld()->SpawnActor<ARuntimeEditableMeshActor>(EditableSpawnClass, FTransform(Location), Params);
        if (IsValid(PendingEmptyObjectPreviewActor))
        {
            PendingEmptyObjectPreviewActor->BeginObject(TEXT("GeneratedMesh_Pending"));
            PendingEmptyObjectPreviewActor->SetActorEnableCollision(false);
        }
    }

    if (IsValid(PendingEmptyObjectPreviewActor))
    {
        PendingEmptyObjectPreviewActor->SetActorLocation(Location);
        PendingEmptyObjectPreviewActor->SetActorHiddenInGame(false);
        PendingEmptyObjectPreviewActor->SetActorEnableCollision(false);
    }
}

void ARuntimeGameplayManager::DestroyPendingEmptyObjectPreview()
{
    if (IsValid(PendingEmptyObjectPreviewActor))
    {
        PendingEmptyObjectPreviewActor->Destroy();
    }
    PendingEmptyObjectPreviewActor = nullptr;
}

void ARuntimeGameplayManager::UpdateObjectCreationPreview()
{
    if (IsEditingGeneratedMesh())
    {
        DestroyPendingEmptyObjectPreview();
        return;
    }

    if (IsSelectedToolbarItemObjectCreation() && CurrentMode == ERuntimeToolMode::PlaceEmptyObject)
    {
        if (!bLastTraceHasPlacementLocation || GetEditableMeshFromHit(LastTraceHit))
        {
            DestroyPendingEmptyObjectPreview();
        }
        else
        {
            UpdatePendingEmptyObjectPreview(LastPreviewLocation);
        }
    }
    else
    {
        DestroyPendingEmptyObjectPreview();
    }
}

void ARuntimeGameplayManager::ClearEditableVertexMoveState()
{
    HighlightedEditableVertexIndex = INDEX_NONE;
    bMovingHighlightedEditableVertex = false;
    bPrimaryVertexPressActive = false;
    bPrimaryVertexDragActive = false;
    PressedEditableVertexIndex = INDEX_NONE;
    ConnectedEditableVertexSourceIndex = INDEX_NONE;
    PrimaryVertexPressStartTime = 0.0;
    PrimaryVertexPressStartLocation = FVector::ZeroVector;
    LastVertexDistance = 0.0f;
    if (IsValid(CurrentEditableActor))
    {
        CurrentEditableActor->SetHighlightedVertexIndex(INDEX_NONE, false);
        CurrentEditableActor->ClearConnectedVertexSource();
    }
}

void ARuntimeGameplayManager::UpdateEditableVertexPreviewAndSelection()
{
    LastVertexDistance = 0.0f;
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized() || CurrentMode != ERuntimeToolMode::EditVertices)
    {
        HighlightedEditableVertexIndex = INDEX_NONE;
        bMovingHighlightedEditableVertex = false;
        bPrimaryVertexPressActive = false;
        bPrimaryVertexDragActive = false;
        PressedEditableVertexIndex = INDEX_NONE;
        return;
    }

    if (bPrimaryVertexPressActive)
    {
        UpdateEditableVertexPrimaryPressDrag();
        return;
    }

    int32 NearestIndex = INDEX_NONE;
    FVector NearestWorld = FVector::ZeroVector;
    float Distance = 0.0f;
    if (CurrentEditableActor->FindNearestVertexToRay(LastTraceStart, LastTraceDirection, VertexSelectionRayDistance, NearestIndex, NearestWorld, Distance))
    {
        HighlightedEditableVertexIndex = NearestIndex;
        LastVertexDistance = Distance;
        CurrentEditableActor->SetHighlightedVertexIndex(NearestIndex, false);
        CurrentEditableActor->ClearPreviewVertex();
    }
    else
    {
        HighlightedEditableVertexIndex = INDEX_NONE;
        CurrentEditableActor->SetHighlightedVertexIndex(INDEX_NONE, false);
        if (bLastTraceHasPlacementLocation)
        {
            CurrentEditableActor->SetPreviewVertexWorld(LastPreviewLocation);
            if (CurrentEditableActor->HasAnyVertex())
            {
                const int32 ConnectedSource = CurrentEditableActor->GetConnectedVertexSourceIndex();
                const FVector ReferenceVertex = ConnectedSource != INDEX_NONE ? CurrentEditableActor->GetVertexWorldLocation(ConnectedSource) : CurrentEditableActor->GetLastVertexWorld();
                LastVertexDistance = FVector::Dist(ReferenceVertex, LastPreviewLocation);
            }
        }
        else
        {
            CurrentEditableActor->ClearPreviewVertex();
        }
    }
}

FString ARuntimeGameplayManager::BuildRuntimeStatusText() const
{
    const FString ModeString = StaticEnum<ERuntimeToolMode>()->GetDisplayNameTextByValue(static_cast<int64>(CurrentMode)).ToString();
    const FRuntimeToolbarItem SelectedItem = GetSelectedToolbarItem();
    const FString SelectedItemName = SelectedItem.DisplayName.IsEmpty() ? TEXT("비어 있음") : SelectedItem.DisplayName;
    const FString ItemListText = bItemListWindowOpen ? TEXT("OPEN") : TEXT("CLOSED");
    const FString TopologyText = IsEditingGeneratedMesh()
        ? (IsCurrentEditableMeshTopologyValid() ? TEXT("VALID / GREEN") : TEXT("INVALID OR INCOMPLETE / RED"))
        : TEXT("NONE");
    const FString VertexSelectText = HighlightedEditableVertexIndex != INDEX_NONE
        ? FString::Printf(TEXT("Selected Vertex: %d %s"), HighlightedEditableVertexIndex, bMovingHighlightedEditableVertex ? TEXT("(DRAG MOVING)") : (bPrimaryVertexPressActive ? TEXT("(PRESSED)") : TEXT("")))
        : TEXT("Selected Vertex: none");
    const FString ConnectedSourceText = ConnectedEditableVertexSourceIndex != INDEX_NONE
        ? FString::Printf(TEXT("Connected Source: %d"), ConnectedEditableVertexSourceIndex)
        : TEXT("Connected Source: none");
    const FString CrosshairPlacementText = !bLastTraceHasPlacementLocation
        ? TEXT("NONE")
        : (bLastTraceBlockingHit ? TEXT("SURFACE") : TEXT("AIR / FREE-SPACE"));

    return FString::Printf(
        TEXT("[Creator Toolbar Runtime]\nMode: %s | PlayMode: %s\nToolbar Slot: %d / 7 | Item: %s (%s)\nInventory Window: %s | Available Items: %d\nSnap: %s / Grid %.0f cm\nCrosshair: X %.0f Y %.0f Z %.0f | Placement: %s | Collision %.0f cm / Max %.0f cm\nMesh Edit: V=%d T=%d | Topology: %s | %s | %s | RayDist %.1f cm\nControls: MouseWheel=toolbar slot, E=full item list, LMB empty=add vertex, LMB tap vertex=linked-vertex source, LMB hold+move vertex=move vertex, RMB=finish vertex editing, SnapAction/G=toggle snap\nFolder: %s\n%s"),
        *ModeString,
        RuntimePlayMode == ERuntimePlayMode::Creator ? TEXT("Creator") : TEXT("RealLife"),
        SelectedToolbarSlotIndex + 1,
        *SelectedItemName,
        *StaticEnum<ERuntimeToolbarItemKind>()->GetDisplayNameTextByValue(static_cast<int64>(SelectedItem.Kind)).ToString(),
        *ItemListText,
        AvailableItems.Num(),
        bSnapToGrid ? TEXT("ON") : TEXT("OFF"),
        GridSize,
        LastPreviewLocation.X,
        LastPreviewLocation.Y,
        LastPreviewLocation.Z,
        *CrosshairPlacementText,
        CrosshairCollisionTraceDistance,
        FreeSpacePlacementDistance,
        GetCurrentEditableMeshVertexCount(),
        GetCurrentEditableMeshTriangleCount(),
        *TopologyText,
        *VertexSelectText,
        *ConnectedSourceText,
        LastVertexDistance,
        *GetWorldRootPath(),
        *LastSaveMessage);
}

FString ARuntimeGameplayManager::BuildHUDText() const
{
    return BuildRuntimeStatusText();
}
