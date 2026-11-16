# Everyday development commands for Crate Decks. `make` lists them.
#
# Variables (make <target> VAR=value):
#   BUILD    build directory                      (build)
#   TYPE     CMake build type                     (RelWithDebInfo)
#   JOBS     parallel jobs                        (all cores)
#   FILTER   GoogleTest filter for `test-unit`    (Crate*)
#   ASSERTS_BUILD  build directory of `test-asserts` (build-asserts)
#   TESTS    ctest regex for `test-asserts`; empty runs the whole suite
#   PROFILE  settings folder for `run`; empty is a new temporary one. Never
#            ~/.cratedecks or ~/.mixxx (AGENTS.md)
#   BASE     branch the checks compare against    (origin/main)
#   SHOTS    output folder for `screenshots`      (a new temporary one)
#   TRACKS   audio folder for `screenshots`       (none)
#   CMAKE_ARGS  extra arguments for `configure`
#   DESKTOP_PROFILE  settings folder of the desktop entry that
#            `install-desktop` installs ($(BUILD)/desktop-profile). Never
#            ~/.cratedecks or ~/.mixxx
#   XDG_DATA_HOME  where `install-desktop` installs (~/.local/share)

BUILD ?= build
TYPE ?= RelWithDebInfo
JOBS ?= $(shell nproc)
FILTER ?= Crate*
ASSERTS_BUILD ?= build-asserts
TESTS ?=
PROFILE ?=
BASE ?= origin/main
SHOTS ?=
TRACKS ?=
CMAKE_ARGS ?=
DESKTOP_PROFILE ?= $(abspath $(BUILD))/desktop-profile
XDG_DATA_HOME ?= $(HOME)/.local/share

# The desktop identity (CRATE_APP_ID in CMakeLists.txt): the app id, the
# desktop entry and the icons, res/images/icons/<size>/apps/<app id>.png
APP_ID := app.cratemusic.decks
APP_ICONS := $(wildcard res/images/icons/*/apps/$(APP_ID).png res/images/icons/*/apps/$(APP_ID).svg)
# What `install-desktop` writes, so that `uninstall-desktop` removes only it
DESKTOP_MARK := Installed by `make install-desktop`

# Widgets render without a display in the unit tests, as on the CI
TEST_ENV := QT_QPA_PLATFORM=offscreen GTEST_COLOR=1

.DEFAULT_GOAL := help
.PHONY: help deps hooks configure build build-tests build-all run run-last \
	install-desktop uninstall-desktop desktop-caches \
	test test-unit test-asserts test-design checks checks-tidy checks-clazy format \
	tokens tokens-check screenshots ci clean

help: ## List the targets
	@awk 'BEGIN {FS = ":.*## "} /^[a-z-]+:.*## / {printf "  \033[36m%-18s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)

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

## Desktop

# The desktop entry runs this build's binary with DESKTOP_PROFILE, so that a
# launcher or dock starts it as Decks (app id app.cratemusic.decks, its icon
# and its name) without touching ~/.cratedecks. TryExec hides it once the
# build is gone.
install-desktop: build ## Install a desktop entry and the icons of this build for your user
	@profile="$(DESKTOP_PROFILE)"; \
	case "$$profile" in "~"*) profile="$$HOME$${profile#\~}";; esac; \
	case "$$(realpath -m "$$profile")" in \
	"$$(realpath -m ~/.cratedecks)"*|"$$(realpath -m ~/.mixxx)"*) \
		echo "DESKTOP_PROFILE must not be the real settings folder: $$profile"; exit 1;; \
	esac; \
	profile="$$(realpath -m "$$profile")"; \
	binary="$$(realpath $(BUILD)/crate-decks)"; \
	applications="$(XDG_DATA_HOME)/applications"; \
	entry="$$applications/$(APP_ID).desktop"; \
	mkdir -p "$$profile" "$$applications"; \
	{ \
		echo '# $(DESKTOP_MARK) from $(CURDIR).'; \
		echo "# Runs $$binary with the disposable settings folder"; \
		echo "# $$profile, never ~/.cratedecks. \`make uninstall-desktop\` removes it."; \
		sed -e "s|^Exec=.*|Exec=\"$$binary\" --settingsPath \"$$profile\"|" \
			-e "/^Exec=/a TryExec=$$binary" res/linux/$(APP_ID).desktop; \
	} > "$$entry.tmp" && mv "$$entry.tmp" "$$entry"; \
	echo "Installed $$entry"; \
	for icon in $(APP_ICONS); do \
		size=$$(basename "$$(dirname "$$(dirname "$$icon")")"); \
		install -D -m 644 "$$icon" "$(XDG_DATA_HOME)/icons/hicolor/$$size/apps/$$(basename "$$icon")"; \
		echo "Installed $(XDG_DATA_HOME)/icons/hicolor/$$size/apps/$$(basename "$$icon")"; \
	done
	@$(MAKE) --no-print-directory desktop-caches

