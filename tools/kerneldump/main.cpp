// kerneldump: read-only DashLaunch plugin that copies the running kernel and xam images
// to a USB drive (or the HDD), so hiddriver's hardcoded addresses can be ported offline
// to another kernel version. It never writes to console memory.
#include "dump_core.h"

struct Target {
	const char* device;
	const char* link;   // \??\ name
	const char* drive;  // what CreateFile sees
	const char* label;
	bool usb;
};

static const Target kTargets[] = {
	{ "\\Device\\Mass0", "\\??\\kdmp0:", "kdmp0:", "Usb0", true },
	{ "\\Device\\Mass1", "\\??\\kdmp1:", "kdmp1:", "Usb1", true },
	{ "\\Device\\Mass2", "\\??\\kdmp2:", "kdmp2:", "Usb2", true },
	{ "\\Device\\Harddisk0\\Partition1", "\\??\\kdmph:", "kdmph:", "Hdd", false },
};

HANDLE MakeThread(LPTHREAD_START_ROUTINE Address, PVOID arg) {
	HANDLE Handle = 0;
	ExCreateThread(&Handle, 0, 0, XapiThreadStartup, Address, arg, (EX_CREATE_FLAG_SUSPENDED | EX_CREATE_FLAG_SYSTEM | 0x18000424));
	XSetThreadProcessor(Handle, 4);
	SetThreadPriority(Handle, THREAD_PRIORITY_NORMAL);
	ResumeThread(Handle);
	return Handle;
}

static bool MountTarget(const Target& t) {
	STRING link, device;
	RtlInitAnsiString(&link, t.link);
	RtlInitAnsiString(&device, t.device);
	ObCreateSymbolicLink(&link, &device); // fails harmlessly if it already exists

	char dir[64];
	_snprintf(dir, sizeof(dir), "%s\\kerneldump", t.drive);
	CreateDirectoryA(dir, NULL);
	return GetFileAttributesA(dir) != INVALID_FILE_ATTRIBUTES;
}

static bool DumpTo(const Target& t) {
	char dir[64];
	_snprintf(dir, sizeof(dir), "%s\\kerneldump", t.drive);
	return DumpAll(dir, NULL);
}

static void Notify(const wchar_t* msg) {
	XNotifyQueueUI(XNOTIFYUI_TYPE_PREFERRED_REVIEW, XUSER_INDEX_ANY, XNOTIFYUI_PRIORITY_HIGH, (PWCHAR)msg, 0);
}

unsigned int __stdcall DumpThread(void*) {
	// Wait for the dashboard and the USB drives to come up, then retry for a while
	Sleep(20000);
	for (int attempt = 0; attempt < 24; attempt++) {
		bool usbDone = false;
		bool anyDone = false;
		wchar_t where[64] = L"";
		for (int i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); i++) {
			const Target& t = kTargets[i];
			// the HDD is only a fallback once the USB drives had their chance
			if (!t.usb && attempt < 6 && !usbDone)
				continue;
			if (MountTarget(t) && DumpTo(t)) {
				anyDone = true;
				usbDone |= t.usb;
				_snwprintf(where + wcslen(where), 63 - wcslen(where), L" %S", t.label);
			}
		}
		if (anyDone) {
			wchar_t msg[96];
			_snwprintf(msg, 95, L"kerneldump pronto:%s", where);
			msg[95] = 0;
			Notify(msg);
			return 0;
		}
		Sleep(5000);
	}
	Notify(L"kerneldump: nenhum pendrive ou HD gravavel");
	return 0;
}

BOOL APIENTRY DllMain(HANDLE Handle, DWORD Reason, PVOID Reserved) {
	if (Reason == DLL_PROCESS_ATTACH)
		MakeThread((LPTHREAD_START_ROUTINE)DumpThread, nullptr);
	return TRUE;
}
