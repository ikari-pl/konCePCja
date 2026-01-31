# Package description for Nix (http://nixos.org)
with import <nixpkgs> {};
koncepcja.overrideDerivation (old: {
  # overrideDerivation allows it to specify additional dependencies
  buildInputs = [ gettext ] ++ old.buildInputs;
})
