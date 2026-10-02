#include <xtl.h>
#include <xkelib.h>
#include <string>
#include <fstream>
#include <time.h>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <vector>
#include "Detours.h"

// Build switches, set by the Makefile (make DIAG=1 STAGE=3):
//   HIDDRIVER_DIAG  0 = use build, like upstream: no log, no notification, no stage file and only
//                       the hooks the controllers need. 1 = diagnostic build (log on the HDD,
//                       heartbeat, notification, hiddriver_etapa.txt, read-only USB loggers).
//   HIDDRIVER_STAGE stage of the use build: 2 = hooks without the USB reset (validated on 17150;
//                   controllers must be plugged after boot), 3 = with the USB reset, like upstream.
#ifndef HIDDRIVER_DIAG
#define HIDDRIVER_DIAG 0
#endif
#ifndef HIDDRIVER_STAGE
#define HIDDRIVER_STAGE 2
#endif
Detour HidAddDeviceDetour;
Detour HidRemoveDeviceDetour;
Detour XamInputGetStateDetour;
Detour XamInputSetStateDetour;
Detour XamInputGetCapabilitiesDetour;
Detour XamInactivityDetectRecentActivityDetour;

uint16_t swap_endianness_16(uint16_t val) {
	return (val >> 8) | (val << 8);
}

BOOL IsTrayOpen() {
	BYTE Input[0x10] = { 0 }, Output[0x10] = { 0 };
	Input[0] = 0xA;
	HalSendSMCMessage(Input, Output);
	return (Output[1] == 0x60);
}

// This console likes to kill non system threads on title switches
HANDLE MakeThread(LPTHREAD_START_ROUTINE Address, PVOID arg) {
	HANDLE Handle = 0;
	ExCreateThread(&Handle, 0, 0, XapiThreadStartup, Address, arg, (EX_CREATE_FLAG_SUSPENDED | EX_CREATE_FLAG_SYSTEM | 0x18000424));
	XSetThreadProcessor(Handle, 4);
	SetThreadPriority(Handle, THREAD_PRIORITY_NORMAL);
	ResumeThread(Handle);
	return Handle;
}

#if HIDDRIVER_DIAG
void Notify(const wchar_t* msg) {
	XNotifyQueueUI(XNOTIFYUI_TYPE_PREFERRED_REVIEW, XUSER_INDEX_ANY, XNOTIFYUI_PRIORITY_HIGH, (PWCHAR)msg, 0);
}

// Diagnostic log for retail consoles, where DbgPrint goes nowhere. USB callbacks only
// format into a ring buffer; DllMain and the notify thread flush it to hiddriver_log.txt at
// the root of the HDD. Only the HDD: the USB drives go away while the USB stack is reset.
//
// Code running in the system process (a DashLaunch plugin) resolves drive links under
// \System??\, not \??\ (that one is the title namespace). As a fallback, DashLaunch itself
// creates the system links hdd: and usb:.
#define DIAG_SLOTS 256
#define DIAG_LINE 192
#define DIAG_ARGS 14
// A slot keeps the format string and the raw argument words, not formatted text: the CRT's
// _vsnprintf is not safe inside USB callbacks (it touches per-thread CRT data and can allocate),
// and calling it there froze the console. The flush thread formats later, on a normal thread.
// Every %s/%S argument must therefore point to static data (string literals or globals).
struct DiagSlot {
	volatile LONG seq;
	const char* fmt;
	DWORD args[DIAG_ARGS];
};
DiagSlot g_diagSlots[DIAG_SLOTS];
volatile LONG g_diagWrite = 0;
volatile LONG g_diagRead = 0;

struct DiagDrive {
	const char* link;   // our own system link, or nullptr to use one DashLaunch created
	const char* device;
	const char* root;
};
static const DiagDrive kHddRoots[] = {
	{ "\\System??\\hidlogh:", "\\Device\\Harddisk0\\Partition1", "hidlogh:\\" },
	{ nullptr, nullptr, "hdd:\\" },
};
static const DiagDrive kUsbRoots[] = {
	{ "\\System??\\hidlogu:", "\\Device\\Mass0", "hidlogu:\\" },
	{ nullptr, nullptr, "usb:\\" },
};
const char* g_logRoot = nullptr; // HDD root that accepted a write

static void MountRoot(const DiagDrive& d) {
	if (!d.link)
		return;
	STRING link, device;
	RtlInitAnsiString(&link, d.link);
	RtlInitAnsiString(&device, d.device);
	ObCreateSymbolicLink(&link, &device); // fails harmlessly if it already exists
}

void MountDiagDrives() {
	for (int i = 0; i < sizeof(kHddRoots) / sizeof(kHddRoots[0]); i++)
		MountRoot(kHddRoots[i]);
	for (int i = 0; i < sizeof(kUsbRoots) / sizeof(kUsbRoots[0]); i++)
		MountRoot(kUsbRoots[i]);

	for (int i = 0; i < sizeof(kHddRoots) / sizeof(kHddRoots[0]) && !g_logRoot; i++) {
		char path[64];
		_snprintf(path, sizeof(path), "%shiddriver_log.txt", kHddRoots[i].root);
		FILE* f = fopen(path, "a");
		if (f) {
			fprintf(f, "\n==== boot, log via %s ====\n", kHddRoots[i].root);
			fclose(f);
			g_logRoot = kHddRoots[i].root;
		}
	}
}

static bool FileExistsIn(const DiagDrive* roots, int count, const char* name) {
	for (int i = 0; i < count; i++) {
		char path[64];
		_snprintf(path, sizeof(path), "%s%s", roots[i].root, name);
		if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
			return true;
	}
	return false;
}

bool DiagFileExists(const char* name) {
	return FileExistsIn(kHddRoots, sizeof(kHddRoots) / sizeof(kHddRoots[0]), name) ||
		FileExistsIn(kUsbRoots, sizeof(kUsbRoots) / sizeof(kUsbRoots[0]), name);
}

// Up to size-1 characters of a text file at the HDD root; empty string if missing
void ReadHddFileText(const char* name, char* buf, int size) {
	buf[0] = 0;
	for (int i = 0; i < sizeof(kHddRoots) / sizeof(kHddRoots[0]); i++) {
		char path[64];
		_snprintf(path, sizeof(path), "%s%s", kHddRoots[i].root, name);
		FILE* f = fopen(path, "r");
		if (f) {
			int n = (int)fread(buf, 1, size - 1, f);
			buf[n > 0 ? n : 0] = 0;
			fclose(f);
			return;
		}
	}
}

ULONG DiagLog(const char* fmt, ...) {
	LONG idx = InterlockedIncrement(&g_diagWrite) - 1;
	DiagSlot* slot = &g_diagSlots[idx % DIAG_SLOTS];
	va_list args;
	va_start(args, fmt);
	for (int i = 0; i < DIAG_ARGS; i++)
		slot->args[i] = va_arg(args, DWORD); // reading past the real arguments is harmless: extras are ignored
	va_end(args);
	slot->fmt = fmt;
	__lwsync();
	slot->seq = idx + 1;
	return idx + 1;
}

// Waits (bounded) until the flush thread has written line 'seq' to disk. Used right after
// logging in the USB hooks: if the console freezes a few ms later, the line is already on the HDD.
void WaitFlushed(ULONG seq, DWORD maxMs) {
	DWORD start = GetTickCount();
	while (g_diagRead < (LONG)seq && GetTickCount() - start < maxMs)
		YieldProcessor();
}

// Open, append, close on every flush, so a crash never loses what was already flushed
void DiagFlush() {
	if (!g_logRoot)
		return;
	char path[64];
	_snprintf(path, sizeof(path), "%shiddriver_log.txt", g_logRoot);
	while (true) {
		DiagSlot* slot = &g_diagSlots[g_diagRead % DIAG_SLOTS];
		LONG seq = slot->seq;
		if (seq <= g_diagRead)
			break;
		char line[DIAG_LINE + 32];
		if (seq > g_diagRead + 1) {
			_snprintf(line, sizeof(line), "[... %d lines lost ...]\n", (int)(seq - 1 - g_diagRead));
			g_diagRead = seq - 1;
		} else {
			const DWORD* a = slot->args;
			int n = _snprintf(line, DIAG_LINE, slot->fmt, a[0], a[1], a[2], a[3], a[4], a[5], a[6],
				a[7], a[8], a[9], a[10], a[11], a[12], a[13]);
			if (n < 0 || n >= DIAG_LINE)
				n = DIAG_LINE - 1;
			line[n] = 0;
			if (!n || line[n - 1] != '\n')
				strcat(line, "\n");
			g_diagRead++;
		}
		FILE* f = fopen(path, "a");
		if (f) {
			fputs(line, f);
			fclose(f);
		}
	}
}

#define DbgPrint DiagLog
#define DbgPrintSync(...) DiagLog(__VA_ARGS__) // no waiting: spinning inside USB callbacks holds the node lock
#else
// Use build: logging compiles to nothing, so no format strings or log code end up in the binary
#define DbgPrint(...) ((void)0)
#define DbgPrintSync(...) ((void)0)
#endif // HIDDRIVER_DIAG

// Re-reads a patched or hooked address, to prove the write to kernel/xam code took effect.
// A hook starts with "lis r0, target@hi" (0x3C00xxxx) once installed.
void LogReadback(const char* what, void* address) {
	DbgPrint("EINTIM: readback %-28s %08X = %08X\n", what, (DWORD)address, *(DWORD*)address);
}

// Per-hook call counters, printed by the heartbeat: they tell "hook never runs" apart from
// "hook runs but its log line is lost"
enum HookCounter { HIT_DEVMATCH, HIT_IFMATCH, HIT_ADDCOMPLETE, HIT_HIDADD, HIT_HIDREMOVE,
	HIT_INACTIVITY, HIT_GETSTATE, HIT_SETSTATE, HIT_CAPS, HIT_REPORT, HIT_COUNT };
volatile LONG g_hits[HIT_COUNT];
#define COUNT_HIT(which) InterlockedIncrement(&g_hits[which])

// Logs the first call of a hook, so a freeze can be pinned to the last hook that ran
#define LOG_FIRST_CALL(name) { static volatile LONG once = 0; if (InterlockedExchange(&once, 1) == 0) DbgPrintSync("EINTIM: first call: %s\n", name); }

// Logs (user, status) only when it changes, per user slot (0-3, 4 = any other value)
// At most 10 lines per second across all callers, so a flapping status can't flood the log
static void LogStatusChange(const char* what, DWORD* last, DWORD user, DWORD status) {
	int slot = (user & 0xFF) < 4 ? (user & 0xFF) : 4;
	if (last[slot] == status)
		return;
	last[slot] = status;

	static DWORD windowStart = 0;
	static int linesInWindow = 0, suppressed = 0;
	DWORD now = GetTickCount();
	if (now - windowStart >= 1000) {
		if (suppressed)
			DbgPrint("EINTIM: (%d status lines suppressed)\n", suppressed);
		windowStart = now;
		linesInWindow = 0;
		suppressed = 0;
	}
	if (linesInWindow++ < 10)
		DbgPrintSync("EINTIM: %s user %X -> status %X\n", what, user, status);
	else
		suppressed++;
}


