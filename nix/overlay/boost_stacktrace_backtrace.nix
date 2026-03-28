final: prev: let
  lib = prev.lib;
in let
  lb = prev.libbacktrace.override {
    enableStatic = true;
    enableShared = true;
  };

  lbPath = prev.symlinkJoin {
    name = "libbacktrace-path";
    paths =
      [lb]
      ++ lib.optional (lb ? dev) lb.dev
      ++ lib.optional (lb ? static) lb.static;
  };

  boost183' =
    (prev.boost183.override {
      extraB2Args = [
        "-sLIBBACKTRACE_PATH=${lbPath}"
      ];
    })
    .overrideAttrs (old: {
      buildInputs = (old.buildInputs or []) ++ [lb];
    });
in {
  libbacktrace = lb;
  boost183 = boost183';
}
