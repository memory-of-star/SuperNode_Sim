.PHONY: all setup check clean evidence
all:
	./tools/build.sh
setup:
	./tools/setup.sh
check:
	python3 tools/check_paper.py
# Requires original local experiment evidence. Ordinary compilation does not.
evidence:
	python3 tools/prepare_evidence.py
clean:
	rm -rf build