struct usb_device_descriptor {
	uint8_t  bLength;             // Size of this descriptor in bytes (18)
	uint8_t  bDescriptorType;     // DEVICE descriptor type (1)
	uint16_t bcdUSB;              // USB Specification Release Number (e.g., 0x0200 for USB 2.0)
	uint8_t  bDeviceClass;        // Class code (assigned by USB-IF)
	uint8_t  bDeviceSubClass;     // Subclass code
	uint8_t  bDeviceProtocol;     // Protocol code
	uint8_t  bMaxPacketSize0;     // Max packet size for endpoint 0 (8, 16, 32, or 64)
	uint16_t idVendor;            // Vendor ID (assigned by USB-IF)
	uint16_t idProduct;           // Product ID (assigned by manufacturer)
	uint16_t bcdDevice;           // Device release number in binary-coded decimal
	uint8_t  iManufacturer;       // Index of string descriptor describing manufacturer
	uint8_t  iProduct;            // Index of string descriptor describing product
	uint8_t  iSerialNumber;       // Index of string descriptor for the device's serial number
	uint8_t  bNumConfigurations;  // Number of possible configurations
};

struct usb_interface_descriptor {
	uint8_t bLength;              // Size of this descriptor in bytes (9)
	uint8_t bDescriptorType;      // INTERFACE descriptor type (4)
	uint8_t bInterfaceNumber;     // Number of this interface
	uint8_t bAlternateSetting;    // Value used to select this alternate setting
	uint8_t bNumEndpoints;        // Number of endpoints used by this interface (excluding EP0)
	uint8_t bInterfaceClass;      // Class code (assigned by USB-IF)
	uint8_t bInterfaceSubClass;   // Subclass code
	uint8_t bInterfaceProtocol;   // Protocol code
	uint8_t iInterface;           // Index of string descriptor describing this interface
};

struct usb_endpoint_descriptor {
	uint8_t  bLength;          // Size of this descriptor in bytes (7)
	uint8_t  bDescriptorType;  // ENDPOINT descriptor type (5)
	uint8_t  bEndpointAddress; // Endpoint address and direction:
							   // Bit 7: Direction (0=OUT, 1=IN)
							   // Bits 3..0: Endpoint number (1–15)
	uint8_t  bmAttributes;     // Transfer type:
							   // Bits 1..0: 00=Control, 01=Isochronous, 10=Bulk, 11=Interrupt
							   // For Isochronous and Interrupt, additional bits define usage
	uint16_t wMaxPacketSize;   // Maximum packet size this endpoint is capable of sending or receiving
	uint8_t  bInterval;        // Polling interval for data transfers (in frames or microframes)
};

// defined by USB HID spec
enum HatSwitch {
	HAT_UP = 0,
	HAT_UP_RIGHT = 1,
	HAT_RIGHT = 2,
	HAT_DOWN_RIGHT = 3,
	HAT_DOWN = 4,
	HAT_DOWN_LEFT = 5,
	HAT_LEFT = 6,
	HAT_UP_LEFT = 7,
	HAT_NEUTRAL = 8
};

enum ControllerType {
	UNKNOWN_DEVICE = -1,
	SONY_DUALSHOCK4,
	SONY_DUALSENSE,
	GENERIC_RAW, // hardcoded byte layout from kRawLayouts
};

const uint16_t SONY_VENDOR_ID = 0x054C;
const uint16_t DUALSHOCK4_V1_PRODUCT_ID = 0x05C4;
const uint16_t DUALSHOCK4_V2_PRODUCT_ID = 0x09CC;
const uint16_t DUALSHOCK4_WIRELESS_ADAPTER_ID = 0x0BA0;
const uint16_t DUALSENSE_PRODUCT_ID = 0x0CE6;
const uint16_t DUALSENSE_EDGE_PRODUCT_ID = 0x0DF2;

#pragma pack(push, 1)
struct Report {
	uint8_t reportId;
};

struct ButtonsReport : Report {
	uint8_t x;
	uint8_t y;
	uint8_t z;
	uint8_t rz;
	uint8_t rx;
	uint8_t ry;
	uint8_t vendorDefined_ff00_20;
	uint8_t triangle : 1;
	uint8_t circle : 1;
	uint8_t cross : 1;
	uint8_t square : 1;
	uint8_t hatSwitch : 4;
	uint8_t r3 : 1;
	uint8_t l3 : 1;
	uint8_t options : 1;
	uint8_t create : 1;
	uint8_t r2 : 1;
	uint8_t l2 : 1;
	uint8_t r1 : 1;
	uint8_t l1 : 1;
	uint8_t pad : 5;
	uint8_t mute : 1;
	uint8_t touchpad : 1;
	uint8_t ps : 1;
};

struct DS4ButtonsReport : Report {
	uint8_t x;
	uint8_t y;
	uint8_t z;
	uint8_t rz;
	uint8_t triangle : 1;
	uint8_t circle : 1;
	uint8_t cross : 1;
	uint8_t square : 1;
	uint8_t hat_switch : 4;
	uint8_t r3 : 1;
	uint8_t l3 : 1;
	uint8_t options : 1;
	uint8_t share : 1;
	uint8_t r2 : 1;
	uint8_t l2 : 1;
	uint8_t r1 : 1;
	uint8_t l1 : 1;
	uint8_t : 6;
			  uint8_t touchpad : 1;
			  uint8_t ps : 1;
			  uint8_t rx;
			  uint8_t ry;
			  uint8_t vendor_defined;
};
#pragma pack(pop)

// Hardcoded layouts for generic DirectInput controllers.
// All offsets are byte offsets into the raw interrupt packet (report ID byte included, if the device uses one).
#define RAW_NONE 0xFF
enum RawButton {
	RAW_A, RAW_B, RAW_X, RAW_Y,
	RAW_LB, RAW_RB, RAW_LT, RAW_RT,
	RAW_BACK, RAW_START, RAW_L3, RAW_R3, RAW_GUIDE,
	RAW_BUTTON_COUNT
};

struct RawLayout {
	const char* name;
	uint16_t vendorId;
	uint16_t productId;
	uint8_t iface;          // USB interface that carries the gamepad (others go to the original driver)
	uint16_t bcdDevice;     // device release that is the gamepad, 0 = any (the 8BitDo receiver reuses its VID/PID while IDLE)
	int16_t reportId;       // -1 if the device sends no report ID
	uint8_t lx, ly, rx, ry; // 8 bit axes, 0x80 = center
	uint8_t lt, rt;         // 8 bit analog triggers, RAW_NONE if digital (then RAW_LT/RAW_RT buttons are used)
	uint8_t hat;            // hat switch byte
	uint8_t hatShift;       // 0 = low nibble, 4 = high nibble
	uint8_t buttons;        // first byte of the button bitfield
	uint8_t bit[RAW_BUTTON_COUNT]; // bit index from 'buttons' for each RawButton, RAW_NONE if absent
};

static const RawLayout kRawLayouts[] = {
	// Filled from captures made with tools/mapear_controle.py (port/logs/mapa_<nome>.txt)

	// EasySMX X05 in DInput mode. Interface 0, report ID 7, 11 bytes:
	// X Y Z Rz | hat (low nibble, 8 = neutral) | 16 buttons | accelerator | brake | pad.
	// HOME is not reported in DInput mode (nothing changed on either interface).
	{ "EasySMX X05 (DInput)", 0x2345, 0xE037, 0, 0, 7,
		1, 2, 3, 4,      // lx ly rx ry
		9, 8,            // lt = brake, rt = accelerator
		5, 0,            // hat byte, low nibble
		6,               // buttons start at byte 6
		//A B  X  Y  LB RB LT RT BACK START L3  R3  GUIDE
		{ 1, 0, 4, 3, 6, 7, 8, 9, 10,  11,   13, 14, RAW_NONE } },

	// 8BitDo Ultimate C 2.4G receiver in DInput, once a controller is connected (bcdDevice 0.01;
	// while IDLE it is 2.00 with only a vendor interface). Interface 0, report ID 1, 10 bytes:
	// 15 buttons | hat (low nibble) | X Y Z Rz | accelerator | brake. Button order checked on the PC.
	{ "8BitDo Ultimate C 2.4G (DInput)", 0x2DC8, 0x3016, 0, 0x0001, 1,
		4, 5, 6, 7,      // lx ly rx ry
		9, 8,            // lt = brake, rt = accelerator
		3, 0,            // hat byte, low nibble
		1,               // buttons start at byte 1
		//A B  X  Y  LB RB LT RT BACK START L3  R3  GUIDE
		{ 0, 1, 3, 4, 6, 7, 8, 9, 10,  11,   13, 14, 12 } },

	// Same receiver as seen by the Xbox after a few failed enumerations (PID 3106, never seen on
	// the PC). Layout assumed to be the same; any bcdDevice until the log tells which phase it is.
	{ "8BitDo Ultimate C 2.4G (PID 3106, layout presumido)", 0x2DC8, 0x3106, 0, 0, 1,
		4, 5, 6, 7,
		9, 8,
		3, 0,
		1,
		{ 0, 1, 3, 4, 6, 7, 8, 9, 10,  11,   13, 14, 12 } },

	{ nullptr } // end marker
};

const RawLayout* FindRawLayout(uint16_t vendorId, uint16_t productId) {
	for (int i = 0; kRawLayouts[i].name; i++) {
		if (kRawLayouts[i].vendorId == vendorId && kRawLayouts[i].productId == productId)
			return &kRawLayouts[i];
	}
	return nullptr;
}


struct deviceHandle;
struct __declspec(align(2)) HidControllerExtension
{
	deviceHandle* deviceHandle;
	DWORD interruptEndpoint;
	DWORD interruptHandler;
	DWORD dwordC;
	BYTE gap10[4];
	BYTE byte14;
	BYTE gap15[3];
	DWORD interruptData;
	DWORD packetSize;
	BYTE gap20[4];
	DWORD defaultEndpoint;
	DWORD keyboardCompletionHandler;
	DWORD defaultEndpoint2;
	BYTE gap30[4];
	BYTE byte34;
	BYTE gap35[3];
	DWORD dword38;
	DWORD dword3C;
	BYTE gap40[4];
	BYTE byte44;
	BYTE byte45;
	WORD word46;
	WORD interfaceNumber;
	WORD word4A;
	BYTE gap4C[4];
	DWORD cleanupHandler;
	BYTE gap54[24];
	DWORD queue;
	BYTE alwaysOne;
	BYTE alwaysOneTwo;
	BYTE unknownFlag;
	BYTE alwaysZero;
	BYTE cleanedUpDone;
	BYTE byte75;
	BYTE alwaysZeroTwo;
	unsigned __int8 deviceType;
	BYTE alwaysZeroThree;
	BYTE alwaysZeroFour;
};

struct deviceHandle // sizeof=0x4
{
	HidControllerExtension* driver;
};

typedef struct _XINPUT_CAPABILITIESEX
{
	BYTE                                Type;
	BYTE                                SubType;
	WORD                                Flags;
	XINPUT_GAMEPAD                      Gamepad;
	XINPUT_VIBRATION                    Vibration;
	DWORD unk1;
	DWORD unk2;
	DWORD unk3;
} XINPUT_CAPABILITIES_EX, *PXINPUT_CAPABILITIES_EX;

#define USB_ENDPOINT_TYPE_CONTROL         0x00
#define USB_ENDPOINT_TYPE_ISOCHRONOUS     0x01
#define USB_ENDPOINT_TYPE_BULK            0x02
#define USB_ENDPOINT_TYPE_INTERRUPT       0x03

#define USB_DIRECTION_IN 1
#define USB_DIRECTION_OUT 0

