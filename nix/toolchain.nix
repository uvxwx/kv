{pkgs}: let
  llvm = pkgs.llvmPackages_21;
in {
  stdenv = llvm.stdenv;
  cc = llvm.clang;
}
