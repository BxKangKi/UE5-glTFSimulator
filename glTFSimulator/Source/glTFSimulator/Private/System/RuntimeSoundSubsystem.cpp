// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/RuntimeSoundSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Sound/SoundWaveProcedural.h"
#include "Simulator/ModelDatabaseSubsystem.h"
#include "System/SafeFileIO.h"
#include "System/WorldArchive.h"

namespace
{
    struct FDecodedPcm16
    {
        TArray<uint8> Bytes;
        int32 SampleRate = 0;
        int32 NumChannels = 0;
        int32 NumFrames = 0;
        FString Error;

        bool IsValid() const
        {
            return Error.IsEmpty() && !Bytes.IsEmpty() && SampleRate > 0
                && NumChannels > 0 && NumFrames > 0;
        }
    };

    bool ReadU16LE(const TArray<uint8>& Data, const int64 Offset, uint16& Out)
    {
        if (Offset < 0 || Offset + 2 > Data.Num()) return false;
        Out = static_cast<uint16>(Data[Offset])
            | static_cast<uint16>(static_cast<uint16>(Data[Offset + 1]) << 8);
        return true;
    }

    bool ReadU32LE(const TArray<uint8>& Data, const int64 Offset, uint32& Out)
    {
        if (Offset < 0 || Offset + 4 > Data.Num()) return false;
        Out = static_cast<uint32>(Data[Offset])
            | (static_cast<uint32>(Data[Offset + 1]) << 8)
            | (static_cast<uint32>(Data[Offset + 2]) << 16)
            | (static_cast<uint32>(Data[Offset + 3]) << 24);
        return true;
    }

    bool MatchFourCC(const TArray<uint8>& Data, const int64 Offset, const char A, const char B, const char C, const char D)
    {
        return Offset >= 0 && Offset + 4 <= Data.Num()
            && Data[Offset] == static_cast<uint8>(A)
            && Data[Offset + 1] == static_cast<uint8>(B)
            && Data[Offset + 2] == static_cast<uint8>(C)
            && Data[Offset + 3] == static_cast<uint8>(D);
    }

    int16 DecodePcmSample(const uint8* Source, const uint16 BitsPerSample)
    {
        switch (BitsPerSample)
        {
        case 8:
            return static_cast<int16>((static_cast<int32>(Source[0]) - 128) << 8);
        case 16:
            return static_cast<int16>(static_cast<uint16>(Source[0])
                | static_cast<uint16>(static_cast<uint16>(Source[1]) << 8));
        case 24:
        {
            int32 Value = static_cast<int32>(Source[0])
                | (static_cast<int32>(Source[1]) << 8)
                | (static_cast<int32>(Source[2]) << 16);
            if ((Value & 0x00800000) != 0) Value |= static_cast<int32>(0xFF000000);
            return static_cast<int16>(Value >> 8);
        }
        case 32:
        {
            const int32 Value = static_cast<int32>(static_cast<uint32>(Source[0])
                | (static_cast<uint32>(Source[1]) << 8)
                | (static_cast<uint32>(Source[2]) << 16)
                | (static_cast<uint32>(Source[3]) << 24));
            return static_cast<int16>(Value >> 16);
        }
        default:
            return 0;
        }
    }

