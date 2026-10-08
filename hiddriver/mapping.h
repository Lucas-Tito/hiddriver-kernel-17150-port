#pragma once
#include <stdint.h>

// Controller mappings made by the mapping assistant, kept in hiddriver.json at the HDD root.
// The file format is upstream's (master, mapping.cpp), so a file made by either one works on
// the other. Unlike upstream, everything is fixed size and only the mapper thread touches it:
// the USB callbacks work on a copy inside their controller slot.

// What a HID button can be mapped to. Names in the JSON: a_button b_button x_button y_button
// l1 r1 l2 r2 back start l3 r3 xbox dpad_left dpad_right dpad_up dpad_down.
enum MapTarget {
	MAP_A, MAP_B, MAP_X, MAP_Y,
	MAP_LB, MAP_RB, MAP_LT, MAP_RT,
	MAP_BACK, MAP_START, MAP_L3, MAP_R3, MAP_GUIDE,
	MAP_DPAD_LEFT, MAP_DPAD_RIGHT, MAP_DPAD_UP, MAP_DPAD_DOWN,
	MAP_TARGET_COUNT
};
#define MAP_NONE 0xFF

// Axis fields, upstream's names: x y = left stick, z rz = right stick, rx ry = analog triggers.
// Each is fed by a Generic Desktop usage (0x30 X .. 0x35 Rz).
enum MapAxis { AXIS_X, AXIS_Y, AXIS_Z, AXIS_RX, AXIS_RY, AXIS_RZ, MAP_AXIS_COUNT };

struct ControllerMapping {
	uint16_t vendorId;
	uint16_t productId;
	uint8_t button[MAP_TARGET_COUNT]; // HID button index (usage - 1) for each target, MAP_NONE if unmapped
	uint8_t axisUsage[MAP_AXIS_COUNT]; // Generic Desktop usage feeding each axis field, 0 if none
	bool invert[MAP_AXIS_COUNT];       // upstream's flags: true on y and rz turns HID "down" into Xbox "up"
};

// Upstream's defaults for a new mapping: usages X..Rz on the fields of the same name, y and rz inverted
void InitDefaultMapping(ControllerMapping* m, uint16_t vid, uint16_t pid);

int LoadMappingsFromFile(const char* path); // 1 loaded, 0 no file, -1 unreadable or invalid
bool SaveMappingsToFile(const char* path);
const ControllerMapping* FindMapping(uint16_t vid, uint16_t pid);
bool StoreMapping(const ControllerMapping& m); // adds or replaces the one with the same VID/PID
int MappingCount();
