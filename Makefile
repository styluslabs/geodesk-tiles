# Linux makefile for Ascend Maps tile server

SERVER ?= 1
ifneq ($(SERVER), 0)
  TARGET = server
  MAIN_SOURCE = server.cpp
else
  TARGET = tiletest
  MAIN_SOURCE = tiletest.cpp
endif

DEBUG ?= 0
ifneq ($(DEBUG), 0)
  BUILDDIR ?= build/Debug
else
  BUILDDIR ?= build/Release
  CFLAGS += -march=native
  LDFLAGS += -march=native
endif

include make/shared.mk


## sqlite
MODULE_BASE = sqlite3

MODULE_SOURCES = sqlite3.c
MODULE_INC_PUBLIC = .
MODULE_DEFS_PRIVATE = SQLITE_ENABLE_FTS5
MODULE_DEFS_PUBLIC = SQLITE_USE_URI=1

include $(ADD_MODULE)

# if only sqlite3.h is included here, first run of make will fail to build sqlite3.o
SQLITE_BASE := $(MODULE_BASE)
SQLITE_GEN := $(SQLITE_BASE)/sqlite3.h $(SQLITE_BASE)/sqlite3.c
GENERATED += $(SQLITE_GEN)

$(SQLITE_GEN):
	cd $(SQLITE_BASE) && curl "https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip" -o sqlite.zip && unzip sqlite.zip && mv sqlite-amalgamation-3490100/sqlite3.* .


## geodesk
MODULE_BASE = libgeodesk

rwildcard=$(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))
MODULE_FULL_SOURCES = $(call rwildcard,$(MODULE_BASE)/src,*.cpp)
#MODULE_FULL_SOURCES = $(wildcard $(MODULE_BASE)/src/*/*.cpp)
MODULE_INC_PUBLIC = include src

MODULE_CXXFLAGS = -Wno-unknown-pragmas -Wno-reorder

include $(ADD_MODULE)


## server
MODULE_BASE := .

MODULE_SOURCES = \
  miniz/miniz.c \
  visvalingam.cpp \
  tilebuilder.cpp \
  ascendtiles.cpp \
  ftsbuilder.cpp \
  $(MAIN_SOURCE)

MODULE_INC_PRIVATE = \
  . \
  sqlite3 \
  cpp-httplib \
  vtzero/include \
  protozero/include

#MODULE_DEFS_PRIVATE = PUGIXML_NO_XPATH PUGIXML_NO_EXCEPTIONS SVGGUI_NO_SDL

MODULE_CXXFLAGS = -Wno-unknown-pragmas -Wno-reorder

include $(ADD_MODULE)

#$(info server: $(SOURCES))
include make/unix.mk

.DEFAULT_GOAL := all
