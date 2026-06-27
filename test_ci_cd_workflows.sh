#!/bin/bash
# Test: CI/CD workflow files are valid and well-structured

set -euo pipefail
PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL: $1"; }

# === Test 1: Check workflow files exist ===
if [ -f ".github/workflows/release.yml" ]; then
    pass "release.yml exists"
else
    fail "release.yml is missing"
fi

if [ -f ".github/workflows/ci.yml" ]; then
    pass "ci.yml exists"
else
    fail "ci.yml is missing"
fi

# === Test 2: Basic YAML structure check ===
for f in .github/workflows/*.yml; do
    if head -1 "$f" | grep -q "^name:" 2>/dev/null; then
        pass "Basic YAML check: $f has name field"
    else
        # Try with CRLF tolerance
        if head -1 "$f" | grep -q "name:" 2>/dev/null; then
            pass "Basic YAML check: $f has name field (CRLF)"
        else
            fail "Basic YAML check: $f missing name field"
        fi
    fi
done

# Test run-name field
for f in .github/workflows/*.yml; do
    if grep -q "run-name:" "$f" 2>/dev/null; then
        pass "Basic YAML check: $f has run-name field"
    else
        fail "Basic YAML check: $f missing run-name field"
    fi
done

# === Test 3: release.yml has correct structure ===
if grep -q "on:" .github/workflows/release.yml; then
    pass "release.yml has 'on:' trigger"
else
    fail "release.yml missing 'on:' trigger"
fi

# Check for v* tag trigger in the file (handling CRLF)
if grep -c "v\*" .github/workflows/release.yml > /dev/null 2>&1; then
    pass "release.yml triggers on v* tags"
else
    fail "release.yml does not trigger on v* tags"
fi

if grep -q "VERSION=" .github/workflows/release.yml; then
    pass "release.yml injects VERSION during build"
else
    fail "release.yml missing VERSION injection"
fi

# === Test 4: ci.yml has correct structure ===
if grep -q "on:" .github/workflows/ci.yml; then
    pass "ci.yml has 'on:' trigger"
else
    fail "ci.yml missing 'on:' trigger"
fi

# Check ci.yml has push/pull_request triggers
if grep -q "push:" .github/workflows/ci.yml; then
    pass "ci.yml triggers on push"
else
    fail "ci.yml missing push trigger"
fi

if grep -q "pull_request:" .github/workflows/ci.yml; then
    pass "ci.yml triggers on pull_request"
else
    fail "ci.yml missing pull_request trigger"
fi

# Check ci.yml has build step
if grep -q "make" .github/workflows/ci.yml; then
    pass "ci.yml has make build step"
else
    fail "ci.yml missing make build step"
fi

# === Summary ===
echo "---"
echo "Total: $((PASS+FAIL)) | Pass: $PASS | Fail: $FAIL"
if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
