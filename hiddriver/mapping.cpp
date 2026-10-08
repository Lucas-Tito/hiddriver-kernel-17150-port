#define RAPIDJSON_ENDIAN RAPIDJSON_BIGENDIAN
#include "mapping.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#define MAX_MAPPINGS 32
#define MAX_JSON_SIZE (64 * 1024)

static ControllerMapping g_mappings[MAX_MAPPINGS];
static int g_mappingCount = 0;

static const char* const kTargetNames[MAP_TARGET_COUNT] = {
	"a_button", "b_button", "x_button", "y_button",
	"l1", "r1", "l2", "r2",
	"back", "start", "l3", "r3", "xbox",
	"dpad_left", "dpad_right", "dpad_up", "dpad_down",
};

static const char* const kAxisNames[MAP_AXIS_COUNT] = { "x", "y", "z", "rx", "ry", "rz" };

static int TargetFromName(const char* name) {
	for (int i = 0; i < MAP_TARGET_COUNT; i++)
		if (strcmp(name, kTargetNames[i]) == 0)
			return i;
	return -1;
}

static int AxisFromName(const char* name) {
	for (int i = 0; i < MAP_AXIS_COUNT; i++)
		if (strcmp(name, kAxisNames[i]) == 0)
			return i;
	return -1;
}

void InitDefaultMapping(ControllerMapping* m, uint16_t vid, uint16_t pid) {
	memset(m, 0, sizeof(*m));
	m->vendorId = vid;
	m->productId = pid;
	memset(m->button, MAP_NONE, sizeof(m->button));
	for (int i = 0; i < MAP_AXIS_COUNT; i++)
		m->axisUsage[i] = (uint8_t)(0x30 + i);
	m->invert[AXIS_Y] = true;
	m->invert[AXIS_RZ] = true;
}

const ControllerMapping* FindMapping(uint16_t vid, uint16_t pid) {
	for (int i = 0; i < g_mappingCount; i++)
		if (g_mappings[i].vendorId == vid && g_mappings[i].productId == pid)
			return &g_mappings[i];
	return nullptr;
}

bool StoreMapping(const ControllerMapping& m) {
	for (int i = 0; i < g_mappingCount; i++) {
		if (g_mappings[i].vendorId == m.vendorId && g_mappings[i].productId == m.productId) {
			g_mappings[i] = m;
			return true;
		}
	}
	if (g_mappingCount >= MAX_MAPPINGS)
		return false;
	g_mappings[g_mappingCount++] = m;
	return true;
}

int MappingCount() {
	return g_mappingCount;
}

static bool ParseMappings(const char* json) {
	using namespace rapidjson;
	Document doc;
	doc.Parse<kParseIterativeFlag>(json); // no recursion: the mapper thread's stack is small
	if (doc.HasParseError() || !doc.IsArray())
		return false;

	g_mappingCount = 0;
	for (SizeType i = 0; i < doc.Size() && g_mappingCount < MAX_MAPPINGS; i++) {
		const Value& entry = doc[i];
		if (!entry.IsObject() || !entry.HasMember("vid") || !entry.HasMember("pid"))
			continue;
		if (!entry["vid"].IsUint() || !entry["pid"].IsUint())
			continue;

		ControllerMapping m;
		InitDefaultMapping(&m, (uint16_t)entry["vid"].GetUint(), (uint16_t)entry["pid"].GetUint());

		// an entry lists only the axes it uses (upstream writes all six)
		if (entry.HasMember("axes") && entry["axes"].IsArray()) {
			memset(m.axisUsage, 0, sizeof(m.axisUsage));
			const Value& axes = entry["axes"];
			for (SizeType j = 0; j < axes.Size(); j++) {
				const Value& a = axes[j];
				if (!a.IsObject() || !a.HasMember("usage") || !a.HasMember("field"))
					continue;
				if (!a["usage"].IsUint() || !a["field"].IsString())
					continue;
				int field = AxisFromName(a["field"].GetString());
				unsigned usage = a["usage"].GetUint();
				if (field >= 0 && usage >= 0x30 && usage <= 0x35)
					m.axisUsage[field] = (uint8_t)usage;
			}
		}

		if (entry.HasMember("buttons") && entry["buttons"].IsArray()) {
			const Value& buttons = entry["buttons"];
			for (SizeType j = 0; j < buttons.Size(); j++) {
				const Value& b = buttons[j];
				if (!b.IsObject() || !b.HasMember("idx") || !b.HasMember("field"))
					continue;
				if (!b["idx"].IsUint() || !b["field"].IsString())
					continue;
				int target = TargetFromName(b["field"].GetString());
				unsigned idx = b["idx"].GetUint();
				if (target >= 0 && idx < MAP_NONE)
					m.button[target] = (uint8_t)idx;
			}
		}

		if (entry.HasMember("invert") && entry["invert"].IsObject()) {
			const Value& inv = entry["invert"];
			for (int a = 0; a < MAP_AXIS_COUNT; a++)
				if (inv.HasMember(kAxisNames[a]) && inv[kAxisNames[a]].IsBool())
					m.invert[a] = inv[kAxisNames[a]].GetBool();
		}

		g_mappings[g_mappingCount++] = m;
	}
	return true;
}

int LoadMappingsFromFile(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return 0;
	char* json = (char*)malloc(MAX_JSON_SIZE + 1);
	if (!json) {
		fclose(f);
		return -1;
	}
	size_t n = fread(json, 1, MAX_JSON_SIZE + 1, f);
	fclose(f);
	bool ok = n <= MAX_JSON_SIZE;
	if (ok) {
		json[n] = 0;
		ok = ParseMappings(json);
	}
	free(json);
	return ok ? 1 : -1;
}

bool SaveMappingsToFile(const char* path) {
	using namespace rapidjson;
	StringBuffer buffer;
	PrettyWriter<StringBuffer> w(buffer);
	w.SetIndent(' ', 2);

	w.StartArray();
	for (int i = 0; i < g_mappingCount; i++) {
		const ControllerMapping& m = g_mappings[i];
		w.StartObject();
		w.Key("vid"); w.Uint(m.vendorId);
		w.Key("pid"); w.Uint(m.productId);

		w.Key("axes");
		w.StartArray();
		for (int a = 0; a < MAP_AXIS_COUNT; a++) {
			if (!m.axisUsage[a])
				continue;
			w.StartObject();
			w.Key("usage"); w.Uint(m.axisUsage[a]);
			w.Key("field"); w.String(kAxisNames[a]);
			w.EndObject();
		}
		w.EndArray();

		w.Key("buttons");
		w.StartArray();
		for (int t = 0; t < MAP_TARGET_COUNT; t++) {
			if (m.button[t] == MAP_NONE)
				continue;
			w.StartObject();
			w.Key("idx"); w.Uint(m.button[t]);
			w.Key("field"); w.String(kTargetNames[t]);
			w.EndObject();
		}
		w.EndArray();

		w.Key("invert");
		w.StartObject();
		for (int a = 0; a < MAP_AXIS_COUNT; a++) {
			w.Key(kAxisNames[a]);
			w.Bool(m.invert[a]);
		}
		w.EndObject();
		w.EndObject();
	}
	w.EndArray();

	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	size_t len = buffer.GetSize();
	bool ok = fwrite(buffer.GetString(), 1, len, f) == len;
	fclose(f);
	return ok;
}