typedef usb_device_descriptor* (*usb_device_descriptor_func_t)(deviceHandle* handle);
typedef usb_interface_descriptor* (*usb_interface_descriptor_func_t)(deviceHandle* handle);
typedef int(*usb_add_device_complete_func_t)(deviceHandle* handle, int status_code);
typedef int(*usb_get_device_speed_func_t)(deviceHandle* handle);
typedef int(*usb_queue_async_transfer_func_t)(deviceHandle* handle, void* endpoint);
typedef NTSTATUS(*usb_queue_close_endpoint_func_t)(deviceHandle* handle, void* endpoint);
typedef NTSTATUS(*usb_remove_device_complete_func_t)(deviceHandle* handle);
typedef NTSTATUS(*usb_close_default_endpoint_func_t)(deviceHandle* handle, DWORD* endpoint);
typedef NTSTATUS(*usb_open_default_endpoint_func_t)(deviceHandle* handle, DWORD* endpoint);
typedef NTSTATUS(*usb_open_endpoint_func_t)(deviceHandle* handle, int transfertype, int endpointAddress, int maxPacketLength, int interval, DWORD* endpoint);
typedef usb_endpoint_descriptor* (*usb_endpoint_descriptor_func_t)(deviceHandle* handle, int index, int transfertype, int direction);
typedef int(*xam_user_bind_device_callback_func_t)(unsigned int controllerId, unsigned int context, unsigned __int8 category, bool disconnect, unsigned __int8* userIndex);
typedef int(*usbd_powerdown_notification_func_t)();
typedef void(*mm_free_physical_memory_func_t)(DWORD type, DWORD address);

usb_device_descriptor_func_t UsbdGetDeviceDescriptor = nullptr;
usb_interface_descriptor_func_t UsbdGetInterfaceDescriptor = nullptr;
usb_endpoint_descriptor_func_t UsbdGetEndpointDescriptor = nullptr;
usb_add_device_complete_func_t UsbdAddDeviceComplete = nullptr;
usb_open_default_endpoint_func_t UsbdOpenDefaultEndpoint = nullptr;
usb_open_endpoint_func_t UsbdOpenEndpoint = nullptr;
usb_get_device_speed_func_t UsbdGetDeviceSpeed = nullptr;
usb_queue_async_transfer_func_t UsbdQueueAsyncTransfer = nullptr;
usb_queue_close_endpoint_func_t UsbdQueueCloseEndpoint = nullptr;
usb_close_default_endpoint_func_t UsbdQueueCloseDefaultEndpoint = nullptr;
usb_remove_device_complete_func_t UsbdRemoveDeviceComplete = nullptr;
xam_user_bind_device_callback_func_t XamUserBindDeviceCallback = nullptr;
usbd_powerdown_notification_func_t UsbdPowerDownNotification = nullptr;
usbd_powerdown_notification_func_t UsbdDriverEntry = nullptr;
mm_free_physical_memory_func_t MmFreePhysicalMemory = nullptr;

DWORD* XampInputRoutedToSysapp = nullptr;

struct Controller {
	deviceHandle* deviceHandle;
	HidControllerExtension* controllerDriver;
	ButtonsReport currentState;
	uint8_t userIndex;
	uint32_t packetNumber;
	ControllerType controllerType;
	const RawLayout* rawLayout;
	void* reportData;
} __declspec(align(4));

Controller connectedControllers[4];

static bool RawBit(const RawLayout* l, const uint8_t* p, RawButton b) {
	uint8_t bit = l->bit[b];
	if (bit == RAW_NONE)
		return false;
	return (p[l->buttons + bit / 8] >> (bit % 8)) & 1;
}

ButtonsReport DecodeRawReport(const RawLayout* l, const uint8_t* p) {
	ButtonsReport b = ButtonsReport();
	b.x = l->lx != RAW_NONE ? p[l->lx] : 0x80;
	b.y = l->ly != RAW_NONE ? p[l->ly] : 0x80;
	b.z = l->rx != RAW_NONE ? p[l->rx] : 0x80;
	b.rz = l->ry != RAW_NONE ? p[l->ry] : 0x80;

	// ButtonsReport keeps the triggers in rx (left) and ry (right)
	b.rx = l->lt != RAW_NONE ? p[l->lt] : (RawBit(l, p, RAW_LT) ? 0xFF : 0);
	b.ry = l->rt != RAW_NONE ? p[l->rt] : (RawBit(l, p, RAW_RT) ? 0xFF : 0);

	uint8_t hat = l->hat != RAW_NONE ? (p[l->hat] >> l->hatShift) & 0x0F : HAT_NEUTRAL;
	b.hatSwitch = hat <= HAT_UP_LEFT ? hat : HAT_NEUTRAL;

	b.cross = RawBit(l, p, RAW_A);
	b.circle = RawBit(l, p, RAW_B);
	b.square = RawBit(l, p, RAW_X);
	b.triangle = RawBit(l, p, RAW_Y);
	b.l1 = RawBit(l, p, RAW_LB);
	b.r1 = RawBit(l, p, RAW_RB);
	b.l2 = RawBit(l, p, RAW_LT);
	b.r2 = RawBit(l, p, RAW_RT);
	b.create = RawBit(l, p, RAW_BACK);
	b.options = RawBit(l, p, RAW_START);
	b.l3 = RawBit(l, p, RAW_L3);
	b.r3 = RawBit(l, p, RAW_R3);
	b.ps = RawBit(l, p, RAW_GUIDE);
	return b;
}

int interruptHandler(DWORD deviceHandle, int32_t a2) {
	HidControllerExtension* driverExtension = (HidControllerExtension*)((deviceHandle - 4));
	Report* report = (Report*)driverExtension->interruptData;
	if (!driverExtension || !driverExtension->deviceHandle || !driverExtension->deviceHandle->driver || driverExtension->deviceHandle->driver->cleanedUpDone)
		return 0;
	int index = -1;

	for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
		if (connectedControllers[i].controllerDriver == driverExtension) {
			index = i;
			break;
		}
	}

	if (index < 0) // not one of ours: upstream indexed connectedControllers[-1]
		return 0;

	if (connectedControllers[index].controllerType == GENERIC_RAW) {
		const RawLayout* l = connectedControllers[index].rawLayout;
		const uint8_t* p = (const uint8_t*)report;
		// first reports of each hardcoded controller, to see whether data arrives and in what shape
		LONG n = InterlockedIncrement(&g_hits[HIT_REPORT]);
		if (n <= 8)
			DbgPrintSync("EINTIM: report %d from %s: status %X bytes %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
				n, l->name, a2, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
		if (l->reportId < 0 || p[0] == l->reportId) {
			connectedControllers[index].currentState = DecodeRawReport(l, p);
		}
		int requeued = UsbdQueueAsyncTransfer(driverExtension->deviceHandle, &driverExtension->interruptEndpoint);
		if (n <= 8) {
			// the transfer block starts at interruptEndpoint (+4): endpoint, handler, +0xC, +0x10, flag +0x14,
			// buffer +0x18, size +0x1C; the received length should be one of these words
			const DWORD* t = (const DWORD*)&driverExtension->interruptEndpoint;
			DbgPrintSync("EINTIM: report %d requeue %X block %08X %08X %08X %08X %08X %08X %08X\n",
				n, requeued, t[0], t[1], t[2], t[3], t[4], t[5], t[6]);
		}
		return requeued;
	}

	if (report->reportId == 1) {
		ButtonsReport buttonReport = ButtonsReport();

		switch (connectedControllers[index].controllerType) {
		case SONY_DUALSENSE:
			buttonReport = *(ButtonsReport*)report;
			break;
		case SONY_DUALSHOCK4:
			DS4ButtonsReport* br = (DS4ButtonsReport*)report;
			buttonReport.circle = br->circle;
			buttonReport.create = br->share;
			buttonReport.cross = br->cross;
			buttonReport.hatSwitch = br->hat_switch;
			buttonReport.l1 = br->l1;
			buttonReport.l2 = br->l2;
			buttonReport.l3 = br->l3;
			buttonReport.options = br->options;
			buttonReport.ps = br->ps;
			buttonReport.r1 = br->r1;
			buttonReport.r2 = br->r2;
			buttonReport.r3 = br->r3;
			buttonReport.rx = br->rx;
			buttonReport.ry = br->ry;
			buttonReport.rz = br->rz;
			buttonReport.square = br->square;
			buttonReport.touchpad = br->touchpad;
			buttonReport.triangle = br->triangle;
			buttonReport.x = br->x;
			buttonReport.y = br->y;
			buttonReport.z = br->z;
			break;
		}

		connectedControllers[index].currentState = buttonReport;
	}

	return UsbdQueueAsyncTransfer(driverExtension->deviceHandle, &driverExtension->interruptEndpoint);
}


int HidRemoveDeviceHook(deviceHandle* deviceHandle2) {
	COUNT_HIT(HIT_HIDREMOVE);
	DbgPrintSync("EINTIM: HID remove device %p\n", deviceHandle2);
	bool found = false;
	int index = 0;
	for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
		if (connectedControllers[i].deviceHandle == deviceHandle2) {
			found = true;
			index = i;
			break;
		}
	}

	if (!found) {
		DbgPrint("EINTIM: Returning original for handle %p\n", deviceHandle2);
		return HidRemoveDeviceDetour.GetOriginal<decltype(&HidRemoveDeviceHook)>()(deviceHandle2);
	}

	DbgPrint("EINTIM: Removing controller with handle %p\n", deviceHandle2);

	if (!deviceHandle2->driver->cleanedUpDone) {
		deviceHandle2->driver->cleanedUpDone = 1;
		connectedControllers[index].controllerDriver = nullptr;
		// Clean up is currently broken, so it leaks up to 1kb of memory on every controller reconnect
		/*
		UsbdQueueCloseEndpoint(deviceHandle2, &deviceHandle2->driver->interruptEndpoint);
		UsbdQueueCloseDefaultEndpoint(deviceHandle2, &deviceHandle2->driver->defaultEndpoint);
		UsbdRemoveDeviceComplete(deviceHandle2);
		delete deviceHandle2->driver;*/
		deviceHandle2->driver = nullptr;
		free(connectedControllers[index].reportData);
		DbgPrint("EINTIM: Removed controller with handle %p\n", deviceHandle2);
		XamUserBindDeviceCallback(0xa7553952 + index, 0x0000000010000005 + index, 0, true, 0);
		DbgPrint("EINTIM: Removed virtual controller from XAM.\n");
		return 0;
	}
	return 0; // already cleaned up (upstream fell off the end here)
}

