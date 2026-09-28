// Read-only copy of the running kernel and xam images, shared by the kerneldump plugin
// and the testeplugin app. Nothing here writes to console memory.
#pragma once
#include <xtl.h>
#include <xkelib.h>
#include <stdio.h>

#define DUMP_PAGE 0x1000

// xboxkrnl's image plus its data, which hiddriver also touches. Starts at the kernel
// base, skipping the hypervisor mapping below it.
#define KERNEL_WINDOW_START 0x80040000
#define KERNEL_WINDOW_END   0x80400000

static const DWORD kKernelOrdinals[] = { 189, 740, 742, 744, 746, 747, 748, 749, 750, 751, 759 };
static const DWORD kXamOrdinals[] = { 401, 402, 685, 1183 };

typedef void (*DumpProgress)(const char* step);

// Copies [start, end) page by page. Pages that are unmapped, or that fault when read,
// are written as zeros instead of taking the caller down.
static DWORD DumpRange(const char* path, DWORD start, DWORD end, DumpProgress progress, DWORD* faultPages) {
	HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;

	static BYTE page[DUMP_PAGE];
	DWORD validPages = 0, faults = 0, written;
	for (DWORD addr = start; addr < end; addr += DUMP_PAGE) {
		if (progress && ((addr - start) % 0x40000) == 0) {
			char step[64];
			_snprintf(step, sizeof(step), "lendo %08X", addr);
			progress(step);
		}
		bool ok = false;
		if (MmIsAddressValid((PVOID)addr)) {
			__try {
				memcpy(page, (const void*)addr, DUMP_PAGE);
				ok = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				faults++;
			}
		}
		if (ok)
			validPages++;
		else
			memset(page, 0, DUMP_PAGE);
		WriteFile(f, page, DUMP_PAGE, &written, NULL);
	}
	CloseHandle(f);
	if (faultPages)
		*faultPages = faults;
	return validPages;
}

static void DescribeModule(FILE* info, const char* name, const DWORD* ordinals, int ordinalCount) {
	PLDR_DATA_TABLE_ENTRY ldr = (PLDR_DATA_TABLE_ENTRY)GetModuleHandleA(name);
	if (!ldr) {
		fprintf(info, "%s: not loaded\n", name);
		return;
	}
	fprintf(info, "%s: ImageBase=%08X SizeOfFullImage=%08X SizeOfNtImage=%08X NtHeadersBase=%08X EntryPoint=%08X TimeDateStamp=%08X CheckSum=%08X\n",
		name, (DWORD)ldr->ImageBase, ldr->SizeOfFullImage, ldr->SizeOfNtImage, (DWORD)ldr->NtHeadersBase,
		(DWORD)ldr->EntryPoint, ldr->TimeDateStamp, ldr->CheckSum);
	for (int i = 0; i < ordinalCount; i++) {
		DWORD addr = 0;
		XexGetProcedureAddress((HANDLE)ldr, ordinals[i], &addr);
		fprintf(info, "  ordinal %4d = %08X\n", ordinals[i], addr);
	}
}

static void ListModules(FILE* info) {
	PLDR_DATA_TABLE_ENTRY kernel = (PLDR_DATA_TABLE_ENTRY)GetModuleHandleA("xboxkrnl.exe");
	if (!kernel)
		return;
	fprintf(info, "\nLoaded modules:\n");
	PLIST_ENTRY head = &kernel->InLoadOrderLinks;
	int guard = 0;
	for (PLIST_ENTRY e = head->Flink; e && e != head && guard < 128; e = e->Flink, guard++) {
		PLDR_DATA_TABLE_ENTRY m = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
		char name[64] = { 0 };
		int n = m->BaseDllName.Length / 2;
		if (n > 63) n = 63;
		for (int i = 0; i < n; i++)
			name[i] = (char)m->BaseDllName.Buffer[i];
		fprintf(info, "  %08X +%08X %s\n", (DWORD)m->ImageBase, m->SizeOfFullImage, name);
	}
}

// Writes info.txt, xam_XXXXXXXX.bin and kernel_XXXXXXXX.bin into 'dir'.
// info.txt is closed before the memory copies, so a crash while copying still leaves it.
static bool DumpAll(const char* dir, DumpProgress progress) {
	char path[128];
	_snprintf(path, sizeof(path), "%s\\info.txt", dir);
	FILE* info = fopen(path, "w");
	if (!info)
		return false;

	fprintf(info, "kerneldump for hiddriver port\n");
	fprintf(info, "Kernel %d.%d.%d.%d\n", XboxKrnlVersion->Major, XboxKrnlVersion->Minor, XboxKrnlVersion->Build, XboxKrnlVersion->Qfe);
	if (MmIsAddressValid((PVOID)0x8010D334))
		fprintf(info, "Devkit flag (0x8010D334==0): %d\n", *(DWORD*)0x8010D334 == 0);
	fprintf(info, "\n");
	if (progress) progress("descrevendo modulos");
	__try {
		DescribeModule(info, "xboxkrnl.exe", kKernelOrdinals, sizeof(kKernelOrdinals) / sizeof(DWORD));
		DescribeModule(info, "xam.xex", kXamOrdinals, sizeof(kXamOrdinals) / sizeof(DWORD));
		ListModules(info);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		fprintf(info, "\n(excecao ao descrever os modulos)\n");
	}
	fclose(info);
	if (progress) progress("info.txt gravado");

	_snprintf(path, sizeof(path), "%s\\ranges.txt", dir);
	FILE* ranges = fopen(path, "w");

	PLDR_DATA_TABLE_ENTRY xam = (PLDR_DATA_TABLE_ENTRY)GetModuleHandleA("xam.xex");
	if (xam) {
		DWORD start = (DWORD)xam->ImageBase & ~(DUMP_PAGE - 1);
		DWORD end = ((DWORD)xam->ImageBase + xam->SizeOfFullImage + DUMP_PAGE - 1) & ~(DUMP_PAGE - 1);
		if (progress) progress("copiando xam");
		_snprintf(path, sizeof(path), "%s\\xam_%08X.bin", dir, start);
		DWORD faults = 0;
		DWORD pages = DumpRange(path, start, end, progress, &faults);
		if (ranges) { fprintf(ranges, "xam %08X-%08X: %d valid pages, %d faulted\n", start, end, pages, faults); fflush(ranges); }
		if (progress) progress("xam copiado");
	}

	if (progress) progress("copiando kernel");
	_snprintf(path, sizeof(path), "%s\\kernel_%08X.bin", dir, KERNEL_WINDOW_START);
	DWORD faults = 0;
	DWORD pages = DumpRange(path, KERNEL_WINDOW_START, KERNEL_WINDOW_END, progress, &faults);
	if (ranges) { fprintf(ranges, "kernel %08X-%08X: %d valid pages, %d faulted\n", KERNEL_WINDOW_START, KERNEL_WINDOW_END, pages, faults); fclose(ranges); }
	if (progress) progress("kernel copiado");
	return true;
}
