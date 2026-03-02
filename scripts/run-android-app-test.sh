#!/usr/bin/env bash
#
# Install and run android-test-app instrumented tests on a device.
#
# Usage:
#   run-android-app-test.sh <serial>
#
# Expects ANDROID_HOME to be set.
set -euo pipefail

SERIAL="${1:?Usage: run-android-app-test.sh <serial>}"
ADB="${ANDROID_HOME}/platform-tools/adb"

APK_DEBUG=$(find android-test-app/app/build -name "app-debug.apk" -not -name "*androidTest*" | head -1)
APK_TEST=$(find android-test-app/app/build -name "app-debug-androidTest.apk" | head -1)

"${ADB}" -s "${SERIAL}" uninstall com.kvs.webrtctest || true
"${ADB}" -s "${SERIAL}" uninstall com.kvs.webrtctest.test || true
"${ADB}" -s "${SERIAL}" install -r -t "${APK_DEBUG}"
"${ADB}" -s "${SERIAL}" install -r -t "${APK_TEST}"

"${ADB}" -s "${SERIAL}" logcat -c || true

OUTPUT_LOG=$(mktemp)
trap 'rm -f "$OUTPUT_LOG"' EXIT

set +o pipefail
"${ADB}" -s "${SERIAL}" shell am instrument -w -r \
  com.kvs.webrtctest.test/androidx.test.runner.AndroidJUnitRunner \
  | tr -d '\r' | tee "$OUTPUT_LOG"
set -o pipefail

STATUS_CODE=$(grep '^INSTRUMENTATION_STATUS_CODE:' "$OUTPUT_LOG" | tail -1 | awk '{print $2}')

if [[ "$STATUS_CODE" != "0" ]]; then
  echo "::error::Tests failed (INSTRUMENTATION_STATUS_CODE: ${STATUS_CODE})"
  echo "=== logcat ==="
  "${ADB}" -s "${SERIAL}" logcat -d -s "webrtc_test_jni:*" "WebRtcNativeTest:*" "TestRunner:*"
  exit 1
fi
