#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules
include $(TOPDIR)/lib/Atmosphere-libs/config/templates/stratosphere.mk

TARGET		:=	net-probe
BUILD		:=	build
OUTDIR		:=	out
TITLE_ID	:=	010000000000EAD1
DISTDIR		:=	$(OUTDIR)/atmosphere/contents/$(TITLE_ID)
SOURCES		:=	src
INCLUDES	:=	src \
			$(TOPDIR)/lib/Atmosphere-libs/libstratosphere/include \
			$(TOPDIR)/lib/Atmosphere-libs/libvapours/include
LIBNAMES	:=	lib/Atmosphere-libs/libstratosphere

APP_VERSION	:= 0.0.1-dev
BUILD_ID	:= $(shell git describe --always --dirty 2>/dev/null || echo "unknown")

DEFINES	+=	-DDISABLE_IPC -D__SWITCH__ \
	-DTARGET="\"$(TARGET)\"" \
	-DVERSION="\"$(APP_VERSION)\"" \
	-DBUILD_ID="\"$(BUILD_ID)\""

CFLAGS	+= $(INCLUDE) -Wno-unused-result
CXXFLAGS += $(CFLAGS) -std=c++23

LIBDIRS	+= $(foreach lib,$(LIBNAMES),$(TOPDIR)/$(lib))

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(OUTDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES 	:= $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			$(foreach dir,$(AMS_LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
			$(foreach dir,$(AMS_LIBDIRS),-L$(dir)/lib) \
			$(foreach dir,$(AMS_LIBDIRS),-L$(dir)/$(ATMOSPHERE_LIBRARY_DIR))

export APP_JSON := $(TOPDIR)/res/module.json

.PHONY	: $(BUILD) clean clean-libs clean-build all libs dist

all	: $(BUILD)

dist	: $(BUILD)
	@[ -d $(DISTDIR) ] || mkdir -p $(DISTDIR)
	@cp $(OUTDIR)/$(TARGET).nsp $(DISTDIR)/exefs.nsp
	@printf '{\n  "name": "net-probe",\n  "tid": "$(TITLE_ID)",\n  "requires_reboot": false,\n  "shutdown_service": "wgp:ctl",\n  "shutdown_cmd": 0\n}\n' > $(DISTDIR)/toolbox.json
#	@[ -d $(DISTDIR)/flags ] || mkdir -p $(DISTDIR)/flags
#	@: > $(DISTDIR)/flags/boot2.flag
	@echo "prepared Atmosphere layout in $(DISTDIR)"

libs	:
	@$(foreach lib,$(LIBNAMES),$(MAKE) --no-print-directory -C $(TOPDIR)/$(lib) && ) true

$(BUILD)	: libs
	@[ -d $@ ] || mkdir -p $@
	@[ -d $(OUTDIR) ] || mkdir -p $(OUTDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean-libs	:
	@echo clean libs $(LIBNAMES) ...
	@$(foreach lib,$(LIBNAMES),$(MAKE) -C $(TOPDIR)/$(lib) clean && ) true

clean-build	:
	@echo clean build ...
	@rm -fr $(BUILD) $(TARGET).kip $(TARGET).nsp $(TARGET).npdm $(TARGET).nso $(TARGET).elf $(OUTDIR)

clean	: clean-libs clean-build

else
.PHONY	:	all

DEPENDS	:=	$(OFILES:.o=.d)

all	: $(OUTPUT).nsp

$(OUTPUT).nsp	: $(OUTPUT).nso $(OUTPUT).npdm
$(OUTPUT).nso : $(OUTPUT).elf

$(OUTPUT).elf	: $(OFILES)

$(OFILES)	: $(ATMOSPHERE_LIBRARIES_DIR)/libstratosphere/$(ATMOSPHERE_LIBRARY_DIR)/libstratosphere.a

%.npdm  :   %.npdm.json
	@echo built ... $< $@
	@npdmtool $< $@
	@echo built ... $(notdir $@)

-include $(DEPENDS)

endif
