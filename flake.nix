{
  description = "Logos monero_wallet_core_module — in-process Monero wallet engine wrapping monero_c (wallet2 C ABI).";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # Monero built from source: the wallet2 C ABI, from the same tree as the family's daemon.
    # TODO: github:logos-co/logos-monero-nix once that repo is published.
    logos-monero-nix = {
      url = "git+file:///Users/dlipicar/repos/logos-monero-nix";
      inputs.logos-nix.follows = "logos-module-builder/logos-nix";
      inputs.nixpkgs.follows = "logos-module-builder/nixpkgs";
    };
    # Follows THIS module-builder: a skewed generated ABI segfaults in provider init.
    monero_node_module = {
      url = "github:logos-co/logos-monero-node-module";
      inputs.logos-module-builder.follows = "logos-module-builder";
    };
  };

  outputs = inputs@{ logos-module-builder, logos-monero-nix, ... }:
    let
      lib = logos-module-builder.inputs.nixpkgs.lib;
      targets = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" "x86_64-windows" ];
      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        externalLibInputs.monero_wallet2_api_c = {
          input = logos-monero-nix;
          packages.default = "monero-c";
        };
      };
    in
    {
      packages = lib.genAttrs targets (system: module.packages.${system} // {
        monero-c = logos-monero-nix.packages.${system}.monero-c;
      });
    };
}
