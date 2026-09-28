# Compila o hiddriver.xex no Linux, dentro do container do XDK (wine).
# Rode pelo tools/build.sh, não direto: ele sobe o container e monta as libs do XDK.
# Flags espelham a config "Release Retail" do hiddriver/hiddriver.vcxproj.

PROJECT_NAME ?= hiddriver

WINDOWS_SHIM := wine

BUILD_DIR ?= build
OUT_DIR := $(BUILD_DIR)/bin
INT_DIR := $(BUILD_DIR)/obj

SRC_DIR ?= hiddriver
XKELIB_DIR := hiddriver/xkelib
SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(SRCS:$(SRC_DIR)/%.cpp=$(INT_DIR)/%.obj)

XDK_BIN_DIR := $(XEDK)/bin/win32
CXX := "$(XDK_BIN_DIR)/cl.exe"
LD := "$(XDK_BIN_DIR)/link.exe"
IMAGEXEX := "$(XDK_BIN_DIR)/imagexex.exe"
XDK_INC_DIR := $(XEDK)/include/xbox
XDK_CRT_DIR := $(XEDK)/include/crt
XDK_LIB_DIR := $(XEDK)/lib/xbox

INCLUDES := -I"$(XKELIB_DIR)" -I"$(SRC_DIR)"

CXX_FLAGS := -c -Zi -nologo -W0 -D NDEBUG -D _XBOX -D LTCG -D _MBCS \
             -Ox -Ob2 -Oi -Ot -GL -GF -Gy -GS- -MT -Gm- -GR- -TP \
             -fp:fast -fp:except- -Zc:wchar_t -Zc:forScope -openmp- \
             -Fd"$(INT_DIR)/vc100.pdb" $(INCLUDES)

LD_FLAGS := -NOLOGO -DLL -LTCG -DEBUG -RELEASE -OPT:REF -OPT:ICF \
            -PDB:"$(OUT_DIR)/$(PROJECT_NAME).pdb" -XEX:NO

LIBS := xkelib.lib xapilib.lib xboxkrnl.lib

all: $(OUT_DIR)/$(PROJECT_NAME).xex

$(OUT_DIR)/$(PROJECT_NAME).xex: $(OUT_DIR)/$(PROJECT_NAME).dll $(SRC_DIR)/xex.xml
	@echo "Creating XEX..."
	@$(WINDOWS_SHIM) $(IMAGEXEX) -nologo -config:"$(SRC_DIR)/xex.xml" -out:"$@" "$<"

$(OUT_DIR)/$(PROJECT_NAME).dll: $(OBJS)
	@echo "Linking..."
	@mkdir -p $(@D)
	@LIB="$(XDK_LIB_DIR);$(XKELIB_DIR)" $(WINDOWS_SHIM) $(LD) $(LD_FLAGS) -OUT:"$@" $^ $(LIBS)

$(INT_DIR)/%.obj: $(SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	@INCLUDE="$(XDK_INC_DIR);$(XDK_CRT_DIR)" $(WINDOWS_SHIM) $(CXX) $(CXX_FLAGS) -Fo"$@" $<

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
