#include <jni.h>
#include <unistd.h>
#include <string>
#include <cstdio>
#include <android/log.h>

#include "gtest/gtest.h"
#include "com/amazonaws/kinesis/video/common/CommonDefs.h"
#include "com/amazonaws/kinesis/video/common/PlatformUtils.h"

#define LOG_TAG "webrtc_test_jni"

// File for persistent logging (survives logcat truncation on low-end devices)
static FILE* g_logFile = nullptr;

// Custom gtest listener that routes output to Android logcat.
class LogcatPrinter : public ::testing::EmptyTestEventListener {
  public:
    void OnTestProgramStart(const ::testing::UnitTest& unit_test) override
    {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[==========] Running %d tests from %d test suites.",
                            unit_test.test_to_run_count(), unit_test.test_suite_to_run_count());
        if (g_logFile) {
            fprintf(g_logFile, "[==========] Running %d tests from %d test suites.\n",
                    unit_test.test_to_run_count(), unit_test.test_suite_to_run_count());
            fflush(g_logFile);
        }
    }

    void OnTestSuiteStart(const ::testing::TestSuite& suite) override
    {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[----------] %d tests from %s", suite.test_to_run_count(), suite.name());
        if (g_logFile) {
            fprintf(g_logFile, "[----------] %d tests from %s\n", suite.test_to_run_count(), suite.name());
            fflush(g_logFile);
        }
    }

    void OnTestStart(const ::testing::TestInfo& info) override
    {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[ RUN      ] %s.%s", info.test_suite_name(), info.name());
        if (g_logFile) {
            fprintf(g_logFile, "[ RUN      ] %s.%s\n", info.test_suite_name(), info.name());
            fflush(g_logFile);
        }
    }

    void OnTestPartResult(const ::testing::TestPartResult& result) override
    {
        if (result.failed()) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "%s:%d: Failure\n%s",
                                result.file_name() ? result.file_name() : "unknown", result.line_number(),
                                result.message() ? result.message() : "");
            if (g_logFile) {
                fprintf(g_logFile, "%s:%d: Failure\n%s\n",
                        result.file_name() ? result.file_name() : "unknown", result.line_number(),
                        result.message() ? result.message() : "");
                fflush(g_logFile);
            }
        }
    }

    void OnTestEnd(const ::testing::TestInfo& info) override
    {
        if (info.result()->Passed()) {
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[       OK ] %s.%s (%lld ms)", info.test_suite_name(), info.name(),
                                (long long) info.result()->elapsed_time());
            if (g_logFile) {
                fprintf(g_logFile, "[       OK ] %s.%s (%lld ms)\n", info.test_suite_name(), info.name(),
                        (long long) info.result()->elapsed_time());
                fflush(g_logFile);
            }
        } else {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "[  FAILED  ] %s.%s (%lld ms)", info.test_suite_name(), info.name(),
                                (long long) info.result()->elapsed_time());
            if (g_logFile) {
                fprintf(g_logFile, "[  FAILED  ] %s.%s (%lld ms)\n", info.test_suite_name(), info.name(),
                        (long long) info.result()->elapsed_time());
                fflush(g_logFile);
            }
        }
    }

    void OnTestSuiteEnd(const ::testing::TestSuite& suite) override
    {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[----------] %d tests from %s (%lld ms total)", suite.test_to_run_count(),
                            suite.name(), (long long) suite.elapsed_time());
        if (g_logFile) {
            fprintf(g_logFile, "[----------] %d tests from %s (%lld ms total)\n", suite.test_to_run_count(),
                    suite.name(), (long long) suite.elapsed_time());
            fflush(g_logFile);
        }
    }

    void OnTestProgramEnd(const ::testing::UnitTest& unit_test) override
    {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[==========] %d tests from %d test suites ran. (%lld ms total)",
                            unit_test.test_to_run_count(), unit_test.test_suite_to_run_count(),
                            (long long) unit_test.elapsed_time());
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[  PASSED  ] %d tests.", unit_test.successful_test_count());
        if (unit_test.failed_test_count() > 0) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "[  FAILED  ] %d tests.", unit_test.failed_test_count());
        }
        if (g_logFile) {
            fprintf(g_logFile, "[==========] %d tests from %d test suites ran. (%lld ms total)\n",
                    unit_test.test_to_run_count(), unit_test.test_suite_to_run_count(),
                    (long long) unit_test.elapsed_time());
            fprintf(g_logFile, "[  PASSED  ] %d tests.\n", unit_test.successful_test_count());
            if (unit_test.failed_test_count() > 0) {
                fprintf(g_logFile, "[  FAILED  ] %d tests.\n", unit_test.failed_test_count());
            }
            fflush(g_logFile);
        }
    }
};

