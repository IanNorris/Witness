{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  name = "witness-dev";

  buildInputs = with pkgs; [
    # Build tools
    cmake
    ninja
    gcc
    pkg-config

    # Core dependencies
    ffmpeg          # video decode/encode (libavcodec, libavformat, etc.)
    opencv4         # computer vision
    libsodium       # cryptography
    sqlite          # database
    openssl         # TLS
    onnxruntime     # neural network inference
    crow            # HTTP/WebSocket server (header-only + cmake config)
    asio            # async I/O (required by Crow)

    # Runtime tools for TLS cert generation
    openssl.bin
  ];

  shellHook = ''
    echo "Witness dev shell ready."
    echo "Build with:"
    echo "  cmake -B build-linux -G Ninja -DWITNESS_USE_SYSTEM_DEPS=ON -DCMAKE_BUILD_TYPE=Release"
    echo "  cmake --build build-linux"
  '';
}
