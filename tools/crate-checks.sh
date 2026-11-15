#!/usr/bin/env bash
# Runs the CI's static checks locally on the files changed since the base
# branch, so their failures show up in seconds instead of after the CI run:
#
#   - pre-commit (clang-format, markdownlint, codespell, eslint...) on every
#     changed file, committed or not;
#   - clang-tidy with the repository's .clang-tidy (the "checks / clang-tidy"
#     job);
#   - clazy with the CI's CLAZY_CHECKS (the "checks / clazy" job).
#
# Both CI jobs build with WARNINGS_FATAL, so any warning here is a failure.
#
# Usage: tools/crate-checks.sh [--base <ref>] [--build <dir>] [--no-tidy]
#                               [--no-clazy] [--no-pre-commit] [files...]
#
# With files, only those are checked. The build directory needs one build
# (the analysers read the generated moc and ui headers) and a
# compile_commands.json, which this script asks CMake for when it is missing.
# Needs clang-tidy and clazy-standalone (Arch: clang, clazy) and pre-commit
# (Arch: pre-commit; uvx pre-commit is used when it is not installed).
set -euo pipefail

base=origin/main
build=build
tidy=1
clazy=1
precommit=1
files=()
while (($#)); do
    case "$1" in
    --base)
        base="$2"
        shift 2
        ;;
    --build)
        build="$2"
        shift 2
        ;;
    --no-tidy)
        tidy=0
        shift
        ;;
    --no-clazy)
        clazy=0
        shift
        ;;
    --no-pre-commit)
        precommit=0
        shift
        ;;
    -h | --help)
        sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        files+=("$1")
        shift
        ;;
    esac
done

cd "$(git rev-parse --show-toplevel)"

# The CI's clazy checks (.github/workflows/checks.yml); kept in step by
# reading them from there
clazy_checks=$(sed -n 's/^ *CLAZY_CHECKS: *//p' .github/workflows/checks.yml | head -n 1)
clazy_ignore=$(sed -n 's/^ *CLAZY_IGNORE_DIRS: *//p' .github/workflows/checks.yml | head -n 1)

if ((${#files[@]} == 0)); then
    mergebase=$(git merge-base "$base" HEAD)
    mapfile -t files < <({
        git diff --name-only --diff-filter=ACMR "$mergebase"
        git ls-files --others --exclude-standard
    } | sort -u)
fi
if ((${#files[@]} == 0)); then
    echo "No changed files since $base."
    exit 0
fi

failed=()

if ((precommit)); then
    echo "== pre-commit (${#files[@]} files)"
    if command -v pre-commit >/dev/null; then
        runner=(pre-commit)
    else
        runner=(uvx pre-commit)
    fi
    if ! "${runner[@]}" run --files "${files[@]}"; then
        failed+=(pre-commit)
    fi
fi

# The translation units to analyse: the changed sources, and for a changed
# header the source next to it, outside the bundled lib/
units=()
for file in "${files[@]}"; do
    [[ "$file" == lib/* || ! -e "$file" ]] && continue
    case "$file" in
    *.cpp) units+=("$file") ;;
    *.h) [[ -e "${file%.h}.cpp" ]] && units+=("${file%.h}.cpp") ;;
    esac
done
if ((${#units[@]} > 0)); then
    mapfile -t units < <(printf '%s\n' "${units[@]}" | sort -u)
fi

if ((${#units[@]} > 0 && (tidy || clazy))); then
    if [[ ! -f "$build/compile_commands.json" ]]; then
        echo "== Asking CMake for $build/compile_commands.json"
        cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$build" >/dev/null
    fi
    # The build may use GCC: its own flags mean nothing to the clang front
    # end of the analysers, and CMake leaves out -std when GCC's default is
    # already the project's standard, which clang's is not
    standard=$(sed -n 's/^set(CMAKE_CXX_STANDARD \([0-9]*\))/\1/p' CMakeLists.txt | head -n 1)
    extra=(--extra-arg="-std=c++${standard:-20}"
        --extra-arg=-Wno-unknown-warning-option
        --extra-arg=-Wno-unused-command-line-argument
        --extra-arg=-Wno-ignored-optimization-argument
        # As the CI's clang-tidy job: a local build may use -ffast-math
        --extra-arg=-Wno-nan-infinity-disabled)
    jobs=$(nproc)
    # A copy of the compile commands without GCC's own options, which the
    # clang front end refuses ("unknown argument")
    database=$(mktemp -d)
    trap 'rm -rf "$database"' EXIT
    python3 - "$build/compile_commands.json" "$database/compile_commands.json" <<'PY'
import json, re, shlex, sys
gcc_only = re.compile(r"^-m(no-)?direct-extern-access$|^-f(no-)?(tree|ipa)-|^-fno-semantic-interposition$")
entries = json.load(open(sys.argv[1]))
for entry in entries:
    arguments = entry.pop("arguments", None) or shlex.split(entry.pop("command"))
    entry["arguments"] = [a for a in arguments if not gcc_only.match(a)]
json.dump(entries, open(sys.argv[2], "w"))
PY

    # Runs "$@ <unit>" for every unit in parallel; any warning or error fails
    analyse() {
        local name="$1"
        shift
        local log
        log=$(mktemp)
        printf '%s\n' "${units[@]}" |
            xargs -P "$jobs" -I{} "$@" {} >"$log" 2>&1 || true
        if grep -q -E '(warning|error): ' "$log"; then
            grep -E -A3 '(warning|error): ' "$log"
            failed+=("$name")
        else
            echo "${#units[@]} files clean."
        fi
        rm -f "$log"
    }

    if ((tidy)); then
        echo "== clang-tidy (${#units[@]} files)"
        analyse clang-tidy clang-tidy -p "$database" --quiet "${extra[@]}"
    fi
    if ((clazy)); then
        echo "== clazy (${#units[@]} files)"
        if command -v clazy-standalone >/dev/null; then
            export CLAZY_CHECKS="$clazy_checks" CLAZY_IGNORE_DIRS="$clazy_ignore"
            analyse clazy clazy-standalone -p "$database" "${extra[@]}"
        else
            echo "clazy-standalone is not installed (Arch: pacman -S clazy)."
            failed+=(clazy)
        fi
    fi
fi

if ((${#failed[@]})); then
    echo "Failed: ${failed[*]}"
    exit 1
fi
echo "All checks passed."
