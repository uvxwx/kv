{...}: let
  devenvLock = builtins.fromJSON (builtins.readFile ../devenv.lock);

  validateNode = name: let
    node = devenvLock.nodes.${name};
    locked = node.locked or null;
    nodeType =
      if locked == null
      then null
      else locked.type or null;
  in
    if name == devenvLock.root || locked == null || nodeType == "path"
    then []
    else if nodeType == "github"
    then
      if builtins.hasAttr "narHash" locked
      then []
      else ["${name} (github)"]
    else [
      "${name} (${
        if nodeType == null
        then "missing type"
        else nodeType
      })"
    ];

  invalidNodes = builtins.concatLists (map validateNode (builtins.attrNames devenvLock.nodes));
in
  if invalidNodes == []
  then {}
  else
    throw ''
      devenv.lock is missing locked.narHash for remote inputs or contains unsupported remote input types: ${builtins.concatStringsSep ", " invalidNodes}
      Update devenv.lock to include locked.narHash for each remote node.
    ''
