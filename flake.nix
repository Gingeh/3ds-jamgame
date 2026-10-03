{
  description = "flake bs for devkitARM";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    devkitNix.url = "github:bandithedoge/devkitNix";
  };

  outputs = { self, nixpkgs, devkitNix }:
    let
      supportedSystems = [ "x86_64-linux" ];
      forEachSystem = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      devShells = forEachSystem (system:
        let
          pkgs = import nixpkgs {
            inherit system;
            overlays = [ devkitNix.overlays.default ];
          };
        in
        {
          default = pkgs.mkShell.override {
            stdenv = pkgs.devkitNix.stdenvARM;
          } {
            packages = with pkgs; [
              cmake
              gnumake
            ];

            shellHook = ''
              export PATH="${pkgs.cmake}/bin:${pkgs.gnumake}/bin:$PATH"
            '';
          };
        }
      );
    };
}
