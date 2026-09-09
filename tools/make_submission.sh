#!/bin/sh
#
# Builds the submission ZIP with the exact name and directory structure the
# assignment's automated checker expects.
#
#   tools/make_submission.sh <ROLL1> <ROLL2>
#
# Roll numbers must be written exactly as they appear on the LMS. They are
# sorted alphabetically for the filename regardless of the order given.

set -eu

if [ $# -ne 2 ]; then
    echo "usage: $0 <ROLL1> <ROLL2>   e.g. $0 2024CS10057 2024CS10093" >&2
    exit 2
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# Alphabetical order, as the naming rules require.
FIRST=$(printf '%s\n%s\n' "$1" "$2" | sort | head -1)
SECOND=$(printf '%s\n%s\n' "$1" "$2" | sort | tail -1)
NAME="A2_${FIRST}_${SECOND}"
ZIP="$ROOT/$NAME.zip"

# Required contents.
REQUIRED="server/run-server client/run-trader client/run-market-data README.md"

missing=""
for path in $REQUIRED; do
    [ -e "$path" ] || missing="$missing $path"
done
if [ -n "$missing" ]; then
    echo "missing required file(s):$missing" >&2
    exit 1
fi

for launcher in server/run-server client/run-trader client/run-market-data; do
    if [ ! -x "$launcher" ]; then
        echo "launcher is not executable: $launcher" >&2
        exit 1
    fi
    if ! grep -q '^exec ' "$launcher"; then
        echo "launcher does not use exec: $launcher" >&2
        exit 1
    fi
done

if [ ! -f report/report.pdf ]; then
    echo "warning: report/report.pdf not found." >&2
    echo "         Export report/report.md to PDF before submitting." >&2
fi

rm -f "$ZIP"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

# The required layout puts server/, client/, src/ and README.md at the root of
# the archive, with no wrapping directory.
DEST="$STAGE/payload"
mkdir -p "$DEST"

# Ship sources and everything needed to build and run; no build products, so
# the submission is reproducible from scratch.
cp -R server client src tests tools Makefile README.md "$DEST/"
[ -f report/report.pdf ] && cp report/report.pdf "$DEST/report.pdf"
[ -f report/report.md ] && cp report/report.md "$DEST/report.md"
[ -f experiment.py ] && cp experiment.py "$DEST/"

# The launchers build on demand, so the archive ships sources only: no build
# products, and nothing compiled for the wrong architecture.
rm -rf "$DEST/bin"
find "$DEST" -name '*.o' -delete
find "$DEST" -name '.DS_Store' -delete
find "$DEST" -type f -perm -u+x ! -name '*.sh' ! -name 'run-*' \
     ! -name '*.py' -exec rm -f {} +

chmod +x "$DEST/server/run-server" "$DEST/client/run-trader" \
         "$DEST/client/run-market-data" "$DEST/tools/"*.sh

( cd "$DEST" && zip -qr "$ZIP" . -x '.*' )

echo "Created $ZIP"
echo
echo "Contents:"
unzip -l "$ZIP" | sed -n '4,60p'

echo
echo "Required paths:"
for path in $REQUIRED src; do
    if unzip -l "$ZIP" | grep -q " $path"; then
        echo "  ok      $path"
    else
        echo "  MISSING $path"
    fi
done