    FDecodedPcm16 DecodeWaveToPcm16(TArray<uint8>&& WaveBytes)
    {
        FDecodedPcm16 Result;
        if (WaveBytes.Num() < 44
            || !MatchFourCC(WaveBytes, 0, 'R', 'I', 'F', 'F')
            || !MatchFourCC(WaveBytes, 8, 'W', 'A', 'V', 'E'))
        {
            Result.Error = TEXT("Sound member is not a RIFF/WAVE file.");
            return Result;
        }

        uint16 Format = 0;
        uint16 Channels = 0;
        uint32 SampleRate = 0;
        uint16 BlockAlign = 0;
        uint16 BitsPerSample = 0;
        int64 DataOffset = INDEX_NONE;
        uint32 DataBytes = 0;
        bool bHasFmt = false;

        int64 Offset = 12;
        while (Offset + 8 <= WaveBytes.Num())
        {
            uint32 ChunkSize = 0;
            if (!ReadU32LE(WaveBytes, Offset + 4, ChunkSize)) break;
            const int64 Payload = Offset + 8;
            const int64 ChunkEnd = Payload + static_cast<int64>(ChunkSize);
            if (ChunkEnd > WaveBytes.Num())
            {
                Result.Error = TEXT("WAV chunk exceeds the stored sound member.");
                return Result;
            }

            if (MatchFourCC(WaveBytes, Offset, 'f', 'm', 't', ' '))
            {
                if (ChunkSize < 16
                    || !ReadU16LE(WaveBytes, Payload, Format)
                    || !ReadU16LE(WaveBytes, Payload + 2, Channels)
                    || !ReadU32LE(WaveBytes, Payload + 4, SampleRate)
                    || !ReadU16LE(WaveBytes, Payload + 12, BlockAlign)
                    || !ReadU16LE(WaveBytes, Payload + 14, BitsPerSample))
                {
                    Result.Error = TEXT("WAV fmt chunk is invalid.");
                    return Result;
                }
                bHasFmt = true;
            }
            else if (MatchFourCC(WaveBytes, Offset, 'd', 'a', 't', 'a'))
            {
                DataOffset = Payload;
                DataBytes = ChunkSize;
            }

            Offset = ChunkEnd + (ChunkSize & 1u);
        }

        const bool bPcm = Format == 1 && (BitsPerSample == 8 || BitsPerSample == 16
            || BitsPerSample == 24 || BitsPerSample == 32);
        const bool bFloat32 = Format == 3 && BitsPerSample == 32;
        if (!bHasFmt || DataOffset == INDEX_NONE || DataBytes == 0 || (!bPcm && !bFloat32)
            || Channels == 0 || Channels > 8 || SampleRate < 8000 || SampleRate > 384000)
        {
            Result.Error = TEXT("WAV must contain PCM 8/16/24/32-bit or IEEE float32 audio with 1-8 channels.");
            return Result;
        }

        const int32 SourceBytesPerSample = BitsPerSample / 8;
        const int32 ExpectedBlockAlign = static_cast<int32>(Channels) * SourceBytesPerSample;
        if (SourceBytesPerSample <= 0 || BlockAlign != ExpectedBlockAlign || DataBytes % BlockAlign != 0)
        {
            Result.Error = TEXT("WAV block alignment is invalid.");
            return Result;
        }

        const int64 FrameCount64 = static_cast<int64>(DataBytes) / BlockAlign;
        const int64 SampleCount64 = FrameCount64 * Channels;
        const int64 DecodedBytes64 = SampleCount64 * 2ll;
        if (FrameCount64 <= 0 || FrameCount64 > MAX_int32
            || DecodedBytes64 <= 0 || DecodedBytes64 > FGWorldArchive::MaxSoundBytes
            || DecodedBytes64 > MAX_int32)
        {
            Result.Error = TEXT("Decoded sound exceeds the runtime PCM safety limit.");
            return Result;
        }

        // Convert in-place so runtime peak memory stays close to one archived WAV buffer. 8-bit PCM
        // expands to 16-bit; decoding it directly from its RIFF data offset can overwrite unread
        // samples near the start of the chunk. Grow once, move the compact 8-bit payload to the tail,
        // then decode forward. 16/24/32-bit sources already write strictly behind their read cursor.
        const int32 DecodedBytes = static_cast<int32>(DecodedBytes64);
        int64 DecodeSourceOffset = DataOffset;
        if (SourceBytesPerSample == 1)
        {
            if (DecodedBytes > WaveBytes.Num())
            {
                WaveBytes.SetNumUninitialized(DecodedBytes);
            }
            const int64 RelocatedOffset = DecodedBytes64 - static_cast<int64>(DataBytes);
            FMemory::Memmove(
                WaveBytes.GetData() + RelocatedOffset,
                WaveBytes.GetData() + DataOffset,
                DataBytes);
            DecodeSourceOffset = RelocatedOffset;
        }

        uint8* Buffer = WaveBytes.GetData();
        auto WritePcm16 = [Buffer](const int64 SampleIndex, const int16 Value)
        {
            const uint16 Bits = static_cast<uint16>(Value);
            const int64 DestOffset = SampleIndex * 2;
            Buffer[DestOffset] = static_cast<uint8>(Bits & 0xFFu);
            Buffer[DestOffset + 1] = static_cast<uint8>((Bits >> 8) & 0xFFu);
        };
        auto DecodeOne = [&](const int64 SampleIndex)
        {
            const uint8* Sample = Buffer + DecodeSourceOffset + SampleIndex * SourceBytesPerSample;
            int16 Value = 0;
            if (bPcm)
            {
                Value = DecodePcmSample(Sample, BitsPerSample);
            }
            else
            {
                const uint32 Bits = static_cast<uint32>(Sample[0])
                    | (static_cast<uint32>(Sample[1]) << 8)
                    | (static_cast<uint32>(Sample[2]) << 16)
                    | (static_cast<uint32>(Sample[3]) << 24);
                float FloatValue = 0.0f;
                FMemory::Memcpy(&FloatValue, &Bits, sizeof(FloatValue));
                if (!FMath::IsFinite(FloatValue)) FloatValue = 0.0f;
                Value = static_cast<int16>(FMath::RoundToInt(
                    FMath::Clamp(FloatValue, -1.0f, 1.0f) * 32767.0f));
            }
            WritePcm16(SampleIndex, Value);
        };

        for (int64 SampleIndex = 0; SampleIndex < SampleCount64; ++SampleIndex)
        {
            DecodeOne(SampleIndex);
        }
        WaveBytes.SetNum(DecodedBytes, EAllowShrinking::No);
        Result.Bytes = MoveTemp(WaveBytes);
        Result.SampleRate = static_cast<int32>(SampleRate);
        Result.NumChannels = Channels;
        Result.NumFrames = static_cast<int32>(FrameCount64);
        return Result;
    }
}

