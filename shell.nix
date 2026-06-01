{
  pkgs ? import <nixpkgs> { },
}:

pkgs.mkShell {
  packages = with pkgs; [
    clang
    clang-tools
    alsa-lib
  ];

  shellHook = ''
    echo "Done"
  '';
}
