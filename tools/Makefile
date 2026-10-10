# Standalone EH57E tools (out-of-tree).
#
#   make -C tools              # builds egis057e_capture and egis057e-match-test
#
# egis057e_capture links against the built libfprint (public API).
# egis057e-match-test compiles the SAME egis057e-matcher.c as the driver, so
# its feature extraction/scoring is bit-identical to the runtime path.
#
# LIBFPRINT_ROOT  points at the source tree root (contains libfprint/fprint.h).
# LIBFPRINT_BUILD points at the meson build dir (contains libfprint/libfprint-2.so).
.RECIPEPREFIX = >

LIBFPRINT_ROOT  ?= $(abspath $(CURDIR)/..)
LIBFPRINT_BUILD ?= $(LIBFPRINT_ROOT)/build
LIBFPRINT_LIBDIR := $(LIBFPRINT_BUILD)/libfprint
DRIVERDIR := $(LIBFPRINT_ROOT)/libfprint/drivers
EXAMPLES_DIR := $(LIBFPRINT_ROOT)/examples

PKGS := glib-2.0 gio-2.0 gobject-2.0 gusb
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -I$(LIBFPRINT_ROOT) -I$(LIBFPRINT_LIBDIR) $(shell pkg-config --cflags $(PKGS))
LDLIBS += -L$(LIBFPRINT_LIBDIR) -lfprint-2 $(shell pkg-config --libs $(PKGS))
LDFLAGS += -Wl,-rpath,$(LIBFPRINT_LIBDIR)

MATCHER_SRC := $(DRIVERDIR)/egis057e-matcher.c

all: egis057e_capture egis057e-match-test egis057e_enroll

egis057e_capture: egis057e_capture.c
> $(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

egis057e-match-test: egis057e-match-test.c $(MATCHER_SRC)
> $(CC) -O2 -g -Wall -Wextra -I$(DRIVERDIR) -o $@ egis057e-match-test.c $(MATCHER_SRC) -lm

# Friendly enroll/verify CLI; reuses the examples' storage API (storage.c,
# which is example code compiled with two -Wextra warnings suppressed).
egis057e_enroll: egis057e_enroll.c storage.o
> $(CC) $(CFLAGS) -I$(EXAMPLES_DIR) -I$(LIBFPRINT_ROOT)/libfprint -o $@ egis057e_enroll.c storage.o $(LDFLAGS) $(LDLIBS)

storage.o: $(EXAMPLES_DIR)/storage.c $(EXAMPLES_DIR)/storage.h
> $(CC) $(CFLAGS) -Wno-unused-parameter -Wno-sign-compare -I$(EXAMPLES_DIR) -I$(LIBFPRINT_ROOT)/libfprint -c -o $@ $(EXAMPLES_DIR)/storage.c

clean:
> rm -f egis057e_capture egis057e-match-test egis057e_enroll storage.o *.pgm test-storage.variant

.PHONY: all clean
