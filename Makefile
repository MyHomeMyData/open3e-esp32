# Host-side tasks. The firmware itself is built with idf.py; see README.md.

PY := .venv/bin/python

.PHONY: help setup db fixtures test sanitize storage lint fwinfo clean

help:
	@echo "make setup     create the Python venv and fetch open3e"
	@echo "make db        regenerate data/o3edb.bin from the open3e sources"
	@echo "make fixtures  regenerate the test vectors from open3e's own codecs"
	@echo "make test      run the host test suite (codec, encode, flatten, ISO-TP)"
	@echo "make sanitize  run the same suite under ASan/UBSan/LeakSanitizer"
	@echo "make storage   assemble the storage partition contents by hand"
	@echo "               (not normally needed: idf.py build does this)"
	@echo "make lint      check includes and cross-module symbols"
	@echo "make fwinfo    show the built firmware's build identity"
	@echo ""
	@echo "Firmware:      idf.py set-target esp32s3 && idf.py build flash"

setup:
	python3 -m venv .venv
	$(PY) -m pip install -q udsoncan
	python3 tools/fetch_open3e.py

db:
	$(PY) tools/gen_dpdb.py -o data/o3edb.bin

fixtures:
	$(PY) tools/gen_fixtures.py -o test/fixtures.json
	$(PY) tools/gen_flat_fixtures.py -o test/flat_fixtures.json
	$(PY) tools/gen_encode_fixtures.py -o test/encode_fixtures.json
	$(PY) tools/gen_em_fixtures.py -o test/em_fixtures.json

test:
	$(MAKE) -C test all

sanitize:
	$(MAKE) -C test sanitize

storage:
	$(PY) tools/build_fs.py

lint:
	python3 tools/check_symbols.py

fwinfo:
	@python3 tools/fwinfo.py

clean:
	$(MAKE) -C test clean
	rm -rf build storage_image storage_image.bin data