int reportData = 0;
int HidAddDeviceHook(deviceHandle* deviceHandle) {
	COUNT_HIT(HIT_HIDADD);
	DbgPrintSync("EINTIM: HID add device %p\n", deviceHandle);
	usb_device_descriptor* device_descriptor = UsbdGetDeviceDescriptor(deviceHandle);
	usb_interface_descriptor* interface_descriptor = UsbdGetInterfaceDescriptor(deviceHandle);
	usb_endpoint_descriptor* endpoint_descriptor = UsbdGetEndpointDescriptor(deviceHandle, 0, USB_ENDPOINT_TYPE_INTERRUPT, USB_DIRECTION_IN);

	uint16_t vendorId = swap_endianness_16(device_descriptor->idVendor);
	uint16_t productId = swap_endianness_16(device_descriptor->idProduct);

	int speed = UsbdGetDeviceSpeed(deviceHandle);
	bool isOhci = speed == 0;

	DbgPrint("EINTIM: IS USB1.0: %d\n", isOhci);
	DbgPrint("EINTIM: USB device descriptor Pointer: %p\n", device_descriptor);
	DbgPrint("EINTIM: USB interface descriptor Pointer: %p\n", interface_descriptor);
	DbgPrint("EINTIM: USB endpoint interrupt in descriptor Pointer: %p\n", endpoint_descriptor);
	DbgPrint("EINTIM: HID device vendor id: %x, product id: %x\n", vendorId, productId);
	DbgPrint("EINTIM: Interface %d class %d subclass %d protocol %d\n", interface_descriptor->bInterfaceNumber,
		interface_descriptor->bInterfaceClass, interface_descriptor->bInterfaceSubClass, interface_descriptor->bInterfaceProtocol);

	ControllerType controllerType = UNKNOWN_DEVICE;
	const RawLayout* rawLayout = FindRawLayout(vendorId, productId);
	uint16_t bcdDevice = swap_endianness_16(device_descriptor->bcdDevice);
	DbgPrint("EINTIM: HID device bcdDevice %04X\n", bcdDevice);
	if (rawLayout && rawLayout->bcdDevice && bcdDevice != rawLayout->bcdDevice) {
		DbgPrint("EINTIM: %s: bcdDevice %04X is not the gamepad one (%04X), passing it on\n", rawLayout->name, bcdDevice, rawLayout->bcdDevice);
		rawLayout = nullptr;
	}
	if (rawLayout && interface_descriptor->bInterfaceNumber != rawLayout->iface) {
		DbgPrint("EINTIM: %s: interface %d is not the gamepad one, passing it on\n", rawLayout->name, interface_descriptor->bInterfaceNumber);
		rawLayout = nullptr;
	}
	if (rawLayout) {
		DbgPrint("EINTIM: Hardcoded layout: %s (interface %d)\n", rawLayout->name, interface_descriptor->bInterfaceNumber);
		controllerType = GENERIC_RAW;
	}

	if (vendorId == SONY_VENDOR_ID) {
		if (productId == DUALSENSE_PRODUCT_ID || productId == DUALSENSE_EDGE_PRODUCT_ID)
			controllerType = SONY_DUALSENSE;
		if (productId == DUALSHOCK4_V1_PRODUCT_ID || productId == DUALSHOCK4_V2_PRODUCT_ID || productId == DUALSHOCK4_WIRELESS_ADAPTER_ID)
			controllerType = SONY_DUALSHOCK4;
	}

	if (controllerType != UNKNOWN_DEVICE) {
		DbgPrint("EINTIM: Controller detected. Initialising custom handler.\n");
		int index = -1;
		for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
			if (!connectedControllers[i].controllerDriver) {
				DbgPrint("Assigning controller to index %d\n", i);
				index = i;
				break;
			}
		}

		if (index == -1) {
			DbgPrint("EINTIM: No free index!\n");
			return HidAddDeviceDetour.GetOriginal<decltype(&HidAddDeviceHook)>()(deviceHandle);
		}

		Controller c = Controller();
		c.controllerType = controllerType;
		c.rawLayout = rawLayout;
		c.currentState.hatSwitch = HAT_NEUTRAL;
		c.currentState.x = c.currentState.y = c.currentState.z = c.currentState.rz = 0x80;
		c.packetNumber = 0;
		HidControllerExtension* controllerDriver = new HidControllerExtension();
		c.deviceHandle = deviceHandle;

		controllerDriver->deviceType = 0; // Hid device type, for controllers it's zero.
		controllerDriver->alwaysOne = 1;
		deviceHandle->driver = controllerDriver;
		controllerDriver->deviceHandle = deviceHandle;
		controllerDriver->alwaysZero = 0;
		controllerDriver->alwaysZeroTwo = 0;
		controllerDriver->alwaysOneTwo = 1;
		controllerDriver->unknownFlag = 0;       // should be zero if type is 0, otherwise 1
		controllerDriver->alwaysZeroThree = 0;
		controllerDriver->alwaysZeroFour = 0;

		controllerDriver->byte14 = 1;

		UsbdAddDeviceComplete(deviceHandle, 0);

		NTSTATUS status = UsbdOpenDefaultEndpoint(deviceHandle, &controllerDriver->defaultEndpoint);
		if (NT_ERROR(status)) {
			DbgPrint("EINTIM: Failed to open control endpoint %x!\n", status);
			return status;
		}

		status = UsbdOpenEndpoint(deviceHandle, 3, endpoint_descriptor->bEndpointAddress, swap_endianness_16(endpoint_descriptor->wMaxPacketSize) & 0x7FF, endpoint_descriptor->bInterval, &controllerDriver->interruptEndpoint);
		if (NT_ERROR(status)) {
			DbgPrint("EINTIM: Failed to open interrupt endpoint %x!\n", status);
			return status;
		}
		controllerDriver->dwordC = controllerDriver->interruptEndpoint;
		controllerDriver->packetSize = swap_endianness_16(endpoint_descriptor->wMaxPacketSize) & 0x7FF;
		controllerDriver->byte75 = 1;
		controllerDriver->byte44 = 33;
		controllerDriver->byte45 = 10;
		controllerDriver->word46 = 0;
		controllerDriver->interfaceNumber = swap_endianness_16(interface_descriptor->bInterfaceNumber);
		controllerDriver->word4A = 0;
		controllerDriver->dword38 = 0;
		controllerDriver->dword3C = 0;
		controllerDriver->byte34 = 1;

		controllerDriver->interruptHandler = (DWORD)interruptHandler;
		c.reportData = malloc((swap_endianness_16(endpoint_descriptor->wMaxPacketSize) & 0x7FF) * 2);
		memset(c.reportData, 0, (swap_endianness_16(endpoint_descriptor->wMaxPacketSize) & 0x7FF) * 2);
		controllerDriver->interruptData = (DWORD)c.reportData;

		c.controllerDriver = controllerDriver;

		uint8_t userIndex = -1;
		XamUserBindDeviceCallback(0xa7553952 + index, 0x0000000010000005 + index, 0, false, &userIndex);
		c.userIndex = userIndex;
		connectedControllers[index] = c;

		DbgPrint("EINTIM: Registered virtual controller inside XAM with index: %d.\n", userIndex);
		int queued = UsbdQueueAsyncTransfer(deviceHandle, &controllerDriver->interruptEndpoint);
		DbgPrintSync("EINTIM: first interrupt transfer queued: %X (endpoint %02X, packet %d)\n",
			queued, endpoint_descriptor->bEndpointAddress, controllerDriver->packetSize);
		return queued;
	}

	DbgPrint("EINTIM: Unrelated USB Device. Calling original...\n");
	return HidAddDeviceDetour.GetOriginal<decltype(&HidAddDeviceHook)>()(deviceHandle);
}


int16_t ConvertToFullRange(uint8_t input, bool invert_y = false) {
	if (!invert_y)
		return static_cast<int16_t>((input - 128) * 256);
	else
		return static_cast<int16_t>((~(input)-128) * 256);
}

DWORD XamInputGetStateHook(DWORD user, DWORD flags, XINPUT_STATE* input_state) {
	COUNT_HIT(HIT_GETSTATE);
	DWORD status = XamInputGetStateDetour.GetOriginal<decltype(&XamInputGetStateHook)>()(user, flags, input_state);
	static DWORD lastStatus[5] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
	LogStatusChange("GetState", lastStatus, user, status);

	if ((user & 0xFF) == 0xFF)
		user = 0;

	if (!input_state)
		return status;

	static DWORD lastPressTime = 0;
	static const DWORD cooldownDuration = 1000;

	if (status == ERROR_DEVICE_NOT_CONNECTED) {
		ButtonsReport b;
		Controller* c = nullptr;
		for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
			if (connectedControllers[i].controllerDriver) {
				if (connectedControllers[i].userIndex == user) {
					c = &connectedControllers[i];
					b = connectedControllers[i].currentState;
					break;
				}
			}
		}

		if (!c)
			return status;

		// HUD is open.
		if (XampInputRoutedToSysapp[c->userIndex]) {
			// 0x1 is used for titles, 0x0 is used by some offhosts and debug input.
			if ((flags == 0x1) || (flags == 0x0)) {
				return ERROR_SUCCESS;
			}
		}


		if (b.ps) {
			DWORD now = GetTickCount();
			if (now - lastPressTime >= cooldownDuration) {
				lastPressTime = now;
				XamInputSendXenonButtonPress(user);
			}
		}

		if (b.cross)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_A;

		if (b.circle)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_B;

		if (b.triangle)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y;

		if (b.square)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_X;

		if (b.options)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_START;

		if (b.create)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_BACK;

		if (b.r3)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;

		if (b.l3)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;

		if (b.l1)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;

		if (b.r1)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;


		switch (b.hatSwitch) {
		case HatSwitch::HAT_UP:
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
			break;
		case HatSwitch::HAT_UP_RIGHT:
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
			break;
		case HatSwitch::HAT_RIGHT:
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
			break;
		case HatSwitch::HAT_DOWN_RIGHT:
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
			break;
		case HatSwitch::HAT_DOWN:
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
			break;
		case HatSwitch::HAT_DOWN_LEFT:
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
			break;
		case HatSwitch::HAT_LEFT:
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
			break;
		case HatSwitch::HAT_UP_LEFT:
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
			break;
		case HatSwitch::HAT_NEUTRAL: // Do nothing
			break;
		}

		input_state->Gamepad.sThumbRX = ConvertToFullRange(b.z);
		input_state->Gamepad.sThumbRY = ConvertToFullRange(b.rz, true);

		input_state->Gamepad.sThumbLX = ConvertToFullRange(b.x);
		input_state->Gamepad.sThumbLY = ConvertToFullRange(b.y, true);

		input_state->Gamepad.bLeftTrigger = b.rx;
		input_state->Gamepad.bRightTrigger = b.ry;
		input_state->dwPacketNumber = ++c->packetNumber;

		return ERROR_SUCCESS;
	}
	return status;
}

DWORD XamInputSetStateHook(DWORD user, DWORD flags, XINPUT_STATE* pInputState, BYTE bAmplitude, BYTE bFrequency, BYTE bOffset) {
	COUNT_HIT(HIT_SETSTATE);
	LOG_FIRST_CALL("XamInputSetState");
	DWORD status = XamInputSetStateDetour.GetOriginal<decltype(&XamInputSetStateHook)>()(user, flags, pInputState, bAmplitude, bFrequency, bOffset);

	if ((user & 0xFF) == 0xFF)
		user = 0;


	if (status == ERROR_DEVICE_NOT_CONNECTED) {
		Controller* c = nullptr;
		for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
			if (connectedControllers[i].controllerDriver) {
				if (connectedControllers[i].userIndex == user) {
					c = &connectedControllers[i];
					break;
				}
			}
		}

		if (!c)
			return status;

		return ERROR_SUCCESS;
	}

	return status;
}

DWORD XamInputGetCapabilitiesExHook(DWORD unk, DWORD user, DWORD flags, XINPUT_CAPABILITIES_EX* capabilities) {
	COUNT_HIT(HIT_CAPS);
	DWORD status = XamInputGetCapabilitiesDetour.GetOriginal<decltype(&XamInputGetCapabilitiesExHook)>()(unk, user, flags, capabilities);
	static DWORD lastStatus[5] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
	LogStatusChange("GetCapabilitiesEx", lastStatus, user, status);

	if ((user & 0xFF) == 0xFF)
		user = 0;

	if (!capabilities)
		return status;

	if (status == ERROR_DEVICE_NOT_CONNECTED) {
		Controller* c = nullptr;
		for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
			if (connectedControllers[i].controllerDriver) {
				if (connectedControllers[i].userIndex == user) {
					c = &connectedControllers[i];
					break;
				}
			}
		}

		if (!c)
			return status;

		capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
		capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
		capabilities->Flags = 0;

		XINPUT_STATE state;
		memset(&state, 0, sizeof(XINPUT_STATE));
		XamInputGetStateHook(user, 0, &state);
		capabilities->Gamepad = state.Gamepad;
		capabilities->Vibration.wLeftMotorSpeed = 0;
		capabilities->Vibration.wRightMotorSpeed = 0;
		return ERROR_SUCCESS;
	}
	return status; // upstream fell off the end here for real controllers
}

