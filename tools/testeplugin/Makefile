# App de teste (título, não DLL). Rode pela raiz: ./tools/build.sh -f tools/testeplugin/Makefile
PROJECT_NAME := testeplugin
WINDOWS_SHIM := wine

SRC_DIR := tools/testeplugin
XKELIB_DIR := hiddriver/xkelib
BUILD_DIR := build/testeplugin
OUT_DIR := $(BUILD_DIR)/bin
INT_DIR := $(BUILD_DIR)/obj

XDK_BIN_DIR := $(XEDK)/bin/win32
CXX := "$(XDK_BIN_DIR)/cl.exe"
LD := "$(XDK_BIN_DIR)/link.exe"
IMAGEXEX := "$(XDK_BIN_DIR)/imagexex.exe"
XDK_INC_DIR := $(XEDK)/include/xbox
XDK_CRT_DIR := $(XEDK)/include/crt
XDK_LIB_DIR := $(XEDK)/lib/xbox

CXX_FLAGS := -c -Zi -nologo -W3 -D NDEBUG -D _XBOX -Ox -Oi -GF -Gy -GS- -MT -Gm- -GR- -TP \
             -fp:fast -Zc:wchar_t -Zc:forScope -Fd"$(INT_DIR)/vc100.pdb" -I"$(XKELIB_DIR)"
LD_FLAGS := -NOLOGO -DEBUG -RELEASE -OPT:REF -STACK:262144,262144 -PDB:"$(OUT_DIR)/$(PROJECT_NAME).pdb" -XEX:NO
LIBS := d3d9.lib xapilib.lib xboxkrnl.lib xkelib.lib

all: $(OUT_DIR)/$(PROJECT_NAME).xex

$(OUT_DIR)/$(PROJECT_NAME).xex: $(OUT_DIR)/$(PROJECT_NAME).exe
	@echo "Creating XEX..."
	@$(WINDOWS_SHIM) $(IMAGEXEX) -nologo -out:"$@" "$<"

$(OUT_DIR)/$(PROJECT_NAME).exe: $(INT_DIR)/main.obj
	@echo "Linking..."
	@mkdir -p $(@D)
	@LIB="$(XDK_LIB_DIR);$(XKELIB_DIR)" $(WINDOWS_SHIM) $(LD) $(LD_FLAGS) -OUT:"$@" $^ $(LIBS)

$(INT_DIR)/main.obj: $(SRC_DIR)/main.cpp
	@mkdir -p $(@D)
	@INCLUDE="$(XDK_INC_DIR);$(XDK_CRT_DIR)" $(WINDOWS_SHIM) $(CXX) $(CXX_FLAGS) -Fo"$@" $<
