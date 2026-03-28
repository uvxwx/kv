{
  pkgs,
  lib,
  config,
  inputs,
  ...
}: let
  pkgsWithOverlay = pkgs.extend (import ./nix/overlay/boost_stacktrace_backtrace.nix);
  llvm21 = pkgsWithOverlay.llvmPackages_21;
  toolchain = import ./nix/toolchain.nix {pkgs = pkgsWithOverlay;};
  userverPython = import ./nix/userver/deps.nix {
    pkgs = pkgsWithOverlay;
    python = pkgsWithOverlay.python3;
  };
  inherit (userverPython) userverDeps userverHelperPython;
  userverPkgs = import ./nix/userver/packages.nix {
    pkgs = pkgsWithOverlay;
    inherit (inputs) userverSrc;
  };

  gitignoreLines = lib.splitString "\n" (builtins.readFile ./.gitignore);
  gitignorePatterns =
    builtins.filter (
      line:
        line
        != ""
        && !(lib.hasPrefix "#" line)
    )
    (map (line: lib.removeSuffix "\r" line) gitignoreLines);

  treefmtExcludesFromGitignore =
    lib.concatMap (
      pattern:
        if lib.hasSuffix "/" pattern
        then ["${lib.removeSuffix "/" pattern}/**"]
        else [pattern "${pattern}/**"]
    )
    gitignorePatterns;

  # Match `../webshot` so we don't accidentally build a different userver
  # variant (and trigger a full rebuild) when switching between repos.
  userverPkg = userverPkgs.userver-debug-addr-ub;
  userverDir = "${userverPkg}/lib/cmake/userver";
  testLibs = userverDeps ++ [pkgsWithOverlay.stdenv.cc.cc];
  runtimeLdLibraryPath = lib.makeLibraryPath testLibs;

  mkConfigure = buildType: buildDir: ''
    cmake -S . -B ${lib.escapeShellArg buildDir} -G Ninja \
      -DCMAKE_BUILD_TYPE=${lib.escapeShellArg buildType} \
      -DCMAKE_C_COMPILER=${lib.escapeShellArg "${toolchain.cc}/bin/clang"} \
      -DCMAKE_CXX_COMPILER=${lib.escapeShellArg "${toolchain.cc}/bin/clang++"} \
      -Duserver_DIR=${lib.escapeShellArg userverDir} \
  '';
in {
  imports = [
    ./devenv/process_compose_compat.nix
    ./devenv/lock_validation.nix
  ];

  cachix.enable = true;

  difftastic.enable = true;

  treefmt = {
    enable = true;
    config = {
      programs = {
        alejandra.enable = true;
        clang-format.enable = true;
        cmake-format.enable = true;
        ruff-format.enable = true;
        yamlfmt.enable = true;
      };
      settings.global.excludes = treefmtExcludesFromGitignore;
    };
  };

  git-hooks.hooks = {
    treefmt = {
      enable = true;
      settings.formatters = builtins.attrValues config.treefmt.config.build.programs;
    };
    ruff.enable = true;
    shellcheck = {
      enable = true;
      args = ["-x"];
    };
    unicode_hygiene = {
      enable = true;
      entry = "python3 check_unicode_hygiene.py";
      package = pkgsWithOverlay.python3;
      language = "system";
      files = "";
    };
    yamllint = {
      enable = true;
      settings.configuration = ''
        extends: relaxed
        rules:
          line-length: disable
      '';
    };
    ty = {
      enable = true;
      entry = "ty check --no-progress";
      package = pkgsWithOverlay.ty;
      language = "system";
      types = ["python"];
      pass_filenames = false;
    };
  };

  packages = with pkgsWithOverlay;
    [
      cmake
      ninja
      ccache
      git
      gdb
      python3
      ruff
      ty
      pkg-config
      mold
      shellcheck
      yamllint

      toolchain.cc
      llvm21.clang-tools
      userverPkg
    ]
    ++ userverDeps;

  env.CMAKE_PREFIX_PATH = lib.makeSearchPath "lib/cmake" [
    userverPkg
    pkgsWithOverlay.boost183.dev
    pkgsWithOverlay.fmt.dev
    pkgsWithOverlay.zstd.dev
    pkgsWithOverlay.cctz
    pkgsWithOverlay.yaml-cpp
  ];

  env.PKG_CONFIG_PATH = lib.makeSearchPath "lib/pkgconfig" [
    pkgsWithOverlay.cryptopp.dev
  ];

  env.USERVER_PYTHON = "${userverHelperPython}/bin/python3";
  env.USERVER_PYTHON_PATH = "${userverHelperPython}/bin/python3";
  env.USERVER_DIR = userverDir;

  tasks."proj:devBuild" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkConfigure "Debug" "build/dev"}
      cmake --build build/dev
    '';
  };

  tasks."proj:devTest" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkConfigure "Debug" "build/dev"}
      cmake --build build/dev
      export LD_LIBRARY_PATH=${lib.escapeShellArg runtimeLdLibraryPath}
      ctest --test-dir build/dev --output-on-failure
    '';
  };

  tasks."proj:relBuild" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkConfigure "Release" "build/release"}
      cmake --build build/release
    '';
  };

  tasks."proj:relTest" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${mkConfigure "Release" "build/release"}
      cmake --build build/release
      export LD_LIBRARY_PATH=${lib.escapeShellArg runtimeLdLibraryPath}
      ctest --test-dir build/release --output-on-failure
    '';
  };
}