// fix for inactivity (screen dimming)
int XamInactivityDetectRecentActivityHook(DWORD r3) {
	COUNT_HIT(HIT_INACTIVITY);
	LOG_FIRST_CALL("XamInactivityDetectRecentActivity");
	// check if a controller is connected
	for (int i = 0; i < 4; i++) {
		if (connectedControllers[i].controllerDriver != (HidControllerExtension*)0) {
			// return active
			return 1;
		}
	}

	return XamInactivityDetectRecentActivityDetour.GetOriginal<decltype(&XamInactivityDetectRecentActivityHook)>()(r3);
}

void* XamInputGetState = nullptr;
void* XamInputSetState = nullptr;
void* XamInputGetCapabilitiesEx = nullptr;
bool isDevkit = true;
bool isKernel17150 = false;
DWORD UsbPhysicalPage = 0;

// Checks the instruction at 'addr' before we rely on it or patch it. 'patched' is the value
// we write there, accepted too so a second load doesn't refuse its own patches.
bool Expect(DWORD addr, DWORD original, DWORD patched, const char* what) {
	DWORD value = *(DWORD*)addr;
	bool ok = value == original || (patched && value == patched);
	DbgPrint("EINTIM: check %-28s %08X = %08X (esperado %08X) %s\n", what, addr, value, original, ok ? "ok" : "DIFERENTE");
	return ok;
}

// Addresses for retail kernel 2.0.17150.0, found from a memory dump of that kernel using the
// signatures in the comments of the 17559 branch (see port/ in the repo). Every one of them is
// checked against the bytes seen in the dump before anything is patched or hooked.
bool check17150() {
	bool ok = true;
	ok &= Expect(0x800D9418, 0x8943000B, 0, "UsbdGetInterfaceDescriptor");
	ok &= Expect(0x816D83A8, 0x7C8B2378, 0, "XamUserBindDeviceCallback");
	ok &= Expect(0x800D9EF8, 0x3D60800E, 0, "UsbdPowerDownNotification");
	ok &= Expect(0x800D9C38, 0x7D8802A6, 0, "UsbdDriverEntry");
	ok &= Expect(0x800D9C94, 0x3F80801A, 0, "UsbPhysicalPage (lis)");
	ok &= Expect(0x800D9C98, 0x93FCC8D8, 0, "UsbPhysicalPage (stw)");
	ok &= Expect(0x816F0804, 0x3D6081AB, 0, "RoutedToSysapp (lis)");
	ok &= Expect(0x816F080C, 0x396BC6E8, 0, "RoutedToSysapp (addi)");
	ok &= Expect(0x800E1514, 0x40820018, 0x48000018, "bugcheck 1");
	ok &= Expect(0x800DE810, 0x40820018, 0x48000018, "bugcheck 2");
	ok &= Expect(0x800D9E30, 0x4BF8DC29, 0x60000000, "registro duplo 1");
	ok &= Expect(0x800D9E20, 0x4BF97189, 0x60000000, "registro duplo 2");
	ok &= Expect(0x800E5CD0, 0x7D8802A6, 0, "HidAddDevice");
	ok &= Expect(0x800E5C90, 0x81630000, 0, "HidRemoveDevice");
	ok &= Expect(0x81695268, 0x3D6081AA, 0, "XamInactivityDetect");
	return ok;
}

bool initFunctionPointers() {
	isKernel17150 = XboxKrnlVersion->Build == 17150;
	isDevkit = !isKernel17150 && *(uint32_t*)(0x8010D334) == 0x00000000;
	HANDLE kernelHandle = GetModuleHandleA("xboxkrnl.exe");

	if (!kernelHandle) {
		DbgPrint("EINTIM: COULDNT GET KERNEL HANDLE!\n");
		return false;
	}

	HANDLE xamHandle = GetModuleHandleA("xam.xex");

	XexGetProcedureAddress(kernelHandle, 759, &UsbdGetDeviceDescriptor);
	XexGetProcedureAddress(kernelHandle, 744, &UsbdGetEndpointDescriptor);
	XexGetProcedureAddress(kernelHandle, 740, &UsbdAddDeviceComplete);
	XexGetProcedureAddress(kernelHandle, 746, &UsbdOpenDefaultEndpoint);
	XexGetProcedureAddress(kernelHandle, 747, &UsbdOpenEndpoint);
	XexGetProcedureAddress(kernelHandle, 742, &UsbdGetDeviceSpeed);
	XexGetProcedureAddress(kernelHandle, 748, &UsbdQueueAsyncTransfer);
	XexGetProcedureAddress(kernelHandle, 750, &UsbdQueueCloseEndpoint);
	XexGetProcedureAddress(kernelHandle, 749, &UsbdQueueCloseDefaultEndpoint);
	XexGetProcedureAddress(kernelHandle, 751, &UsbdRemoveDeviceComplete);
	XexGetProcedureAddress(kernelHandle, 189, &MmFreePhysicalMemory);


	XexGetProcedureAddress(xamHandle, 685, &XamInputGetCapabilitiesEx);
	XexGetProcedureAddress(xamHandle, 401, &XamInputGetState);
	XexGetProcedureAddress(xamHandle, 402, &XamInputSetState);

	if (isKernel17150) {
		DbgPrint("EINTIM: Running on retail kernel 17150\n");
		if (!check17150()) {
			DbgPrint("EINTIM: 17150 addresses don't match this console. Aborting before touching anything.\n");
			return false;
		}
		UsbdGetInterfaceDescriptor = (usb_interface_descriptor_func_t)0x800D9418;
		XamUserBindDeviceCallback = (xam_user_bind_device_callback_func_t)0x816D83A8;
		UsbdPowerDownNotification = (usbd_powerdown_notification_func_t)0x800D9EF8; // handler in the struct passed by UsbdDriverEntry's last call
		UsbdDriverEntry = (usbd_powerdown_notification_func_t)0x800D9C38;

		// read right before the call to XamShouldSuppressSystemInput inside XamInputGetState
		XampInputRoutedToSysapp = (DWORD*)0x81AAC6E8;

		UsbPhysicalPage = 0x8019C8D8;
		// the USB reset patches are applied by ApplyUsbResetPatches17150, only in stage 3
	}
	else if (isDevkit) {
		DbgPrint("EINTIM: Running in devkit mode\n");
		UsbdGetInterfaceDescriptor = (usb_interface_descriptor_func_t)0x8010D2D0; // 89 43 ? ? 3D 60 ? ? 89 2D ? ? 39 6B ? ? 55 4A FF 3A 2B 09 ? ? 7D 6A 58 2E ? ? ? ? ? ? ? ? 89 4D ? ? 2B 0A ? ? ? ? ? ? ? ? ? ? 81 4B ? ? 7F 03 50 40 ? ? ? ? ? ? ? ? A1 4B very bad direct signature. XREF sig: 89 63 ? ? 38 A1
		XamUserBindDeviceCallback = (xam_user_bind_device_callback_func_t)0x817A34B8; // 7C 8B 23 78 7C A4 2B 78 54 CA 06 3F
		UsbdPowerDownNotification = (usbd_powerdown_notification_func_t)0x8010E140; // argument to last function call in UsbdDriverEntry
		UsbdDriverEntry = (usbd_powerdown_notification_func_t)0x8010DE48; // 7D 88 02 A6 ? ? ? ? 94 21 ? ? 3C 80 ? ? 38 A0 
		 
		//Remove two usb related bugchecks to allow reinitialisation of the usb driver
		*(DWORD*)0x80116298 = 0x48000018;
		*(DWORD*)0x801132A4 = 0x48000018;

		// Right above 'XamShouldSuppressSystemInput', inside of XamInputGetState 
		// 3D 60 ? ? 39 6B ? ? 81 4B 00 00 2B 0A 00 00 41 9A 00 24 
		XampInputRoutedToSysapp = (DWORD*)0x81D4F650;

		//DEVKIT only: Remove assertions(Microsoft did not think that we'd come and reset the usb driver, never let them know your next move typa shit)
		/*
		*(DWORD*)0x80096B84 = 0x60000000;
		*(DWORD*)0x80095F6C = 0x60000000;
		*(DWORD*)0x80116584 = 0x60000000;
		*(DWORD*)0x80116598 = 0x60000000;
		*/

		// Prevent double registration of Usbd handlers because the console wont shutdown cleanly otherwise
		*(DWORD*)0x8010E04C = 0x60000000;
		*(DWORD*)0x8010E05C = 0x60000000;
		UsbPhysicalPage = 0x8020A9B8;
	}
	else {
		DbgPrint("EINTIM: Running in retail mode\n");
		UsbdGetInterfaceDescriptor = (usb_interface_descriptor_func_t)0x800D8500; // 89 43 ? ? 3D 60 ? ? 39 6B ? ? 55 4A FF 3A 7D 6A 58 2E A1 4B
		XamUserBindDeviceCallback = (xam_user_bind_device_callback_func_t)0x816D9060; // 7C 8B 23 78 7C A4 2B 78 54 CA 06 3F
		UsbdPowerDownNotification = (usbd_powerdown_notification_func_t)0x800D8FC8; // argument to last function call in UsbdDriverEntry
		UsbdDriverEntry = (usbd_powerdown_notification_func_t)0x800D8D08; // 7D 88 02 A6 ? ? ? ? 94 21 ? ? 3C 80 ? ? 38 A0 
		
		XampInputRoutedToSysapp = (DWORD*)0x81AAC2A0;

		//Remove two usb related bugchecks to allow reinitialisation of the usb driver
		*(DWORD*)0x800E05E4 = 0x48000018;
		*(DWORD*)0x800DD8E0 = 0x48000018;

		// Prevent double registration of Usbd handlers because the console wont shutdown cleanly otherwise
		*(DWORD*)0x800D8F00 = 0x60000000;
		*(DWORD*)0x800D8EF0 = 0x60000000;
		UsbPhysicalPage = 0x801A8098;
	}

	return true;
}

#define KILL_SWITCH_FILE "hiddriver_desligar.txt"
#define STAGE_FILE "hiddriver_etapa.txt"

// Stages, chosen by the first character of hiddriver_etapa.txt at the HDD root (default 1):
//   1 = load, log and notify only; nothing in the USB stack is touched
//   2 = + hooks (new devices go through HidAddDeviceHook), no USB reset
//   3 = + USB reset patches and the USB reset, i.e. the full upstream behaviour
int g_stage = 1;
const wchar_t* g_notifyMessage = L"hiddriver: ativo";

void ApplyUsbResetPatches17150() {
	//Remove two usb related bugchecks to allow reinitialisation of the usb driver
	*(DWORD*)0x800E1514 = 0x48000018;
	*(DWORD*)0x800DE810 = 0x48000018;

	// Prevent double registration of Usbd handlers because the console wont shutdown cleanly otherwise
	*(DWORD*)0x800D9E30 = 0x60000000;
	*(DWORD*)0x800D9E20 = 0x60000000;

	FlushCodeRange((void*)0x800E1514, 4);
	FlushCodeRange((void*)0x800DE810, 4);
	FlushCodeRange((void*)0x800D9E20, 0x14);

	LogReadback("bugcheck 1", (void*)0x800E1514);
	LogReadback("bugcheck 2", (void*)0x800DE810);
	LogReadback("registro duplo 1", (void*)0x800D9E30);
	LogReadback("registro duplo 2", (void*)0x800D9E20);
}

