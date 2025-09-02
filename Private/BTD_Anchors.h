#pragma once
#include "CoreMinimal.h"
#include "Misc/SecureHash.h"         // FSHA1
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

namespace BTD
{
    inline FString Hex(const uint8* Data, int32 Len)
    {
        static const TCHAR* D = TEXT("0123456789abcdef");
        FString Out; Out.Reserve(Len * 2);
        for (int32 i = 0; i < Len; ++i) { uint8 b = Data[i]; Out.AppendChar(D[b >> 4]); Out.AppendChar(D[b & 0xF]); }
        return Out;
    }

    inline FString SHA1(const void* Data, int32 Len)
    {
        uint8 Digest[20];
        FSHA1::HashBuffer(Data, Len, Digest);
        return Hex(Digest, 20);
    }

    inline FString NodeSignature(const UEdGraphNode* N)
    {
        if (!N) return TEXT("");
        FString S;
        S += N->GetClass()->GetName(); S += TEXT("|");
        S += N->GetNodeTitle(ENodeTitleType::ListView).ToString(); S += TEXT("|");

        // Pins (stable order by name): copy, drop nulls, then sort
        TArray<UEdGraphPin*> Pins = N->Pins;
        Pins.RemoveAll([](UEdGraphPin* P) { return P == nullptr; });

        // NOTE: for pointer arrays, Sort's comparator gets **const UEdGraphPin&** (dereferenced)
        Pins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B)
            {
                const FString AN = A.PinName.ToString();
                const FString BN = B.PinName.ToString();
                if (AN != BN) return AN < BN;

                if (A.Direction != B.Direction) return A.Direction < B.Direction;

                const bool Ax = (A.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
                const bool Bx = (B.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
                if (Ax != Bx) return Ax < Bx;

                // 최후 타이브레이커: 주소 (안정적 정렬 보조)
                return &A < &B;
            });

        for (UEdGraphPin* P : Pins)
        {
            // P는 null이 아님 (위에서 제거)
            S += P->PinName.ToString(); S += TEXT(":");
            S += P->PinType.PinCategory.ToString(); S += TEXT(":");
            S += P->PinType.PinSubCategory.ToString(); S += TEXT(":");
            S += (P->Direction == EGPD_Input) ? TEXT("in") : TEXT("out"); S += TEXT(":");
            S += (P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) ? TEXT("x") : TEXT("d"); S += TEXT(";");
        }
        return S;
    }



    // Stable anchor: sha1(GUID|sig) → "A" + first 10 hex
    inline FString AnchorForNode(const UEdGraphNode* N)
    {
        FString Sig;
        if (N && N->NodeGuid.IsValid())
        {
            Sig += N->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces);
            Sig += TEXT("|");
        }
        Sig += NodeSignature(N);
        FTCHARToUTF8 Bytes(*Sig);
        const FString H = SHA1(Bytes.Get(), Bytes.Length());
        return FString::Printf(TEXT("A%.*s"), 10, *H);
    }
}