void URuntimeSoundSubsystem::Deinitialize()
{
    ++RequestGeneration;
    Super::Deinitialize();
}

URuntimeSoundSubsystem* URuntimeSoundSubsystem::Get(const UObject* WorldContextObject)
{
    if (!IsValid(WorldContextObject)) return nullptr;
    const UWorld* World = WorldContextObject->GetWorld();
    UGameInstance* GameInstance = World ? World->GetGameInstance() : Cast<UGameInstance>(const_cast<UObject*>(WorldContextObject));
    return IsValid(GameInstance) ? GameInstance->GetSubsystem<URuntimeSoundSubsystem>() : nullptr;
}

void URuntimeSoundSubsystem::LoadSoundAsync(
    const FString& SoundReference,
    FRuntimeSoundLoadCompleted Completion)
{
    check(IsInGameThread());

    FGuid UUID;
    if (!FGWorldArchive::ParseSoundReference(SoundReference.TrimStartAndEnd(), UUID))
    {
        Completion.ExecuteIfBound(nullptr, TEXT("Sound reference must be gworld://sound/<UUID>."));
        return;
    }

    UGameInstance* GameInstance = GetGameInstance();
    UModelDatabaseSubsystem* Database = IsValid(GameInstance)
        ? GameInstance->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader = IsValid(Database)
        ? Database->GetArchiveReaderForSound(UUID) : nullptr;
    if (!Reader.IsValid())
    {
        Completion.ExecuteIfBound(nullptr, TEXT("The requested Sound asset is not present in the mounted .gworld/.gasset set."));
        return;
    }

    const uint64 Generation = RequestGeneration;
    const TWeakObjectPtr<URuntimeSoundSubsystem> WeakThis(this);
    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, Reader, UUID, Generation, Completion]() mutable
        {
            TArray<uint8> WaveBytes;
            FString Error;
            FDecodedPcm16 Decoded;
            if (!Reader->ReadSoundWave(UUID, WaveBytes, Error))
            {
                Decoded.Error = Error.IsEmpty() ? TEXT("Failed to read the Sound WAV member.") : MoveTemp(Error);
            }
            else
            {
                Decoded = DecodeWaveToPcm16(MoveTemp(WaveBytes));
            }

            FSafeFileIO::DispatchTrackedGameThread(
                [WeakThis, Generation, Completion, Decoded = MoveTemp(Decoded)]() mutable
                {
                    URuntimeSoundSubsystem* Self = WeakThis.Get();
                    if (!IsValid(Self) || Self->RequestGeneration != Generation)
                    {
                        return;
                    }
                    if (!Decoded.IsValid())
                    {
                        Completion.ExecuteIfBound(nullptr, Decoded.Error);
                        return;
                    }

                    USoundWaveProcedural* Sound = NewObject<USoundWaveProcedural>(Self, NAME_None, RF_Transient);
                    if (!IsValid(Sound))
                    {
                        Completion.ExecuteIfBound(nullptr, TEXT("Could not allocate USoundWaveProcedural."));
                        return;
                    }

                    Sound->NumChannels = Decoded.NumChannels;
                    Sound->SetSampleRate(Decoded.SampleRate);
                    Sound->SetNumFrames(Decoded.NumFrames);
                    Sound->SampleByteSize = 2;
                    Sound->Duration = static_cast<float>(Decoded.NumFrames) / static_cast<float>(Decoded.SampleRate);
                    Sound->QueueAudio(Decoded.Bytes.GetData(), Decoded.Bytes.Num());
                    Completion.ExecuteIfBound(Sound, FString());
                });
        });

    if (!bQueued)
    {
        Completion.ExecuteIfBound(nullptr, TEXT("Sound worker queue is shutting down."));
    }
}
