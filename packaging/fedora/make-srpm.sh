#!/bin/sh
set -eu

outdir=$(realpath -m "${1:-.}")
spec_template=${2:-packaging/fedora/varuna-compositor.spec}

top=$(git rev-parse --show-toplevel)
cd "$top"
git submodule update --init --recursive

base=$(cat VERSION)
commits=$(git rev-list --count HEAD)
commit=$(git rev-parse HEAD)
short=$(git rev-parse --short=7 HEAD)
branch=$(git rev-parse --abbrev-ref HEAD)
[ "$branch" != HEAD ] || branch=varuna/main
date=$(LC_ALL=C git log -1 --format=%cd --date=format:'%a %b %d %H:%M:%S %Y')

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

git ls-files --recurse-submodules -z \
    | tar --null -czf "$work/varuna-compositor.tar.gz" --transform 's|^|varuna-compositor/|' -T -

sed -e "s|@VERSION@|${base}^${commits}.git${short}|" \
    -e "s|@GIT_COMMIT@|${commit}|" \
    -e "s|@GIT_BRANCH@|${branch}|" \
    -e "s|@GIT_DATE@|${date}|" \
    -e "s|@GIT_COMMITS@|${commits}|" \
    "$spec_template" > "$work/varuna-compositor.spec"

mkdir -p "$outdir"
rpmbuild -bs \
    --define "_sourcedir $work" \
    --define "_specdir $work" \
    --define "_srcrpmdir $outdir" \
    "$work/varuna-compositor.spec"
