#!/bin/bash
# Install git hooks for tree-sitter-razor development.
# These hooks verify C# scanner compatibility when the submodule is updated.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
HOOKS_DIR="$ROOT_DIR/.git/hooks"

if [ ! -d "$HOOKS_DIR" ]; then
    echo "Error: .git/hooks directory not found. Are you in a git repository?"
    exit 1
fi

echo "Installing git hooks..."

for hook in post-checkout post-merge; do
    SOURCE="$SCRIPT_DIR/git-hooks/$hook"
    TARGET="$HOOKS_DIR/$hook"

    if [ -f "$TARGET" ] && [ ! -L "$TARGET" ]; then
        echo "Warning: $TARGET already exists and is not a symlink."
        echo "  Backing up to $TARGET.bak"
        mv "$TARGET" "$TARGET.bak"
    fi

    ln -sf "../../scripts/git-hooks/$hook" "$TARGET"
    echo "  Installed $hook"
done

echo "Done! Git hooks will now check C# token compatibility on checkout/merge."
