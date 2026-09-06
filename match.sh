#!/bin/sh
set -eu

if [ "${1-}" = "namespace" ]; then
	shift
	exec python3 "$(dirname "$0")/tools/matching/namespace_match.py" "$@"
fi

exec python3 "$(dirname "$0")/tools/matching/match.py" "$@"
