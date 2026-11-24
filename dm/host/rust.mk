# The host's Rust code (dm/host/rust; docs/development.md, "Rust in the host"): one no_std static library that dm/host/Makefile and dm/host/tests/Makefile link
# ($(RS_LIB)), built by cargo with the Rust rust-toolchain.toml pins, from no crate outside the repository (--locked --offline). The rule runs cargo every time (FORCE):
# cargo knows what the library depends on, the toolchain included, and leaves an up-to-date library untouched, so nothing that links it is made again
RS_HOST := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
include $(RS_HOST)/../mac/min_os.mk
# cargo from the PATH, else where rustup puts it (a shell that has not read the profile rustup's installer extended)
CARGO ?= $(or $(shell command -v cargo 2>/dev/null),$(HOME)/.cargo/bin/cargo)
RS_TARGET_DIR := $(abspath $(RS_HOST)/gen/rust)
RS_LIB := $(RS_HOST)/gen/rust/aarch64-apple-darwin/release/libdmhost_rs.a
$(RS_LIB): FORCE
	cd $(RS_HOST)/rust && MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN) $(CARGO) build --quiet --release --locked --offline --target aarch64-apple-darwin --target-dir $(RS_TARGET_DIR)
