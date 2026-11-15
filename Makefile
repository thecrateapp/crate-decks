# Everyday development commands for Crate Decks. `make` lists them.
#
# Variables (make <target> VAR=value):
#   BUILD    build directory                      (build)
#   TYPE     CMake build type                     (RelWithDebInfo)
#   JOBS     parallel jobs                        (all cores)
#   FILTER   GoogleTest filter for `test-unit`    (Crate*)
#   PROFILE  settings folder for `run`; empty is a new temporary one. Never
#            ~/.cratedecks or ~/.mixxx (AGENTS.md)
#   BASE     branch the checks compare against    (origin/main)
#   SHOTS    output folder for `screenshots`      (a new temporary one)
#   TRACKS   audio folder for `screenshots`       (none)
#   CMAKE_ARGS  extra arguments for `configure`

BUILD ?= build
TYPE ?= RelWithDebInfo
JOBS ?= $(shell nproc)
FILTER ?= Crate*
PROFILE ?=
BASE ?= origin/main
SHOTS ?=
TRACKS ?=
CMAKE_ARGS ?=

# Widgets render without a display in the unit tests, as on the CI
TEST_ENV := QT_QPA_PLATFORM=offscreen GTEST_COLOR=1

.DEFAULT_GOAL := help
.PHONY: help deps hooks configure build build-tests build-all run run-last \
	test test-unit test-design checks checks-tidy checks-clazy format \
	tokens tokens-check screenshots ci clean

help: ## List the targets
	@awk 'BEGIN {FS = ":.*## "} /^[a-z-]+:.*## / {printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)

## Setup

deps: ## Install the build dependencies (Arch/CachyOS; asks for sudo)
	tools/archlinux_buildenv.sh setup

hooks: ## Run pre-commit (clang-format, markdownlint...) on every commit
	pre-commit install

## Build

$(BUILD)/build.ninja:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=$(TYPE) -DBUILD_TESTING=ON \
		-DCRATE=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
		$(CMAKE_ARGS)

configure: ## Configure $(BUILD) again (CMAKE_ARGS adds options)
	@rm -f $(BUILD)/build.ninja
	@$(MAKE) --no-print-directory $(BUILD)/build.ninja

build: $(BUILD)/build.ninja ## Build the app (crate-decks)
	cmake --build $(BUILD) --target crate-decks -j $(JOBS)

build-tests: $(BUILD)/build.ninja ## Build the test binary (mixxx-test)
	cmake --build $(BUILD) --target mixxx-test -j $(JOBS)

build-all: $(BUILD)/build.ninja ## Build every target
	cmake --build $(BUILD) -j $(JOBS)

## Run

run: build ## Run the app with a disposable settings folder (PROFILE reuses one)
	@profile="$(PROFILE)"; \
	if [ -z "$$profile" ]; then profile=$$(mktemp -d -t crate-decks.XXXXXX); fi; \
	case "$$profile" in "~"*) profile="$$HOME$${profile#\~}";; esac; \
	case "$$(realpath -m "$$profile")" in \
	"$$(realpath -m ~/.cratedecks)"*|"$$(realpath -m ~/.mixxx)"*) \
		echo "PROFILE must not be the real settings folder: $$profile"; exit 1;; \
	esac; \
	mkdir -p "$$profile"; echo "$$profile" > $(BUILD)/.last-profile; \
	echo "Settings: $$profile"; \
	$(BUILD)/crate-decks --settingsPath "$$profile"

run-last: build ## Run the app again with the settings folder of the last `run`
	@test -s $(BUILD)/.last-profile || { echo "No earlier make run."; exit 1; }
	@$(MAKE) --no-print-directory run PROFILE="$$(cat $(BUILD)/.last-profile)"

## Tests

test: build-all ## Run the whole suite as the CI does (ctest, one process per test)
	$(TEST_ENV) CTEST_OUTPUT_ON_FAILURE=1 ctest --test-dir $(BUILD) --timeout 45 -j $(JOBS)

test-unit: build-tests ## Run the tests matching FILTER in one process (fast)
	$(TEST_ENV) $(BUILD)/mixxx-test --gtest_filter='$(FILTER)'

test-design: ## Test the design-token generator and the brand inventory
	python3 -m pytest -q -p no:cacheprovider tools/crate-design tools/crate-brand

## Checks

checks: ## pre-commit, clang-tidy and clazy on the files changed since BASE
	tools/crate-checks.sh --base $(BASE) --build $(BUILD)

checks-tidy: ## Only clang-tidy on the changed files
	tools/crate-checks.sh --base $(BASE) --build $(BUILD) --no-clazy --no-pre-commit

checks-clazy: ## Only clazy on the changed files
	tools/crate-checks.sh --base $(BASE) --build $(BUILD) --no-tidy --no-pre-commit

format: ## Only pre-commit (formatters and linters) on the changed files
	tools/crate-checks.sh --base $(BASE) --no-tidy --no-clazy

## Design

tokens: ## Generate the theme files from res/crate-design/tokens.json
	python3 tools/crate-design/generate.py

tokens-check: ## Fail when the generated theme files are out of date
	python3 tools/crate-design/generate.py --check

screenshots: build-tests ## Render the skin and dialogs to PNG (SHOTS, TRACKS)
	@shots="$(SHOTS)"; \
	if [ -z "$$shots" ]; then shots=$$(mktemp -d -t crate-shots.XXXXXX); fi; \
	tools/crate-design/skin-screenshots.sh "$$shots" $(if $(TRACKS),"$(TRACKS)","") $(BUILD); \
	echo "Screenshots: $$shots"

## Before a push

ci: checks tokens-check test-design test ## What the CI checks, before a push (the full suite takes a while)

clean: ## Remove the build outputs (keeps the configuration)
	cmake --build $(BUILD) --target clean
