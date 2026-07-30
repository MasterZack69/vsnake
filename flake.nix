{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default";
  };

  outputs =
    { self, nixpkgs, systems, ... }:
    let
      eachSystem = nixpkgs.lib.genAttrs (import systems);
    in
    {
      packages = eachSystem (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "vsnake";
            version = "0.1.0";
            src = ./.;

            nativeBuildInputs = with pkgs; [ makeWrapper ];
            buildInputs = with pkgs; [ alsa-utils ];

            buildPhase = ''
              runHook preBuild
              $CXX -std=c++11 -O2 snake.cpp -o vsnake -lm
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin
              cp vsnake $out/bin/
              wrapProgram $out/bin/vsnake --prefix PATH : ${pkgs.alsa-utils}/bin
              runHook postInstall
            '';

            meta = {
              description = "Vibe-Coded recreation of the classic snake game in C++";
              homepage = "https://github.com/MasterZack69/vsnake";
              license = pkgs.lib.licenses.agpl3Only;
              mainProgram = "vsnake";
              platforms = pkgs.lib.platforms.linux;
            };
          };
        }
      );

      checks = eachSystem (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = self.packages.${system}.default;
        }
      );

      apps = eachSystem (
        system:
        {
          default = {
            type = "app";
            program = "${self.packages.${system}.default}/bin/vsnake";
          };
        }
      );

      formatter = eachSystem (system: nixpkgs.legacyPackages.${system}.nixpkgs-fmt);

      devShells = eachSystem (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            buildInputs = with pkgs; [
              gcc
              clang-tools
              alsa-utils
            ];
          };
        }
      );
    };
}
