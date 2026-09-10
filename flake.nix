{
  description = "Logos monero_wallet_core_module — in-process Monero wallet engine wrapping monero_c (wallet2 C ABI).";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    logos-nix.url = "github:logos-co/logos-nix";
    # Follows THIS module-builder: a skewed generated ABI segfaults in provider init.
    monero_node_module = {
      url = "github:logos-co/logos-monero-node-module";
      inputs.logos-module-builder.follows = "logos-module-builder";
    };
  };

  outputs = inputs@{ self, logos-module-builder, logos-nix, ... }:
    let
      nixpkgs = logos-module-builder.inputs.nixpkgs;
      lib = nixpkgs.lib;
      nativeSystems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      targets = nativeSystems ++ [ "x86_64-windows" ];
      pkgsFor = system:
        if system == "x86_64-windows"
        then logos-nix.lib.mkWindowsPkgs { buildSystem = "x86_64-linux"; }
        else import nixpkgs { inherit system; };
      # The prebuilt wallet2 C ABI, per target, shaped like a flake input so the builder's
      # `packages.${system}.default` lookup resolves it.
      moneroC = {
        packages = lib.genAttrs targets (system: {
          default = import ./nix/monero-c.nix { pkgs = pkgsFor system; src = self; };
        });
      };
      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        externalLibInputs = { monero_wallet2_api_c = moneroC; };
      };
    in
    {
      packages = lib.genAttrs targets (system: module.packages.${system} // {
        monero-c = moneroC.packages.${system}.default;
      });
    };
}
