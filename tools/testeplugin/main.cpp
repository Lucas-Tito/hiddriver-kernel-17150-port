// testeplugin: app launched from FreeStyle that copies the running kernel and xam images
// into game:\kerneldump, for porting hiddriver to another kernel. Loading the kerneldump
// plugin from a title was refused with STATUS_ACCESS_DENIED, so the app copies by itself.
//
// Two channels, like CollectionUI's first test: the screen color answers "did it work?"
// from the couch, and game:\testeplugin.log answers "why not?" on the PC.
//   yellow = starting, blue = copying, green = copy done, red = copy failed
#include "../kerneldump/dump_core.h"

// Each line opens, appends and closes the file: a crash can't lose lines that
// were only buffered or never made it into the FAT directory entry.
static void Log(const char* fmt, ...) {
	FILE* f = fopen("game:\\testeplugin.log", "a");
	if (!f)
		return;
	va_list args;
	va_start(args, fmt);
	vfprintf(f, fmt, args);
	va_end(args);
	fputc('\n', f);
	fclose(f);
}

static IDirect3DDevice9* CreateDevice() {
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!d3d)
		return NULL;
	D3DPRESENT_PARAMETERS pp;
	ZeroMemory(&pp, sizeof(pp));
	pp.BackBufferWidth = 1280;
	pp.BackBufferHeight = 720;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.BackBufferCount = 1;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
	IDirect3DDevice9* device = NULL;
	if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device)))
		return NULL;
	return device;
}

static void Paint(IDirect3DDevice9* device, D3DCOLOR color) {
	if (!device)
		return;
	device->Clear(0, NULL, D3DCLEAR_TARGET, color, 1.0f, 0);
	device->Present(NULL, NULL, NULL, NULL);
}

static IDirect3DDevice9* g_progressDevice = NULL;

// Logged before each step, so if the console freezes the log shows where
static void Progress(const char* step) {
	Log("  %s", step);
	Paint(g_progressDevice, D3DCOLOR_XRGB(0, 80, 220));
}

void __cdecl main() {
	DeleteFileA("game:\\testeplugin.log");
	Log("testeplugin");
	Log("Kernel %d.%d.%d.%d", XboxKrnlVersion->Major, XboxKrnlVersion->Minor, XboxKrnlVersion->Build, XboxKrnlVersion->Qfe);

	IDirect3DDevice9* device = CreateDevice();
	Log("D3D: %s", device ? "ok" : "falhou");
	for (int i = 0; i < 60; i++)
		Paint(device, D3DCOLOR_XRGB(255, 200, 0));

	Paint(device, D3DCOLOR_XRGB(0, 80, 220));
	CreateDirectoryA("game:\\kerneldump", NULL);
	g_progressDevice = device;
	bool ok = DumpAll("game:\\kerneldump", Progress);
	Log("copia: %s", ok ? "concluida" : "FALHOU ao criar game:\\kerneldump\\info.txt");

	D3DCOLOR color = ok ? D3DCOLOR_XRGB(0, 200, 0) : D3DCOLOR_XRGB(220, 0, 0);
	for (;;)
		Paint(device, color); // never return: leaving main takes the title down
}
