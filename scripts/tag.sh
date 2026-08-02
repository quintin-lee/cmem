#!/usr/bin/env bash
#
# tag.sh - Tag release and update version numbers.
#
# Usage:
#   ./scripts/tag.sh 1.2.3
#   ./scripts/tag.sh --bump patch
#   ./scripts/tag.sh --bump minor
#   ./scripts/tag.sh --bump major
#   ./scripts/tag.sh --dry-run 1.2.3
#
# Updates:
#   - VERSION (single source of truth)
#
# Creates a git tag: v<MAJOR>.<MINOR>.<PATCH>
#
# Note:
#   CMakeLists.txt and Makefile both read from VERSION, so only VERSION needs updating.
#
# Commit behavior:
#   - By default, version file changes are committed before tagging.
#   - Pass --no-commit to skip the commit step.
#

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

FILES_TO_UPDATE=(
    "$REPO_ROOT/VERSION"
    "$REPO_ROOT/CHANGELOG.md"
)

usage() {
    cat <<EOF
Usage:
  $0 <version>        Set explicit version (e.g. 1.2.3)
  $0 --bump <part>    Bump patch/minor/major
  $0 --dry-run <ver>  Show changes without applying

Examples:
  $0 1.2.3
  $0 --bump patch
  $0 --bump minor
  $0 --bump major
  $0 --dry-run --bump patch
EOF
    exit 1
}

get_current_version() {
    local text
    text="$(cat "$REPO_ROOT/VERSION")"
    text="$(echo "$text" | tr -d '[:space:]')"
    local major minor patch
    major="$(echo "$text" | cut -d. -f1)"
    minor="$(echo "$text" | cut -d. -f2)"
    patch="$(echo "$text" | cut -d. -f3)"
    echo "${major:-0} ${minor:-0} ${patch:-0}"
}

bump_version() {
    local major="$1" minor="$2" patch="$3" part="$4"
    case "$part" in
        major)
            echo $((major + 1)) 0 0
            ;;
        minor)
            echo "$major" $((minor + 1)) 0
            ;;
        patch)
            echo "$major" "$minor" $((patch + 1))
            ;;
        *)
            echo "ERROR: unknown bump component: $part" >&2
            exit 1
            ;;
    esac
}

update_file() {
    local file="$1" old_version="$2" new_version="$3"
    local old_str new_str
    old_str="$(echo "$old_version" | tr ' ' '.')"
    new_str="$(echo "$new_version" | tr ' ' '.')"
    if [[ "$(basename "$file")" == "VERSION" ]]; then
        echo "$new_str" > "$file"
    else
        if [[ "$OSTYPE" == "darwin"* ]]; then
            sed -i '' "s/$old_str/$new_str/g" "$file"
        else
            sed -i "s/$old_str/$new_str/g" "$file"
        fi
    fi
}

update_changelog() {
    local new_version="$1"
    local date
    date="$(date +%Y-%m-%d)"
    local changelog="$REPO_ROOT/CHANGELOG.md"
    if [[ ! -f "$changelog" ]]; then
        echo "  [WARN] CHANGELOG.md not found"
        return
    fi
    local tmpfile
    tmpfile="$(mktemp)"
    local replaced=false
    while IFS= read -r line; do
        if [[ "$replaced" == false ]] && [[ "$line" == "## [Unreleased]" ]]; then
            echo "## [$new_version] - $date" >> "$tmpfile"
            echo "" >> "$tmpfile"
            echo "## [Unreleased]" >> "$tmpfile"
            replaced=true
            continue
        fi
        echo "$line" >> "$tmpfile"
    done < "$changelog"
    if [[ "$replaced" == false ]]; then
        echo "" >> "$tmpfile"
        echo "## [$new_version] - $date" >> "$tmpfile"
        echo "" >> "$tmpfile"
        echo "## [Unreleased]" >> "$tmpfile"
    fi
    mv "$tmpfile" "$changelog"
}

git() {
    command git -C "$REPO_ROOT" "$@"
}

# Parse arguments
DRY_RUN=false
NO_COMMIT=false
VERSION=""
BUMP_PART=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --no-commit)
            NO_COMMIT=true
            shift
            ;;
        --bump)
            BUMP_PART="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            VERSION="$1"
            shift
            ;;
    esac
done

if [[ -z "$VERSION" && -z "$BUMP_PART" ]]; then
    usage
fi

# Read current version
read -r MAJOR MINOR PATCH <<< "$(get_current_version)"
CURRENT_VERSION="${MAJOR}.${MINOR}.${PATCH}"
echo "Current version: ${CURRENT_VERSION}"

# Determine new version
if [[ -n "$BUMP_PART" ]]; then
    read -r NEW_MAJOR NEW_MINOR NEW_PATCH <<< "$(bump_version "$MAJOR" "$MINOR" "$PATCH" "$BUMP_PART")"
else
    if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "ERROR: invalid version format: $VERSION (expected x.y.z)" >&2
        exit 1
    fi
    NEW_MAJOR="$(echo "$VERSION" | cut -d. -f1)"
    NEW_MINOR="$(echo "$VERSION" | cut -d. -f2)"
    NEW_PATCH="$(echo "$VERSION" | cut -d. -f3)"
fi

NEW_VERSION="${NEW_MAJOR}.${NEW_MINOR}.${NEW_PATCH}"
TAG="v${NEW_VERSION}"

echo "New version:    ${NEW_VERSION}"
echo "Git tag:        ${TAG}"

if $DRY_RUN; then
    echo ""
    echo "[dry-run] Would update:"
    for f in "${FILES_TO_UPDATE[@]}"; do
        echo "  $f"
    done
    echo ""
    echo "[dry-run] Would commit: chore(version): 🧹 bump version to ${NEW_VERSION}"
    echo "[dry-run] Would tag:    ${TAG}"
    exit 0
fi

# Update files
echo ""
echo "Updating version in files..."
for f in "${FILES_TO_UPDATE[@]}"; do
    if [[ -f "$f" ]]; then
        if [[ "$(basename "$f")" == "CHANGELOG.md" ]]; then
            update_changelog "${NEW_VERSION}"
        else
            update_file "$f" "${CURRENT_VERSION}" "${NEW_VERSION}"
        fi
        echo "  Updated: $f"
    else
        echo "  [WARN] not found: $f"
    fi
done

# Commit
if ! $NO_COMMIT; then
    echo ""
    echo "Committing version bump..."
    git add "${FILES_TO_UPDATE[@]}" 2>/dev/null || true
    git commit -m "chore(version): 🧹 bump version to ${NEW_VERSION}"
    echo "  Committed: chore(version): 🧹 bump version to ${NEW_VERSION}"
fi

# Tag
echo ""
echo "Creating git tag ${TAG}..."
if git tag -l "$TAG" | grep -q "$TAG"; then
    echo "ERROR: tag $TAG already exists" >&2
    exit 1
fi
git tag "$TAG"
echo "  Tagged: ${TAG}"

echo ""
echo "Done. Version bumped to ${NEW_VERSION} and tagged as ${TAG}."
if ! $NO_COMMIT; then
    echo "Push with:"
    echo "  git push origin ${TAG}"
fi
