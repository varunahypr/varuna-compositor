.PHONY: srpm

outdir ?= $(CURDIR)
spec ?= packaging/fedora/varuna-compositor.spec

srpm:
	dnf install -y --nodocs git-core rpm-build tar gzip
	git config --global --add safe.directory '*'
	sh packaging/fedora/make-srpm.sh "$(outdir)" "$(spec)"