uninstall-desktop: ## Remove what install-desktop installed
	@entry="$(XDG_DATA_HOME)/applications/$(APP_ID).desktop"; \
	if [ -f "$$entry" ]; then \
		if grep -qF '$(DESKTOP_MARK)' "$$entry"; then \
			rm -f "$$entry"; echo "Removed $$entry"; \
		else \
			echo "Left $$entry: make install-desktop did not write it"; \
		fi; \
	fi; \
	for icon in $(APP_ICONS); do \
		size=$$(basename "$$(dirname "$$(dirname "$$icon")")"); \
		installed="$(XDG_DATA_HOME)/icons/hicolor/$$size/apps/$$(basename "$$icon")"; \
		if [ -f "$$installed" ]; then rm -f "$$installed"; echo "Removed $$installed"; fi; \
	done
	@$(MAKE) --no-print-directory desktop-caches

# Refreshes the caches of the user's desktop entries and icons, when the
# tools are installed
desktop-caches:
	@if command -v update-desktop-database >/dev/null 2>&1; then \
		update-desktop-database -q "$(XDG_DATA_HOME)/applications" || true; \
	fi
	@if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -d "$(XDG_DATA_HOME)/icons/hicolor" ]; then \
		gtk-update-icon-cache -q -f -t "$(XDG_DATA_HOME)/icons/hicolor" || true; \
	fi

## Tests

test: build-all ## Run the whole suite as the CI does (ctest, one process per test)
	$(TEST_ENV) CTEST_OUTPUT_ON_FAILURE=1 ctest --test-dir $(BUILD) --timeout 45 -j $(JOBS)

test-unit: build-tests ## Run the tests matching FILTER in one process (fast)
	$(TEST_ENV) $(BUILD)/mixxx-test --gtest_filter='$(FILTER)'

# DEBUG_ASSERT is on in the CI's coverage job (a Debug build) and off in a
# RelWithDebInfo one: this build turns the assertions on, and fatal
$(ASSERTS_BUILD)/build.ninja:
	cmake -S . -B $(ASSERTS_BUILD) -G Ninja -DCMAKE_BUILD_TYPE=$(TYPE) -DBUILD_TESTING=ON \
		-DCRATE=ON -DDEBUG_ASSERTIONS_FATAL=ON \
		-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
		$(CMAKE_ARGS)

test-asserts: $(ASSERTS_BUILD)/build.ninja ## Run the tests with DEBUG_ASSERT on, as the CI's coverage job (TESTS)
	cmake --build $(ASSERTS_BUILD) --target mixxx-test -j $(JOBS)
	$(TEST_ENV) CTEST_OUTPUT_ON_FAILURE=1 ctest --test-dir $(ASSERTS_BUILD) --timeout 45 -j $(JOBS) \
		$(if $(TESTS),-R '$(TESTS)')

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

ci: checks tokens-check test-design test test-asserts ## What the CI checks, before a push (the full suite, twice, takes a while)

clean: ## Remove the build outputs (keeps the configuration)
	cmake --build $(BUILD) --target clean
