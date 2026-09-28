#include "Detours.h"

BYTE   Detour::TrampolineBuffer[200 * 20] = {};
SIZE_T Detour::TrampolineSize = 0;
// Makes freshly written code visible to instruction fetch: write the data cache line back,
// then invalidate the instruction cache line (dcbst; sync; icbi; isync). Without this, a
// function that ran recently can keep executing its old bytes from the instruction cache.
// The XDK has no intrinsic for icbi, so the sequence is emitted by hand; the line is in r3.
__declspec(naked) static void FlushCacheLine(const void* line)
{
	__emit(0x7C00186C); // dcbst r0, r3
	__emit(0x7C0004AC); // sync
	__emit(0x7C001FAC); // icbi  r0, r3
	__emit(0x4C00012C); // isync
	__emit(0x4E800020); // blr
}

void FlushCodeRange(const void* Address, SIZE_T Size)
{
	const UINT32 LineSize = 128;
	for (UINT32 Line = (UINT32)Address & ~(LineSize - 1); Line < (UINT32)Address + Size; Line += LineSize)
		FlushCacheLine((const void*)Line);
}
