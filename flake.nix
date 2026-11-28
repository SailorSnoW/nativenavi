{
  description = "NativeNavi development shells for Apple silicon Macs";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  # The official Rust builds, by version: the host's Rust library is built with the one dm/host/rust/rust-toolchain.toml names
  inputs.rust-overlay = {
    url = "github:oxalica/rust-overlay";
    inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs =
    {
      self,
      nixpkgs,
      rust-overlay,
    }:
    let
      inherit (nixpkgs) lib;
      systems = [ "aarch64-darwin" ];
      eachSystem = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Read from the workflow so the LLVM release is pinned in one place
      workflowEnv =
        file: key:
        let
          m = lib.findFirst (x: x != null) null (
            map (builtins.match " *${key}: *([^ #]+).*") (lib.splitString "\n" (builtins.readFile file))
          );
        in
        if m == null then throw "${baseNameOf (toString file)}: no ${key} line" else builtins.head m;
      lintC = ./.github/workflows/lint-c.yml;

      # atheris 3.1.0 has no macOS wheel and no sdist on PyPI
      atheris = "git+https://github.com/google/atheris@e36da74cf42b5834c4e31d7cabb58d796695d29f";

      # Read from the file rustup reads outside Nix, so the Rust version is pinned in one place
      rustPin = (lib.importTOML ./dm/host/rust/rust-toolchain.toml).toolchain;
    in
    {
      packages = eachSystem (
        pkgs:
        let
          rustBin = rust-overlay.lib.mkRustBin { } pkgs;
        in
        {
          llvm = pkgs.callPackage ./dm/nix/llvm.nix {
            version = workflowEnv lintC "LLVM_VERSION";
            sha256 = workflowEnv lintC "LLVM_SHA256";
          };
          # The host's Rust library (dm/host/rust.mk), with the lints its checks run
          rust = rustBin.stable.${rustPin.channel}.minimal.override {
            extensions = [
              "clippy"
              "rustfmt"
            ];
            inherit (rustPin) targets;
          };
          # Miri for the Rust library's tests (the miri shell): the newest nightly rust-overlay had when flake.lock was last updated
          rust-miri = rustBin.selectLatestNightlyWith (
            toolchain:
            toolchain.minimal.override {
              extensions = [
                "miri"
                "rust-src"
              ];
            }
          );
        }
      );

      formatter = eachSystem (pkgs: pkgs.nixfmt-tree);

      devShells = eachSystem (
        pkgs:
        let
          own = self.packages.${pkgs.stdenv.hostPlatform.system};
          python = pkgs.python314;
          # Microsoft's build: the source-built SDK's NativeAOT pack links nixpkgs' brotli into dmsh_xlate (docs/reference/packing.md)
          dotnet = pkgs.dotnetCorePackages.sdk_10_0-bin;
          build = [
            python
            dotnet
            pkgs.zstd
            pkgs.spirv-tools
            own.rust
          ];
          # The SDK wrapper does not set it, and the translator's apphost cannot find the runtime without it
          dotnetRoot = {
            DOTNET_ROOT = "${dotnet.unwrapped}/share/dotnet";
          };

          # mkShellNoCC: Nix's cc wrapper and SDK would replace Xcode's clang
          shell =
            name:
            {
              tools,
              requirements,
              env ? { },
            }:
            pkgs.mkShellNoCC (
              {
                name = "nativenavi-${name}";
                packages = tools;
                NATIVENAVI_SHELL = name;
                NATIVENAVI_TOOLS = lib.makeBinPath tools;
                NATIVENAVI_STDENV_PATH = lib.makeBinPath pkgs.stdenvNoCC.initialPath;
                NATIVENAVI_PYTHON = python.interpreter;
                NATIVENAVI_REQUIREMENTS = lib.concatMapStringsSep " " (r: ".github/requirements/${r}") requirements;
                shellHook = "source ${./dm/nix/shell_hook.sh}";
              }
              // env
            );
        in
        {
          default = shell "default" {
            tools = build;
            requirements = [
              "tools.txt"
              "tests.txt"
            ];
            env = dotnetRoot;
          };

          # tools.txt with each tool's own list: the tests and pyright see the packages the tools import, at the versions the default shell has
          coverage = shell "coverage" {
            tools = build;
            requirements = [
              "coverage.txt"
              "tools.txt"
              "tests.txt"
            ];
            env = dotnetRoot;
          };

          lint = shell "lint" {
            tools = [
              python
              pkgs.nodejs
            ];
            requirements = [
              "lint.txt"
              "tools.txt"
            ];
            env = {
              LLVM = "${own.llvm}";
            };
          };

          # Undefined behaviour in the Rust library: cargo miri test in dm/host/rust (docs/development.md, "Rust in the host")
          miri = shell "miri" {
            tools = [ own.rust-miri ];
            requirements = [ ];
          };

          # Apple's clang has no libFuzzer
          fuzz = shell "fuzz" {
            tools = [ python ];
            requirements = [ "fuzz.txt" ];
            env = {
              NATIVENAVI_PIP_SKIP = "atheris";
              NATIVENAVI_PIP_EXTRA = atheris;
              NATIVENAVI_PIP_ENV = "CLANG_BIN=${own.llvm}/bin/clang CC=${own.llvm}/bin/clang CXX=${own.llvm}/bin/clang++";
            };
          };
        }
      );
    };
}
