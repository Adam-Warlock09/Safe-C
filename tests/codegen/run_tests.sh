#!/bin/bash
# tests/codegen/run_tests.sh
# End-to-end testing: Emits LLVM IR, and executes binaries if main() exists.

COMPILER="./safec"
TEST_DIR="tests/sema/ownership"
PASSED=0
FAILED=0

echo "================================================="
echo " Phase 5: LLVM IR & Native Execution Tests       "
echo "================================================="

mkdir -p tests/codegen/bin

for file in $TEST_DIR/0*.safec $TEST_DIR/1[0-5]*.safec; do
    basename=$(basename "$file" .safec)
    
    # Smart check: Does this file actually have a main() function?
    if grep -qw "main" "$file"; then
        bin_out="tests/codegen/bin/$basename.out"
        
        # Compile to binary (omitting ASan flag to avoid driver rejection)
        $COMPILER "$file" -O2 -o "$bin_out" > /dev/null 2>&1
        
        if [ $? -ne 0 ]; then
            echo "[LINK FAIL]  $basename"
            FAILED=$((FAILED + 1))
            continue
        fi

        # Execute
        ./"$bin_out" > /dev/null 2>&1
        if [ $? -eq 0 ]; then
            echo "[EXEC PASS]  $basename"
            PASSED=$((PASSED + 1))
        else
            echo "[EXEC FAIL]  $basename"
            FAILED=$((FAILED + 1))
        fi
    else
        # No main() function: just verify LLVM IR emission works without crashing
        $COMPILER --emit-llvm "$file" > /dev/null 2>&1
        if [ $? -eq 0 ]; then
            echo "[EMIT PASS]  $basename"
            PASSED=$((PASSED + 1))
        else
            echo "[EMIT FAIL]  $basename"
            FAILED=$((FAILED + 1))
        fi
    fi
done

echo "-------------------------------------------------"
echo " Codegen Tests Completed: $PASSED passed, $FAILED failed."
echo "================================================="

if [ $FAILED -ne 0 ]; then
    exit 1
fi