// Logcat-backed log function matching kvspic's logPrintFunc signature.
static VOID logcatLogPrint(UINT32 level, const PCHAR tag, const PCHAR fmt, ...)
{
    int androidLevel;
    switch (level) {
        case LOG_LEVEL_VERBOSE:
            // androidLevel = ANDROID_LOG_VERBOSE;
            // break;
            // too much verbose logs
            return;
        case LOG_LEVEL_DEBUG:
            androidLevel = ANDROID_LOG_DEBUG;
            break;
        case LOG_LEVEL_INFO:
            androidLevel = ANDROID_LOG_INFO;
            break;
        case LOG_LEVEL_WARN:
            androidLevel = ANDROID_LOG_WARN;
            break;
        case LOG_LEVEL_ERROR:
            androidLevel = ANDROID_LOG_ERROR;
            break;
        case LOG_LEVEL_FATAL:
            androidLevel = ANDROID_LOG_FATAL;
            break;
        default:
            androidLevel = ANDROID_LOG_DEFAULT;
            break;
    }
    va_list args;
    va_start(args, fmt);
    if (tag) {
        char taggedFmt[1024];
        snprintf(taggedFmt, sizeof(taggedFmt), "[%s] %s", tag, fmt);
        __android_log_vprint(androidLevel, LOG_TAG, taggedFmt, args);
        if (g_logFile) {
            va_end(args);
            va_start(args, fmt);
            vfprintf(g_logFile, taggedFmt, args);
            fprintf(g_logFile, "\n");
            fflush(g_logFile);
        }
    } else {
        __android_log_vprint(androidLevel, LOG_TAG, fmt, args);
        if (g_logFile) {
            va_end(args);
            va_start(args, fmt);
            vfprintf(g_logFile, fmt, args);
            fprintf(g_logFile, "\n");
            fflush(g_logFile);
        }
    }
    va_end(args);
}

extern "C" JNIEXPORT jint JNICALL Java_com_kvs_webrtctest_NativeTestLib_runTests(JNIEnv* env, jclass /* clazz */, jstring workDir,
                                                                                  jstring filter, jstring logDir)
{
    // Route KVS SDK logs to logcat
    globalCustomLogPrintFn = logcatLogPrint;

    const char* workDirStr = env->GetStringUTFChars(workDir, nullptr);
    const char* filterStr = env->GetStringUTFChars(filter, nullptr);
    const char* logDirStr = env->GetStringUTFChars(logDir, nullptr);

    // Open log file for persistent logging (survives logcat truncation)
    std::string logPath = std::string(logDirStr) + "/test.log";
    g_logFile = fopen(logPath.c_str(), "w");
    if (g_logFile) {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Logging to %s", logPath.c_str());
    } else {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Failed to open log file %s", logPath.c_str());
    }

    // Change to working directory so tests find sample data at ../samples/
    chdir(workDirStr);

    // Build gtest argv
    std::string filterArg = std::string("--gtest_filter=") + filterStr;
    char arg0[] = "webrtc_test_jni";
    char* argv[] = {arg0, const_cast<char*>(filterArg.c_str()), "--gtest_fail_fast", nullptr};
    int argc = 3;

    ::testing::InitGoogleTest(&argc, argv);

    // Replace default stdout printer with logcat printer
    ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();
    delete listeners.Release(listeners.default_result_printer());
    listeners.Append(new LogcatPrinter());

    int rc = RUN_ALL_TESTS();

    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }

    env->ReleaseStringUTFChars(logDir, logDirStr);
    env->ReleaseStringUTFChars(filter, filterStr);
    env->ReleaseStringUTFChars(workDir, workDirStr);

    return rc;
}
