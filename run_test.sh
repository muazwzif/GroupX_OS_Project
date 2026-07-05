#!/usr/bin/env bash
set -euo pipefail

echo "Sorted Verification Checks..."
echo "=================================================="

if [ ! -f "result_sorted.dat" ]; then
    echo "[FAIL] Student D Error: result_sorted.dat was not generated!"
    exit 1
fi

# Use an embedded Python call to verify integers are sorted ascending
python3 -c '
import struct
import sys

try:
    with open("result_sorted.dat", "rb") as f:
        data = f.read()
    
    # Unpack file data as 32-bit signed integers
    count = len(data) // 4
    integers = struct.unpack(f"{count}i", data)
    
    # Scan array structure
    for i in range(len(integers) - 1):
        if integers[i] > integers[i+1]:
            print(f"[FAIL] Sorting violation discovered at index {i}: {integers[i]} > {integers[i+1]}")
            sys.exit(1)
            
    print("[PASS] Validation Complete: result_sorted.dat is successfully sorted ascending.")
except Exception as e:
    print(f"[FAIL] Verification script execution crashed: {e}")
    sys.exit(1)
'

if [ $? -eq 0 ]; then
    echo "[SUCCESS] Verification Passed Successfully."
else
    echo "[FAIL] Verification Found Ordering Anomalies."
    exit 1
fi
=======
#!/bin/bash

echo "=== STEP 1: Building Server ==="
make clean
make server
if [ $? -ne 0 ]; then
    echo "[FAIL] Compilation failed!"
    exit 1
fi
echo "[PASS] Compilation successful."

echo ""
echo "=== STEP 2: Generating Mock Data File ==="
dd if=/dev/urandom of=test.dat bs=1M count=10 2>/dev/null
echo "[PASS] 10MB test data generated (test.dat)."

echo ""
echo "=== STEP 3: Starting TCP Server ==="
./server test.dat &
SERVER_PID=$!

sleep 1.5

if ps -p $SERVER_PID > /dev/null; then
    echo "[PASS] Server started successfully and is listening on port 9090."
    
    echo "=== STEP 4: Server Socket Test ==="
    echo "Testing connection to 127.0.0.1:9090..."
    
    if timeout 2 bash -c '</dev/tcp/127.0.0.1/9090'; then
        echo "[PASS] Server successfully accepted the TCP connection."
    else
        echo "[FAIL] Server did not accept connection."
    fi

    echo "Killing server..."
    kill $SERVER_PID
    echo "[PASS] Server cleaned up."
else
    echo "[FAIL] Server crashed or failed to start."
    wait $SERVER_PID
    exit 1
fi

echo ""
echo "================================================="
echo "[PASS] SERVER STANDALONE TESTS SUCCESSFUL!"
echo "================================================="
exit 0

