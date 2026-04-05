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

  userverDebugPkg = userverPkgs.userver-debug-addr-ub;
  userverReleasePkg = userverPkgs.userver-release;
  userverDebugDir = "${userverDebugPkg}/lib/cmake/userver";
  userverReleaseDir = "${userverReleasePkg}/lib/cmake/userver";
  testLibs = userverDeps ++ [pkgsWithOverlay.stdenv.cc.cc];
  runtimeLdLibraryPath = lib.makeLibraryPath testLibs;

  mkConfigure = {
    buildType,
    buildDir,
    extraCmakeFlags ? [],
  }: ''
    cmake -S . -B ${lib.escapeShellArg buildDir} -G Ninja \
      -DCMAKE_BUILD_TYPE=${lib.escapeShellArg buildType} \
      -DCMAKE_C_COMPILER=${lib.escapeShellArg "${toolchain.cc}/bin/clang"} \
      -DCMAKE_CXX_COMPILER=${lib.escapeShellArg "${toolchain.cc}/bin/clang++"} \
      -DUSERVER_DEBUG_INFO_COMPRESSION=z \
      -Duserver_DIR=${lib.escapeShellArg (
      if buildType == "Release"
      then userverReleaseDir
      else userverDebugDir
    )} \
      ${lib.concatMapStringsSep " \\\n      " lib.escapeShellArg extraCmakeFlags}
  '';

  mkClangdConfig = name: buildDir:
    pkgsWithOverlay.writeText "kv-clangd-${name}" ''
      CompileFlags:
        CompilationDatabase: ${buildDir}
    '';

  mkConfigureTaskCommands = {
    buildType,
    buildDir,
    clangdConfig,
    extraCmakeFlags ? [],
  }: ''
    ${
      mkConfigure {
        inherit buildType buildDir extraCmakeFlags;
      }
    }
    ln -sf "${clangdConfig}" .clangd
  '';
in {
  imports = [
    ./devenv/process_compose_compat.nix
    ./devenv/lock_validation.nix
  ];

  cachix.enable = true;

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
      perf
      python3
      ruff
      ty
      pkg-config
      mold
      shellcheck
      yamllint
      flamegraph

      toolchain.cc
      llvm21.llvm
      llvm21.clang-tools
      userverDebugPkg
      userverReleasePkg
    ]
    ++ userverDeps;

  env.CMAKE_PREFIX_PATH = lib.makeSearchPath "lib/cmake" [
    userverDebugPkg
    userverReleasePkg
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
  env.USERVER_DIR = userverDebugDir;

  tasks."proj:devBuild" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${
        mkConfigureTaskCommands {
          buildType = "Debug";
          buildDir = "build/dev";
          clangdConfig = mkClangdConfig "dev" "build/dev";
        }
      }
      cmake --build build/dev
    '';
  };

  tasks."proj:devTest" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${
        mkConfigureTaskCommands {
          buildType = "Debug";
          buildDir = "build/dev";
          clangdConfig = mkClangdConfig "dev" "build/dev";
        }
      }
      cmake --build build/dev
      export LD_LIBRARY_PATH=${lib.escapeShellArg runtimeLdLibraryPath}
      ctest --test-dir build/dev --output-on-failure
    '';
  };

  tasks."proj:relBuild" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${
        mkConfigureTaskCommands {
          buildType = "Release";
          buildDir = "build/release";
          clangdConfig = mkClangdConfig "rel" "build/release";
        }
      }
      cmake --build build/release
    '';
  };

  tasks."proj:relTest" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${
        mkConfigureTaskCommands {
          buildType = "Release";
          buildDir = "build/release";
          clangdConfig = mkClangdConfig "rel" "build/release";
        }
      }
      cmake --build build/release
      export LD_LIBRARY_PATH=${lib.escapeShellArg runtimeLdLibraryPath}
      ctest --test-dir build/release --output-on-failure
    '';
  };

  tasks."proj:covTest" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${
        mkConfigureTaskCommands {
          buildType = "Release";
          buildDir = "build/cov";
          clangdConfig = mkClangdConfig "cov" "build/cov";
          extraCmakeFlags = ["-DKV_ENABLE_COVERAGE=ON"];
        }
      }
      export LD_LIBRARY_PATH=${lib.escapeShellArg runtimeLdLibraryPath}
      cmake --build build/cov --target coverage_html
    '';
  };

  tasks."proj:benchFlame" = {
    cwd = config.devenv.root;
    exec = ''
      set -euo pipefail
      ${
        mkConfigureTaskCommands {
          buildType = "Release";
          buildDir = "build/bench";
          clangdConfig = mkClangdConfig "bench" "build/bench";
          extraCmakeFlags = ["-DKV_ENABLE_BENCHMARK_PROFILING=ON"];
        }
      }
      export LD_LIBRARY_PATH=${lib.escapeShellArg runtimeLdLibraryPath}
      cmake --build build/bench --target benchmark_flamegraph
    '';
  };
}
