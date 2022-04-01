PREFIX = /usr/local

xedit: xedit.c
	$(CC) xedit.c -o xedit -Wall -Wextra -pedantic -std=c99

.PHONY: install
install: xedit
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $< $(DESTDIR)$(PREFIX)/bin/xedit

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/xedit