// XNotify drops notifications queued from the system process unless this branch in xam
// ordinal 1183 is made unconditional (upstream commit 3049f04; JRPC2 does the same).
// Only patched when the expected instruction is there.
void ApplyNotifyPatch() {
	HANDLE xamHandle = GetModuleHandleA("xam.xex");
	DWORD notify = 0;
	if (!xamHandle || XexGetProcedureAddress(xamHandle, 1183, &notify) != 0 || !notify) {
		DbgPrint("EINTIM: notify patch: xam ordinal 1183 not found\n");
		return;
	}
	short* site = (short*)(notify + 48);
	DbgPrint("EINTIM: notify patch: ordinal 1183 at %08X, +48 = %04X\n", notify, (unsigned short)*site);
	if (*site == 0x409A) {
		*site = 0x4800;
		FlushCodeRange(site, sizeof(*site));
		DbgPrint("EINTIM: notify patch applied, readback %04X\n", (unsigned short)*site);
	} else if (*site == 0x4800) {
		DbgPrint("EINTIM: notify patch already present (JRPC2 or a previous load)\n");
	} else {
		DbgPrint("EINTIM: notify patch skipped, unexpected instruction\n");
	}
}

// Read-only hook on the kernel function that picks the class driver for each USB
// interface (17150: 0x800D7170). Logs what was plugged and which driver took it.
Detour UsbMatchDetour;
typedef DWORD (*usb_match_func_t)(BYTE* device, BYTE* iface);

const char* UsbDriverName(DWORD entry) {
	switch (entry) {
	case 0: return "nenhum driver";
	case 0x80162254: return "HID (HidAddDevice)";
	case 0x80162238: return "mass storage";
	case 0x8016228C: return "classe 6 / composto";
	case 0x801622B8: return "camera (classe E)";
	case 0x801626C8: case 0x80162634: case 0x80162578: case 0x80162618:
	case 0x80162934: case 0x80162A00: case 0x80162960: case 0x801625D0: return "XInput (FF/5D)";
	case 0x8016CFC4: return "lista especial";
	}
	if (entry >= 0x90000000 && entry < 0xA0000000)
		return "lista dinamica (outro modulo)";
	return "outro";
}

DWORD UsbMatchHook(BYTE* device, BYTE* iface) {
	COUNT_HIT(HIT_IFMATCH);
	DWORD entry = UsbMatchDetour.GetOriginal<usb_match_func_t>()(device, iface);
	const BYTE* dd = device + 0x4C; // device descriptor, little endian
	DbgPrintSync("EINTIM: USB match: VID %04X PID %04X if %d class %02X/%02X/%02X -> %08X %s\n",
		dd[8] | (dd[9] << 8), dd[10] | (dd[11] << 8), iface[2], iface[5], iface[6], iface[7], entry, UsbDriverName(entry));
	return entry;
}

// Device-level selection (17150: 0x800D70D8), used instead of the interface-level one when
// the node is a whole device; it also consults the dynamic claim list.
Detour UsbDeviceMatchDetour;
typedef DWORD (*usb_device_match_func_t)(BYTE* device, BYTE* iface);

DWORD UsbDeviceMatchHook(BYTE* device, BYTE* iface) {
	COUNT_HIT(HIT_DEVMATCH);
	DWORD entry = UsbDeviceMatchDetour.GetOriginal<usb_device_match_func_t>()(device, iface);
	const BYTE* dd = device + 0x4C;
	DbgPrintSync("EINTIM: USB device match: VID %04X PID %04X device class %02X/%02X/%02X -> %08X %s\n",
		dd[8] | (dd[9] << 8), dd[10] | (dd[11] << 8), dd[4], dd[5], dd[6], entry, UsbDriverName(entry));
	return entry;
}

// Logs devices that end up rejected (no driver took them, or a driver refused them)
Detour UsbdAddDeviceCompleteDetour;

int UsbdAddDeviceCompleteHook(deviceHandle* handle, int status) {
	COUNT_HIT(HIT_ADDCOMPLETE);
	if (status != 0) {
		usb_device_descriptor* dd = UsbdGetDeviceDescriptor(handle);
		DbgPrintSync("EINTIM: USB add complete: handle %p status %X (rejeitado) VID %04X PID %04X\n", handle, status,
			dd ? swap_endianness_16(dd->idVendor) : 0, dd ? swap_endianness_16(dd->idProduct) : 0);
	}
	return UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(handle, status);
}

// Enumeration steps before driver selection, to see where a device that never reaches
// "USB match" (the 8BitDo receiver) is dropped. The callbacks get node+0xC and the status of the
// request; the discard routine gets the node. The device descriptor is inline at node+0x4C.
extern "C" void* _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)

static void NodeIds(const BYTE* node, DWORD* vid, DWORD* pid) {
	const BYTE* dd = node + 0x4C;
	*vid = dd[8] | (dd[9] << 8);
	*pid = dd[10] | (dd[11] << 8);
}

Detour UsbMsOsReplyDetour;      // 0x800D8148: reply to the string 0xEE ("MSFT100") request
Detour UsbCompatReplyDetour;    // 0x800D80A0: reply to the Extended Compat ID request
Detour UsbDiscardDetour;        // 0x800D7D00: drops (or retries) a device
typedef void (*usb_reply_func_t)(BYTE* nodePlusC, DWORD status);
typedef void (*usb_discard_func_t)(BYTE* node);

void UsbMsOsReplyHook(BYTE* nodePlusC, DWORD status) {
	DWORD vid, pid;
	NodeIds(nodePlusC - 0xC, &vid, &pid);
	DbgPrintSync("EINTIM: USB 0xEE reply: VID %04X PID %04X status %08X\n", vid, pid, status);
	UsbMsOsReplyDetour.GetOriginal<usb_reply_func_t>()(nodePlusC, status);
}

void UsbCompatReplyHook(BYTE* nodePlusC, DWORD status) {
	DWORD vid, pid;
	NodeIds(nodePlusC - 0xC, &vid, &pid);
	DbgPrintSync("EINTIM: USB compat ID reply: VID %04X PID %04X status %08X\n", vid, pid, status);
	UsbCompatReplyDetour.GetOriginal<usb_reply_func_t>()(nodePlusC, status);
}

// Completion callbacks of the enumeration requests right before the 0xEE probe. The 8BitDo
// receiver is discarded from the SET_CONFIGURATION one (0x800D83F8), so log their status.
Detour UsbSetConfigDoneDetour;    // 0x800D83F8: SET_CONFIGURATION (request set up at 0x800D84F0)
Detour UsbConfigDescDoneDetour;   // 0x800D8468: GET_DESCRIPTOR(configuration)

// Time base ticks at the end of the config descriptor callback, which is where the original
// sends SET_CONFIGURATION. The Xbox 360 time base runs at about 50 MHz (50000 ticks per ms).
#define TB_TICKS_PER_MS 50000
volatile DWORD g_setConfigSentTb = 0;
volatile BYTE* g_setConfigSentNode = nullptr;

bool StartHidInit(BYTE* node); // 8BitDo: SET_IDLE + report descriptor after SET_CONFIGURATION

void UsbSetConfigDoneHook(BYTE* nodePlusC, DWORD status) {
	DWORD now = __mftb32();
	DWORD vid, pid;
	BYTE* node = nodePlusC - 0xC;
	NodeIds(node, &vid, &pid);
	DWORD ms = g_setConfigSentNode == node ? (now - g_setConfigSentTb) / TB_TICKS_PER_MS : 0xFFFFFFFF;
	DbgPrintSync("EINTIM: USB SET_CONFIGURATION done: VID %04X PID %04X config %d status %08X after %d ms\n", vid, pid, node[0x65], status, ms);
	if (vid == 0x2DC8 && status == 0 && StartHidInit(node))
		return;

	UsbSetConfigDoneDetour.GetOriginal<usb_reply_func_t>()(nodePlusC, status);
}

// The 8BitDo drops off the bus when it gets SET_CONFIGURATION from the Xbox (issue #1). Linux,
// where it works, asks for the string descriptors between the configuration descriptor and
// SET_CONFIGURATION; the Xbox does not. For VID 2DC8 only, ask for them the way Linux does
// (language list, product, manufacturer, serial, wLength 255) and only then let the original
// callback send SET_CONFIGURATION.
//
// Each request follows the kernel's own pattern (0x800D84F0, 0x800D85AC, 0x800D8318): setup packet
// at node+0x2C, completion callback at node+0x10, data pointer/size at node+0x20/0x24, direction at
// node+0x1C, submit with 0x800D9190(info, node+0xC), then arm the 5 s timer at node+0x34. Every
// completion first cancels that timer. The data goes to the node's scratch buffer at node+0x1B4
// (0xC4 bytes before the node fields at 0x278); the configuration itself is kept at node+0x60.
typedef void (*usb_submit_func_t)(BYTE* info, BYTE* request);
typedef DWORD (*usb_timer_cancel_func_t)(BYTE* timer);
typedef void (*usb_timer_arm_func_t)(BYTE* timer, DWORD ms);
static const usb_submit_func_t UsbSubmitRequest = (usb_submit_func_t)0x800D9190;
static const usb_timer_cancel_func_t UsbTimerCancel = (usb_timer_cancel_func_t)0x800DA170;
static const usb_timer_arm_func_t UsbTimerArm = (usb_timer_arm_func_t)0x800DA2B8;
static const usb_discard_func_t UsbDiscardOriginal = (usb_discard_func_t)0x800D7D00; // goes through our logging hook

#define STRING_BUFFER_OFFSET 0x1B4
// wLength 255 is what Linux asks for. The host controller sizes the transfer from the buffer
// size (node+0x24), not from wLength, so the buffer is capped at the 0xC4 free bytes before the
// node fields at 0x278: a longer reply becomes a transfer error instead of corrupting the node.
// The longest 8BitDo string ("Ultimate C 2.4G Wireless Controller ") is 74 bytes.
#define STRING_REQUEST_LENGTH 0xFF
#define STRING_BUFFER_SIZE 0xC4
bool g_stringRequestsReady = false; // set once the addresses above were checked

struct StringSequence {
	BYTE* node;      // node being handled, nullptr when idle
	int step;
	WORD langid;
	// node fields touched by the string requests, restored before SET_CONFIGURATION
	BYTE dir;
	DWORD data, size, length;
};
StringSequence g_strings;

// Linux order: language list first (usb_get_langid), then product, manufacturer, serial
static const BYTE kStringIndexes[] = { 0, 2, 1, 3 };

void UsbStringDoneCallback(BYTE* nodePlusC, DWORD status);

void SubmitStringRequest(BYTE* node, BYTE index, WORD langid) {
	node[0x2C] = 0x80;                 // device to host, standard, device
	node[0x2D] = 6;                    // GET_DESCRIPTOR
	node[0x2E] = index;                // wValue (little endian): index, type 3 = string
	node[0x2F] = 3;
	node[0x30] = (BYTE)(langid & 0xFF); // wIndex = language
	node[0x31] = (BYTE)(langid >> 8);
	node[0x32] = STRING_REQUEST_LENGTH; // wLength (little endian)
	node[0x33] = 0;
	*(DWORD*)(node + 0x10) = (DWORD)UsbStringDoneCallback;
	*(DWORD*)(node + 0x20) = (DWORD)(node + STRING_BUFFER_OFFSET);
	*(DWORD*)(node + 0x24) = STRING_BUFFER_SIZE;
	node[0x1C] = 1;
	UsbSubmitRequest(*(BYTE**)(node + 4), node + 0xC);
	UsbTimerArm(node + 0x34, 5000);
}

