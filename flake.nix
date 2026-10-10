{
  description = "Yaesu TUN/LIN to Icom AH-4 adapter";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        py = pkgs.python3.withPackages (ps: [ ps.pyserial ]);
      in
      {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            arduino-cli        # builds sketches; Teensy core installed into .arduino15/
            teensy-loader-cli  # fallback flasher for Teensy 2.0 (HalfKay)
            py                 # tools/*.py (CAT control, logging)
            picocom
            ripgrep-all
            poppler-utils      # pdftotext / pdftoppm
          ];
          shellHook = ''
            # Keep arduino-cli state inside the repo (gitignored).
            export ARDUINO_DIRECTORIES_DATA="$PWD/.arduino15"
            export ARDUINO_DIRECTORIES_DOWNLOADS="$PWD/.arduino15/staging"
            export ARDUINO_DIRECTORIES_USER="$PWD/.arduino15/user"
            export ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS="https://www.pjrc.com/teensy/package_teensy_index.json"
          '';
        };
      });
}
