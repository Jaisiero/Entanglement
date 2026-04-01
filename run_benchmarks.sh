#!/bin/bash
# Benchmark script to run on the VM
set -e

cd ~/Projects/Entanglement
git pull origin message_coalescing

# Build
cd build
cmake --build . --config Release -j8 2>&1 | tail -5

echo "=== BUILD COMPLETE ==="

# Kill any old server
pkill -f EntanglementNetBench || true
sleep 1

echo "=== READY FOR BENCHMARKS ==="
echo "Start the server with: cd ~/Projects/Entanglement/build && ./EntanglementNetBench server 9876 8"
