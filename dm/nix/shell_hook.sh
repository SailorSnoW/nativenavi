# shellcheck shell=bash
# Sourced by flake.nix's shells. Must stay bash 3.2 compatible (direnv may run /bin/bash) and must never exit.

_nx_red() { printf '[RED] NativeNavi %s shell: %s\n' "$NATIVENAVI_SHELL" "$*" >&2; }
_nx_note() { printf 'NativeNavi %s shell: %s\n' "$NATIVENAVI_SHELL" "$*" >&2; }

# Apple's tools before any GNU ones (nix develop's or nix-darwin's): the scripts rely on BSD cp -c, stat -f and bsdtar's xattrs, and CI uses Xcode's make
_nx_path=$NATIVENAVI_TOOLS:/usr/bin:/bin:/usr/sbin:/sbin
_nx_ifs=$IFS; IFS=:
for _nx_d in $PATH; do
  [ -n "$_nx_d" ] || continue
  case ":$NATIVENAVI_STDENV_PATH:$_nx_path:" in *":$_nx_d:"*) continue ;; esac
  _nx_path=$_nx_path:$_nx_d
done
IFS=$_nx_ifs
export PATH=$_nx_path

# nix develop sets these for Nix builds: MACOSX_DEPLOYMENT_TARGET=14.0 reaches xcodebuild, PATH_LOCALE redirects macOS libc's locales,
# the Python ones leak into the venv, and its TMPDIR is deleted when the shell exits
unset MACOSX_DEPLOYMENT_TARGET SOURCE_DATE_EPOCH PATH_LOCALE PYTHONPATH PYTHONHASHSEED PYTHONNOUSERSITE PYTHONHOME TEMP TEMPDIR TMP
for _nx_v in DEVELOPER_DIR SDKROOT; do
  case ${!_nx_v-} in /nix/store/*) unset "$_nx_v" ;; esac
done
if TMPDIR=$(getconf DARWIN_USER_TEMP_DIR 2>/dev/null) && [ -n "$TMPDIR" ]; then export TMPDIR; else unset TMPDIR; fi

_nx_root=$(git rev-parse --show-toplevel 2>/dev/null) || _nx_root=$PWD
if [ ! -f "$_nx_root/dm/gfx/tools/own_compilers.py" ]; then
  _nx_note "$PWD is not in a NativeNavi checkout: no venv and no Xcode check"
else
  if [ "${NATIVENAVI_NO_XCODE_CHECK-}" != 1 ]; then
    _nx_want=$(sed -n "s/^XCODE_VERSION = '\([0-9.]*\)'.*/\1/p" "$_nx_root/dm/gfx/tools/own_compilers.py")
    _nx_xcode=$(xcodebuild -version 2>/dev/null | head -n 1)
    if [ -z "$_nx_xcode" ]; then
      _nx_red "no Xcode is selected ($(xcode-select -p 2>/dev/null || echo none)): install Xcode $_nx_want and select it (sudo xcode-select -s /Applications/Xcode.app)"
    else
      [ "$_nx_xcode" = "Xcode $_nx_want" ] || _nx_red "$_nx_xcode is selected; NativeNavi builds with Xcode $_nx_want"
      # own_compilers.py reports nothing when the compiler is missing
      if ! xcrun -sdk macosx metal --version >/dev/null 2>&1; then
        _nx_red "the selected Xcode has no Metal toolchain: xcodebuild -downloadComponent MetalToolchain"
      elif ! _nx_msg=$(python3 "$_nx_root/dm/gfx/tools/own_compilers.py" --check-metal 2>&1); then
        printf '%s\n' "$_nx_msg" >&2
      fi
    fi
  fi

  # cargo looks for its subcommands (clippy, fmt, miri) in $CARGO_HOME/bin before PATH, where rustup's proxies in ~/.cargo/bin would run rustup's toolchains
  # in place of the shell's: a CARGO_HOME of the shells' own, with no bin of its own (the library has no crates to cache)
  export CARGO_HOME=${NATIVENAVI_VENVS:-$_nx_root/.venv}/cargo

  # The stamp's first line is the interpreter: a new one (flake.lock update) must recreate the venv, whose python links into the old store path
  _nx_venv=${NATIVENAVI_VENVS:-$_nx_root/.venv}/$NATIVENAVI_SHELL
  _nx_python_line="python $NATIVENAVI_PYTHON"
  # shellcheck disable=SC2086
  [ -z "$NATIVENAVI_REQUIREMENTS" ] || _nx_stamp=$(cd "$_nx_root" && printf '%s\n%s\n' "$_nx_python_line" "${NATIVENAVI_PIP_EXTRA-}" && shasum -a 256 $NATIVENAVI_REQUIREMENTS)
  if [ -z "$NATIVENAVI_REQUIREMENTS" ]; then
    :   # a shell with no Python packages (miri) makes no venv; shasum would have read the terminal
  elif [ -e "$_nx_venv" ] && [ ! -f "$_nx_venv/.nativenavi" ]; then   # never --clear a folder this hook did not create
    _nx_red "$_nx_venv is not a venv this shell made, so it is left alone and not used: remove it, or set NATIVENAVI_VENVS to another folder"
  else
    if [ "$(cat "$_nx_venv/.nativenavi" 2>/dev/null)" != "$_nx_stamp" ]; then
      if [ "$(head -n 1 "$_nx_venv/.nativenavi" 2>/dev/null)" != "$_nx_python_line" ]; then
        _nx_note "making the venv $_nx_venv"
        "$NATIVENAVI_PYTHON" -m venv --clear "$_nx_venv" && printf '%s\n' "$_nx_python_line" > "$_nx_venv/.nativenavi"
      fi
      if [ -f "$_nx_venv/.nativenavi" ]; then
        _nx_note "installing $NATIVENAVI_REQUIREMENTS${NATIVENAVI_PIP_EXTRA:+ and $NATIVENAVI_PIP_EXTRA} into $_nx_venv"
        _nx_args=
        for _nx_f in $NATIVENAVI_REQUIREMENTS; do
          if [ -n "${NATIVENAVI_PIP_SKIP-}" ]; then
            grep -v -i "^${NATIVENAVI_PIP_SKIP}[=<>~! ]" "$_nx_root/$_nx_f" > "$_nx_venv/$(basename "$_nx_f")"
            _nx_args="$_nx_args -r $_nx_venv/$(basename "$_nx_f")"
          else
            _nx_args="$_nx_args -r $_nx_root/$_nx_f"
          fi
        done
        # Source builds need the Xcode SDK: the Nix Python's sysconfig names a bare clang
        _nx_sdk=$(xcrun --show-sdk-path 2>/dev/null)
        # shellcheck disable=SC2086
        if env ${_nx_sdk:+"SDKROOT=$_nx_sdk"} ${NATIVENAVI_PIP_ENV-} "$_nx_venv/bin/python" -m pip install --disable-pip-version-check --quiet $_nx_args ${NATIVENAVI_PIP_EXTRA-}; then
          printf '%s\n' "$_nx_stamp" > "$_nx_venv/.nativenavi"
        else
          _nx_red "pip could not install the packages into $_nx_venv; the next shell tries again"
        fi
      fi
    fi
    if [ -x "$_nx_venv/bin/python" ]; then
      export VIRTUAL_ENV=$_nx_venv
      export PATH=$_nx_venv/bin:$PATH
    fi
  fi
fi

unset _nx_path _nx_ifs _nx_d _nx_v _nx_root _nx_want _nx_xcode _nx_msg _nx_venv _nx_python_line _nx_stamp _nx_args _nx_f _nx_sdk
unset -f _nx_red _nx_note