void UsbStringDoneCallback(BYTE* nodePlusC, DWORD status) {
	BYTE* node = nodePlusC - 0xC;
	UsbTimerCancel(node + 0x34);
	const BYTE* info = *(const BYTE**)(node + 4);
	const BYTE* buf = node + STRING_BUFFER_OFFSET;
	DWORD got = *(DWORD*)(node + 0x28);
	BYTE index = kStringIndexes[g_strings.step];
	DbgPrintSync("EINTIM: 8BitDo: string %d done status %08X, %d bytes (bLength %02X type %02X), removed %02X\n",
		index, status, got, buf[0], buf[1], info[8] & 0x80);

	if (info[8] & 0x80) {
		DbgPrintSync("EINTIM: 8BitDo: device gone during the string requests\n");
		g_strings.node = nullptr;
		UsbDiscardOriginal(node);
		return;
	}
	if (index == 0 && status == 0 && got >= 4 && buf[1] == 3)
		g_strings.langid = buf[2] | (buf[3] << 8);

	g_strings.step++;
	if (g_strings.step < (int)sizeof(kStringIndexes)) {
		SubmitStringRequest(node, kStringIndexes[g_strings.step], g_strings.langid);
		return;
	}

	// done: put back what the original expects and let it send SET_CONFIGURATION
	node[0x1C] = g_strings.dir;
	*(DWORD*)(node + 0x20) = g_strings.data;
	*(DWORD*)(node + 0x24) = g_strings.size;
	*(DWORD*)(node + 0x28) = g_strings.length;
	g_strings.node = nullptr;
	DbgPrintSync("EINTIM: 8BitDo: strings done, handing over to SET_CONFIGURATION\n");
	UsbConfigDescDoneDetour.GetOriginal<usb_reply_func_t>()(nodePlusC, 0);
	g_setConfigSentNode = node;
	g_setConfigSentTb = __mftb32();
}

// After SET_CONFIGURATION the 8BitDo is configured but sends no input on the Xbox (one empty
// completion, then nothing). Linux's usbhid_parse, where it works, sends SET_IDLE(0) to the
// interface and reads the HID report descriptor before polling the interrupt endpoint. For VID
// 2DC8 only, do the same right after SET_CONFIGURATION succeeds, then let the original
// SET_CONFIGURATION callback carry on to driver selection. Same request mechanism as the strings.
struct HidInitSequence {
	BYTE* node;      // node being handled, nullptr when idle
	int step;        // 0 = SET_IDLE, 1 = GET_DESCRIPTOR(report)
	BYTE iface;
	WORD reportLength;
	BYTE dir;
	DWORD data, size, length;
};
HidInitSequence g_hidInit;

void UsbHidInitDoneCallback(BYTE* nodePlusC, DWORD status);

void SubmitHidInitRequest(BYTE* node) {
	BYTE iface = g_hidInit.iface;
	if (g_hidInit.step == 0) {
		// SET_IDLE: class request to the interface, duration 0 (only report on change), all reports
		node[0x2C] = 0x21; node[0x2D] = 0x0A;
		node[0x2E] = 0; node[0x2F] = 0;
		node[0x30] = iface; node[0x31] = 0;
		node[0x32] = 0; node[0x33] = 0;
		*(DWORD*)(node + 0x20) = 0;
		*(DWORD*)(node + 0x24) = 0;
		node[0x1C] = 0;
	} else {
		// GET_DESCRIPTOR(report) from the interface, into the scratch buffer
		WORD len = g_hidInit.reportLength;
		node[0x2C] = 0x81; node[0x2D] = 6;
		node[0x2E] = 0; node[0x2F] = 0x22;
		node[0x30] = iface; node[0x31] = 0;
		node[0x32] = (BYTE)(len & 0xFF); node[0x33] = (BYTE)(len >> 8);
		*(DWORD*)(node + 0x20) = (DWORD)(node + STRING_BUFFER_OFFSET);
		*(DWORD*)(node + 0x24) = len;
		node[0x1C] = 1;
	}
	*(DWORD*)(node + 0x10) = (DWORD)UsbHidInitDoneCallback;
	UsbSubmitRequest(*(BYTE**)(node + 4), node + 0xC);
	UsbTimerArm(node + 0x34, 5000);
}

void UsbHidInitDoneCallback(BYTE* nodePlusC, DWORD status) {
	BYTE* node = nodePlusC - 0xC;
	UsbTimerCancel(node + 0x34);
	const BYTE* info = *(const BYTE**)(node + 4);
	const BYTE* buf = node + STRING_BUFFER_OFFSET;
	DbgPrintSync("EINTIM: 8BitDo: %s done status %08X, %d bytes (%02X %02X %02X %02X), removed %02X\n",
		g_hidInit.step == 0 ? "SET_IDLE" : "report descriptor", status, *(DWORD*)(node + 0x28),
		buf[0], buf[1], buf[2], buf[3], info[8] & 0x80);

	if (info[8] & 0x80) {
		DbgPrintSync("EINTIM: 8BitDo: device gone during the HID init\n");
		g_hidInit.node = nullptr;
		UsbDiscardOriginal(node);
		return;
	}

	g_hidInit.step++;
	if (g_hidInit.step < 2) {
		SubmitHidInitRequest(node);
		return;
	}

	node[0x1C] = g_hidInit.dir;
	*(DWORD*)(node + 0x20) = g_hidInit.data;
	*(DWORD*)(node + 0x24) = g_hidInit.size;
	*(DWORD*)(node + 0x28) = g_hidInit.length;
	g_hidInit.node = nullptr;
	DbgPrintSync("EINTIM: 8BitDo: HID init done, handing over to driver selection\n");
	UsbSetConfigDoneDetour.GetOriginal<usb_reply_func_t>()(nodePlusC, 0);
}

// Called from UsbSetConfigDoneHook. Returns true when it took over (the original runs later).
bool StartHidInit(BYTE* node) {
	const BYTE* info = *(const BYTE**)(node + 4);
	if (!g_stringRequestsReady || (g_hidInit.node && g_hidInit.node != node) || (info[8] & 0x80))
		return false;
	// configuration copy at node+0x60: config (9) + interface (9) + HID descriptor (9)
	const BYTE* iface = node + 0x60 + 9;
	const BYTE* hid = iface + 9;
	if (iface[1] != 4 || iface[5] != 3 || hid[1] != 0x21) {
		DbgPrintSync("EINTIM: 8BitDo: no HID descriptor right after the interface (%02X %02X %02X), skipping HID init\n", iface[1], iface[5], hid[1]);
		return false;
	}
	WORD len = hid[7] | (hid[8] << 8);
	if (len == 0 || len > STRING_BUFFER_SIZE)
		len = STRING_BUFFER_SIZE;
	UsbTimerCancel(node + 0x34);
	g_hidInit.node = node;
	g_hidInit.step = 0;
	g_hidInit.iface = iface[2];
	g_hidInit.reportLength = len;
	g_hidInit.dir = node[0x1C];
	g_hidInit.data = *(DWORD*)(node + 0x20);
	g_hidInit.size = *(DWORD*)(node + 0x24);
	g_hidInit.length = *(DWORD*)(node + 0x28);
	DbgPrintSync("EINTIM: 8BitDo: SET_IDLE and report descriptor (%d bytes) on interface %d before driver selection\n", len, iface[2]);
	SubmitHidInitRequest(node);
	return true;
}

void UsbConfigDescDoneHook(BYTE* nodePlusC, DWORD status) {
	DWORD vid, pid;
	BYTE* node = nodePlusC - 0xC;
	NodeIds(node, &vid, &pid);
	DbgPrintSync("EINTIM: USB config descriptor done: VID %04X PID %04X status %08X\n", vid, pid, status);

	const BYTE* info = *(const BYTE**)(node + 4);
	// A new configuration completion on the node we were handling is a new enumeration: start over
	if (vid == 0x2DC8 && status == 0 && g_stringRequestsReady && (!g_strings.node || g_strings.node == node) && !(info[8] & 0x80)) {
		UsbTimerCancel(node + 0x34); // the original would do this first thing
		g_strings.node = node;
		g_strings.step = 0;
		g_strings.langid = 0x0409;
		g_strings.dir = node[0x1C];
		g_strings.data = *(DWORD*)(node + 0x20);
		g_strings.size = *(DWORD*)(node + 0x24);
		g_strings.length = *(DWORD*)(node + 0x28);
		DbgPrintSync("EINTIM: 8BitDo: asking for the strings before SET_CONFIGURATION (config length %d)\n", g_strings.length);
		SubmitStringRequest(node, kStringIndexes[0], 0);
		return;
	}

	UsbConfigDescDoneDetour.GetOriginal<usb_reply_func_t>()(nodePlusC, status);
	// the original has just queued SET_CONFIGURATION (0x800D84F0-0x800D8528) if status was ok
	g_setConfigSentNode = nodePlusC - 0xC;
	g_setConfigSentTb = __mftb32();
}

void UsbDiscardHook(BYTE* node) {
	DWORD vid, pid;
	NodeIds(node, &vid, &pid);
	if (node == g_strings.node)
		g_strings.node = nullptr; // discarded by a path that never reached our string callback
	if (node == g_hidInit.node)
		g_hidInit.node = nullptr;
	DbgPrintSync("EINTIM: USB discard: node %p VID %04X PID %04X retry flag %d, called from %p\n",
		node, vid, pid, node[0x27F], _ReturnAddress());
	UsbDiscardDetour.GetOriginal<usb_discard_func_t>()(node);
}

// Letters after the stage number in hiddriver_etapa.txt skip hooks, to bisect a freeze:
// a = HidAddDevice/HidRemoveDevice, m = USB match logger, i = XamInactivityDetect,
// g = XamInputGetState, s = XamInputSetState, c = XamInputGetCapabilitiesEx
char g_skipHooks[16] = "";
bool HookEnabled(char letter) { return strchr(g_skipHooks, letter) == nullptr; }

