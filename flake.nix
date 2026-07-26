{
  description = "HyprCapture, a Hyprland screenshot and recording plugin";

  inputs = {
    hyprland.url = "github:hyprwm/Hyprland";
    nixpkgs.follows = "hyprland/nixpkgs";
    systems.follows = "hyprland/systems";
  };

  outputs =
    {
      self,
      hyprland,
      nixpkgs,
      systems,
      ...
    }:
    let
      inherit (nixpkgs) lib;
      eachSystem = lib.genAttrs (import systems);
      pkgsFor = eachSystem (
        system:
        import nixpkgs {
          localSystem.system = system;
          overlays = [
            hyprland.overlays.hyprland-packages
            self.overlays.default
          ];
        }
      );
    in
    {
      packages = eachSystem (system: {
        default = pkgsFor.${system}.hyprcapture;
        inherit (pkgsFor.${system}) hyprcapture;
      });

      overlays.default = final: _prev: {
        hyprcapture = final.callPackage ./nix/package.nix {
          src = self;
        };
      };

      checks = eachSystem (system: {
        inherit (self.packages.${system}) hyprcapture;
      });
    };
}
