.PHONY: all run host run-host setup-usb test clean sanitize

all run host run-host setup-usb test clean sanitize:
	+$(MAKE) -C host $@
