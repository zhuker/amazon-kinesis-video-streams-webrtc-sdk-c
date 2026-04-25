#!/usr/bin/env bash
#
# Push test binary and samples to an Android device/emulator and run tests.
#
# Usage:
#   test-ci-android.sh --build-dir <dir> --serial <serial> --arch <abi> [--asan] [--ubsan]
#
set -euo pipefail

BUILD_DIR=""
SERIAL=""
ARCH=""
ASAN=false
UBSAN=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)  BUILD_DIR="$2"; shift 2 ;;
        --serial)     SERIAL="$2";    shift 2 ;;
        --arch)       ARCH="$2";      shift 2 ;;
        --asan)       ASAN=true;      shift   ;;
        --ubsan)      UBSAN=true;     shift   ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "${BUILD_DIR}" || -z "${SERIAL}" || -z "${ARCH}" ]]; then
    echo "Usage: test-ci-android.sh --build-dir <dir> --serial <serial> --arch <abi> [--asan] [--ubsan]" >&2
    exit 1
fi

ADB="${ANDROID_HOME}/platform-tools/adb"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Verify device supports the requested architecture
DEVICE_ABILIST=$("${ADB}" -s "${SERIAL}" shell getprop ro.product.cpu.abilist 2>/dev/null | tr -d '\r')
if [[ ",$DEVICE_ABILIST," != *",${ARCH},"* ]]; then
    echo "ERROR: Device ${SERIAL} does not support ABI '${ARCH}'." >&2
    echo "  Device supports: ${DEVICE_ABILIST}" >&2
    exit 1
fi
echo "=== Device ${SERIAL} supports ${ARCH} (abilist: ${DEVICE_ABILIST}) ==="

# Push ASAN runtime if requested
if [[ "${ASAN}" == "true" ]]; then
    case "${ARCH}" in
        arm64-v8a)   ASAN_CLANG_ARCH="aarch64" ;;
        armeabi-v7a) ASAN_CLANG_ARCH="arm" ;;
        x86_64)      ASAN_CLANG_ARCH="x86_64" ;;
        x86)         ASAN_CLANG_ARCH="i686" ;;
        *)           echo "ERROR: Unknown ABI '${ARCH}' for ASan" >&2; exit 1 ;;
    esac
    ASAN_RT=$(find "${ANDROID_NDK}/toolchains/llvm/prebuilt" \
        -name "libclang_rt.asan-${ASAN_CLANG_ARCH}-android.so" | head -1)
    "${ADB}" -s "${SERIAL}" push "${ASAN_RT}" /data/local/tmp/
fi

# Create target directories
"${ADB}" -s "${SERIAL}" shell mkdir -p /data/local/tmp/tst /data/local/tmp/samples

# Push test binary
"${ADB}" -s "${SERIAL}" push "${BUILD_DIR}/sdk/tst/webrtc_client_test" /data/local/tmp/tst/
"${ADB}" -s "${SERIAL}" shell chmod +x /data/local/tmp/tst/webrtc_client_test

# Push sample data
for d in h264SampleFrames h265SampleFrames opusSampleFrames girH264 bbbH264; do
    [ -d "samples/$d" ] && "${ADB}" -s "${SERIAL}" push --sync "samples/$d" /data/local/tmp/samples/
done

# Push and run the on-device test runner
"${ADB}" -s "${SERIAL}" push "${SCRIPT_DIR}/run-tests-on-device.sh" /data/local/tmp/
"${ADB}" -s "${SERIAL}" shell chmod +x /data/local/tmp/run-tests-on-device.sh

# Build environment string
ENV_VARS="AWS_KVS_LOG_LEVEL=${AWS_KVS_LOG_LEVEL:-2}"
if [[ "${ASAN}" == "true" ]]; then
    ENV_VARS="${ENV_VARS} ASAN_OPTIONS=detect_odr_violation=0:symbolize=0:print_module_map=2"
fi
if [[ "${UBSAN}" == "true" ]]; then
    ENV_VARS="${ENV_VARS} UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
fi

# Clear logcat before running so crash buffer only contains this run
"${ADB}" -s "${SERIAL}" logcat -c || true

# Run tests, capturing output for post-mortem symbolization
# exec-out gives unbuffered output but does not propagate exit codes,
# so we parse the exit code from the on-device script's output.
TEST_LOG="${BUILD_DIR}/test-output.log"
"${ADB}" -s "${SERIAL}" exec-out "${ENV_VARS} /data/local/tmp/run-tests-on-device.sh '*'" | tee "${TEST_LOG}"
TEST_EXIT=$(sed -n 's/.*exit code: \([0-9]*\).*/\1/p' "${TEST_LOG}" | tail -1)
TEST_EXIT=${TEST_EXIT:-0}

# A gtest assertion failure already produced a "[  FAILED  ]" line with the
# root cause; the SIGABRT tombstone and ASan symbolization that follow are
# redundant noise. Skip them unless the failure looks like a real native crash
# or a sanitizer report.
GTEST_FAILED=false
if grep -qE '^\[  FAILED  \]' "${TEST_LOG}"; then
    GTEST_FAILED=true
fi
SANITIZER_REPORT=false
if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "${TEST_LOG}"; then
    SANITIZER_REPORT=true
fi

# Dump crash logcat only on real native crashes (not plain assertion failures).
if [[ ${TEST_EXIT} -ne 0 ]] && { [[ "${GTEST_FAILED}" == "false" ]] || [[ "${SANITIZER_REPORT}" == "true" ]]; }; then
    echo ""
    echo "=== crash log ==="
    "${ADB}" -s "${SERIAL}" logcat -d -b crash
fi

# Symbolize ASan/UBSan stack traces only when an actual sanitizer report was emitted.
if [[ "${ASAN}" == "true" && ${TEST_EXIT} -ne 0 && "${SANITIZER_REPORT}" == "true" ]]; then
    HOST_BINARY="${BUILD_DIR}/sdk/tst/webrtc_client_test"
    LLVM_SYMBOLIZER=$(find "${ANDROID_NDK}/toolchains/llvm/prebuilt" -name "llvm-symbolizer" -type f | head -1)
    if [[ -x "${LLVM_SYMBOLIZER}" && -f "${HOST_BINARY}" ]]; then
        echo ""
        echo "=== Symbolized stack traces ==="
        # Replace each (webrtc_client_test+0xOFFSET) with resolved function/file info
        while IFS= read -r line; do
            if [[ "${line}" =~ (webrtc_client_test\+0x([0-9a-fA-F]+)) ]]; then
                offset="0x${BASH_REMATCH[2]}"
                sym=$("${LLVM_SYMBOLIZER}" --obj="${HOST_BINARY}" "${offset}" 2>/dev/null | head -2 | tr '\n' ' ')
                if [[ -n "${sym}" ]]; then
                    echo "${line}  ${sym}"
                else
                    echo "${line}"
                fi
            else
                echo "${line}"
            fi
        done < <(grep -E '#[0-9]+ 0x|ERROR: AddressSanitizer|SUMMARY:' "${TEST_LOG}")
    else
        echo "WARN: Cannot symbolize — llvm-symbolizer or unstripped binary not found."
    fi
fi

exit ${TEST_EXIT}
