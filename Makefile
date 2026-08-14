#-------------------------------------------------------------------------------
.SUFFIXES:
#-------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

include $(DEVKITPRO)/wups/share/wups_rules

WUT_ROOT  := $(DEVKITPRO)/wut
WUMS_ROOT := $(DEVKITPRO)/wums
#-------------------------------------------------------------------------------
# TARGET   : name of the output (.wps)
# BUILD    : intermediate object dir
# SOURCES  : dirs with source
# INCLUDES : dirs with headers
#-------------------------------------------------------------------------------
TARGET   := wuc24
BUILD    := build
SOURCES  := source third_party/fatfs
DATA     := data
INCLUDES := source third_party/fatfs

#-------------------------------------------------------------------------------
# version
#
# Builds are marked -dev unless RELEASE=1, which the tag workflow sets. Dev
# builds also carry the commit they came from: they are handed out as CI
# artifacts, which arrive with no other clue about their provenance, and the
# version string is what Aroma shows in the plugin list.
#
# GIT_REV is overridable because the build usually runs inside a container where
# the checkout is not git's to read. If it cannot be worked out, the suffix is
# simply left off.
#-------------------------------------------------------------------------------
VERSION := 0.1
GIT_REV ?= $(shell git rev-parse --short=7 HEAD 2>/dev/null)

ifeq ($(RELEASE),1)
    VERSION_STRING := v$(VERSION)
else ifeq ($(strip $(GIT_REV)),)
    VERSION_STRING := v$(VERSION)-dev
else
    VERSION_STRING := v$(VERSION)-dev+$(GIT_REV)
endif

#-------------------------------------------------------------------------------
# code generation
#-------------------------------------------------------------------------------
CFLAGS   := -Wall -O2 -ffunction-sections \
            $(MACHDEP)

CFLAGS   += $(INCLUDE) -D__WIIU__ -D__WUT__ -D__WUPS__
CFLAGS   += -DWUC24_VERSION=\"$(VERSION_STRING)\"

CXXFLAGS := $(CFLAGS) -std=gnu++20

ASFLAGS  := $(ARCH)
LDFLAGS   = $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map) $(WUPSSPECS)

# -lmocha : vWii NAND (slccmpt) access from Wii U mode
LIBS := -lnotifications -lmocha -lwups -lwut

#-------------------------------------------------------------------------------
# top-level library dirs (each must contain include/ and lib/)
#-------------------------------------------------------------------------------
LIBDIRS := $(PORTLIBS) $(WUMS_ROOT) $(WUPS_ROOT) $(WUT_ROOT) $(WUT_ROOT)/usr

#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#-------------------------------------------------------------------------------

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH  := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                 $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#-------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#-------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

#-------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@$(shell [ ! -d $(BUILD) ] && mkdir -p $(BUILD))
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#-------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).wps $(TARGET).elf

#-------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

#-------------------------------------------------------------------------------
all: $(OUTPUT).wps

$(OUTPUT).wps : $(OUTPUT).elf
$(OUTPUT).elf : $(OFILES)

$(OFILES_SRC) : $(HFILES_BIN)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#-------------------------------------------------------------------------------
endif
#-------------------------------------------------------------------------------
