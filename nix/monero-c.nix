# Pinned monero_c prebuilt (MrCyjaneK/monero_c, LGPL-3.0), one shared library per target.
# release-bundle.zip stores RAW libs at v<ver>/<abi>/libmonero_wallet2_api_c.<ext> (P0-verified).
# The derivation lays out lib/ + include/ so LogosModule.cmake can consume it as a package root.
{ pkgs, src }:
let
  version = "0.18.4.6-RC2";
  tag = "v${version}";
  bundle = pkgs.fetchurl {
    url = "https://github.com/MrCyjaneK/monero_c/releases/download/${tag}/release-bundle.zip";
    sha256 = "94ae3d99f878d1e392b9c49d4c5431e4d0b7780c6177081df5369d05e68cea70";
  };
  targets = {
    "aarch64-darwin" = { abi = "aarch64-apple-darwin"; ext = "dylib"; };
    "x86_64-darwin"  = { abi = "x86_64-apple-darwin";  ext = "dylib"; };
    "aarch64-linux"  = { abi = "aarch64-linux-gnu";    ext = "so";    };
    "x86_64-linux"   = { abi = "x86_64-linux-gnu";     ext = "so";    };
    "x86_64-windows" = { abi = "x86_64-w64-mingw32";   ext = "dll";   };
  };
  system = pkgs.stdenv.hostPlatform.system;
  t = targets.${system} or (throw "monero_c: no prebuilt for ${system}");
  # A DLL is two files: the .dll to ship and the .dll.a import lib to link against.
  extra = if t.ext == "dll" then [ "libmonero_wallet2_api_c.dll.a" ] else [ ];
  # monero_c is built against the winpthread threading model while the module plugins around it
  # use mcfgthread, so nothing else in a payload brings this one in. MEASURED, not assumed:
  # `objdump -p libmonero_wallet2_api_c.dll` lists libwinpthread-1.dll in its import table, and
  # the portable payload gate refuses to package the module without it -- "neither a Windows
  # system DLL nor claimed by windowsHostLibs". It ships in bin/ of the mingw pthreads package.
  # Assembled here rather than inside installPhase: a nested '' .. '' cannot carry an antiquote.
  winpthread =
    if t.ext == "dll"
    then ''install -m0644 ${pkgs.windows.mingw_w64_pthreads}/bin/libwinpthread-1.dll "$out/lib/"''
    else ":";
  entries = map (f: "${tag}/${t.abi}/${f}") ([ "libmonero_wallet2_api_c.${t.ext}" ] ++ extra);
in
pkgs.stdenvNoCC.mkDerivation {
  pname = "monero_wallet2_api_c";
  inherit version;
  srcs = [ bundle ];
  # unzip must run on the BUILD machine, not be a target-platform binary under cross.
  nativeBuildInputs = [ pkgs.buildPackages.unzip ];
  dontConfigure = true; dontBuild = true;
  unpackPhase = ''
    runHook preUnpack
    unzip -j -o ${bundle} ${pkgs.lib.escapeShellArgs entries} -d .
    runHook postUnpack
  '';
  installPhase = ''
    runHook preInstall
    mkdir -p "$out/lib" "$out/include" "$out/share/licenses/monero_c"
    for f in libmonero_wallet2_api_c.*; do install -m0644 "$f" "$out/lib/$f"; done
    install -m0644 ${src}/lib/monero_wallet2_api_c.h ${src}/lib/monero_checksum.h "$out/include/"
    install -m0644 ${src}/LICENSE.monero_c "$out/share/licenses/monero_c/LICENSE"
    # Also in lib/, because that is the only place the module builder's `include`
    # staging looks — LGPL-3.0 §4(d) wants the text beside the library it covers.
    install -m0644 ${src}/LICENSE.monero_c "$out/lib/LICENSE.monero_c"
    ${winpthread}
    runHook postInstall
  '';
  meta = {
    description = "monero_c prebuilt wallet2 C ABI (${t.abi})";
    license = pkgs.lib.licenses.lgpl3Only;
  };
}
