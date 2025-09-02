#pragma once
#include "CoreMinimal.h"
#include "Misc/SecureHash.h"
#include "Misc/FileHelper.h"

namespace BTD
{
    inline FString HexLower(const uint8* Data, int32 Len)
    {
        static const TCHAR* D = TEXT("0123456789abcdef");
        FString Out; Out.Reserve(Len * 2);
        for (int32 i = 0; i < Len; ++i) { uint8 b = Data[i]; Out.AppendChar(D[b >> 4]); Out.AppendChar(D[b & 0xF]); }
        return Out;
    }

    // UE5.6���� FSHA256 ����. Ȥ�� ���ٸ� FSHA1�� ��ü(���ξ� "sha1:")
    inline FString BytesSHA256(const TArray<uint8>& Bytes)
    {
#if defined(FSHA256_DIGESTSIZE) || defined(UE_VERSION_5_2_OR_LATER)
        uint8 Digest[32];
        FSHA256::HashBuffer(Bytes.GetData(), Bytes.Num(), Digest);
        return HexLower(Digest, 32);
#else
        uint8 Digest[20];
        FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Digest);
        return FString(TEXT("sha1:")) + HexLower(Digest, 20);
#endif
    }

    inline FString FileSHA256(const FString& Path)
    {
        TArray<uint8> Buf;
        if (!FFileHelper::LoadFileToArray(Buf, *Path)) return TEXT("");
        return BytesSHA256(Buf);
    }

    // ���� ������ ������� �̾���� ����Ʈ�� �ؽ�
    inline FString MultiFileSHA256(const TArray<FString>& Paths)
    {
        FString Combined;
        Combined.Reserve(Paths.Num() * 80);
       for (const FString& P : Paths)
             {
            TArray<uint8> Buf;
            if (FFileHelper::LoadFileToArray(Buf, *P))
                 {
                const FString One = BytesSHA256(Buf);
                Combined += One;
                Combined += TEXT("|");
                Combined += P;
                Combined += TEXT(";");
                }
             }
         FTCHARToUTF8 Conv(*Combined);
        TArray<uint8> AsBytes;
        AsBytes.Append(reinterpret_cast<const uint8*>(Conv.Get()), Conv.Length());
        return BytesSHA256(AsBytes);
    }
}
