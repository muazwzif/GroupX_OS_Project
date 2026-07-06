#!/bin/bash

# CNS6214 Operating Systems Project - Test Verification Script

echo "=== STEP 1: Building Binaries ==="
make clean
make all
if [ $? -ne 0 ]; then
    echo "[FAIL] Compilation failed!"
    exit 1
fi
echo "[PASS] Compilation successful."

echo ""
echo "=== STEP 2: Generating 1GB Binary Dataset ==="
./generate_data original.dat
if [ $? -ne 0 ]; then
    echo "[FAIL] Data generation failed!"
    exit 1
fi
echo "[PASS] 1GB data generated successfully."

echo ""
echo "=== STEP 3: Starting TCP Server & Client [VAL: Student B (Socket Stress/Error) & Student A (Chunk Transfer)] ==="
./server original.dat &
SERVER_PID=$!

# Give the server a moment to bind and listen
sleep 1.5

# Run the client (which downloads chunks, reassembles, and triggers operations)
./client -p 4 -t 8
CLIENT_EXIT=$?

# Wait for the background server to finish
wait $SERVER_PID
SERVER_EXIT=$?

if [ $CLIENT_EXIT -ne 0 ]; then
    echo "[FAIL] Client returned non-zero exit status: $CLIENT_EXIT"
    exit 1
fi

if [ $SERVER_EXIT -ne 0 ]; then
    echo "[FAIL] Server returned non-zero exit status: $SERVER_EXIT"
    exit 1
fi
echo "[PASS] Client and Server completed execution successfully."

echo ""
echo "=== STEP 4: Validating Output File Integrity [VAL: Student A (Chunk Integrity & MD5)] ==="
# Check if reassembled.dat exists
if [ ! -f "reassembled.dat" ]; then
    echo "[FAIL] reassembled.dat does not exist!"
    exit 1
fi

# MD5 Hash validation
MD5_ORIG=$(md5sum original.dat | awk '{print $1}')
MD5_REAS=$(md5sum reassembled.dat | awk '{print $1}')

echo "original.dat    MD5: $MD5_ORIG"
echo "reassembled.dat MD5: $MD5_REAS"

if [ "$MD5_ORIG" != "$MD5_REAS" ]; then
    echo "[FAIL] MD5 checksum mismatch!"
    exit 1
fi

# Byte-by-byte comparison
cmp original.dat reassembled.dat
if [ $? -ne 0 ]; then
    echo "[FAIL] Byte-by-byte comparison failed! Files are different."
    exit 1
fi
echo "[PASS] File Integrity verified (MD5 and cmp match)."

echo ""
echo "=== STEP 5: Validating Parallel Analytics [VAL: Student D (Sorted/Output Accuracy) & Student C (Threads & Timing)] ==="
./verify
if [ $? -ne 0 ]; then
    echo "[FAIL] Output verification failed!"
    exit 1
fi
echo "[PASS] Analytics results and sort verified."

echo ""
echo "=== STEP 6: Validating Structured Logs [VAL: Student D (Log compliance)] ==="
if [ ! -f "execution_log.txt" ]; then
    echo "[FAIL] execution_log.txt was not generated!"
    exit 1
fi

echo "--- execution_log.txt contents ---"
cat execution_log.txt
echo "----------------------------------"

# Grep validations
grep -q "^\[PART1\] CHUNKS=" execution_log.txt && \
grep -q "SYNC_USED=mutex,sem,condvar" execution_log.txt && \
grep -q "^\[PART2\] THREADS=" execution_log.txt && \
grep -q "DATA_PARALLEL=min,max" execution_log.txt && \
grep -q "TASK_PARALLEL=sort" execution_log.txt && \
grep -q "SORT_ALGO=parallel_merge_sort" execution_log.txt && \
grep -q "^\[STATUS\] SUCCESS" execution_log.txt

if [ $? -ne 0 ]; then
    echo "[FAIL] execution_log.txt format or fields are malformed!"
    exit 1
fi
echo "[PASS] Execution log format compliant."

echo ""
echo "======================================"
echo "[PASS] ALL TESTS PASSED SUCCESSFULLY!"
echo "======================================"
exit 0
