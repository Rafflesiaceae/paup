name := paup
deps := xcb xcb-keysyms xcb-util x11 libpulse

# Flags
base_CXXFLAGS = -std=c++20 -Wall -Wextra -pedantic -O2 -DDEBUG -g
base_CFLAGS   = -Wall -Wextra -pedantic -O2 -DDEBUG -g
base_LIBS	  = -lm

CXXFLAGS := $(base_CXXFLAGS) $(foreach dep, $(deps), $(shell pkg-config --cflags $(dep))) -DPONYMIX_VERSION=\"5\"
CFLAGS	 := $(base_CFLAGS) $(foreach dep, $(deps), $(shell pkg-config --cflags $(dep)))
LDLIBS   := $(base_LIBS) $(foreach dep, $(deps), $(shell pkg-config --libs $(dep)))


# Targets
all: $(name)

$(name): $(name).cpp pulse.cc

.PHONY: install clean

install: $(name)
	@sudo install -Dm755 $(name) $(DESTDIR)/usr/bin/$(name)

clean:
	$(RM) $(name)
