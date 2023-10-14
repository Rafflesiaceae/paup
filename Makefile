name := paup
deps := xcb xcb-keysyms

# Flags
base_CXXFLAGS = -std=c++11 -Wall -Wextra -pedantic -O2 -g
base_CFLAGS = -Wall -Wextra -pedantic -O2 -g
base_LIBS = -lm

CXXFLAGS := $(base_CXXFLAGS) $(foreach dep, $(deps), $(shell pkg-config --cflags $(dep)))
CFLAGS := $(base_CFLAGS) $(foreach dep, $(deps), $(shell pkg-config --cflags $(dep)))
LDLIBS   := $(base_LIBS) $(foreach dep, $(deps), $(shell pkg-config --libs $(dep)))


# Targets
all: $(name)

$(name): $(name).cpp

.PHONY: install clean

install: $(name)
	install -Dm755 $(name) $(DESTDIR)/usr/bin/$(name)

clean:
	$(RM) $(name)
