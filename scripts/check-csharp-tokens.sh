#!/bin/bash
# Verifies that CSHARP_TOKEN_COUNT in scanner.c matches the C# grammar's external token count.
# Run this after updating the tree-sitter-c-sharp submodule.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

CSHARP_GRAMMAR="$ROOT_DIR/tree-sitter-c-sharp/src/grammar.json"
SCANNER_C="$ROOT_DIR/src/scanner.c"

if [ ! -f "$CSHARP_GRAMMAR" ]; then
    echo "Error: C# grammar.json not found at $CSHARP_GRAMMAR"
    echo "Did you initialize the submodule? Run: git submodule update --init --recursive"
    exit 1
fi

if [ ! -f "$SCANNER_C" ]; then
    echo "Error: scanner.c not found at $SCANNER_C"
    exit 1
fi

# Get the actual count from C# grammar
if ! command -v jq &> /dev/null; then
    echo "Error: jq is required but not installed."
    echo "Install with: apt install jq (Debian/Ubuntu) or brew install jq (macOS)"
    exit 1
fi

ACTUAL_COUNT=$(jq '.externals | length' "$CSHARP_GRAMMAR")

# Get the expected count from scanner.c
EXPECTED_COUNT=$(grep -E '^#define CSHARP_TOKEN_COUNT' "$SCANNER_C" | awk '{print $3}')

if [ -z "$EXPECTED_COUNT" ]; then
    echo "Error: Could not find CSHARP_TOKEN_COUNT in scanner.c"
    exit 1
fi

if [ "$ACTUAL_COUNT" != "$EXPECTED_COUNT" ]; then
    echo "ERROR: CSHARP_TOKEN_COUNT mismatch!"
    echo "  scanner.c defines:     $EXPECTED_COUNT"
    echo "  C# grammar.json has:   $ACTUAL_COUNT"
    echo ""
    echo "The C# grammar's external tokens are:"
    jq -r '.externals | to_entries | .[] | "  \(.key): \(.value.name)"' "$CSHARP_GRAMMAR"
    echo ""
    echo "Update CSHARP_TOKEN_COUNT in src/scanner.c to $ACTUAL_COUNT"
    exit 1
fi

echo "CSHARP_TOKEN_COUNT is correct: $ACTUAL_COUNT tokens"

# Optionally show the token list
if [ "$1" = "-v" ] || [ "$1" = "--verbose" ]; then
    echo "C# external tokens:"
    jq -r '.externals | to_entries | .[] | "  \(.key): \(.value.name)"' "$CSHARP_GRAMMAR"
fi