bool InitDriver(bool resetUsb) {
	if (!initFunctionPointers())
		return false;

	if (isKernel17150 && HookEnabled('m')) {
#if HIDDRIVER_DIAG
		if (Expect(0x800D7170, 0x7D8802A6, 0, "UsbMatch (prologo)") && Expect(0x800D7190, 0x8B7E0005, 0, "UsbMatch (lbz class)")) {
			UsbMatchDetour = Detour((void*)0x800D7170, (void*)UsbMatchHook);
			UsbMatchDetour.Install();
			LogReadback("UsbMatch", (void*)0x800D7170);
			DbgPrint("EINTIM: hook on: USB match logger\n");
		}
		if (Expect(0x800D70D8, 0x7D8802A6, 0, "UsbDeviceMatch (prologo)") && Expect(0x800D7110, 0x2B0A0009, 0, "UsbDeviceMatch (hub)")) {
			UsbDeviceMatchDetour = Detour((void*)0x800D70D8, (void*)UsbDeviceMatchHook);
			UsbDeviceMatchDetour.Install();
			LogReadback("UsbDeviceMatch", (void*)0x800D70D8);
			DbgPrint("EINTIM: hook on: USB device match logger\n");
		}
		if (Expect(0x800D8148, 0x7D8802A6, 0, "UsbMsOsReply") && Expect(0x800D8154, 0x3BE3FFF4, 0, "UsbMsOsReply (node)")) {
			UsbMsOsReplyDetour = Detour((void*)0x800D8148, (void*)UsbMsOsReplyHook);
			UsbMsOsReplyDetour.Install();
			LogReadback("UsbMsOsReply", (void*)0x800D8148);
		}
		if (Expect(0x800D80A0, 0x7D8802A6, 0, "UsbCompatReply") && Expect(0x800D80AC, 0x3BE3FFF4, 0, "UsbCompatReply (node)")) {
			UsbCompatReplyDetour = Detour((void*)0x800D80A0, (void*)UsbCompatReplyHook);
			UsbCompatReplyDetour.Install();
			LogReadback("UsbCompatReply", (void*)0x800D80A0);
		}
#endif // HIDDRIVER_DIAG
		// these three carry the 8BitDo fix (strings, SET_IDLE + report descriptor) in every build
		if (Expect(0x800D83F8, 0x7D8802A6, 0, "UsbSetConfigDone") && Expect(0x800D8404, 0x3BE3FFF4, 0, "UsbSetConfigDone (node)")) {
			UsbSetConfigDoneDetour = Detour((void*)0x800D83F8, (void*)UsbSetConfigDoneHook);
			UsbSetConfigDoneDetour.Install();
			LogReadback("UsbSetConfigDone", (void*)0x800D83F8);
		}
		if (Expect(0x800D8468, 0x7D8802A6, 0, "UsbConfigDescDone") && Expect(0x800D8474, 0x3BE3FFF4, 0, "UsbConfigDescDone (node)")) {
			UsbConfigDescDoneDetour = Detour((void*)0x800D8468, (void*)UsbConfigDescDoneHook);
			UsbConfigDescDoneDetour.Install();
			LogReadback("UsbConfigDescDone", (void*)0x800D8468);
		}
		g_stringRequestsReady = Expect(0x800D9190, 0x3D60801A, 0, "UsbSubmitRequest") &&
			Expect(0x800D9194, 0x89230008, 0, "UsbSubmitRequest (lbz)") &&
			Expect(0x800DA170, 0x7D8802A6, 0, "UsbTimerCancel") &&
			Expect(0x800DA180, 0xE9630010, 0, "UsbTimerCancel (ld)") &&
			Expect(0x800DA2B8, 0x3D60800E, 0, "UsbTimerArm") &&
			Expect(0x800DA2C0, 0x91630004, 0, "UsbTimerArm (stw)");
		DbgPrint("EINTIM: 8BitDo string requests %s\n", g_stringRequestsReady ? "enabled" : "DISABLED (addresses differ)");
		if (Expect(0x800D7D00, 0x7D8802A6, 0, "UsbDiscard") && Expect(0x800D7D20, 0x897F027F, 0, "UsbDiscard (retry flag)")) {
			UsbDiscardDetour = Detour((void*)0x800D7D00, (void*)UsbDiscardHook);
			UsbDiscardDetour.Install();
			LogReadback("UsbDiscard", (void*)0x800D7D00);
		}
#if HIDDRIVER_DIAG
		if (Expect((DWORD)UsbdAddDeviceComplete, 0x7D8802A6, 0, "UsbdAddDeviceComplete")) {
			UsbdAddDeviceCompleteDetour = Detour((void*)UsbdAddDeviceComplete, (void*)UsbdAddDeviceCompleteHook);
			UsbdAddDeviceCompleteDetour.Install();
			LogReadback("UsbdAddDeviceComplete", (void*)UsbdAddDeviceComplete);
			DbgPrint("EINTIM: hook on: UsbdAddDeviceComplete logger\n");
		}
#endif
	}

	if (isKernel17150) {
		HidAddDeviceDetour = Detour((void*)0x800E5CD0, (void*)HidAddDeviceHook);
		HidRemoveDeviceDetour = Detour((void*)0x800E5C90, (void*)HidRemoveDeviceHook);
		XamInactivityDetectRecentActivityDetour = Detour((void*)0x81695268, (void*)XamInactivityDetectRecentActivityHook);
	}
	else if (isDevkit) {
		HidAddDeviceDetour = Detour((void*)0x8011AE38, (void*)HidAddDeviceHook); // 7D 88 02 A6 ? ? ? ? 94 21 ? ? 7C 7C 1B 78 ? ? ? ? 7C 7F 1B 79
		HidRemoveDeviceDetour = Detour((void*)0x8011ADF8, (void*)HidRemoveDeviceHook); // 81 63 ? ? 39 40 ? ? 39 20 ? ? 99 4B
		XamInactivityDetectRecentActivityDetour = Detour((void*)0x81750588, (void*)XamInactivityDetectRecentActivityHook); // 3D 60 81 ?? 3D 40 81 ?? E8 6B ?? ?? E9 6A ?? ?? 7F 23 58 40 40 98 00 0C
	}
	else {
		HidAddDeviceDetour = Detour((void*)0x800E4D68, (void*)HidAddDeviceHook); // 7D 88 02 A6 ? ? ? ? 94 21 ? ? 7C 7B 1B 78 ? ? ? ? 7C 7F 1B 79
		HidRemoveDeviceDetour = Detour((void*)0x800E4D28, (void*)HidRemoveDeviceHook); // 81 63 ? ? 39 40 ? ? 39 20 ? ? 99 4B
		XamInactivityDetectRecentActivityDetour = Detour((void*)0x81695DE8, (void*)XamInactivityDetectRecentActivityHook); // 3D 60 81 ?? 3D 40 81 ?? E8 6B ?? ?? E9 6A ?? ?? 7F 23 58 40 40 98 00 0C
	}

	XamInputGetCapabilitiesDetour = Detour(XamInputGetCapabilitiesEx, (void*)XamInputGetCapabilitiesExHook);
	XamInputGetStateDetour = Detour(XamInputGetState, (void*)XamInputGetStateHook);
	XamInputSetStateDetour = Detour(XamInputSetState, (void*)XamInputSetStateHook);

	if (HookEnabled('a')) { HidAddDeviceDetour.Install(); HidRemoveDeviceDetour.Install(); DbgPrint("EINTIM: hook on: HidAddDevice/HidRemoveDevice\n"); }
	if (HookEnabled('g')) { XamInputGetStateDetour.Install(); DbgPrint("EINTIM: hook on: XamInputGetState\n"); }
	if (HookEnabled('s')) { XamInputSetStateDetour.Install(); DbgPrint("EINTIM: hook on: XamInputSetState\n"); }
	if (HookEnabled('c')) { XamInputGetCapabilitiesDetour.Install(); DbgPrint("EINTIM: hook on: XamInputGetCapabilitiesEx\n"); }
	if (HookEnabled('i')) { XamInactivityDetectRecentActivityDetour.Install(); DbgPrint("EINTIM: hook on: XamInactivityDetectRecentActivity\n"); }
	if (isKernel17150) {
		LogReadback("HidAddDevice", (void*)0x800E5CD0);
		LogReadback("HidRemoveDevice", (void*)0x800E5C90);
		LogReadback("XamInactivityDetect", (void*)0x81695268);
	}
	LogReadback("XamInputGetState", XamInputGetState);
	DbgPrint("EINTIM: Hooks installed (skipped: \"%s\")\n", g_skipHooks);

	if (!resetUsb) {
		DbgPrint("EINTIM: Stage 2: USB reset skipped\n");
		return true;
	}

	if (isKernel17150)
		ApplyUsbResetPatches17150();

	DbgPrint("EINTIM: Resetting USB driver!\n");
#if HIDDRIVER_DIAG
	DiagFlush();
#endif
	UsbdPowerDownNotification();
	//For some reason microsoft doesnt clean up this page by themselves in the shutdown notification, so ill do it for them, call me mr nice guy :)
	MmFreePhysicalMemory(0, *(DWORD*)UsbPhysicalPage);
	DbgPrint("EINTIM: USB driver shutdown complete.\n");
	UsbdDriverEntry();
	DbgPrint("EINTIM: USB driver reset complete.\n");
	return true;
}

#if HIDDRIVER_DIAG
// Shows the result once the dashboard is up. Runs on its own thread: in every earlier log,
// nothing was ever written after XNotifyQueueUI, so it may never return here.
unsigned int __stdcall NotifyThread(void*) {
	Sleep(15000);
	DbgPrint("EINTIM: notify: calling XNotifyQueueUI \"%S\"\n", g_notifyMessage);
	Notify(g_notifyMessage);
	DbgPrint("EINTIM: notify: XNotifyQueueUI returned\n");
	return 0;
}

// Flushes the log every 10 ms, plus a heartbeat with the hook counters every 5 s
unsigned int __stdcall FlushThread(void*) {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	DWORD lastBeat = GetTickCount();
	while (true) {
		DWORD now = GetTickCount();
		if (now - lastBeat >= 5000) {
			lastBeat = now;
			DbgPrint("EINTIM: alive t=%u write=%d read=%d hits dev=%d if=%d addc=%d hidadd=%d hidrem=%d inact=%d get=%d set=%d caps=%d reports=%d\n",
				now / 1000, g_diagWrite, g_diagRead, g_hits[HIT_DEVMATCH], g_hits[HIT_IFMATCH], g_hits[HIT_ADDCOMPLETE],
				g_hits[HIT_HIDADD], g_hits[HIT_HIDREMOVE], g_hits[HIT_INACTIVITY], g_hits[HIT_GETSTATE],
				g_hits[HIT_SETSTATE], g_hits[HIT_CAPS], g_hits[HIT_REPORT]);
		}
		DiagFlush();
		Sleep(10);
	}
	return 0;
}

#endif // HIDDRIVER_DIAG

BOOL APIENTRY DllMain(HANDLE Handle, DWORD Reason, PVOID Reserved)
{
	if (Reason == DLL_PROCESS_ATTACH)
	{
		if (XboxKrnlVersion->Build != 17559 && XboxKrnlVersion->Build != 17489 && XboxKrnlVersion->Build != 17150)
			return FALSE;

#if !HIDDRIVER_DIAG
		// Use build, as upstream: open disc tray = don't start; otherwise install and go
		if (IsTrayOpen())
			return FALSE;
		g_stage = HIDDRIVER_STAGE;
		if (g_stage >= 2)
			InitDriver(g_stage >= 3);
		return TRUE;
#else
		MountDiagDrives();
		char stageText[16];
		ReadHddFileText(STAGE_FILE, stageText, sizeof(stageText));
		g_stage = (stageText[0] >= '1' && stageText[0] <= '3') ? stageText[0] - '0' : 1;
		for (int i = 1, n = 0; stageText[0] && stageText[i] && n < (int)sizeof(g_skipHooks) - 1; i++)
			if (stageText[i] >= 'a' && stageText[i] <= 'z')
				g_skipHooks[n++] = stageText[i];
		DbgPrint("EINTIM: HELLO from xbox 360 HID controller driver version 0.5 (port 17150), kernel %d, stage %d, skip \"%s\", log root %s\n",
			XboxKrnlVersion->Build, g_stage, g_skipHooks, g_logRoot ? g_logRoot : "(none)");

		if (IsTrayOpen() || DiagFileExists(KILL_SWITCH_FILE)) {
			DbgPrint("EINTIM: Disc tray open or " KILL_SWITCH_FILE " found. Not starting.\n");
			DiagFlush();
			return FALSE;
		}

		ApplyNotifyPatch();

		if (g_stage == 1) {
			g_notifyMessage = L"hiddriver: etapa 1 ativa (so log)";
		} else if (InitDriver(g_stage >= 3)) {
			g_notifyMessage = g_stage == 2 ? L"hiddriver: etapa 2 ativa (ganchos, sem reset USB)"
				: L"hiddriver: etapa 3 ativa (ganchos + reset USB)";
		} else {
			g_notifyMessage = L"hiddriver: enderecos nao conferem, nada foi alterado";
		}
		DiagFlush();
		MakeThread((LPTHREAD_START_ROUTINE)FlushThread, nullptr);
		MakeThread((LPTHREAD_START_ROUTINE)NotifyThread, nullptr);
#endif // HIDDRIVER_DIAG
	}
	return TRUE;
}
