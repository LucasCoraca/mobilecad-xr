#!/bin/sh
# Builds and runs the headless interaction checks in the simulator image (sim/Dockerfile: mobilecad-xrsim).
set -e
cd "$(dirname "$0")/.."
docker run --rm -v "$PWD":/src -w /src mobilecad-xrsim sh -c '
  g++ -std=c++17 -O1 -Icore tests/interaction_test.cpp core/interaction.cpp core/renderer.cpp core/scene.cpp core/hands.cpp core/gltf_model.cpp \
      -lGL -o /tmp/interaction_test && /tmp/interaction_test'
