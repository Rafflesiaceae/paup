name := paup
deps := xcb xcb-keysyms xcb-util x11 libpulse

# Keep the transitional Make build strict while the C port is validated.
CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -pedantic -O2 -g \
	$(foreach dep, $(deps), $(shell pkg-config --cflags $(dep)))
LDLIBS := $(foreach dep, $(deps), $(shell pkg-config --libs $(dep)))


# Targets
all: $(name)

$(name): $(name).c pulse.c pulse.h
	$(CC) $(CFLAGS) $(name).c pulse.c $(LDLIBS) -o $@

.PHONY: install clean

install: $(name)
	@sudo install -Dm755 $(name) $(DESTDIR)/usr/bin/$(name)

clean:
	$(RM) $(name)
