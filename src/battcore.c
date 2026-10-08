/*
 * battcore — native reimplementation of system/bin/batteryhealth.
 *
 * Reproduces, byte-for-byte in its text output, the same 030103 flow the
 * shell script used to run: FCC via the charger HAL (through ChgFcc.dex +
 * app_process, unchanged), design capacity via dumpsys/power_profile.xml,
 * the integrate_gauge_fcc_flag existence check, the good/excellent
 * threshold table, the unrounded percent/grade arithmetic, and kv/json/
 * human formatting. It also owns the local HTTP server that used to be
 * busybox httpd (static webroot + an internal /cgi-bin/read endpoint), so
 * the module no longer depends on busybox at all.
 *
 * Every data source, path and threshold below is a straight port of
 * system/bin/batteryhealth; see that file's history / README.md for the
 * reverse-engineering trail. Nothing here invents a new source.
 *
 * Build (Android NDK, both ABIs; see system/bin/batteryhealth for the
 * runtime ABI switch):
 *
 *   NDK=<path to NDK, e.g. .../android-sdk/ndk/27.2.12479018>
 *   CLANG=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe
 *   $CLANG --target=aarch64-linux-android24  -O2 -o battcore.arm64 src/battcore.c
 *   $CLANG --target=armv7a-linux-androideabi24 -O2 -o battcore.arm   src/battcore.c
 *   $NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe --strip-all battcore.arm64 battcore.arm
 *   -> copy both into system/bin/
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

#define DEFAULT_PORT 8899

static char g_moddir[512];

/* ------------------------------------------------------- string obfuscation
 *
 * Every path, HAL/property name, HTTP literal and user-facing label the
 * module reads or prints is XOR-masked at rest (key below) instead of
 * sitting in .rodata as a plain string a `strings` pass can read straight
 * off the binary. This does not hide anything from someone willing to
 * attach a disassembler or trace syscalls at runtime - see the project
 * notes - it only raises the bar past "grep the binary".
 */
#define XK 0x5a

static void xdec(const volatile unsigned char *enc, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) out[i] = (char)(enc[i] ^ XK);
    out[n] = '\0';
}

#define XDECL(name, var) char var[sizeof(name) + 1]; xdec(name, sizeof(name), var)

static const volatile unsigned char K_FCC_NODE[] = {0x75,0x29,0x23,0x29,0x75,0x39,0x36,0x3b,0x29,0x29,0x75,0x35,0x2a,0x36,0x2f,0x29,0x5,0x39,0x32,0x3d,0x75,0x38,0x3b,0x2e,0x2e,0x3f,0x28,0x23,0x75,0x38,0x3b,0x2e,0x2e,0x3f,0x28,0x23,0x5,0x3c,0x39,0x39};
static const volatile unsigned char K_FLAG_NODE[] = {0x75,0x2a,0x28,0x35,0x39,0x75,0x39,0x32,0x3b,0x28,0x3d,0x3f,0x28,0x75,0x33,0x34,0x2e,0x3f,0x3d,0x28,0x3b,0x2e,0x3f,0x5,0x3d,0x3b,0x2f,0x3d,0x3f,0x5,0x3c,0x39,0x39,0x5,0x3c,0x36,0x3b,0x3d};
static const volatile unsigned char K_DEX_SUFFIX[] = {0x75,0x38,0x33,0x34,0x75,0x39,0x32,0x3d,0x3c,0x39,0x39,0x74,0x3e,0x3f,0x22};
static const volatile unsigned char K_WEBROOT_SUFFIX[] = {0x75,0x2d,0x3f,0x38,0x28,0x35,0x35,0x2e};
static const volatile unsigned char K_WEBUI_PORT_SUFFIX[] = {0x75,0x2d,0x3f,0x38,0x2f,0x33,0x5,0x2a,0x35,0x28,0x2e};
static const volatile unsigned char K_DEFAULT_MODDIR[] = {0x75,0x3e,0x3b,0x2e,0x3b,0x75,0x3b,0x3e,0x38,0x75,0x37,0x35,0x3e,0x2f,0x36,0x3f,0x29,0x75,0x35,0x2a,0x2a,0x35,0x5,0x38,0x3b,0x2e,0x2e,0x3f,0x28,0x23,0x5,0x32,0x3f,0x3b,0x36,0x2e,0x32};
static const volatile unsigned char K_PIDFILE[] = {0x75,0x3e,0x3b,0x2e,0x3b,0x75,0x36,0x35,0x39,0x3b,0x36,0x75,0x2e,0x37,0x2a,0x75,0x35,0x2a,0x2a,0x35,0x5,0x38,0x3b,0x2e,0x2e,0x3f,0x28,0x23,0x5,0x32,0x3f,0x3b,0x36,0x2e,0x32,0x5,0x32,0x2e,0x2e,0x2a,0x3e,0x74,0x2a,0x33,0x3e};
static const volatile unsigned char K_JAR1[] = {0x75,0x29,0x23,0x29,0x2e,0x3f,0x37,0x5,0x3f,0x22,0x2e,0x75,0x3c,0x28,0x3b,0x37,0x3f,0x2d,0x35,0x28,0x31,0x75,0x2c,0x3f,0x34,0x3e,0x35,0x28,0x74,0x35,0x2a,0x36,0x2f,0x29,0x74,0x32,0x3b,0x28,0x3e,0x2d,0x3b,0x28,0x3f,0x74,0x39,0x32,0x3b,0x28,0x3d,0x3f,0x28,0x77,0xc,0x6b,0x77,0x30,0x3b,0x2c,0x3b,0x74,0x30,0x3b,0x28};
static const volatile unsigned char K_JAR2[] = {0x75,0x29,0x23,0x29,0x2e,0x3f,0x37,0x5,0x3f,0x22,0x2e,0x75,0x3c,0x28,0x3b,0x37,0x3f,0x2d,0x35,0x28,0x31,0x75,0x2c,0x3f,0x34,0x3e,0x35,0x28,0x74,0x35,0x2a,0x36,0x2f,0x29,0x74,0x32,0x3b,0x28,0x3e,0x2d,0x3b,0x28,0x3f,0x74,0x39,0x32,0x3b,0x28,0x3d,0x3f,0x28,0x77,0xc,0x69,0x77,0x30,0x3b,0x2c,0x3b,0x74,0x30,0x3b,0x28};
static const volatile unsigned char K_JAR3[] = {0x75,0x29,0x23,0x29,0x2e,0x3f,0x37,0x5,0x3f,0x22,0x2e,0x75,0x3c,0x28,0x3b,0x37,0x3f,0x2d,0x35,0x28,0x31,0x75,0x2c,0x3f,0x34,0x3e,0x35,0x28,0x74,0x35,0x2a,0x36,0x2f,0x29,0x74,0x32,0x3b,0x28,0x3e,0x2d,0x3b,0x28,0x3f,0x74,0x39,0x32,0x3b,0x28,0x3d,0x3f,0x28,0x77,0xc,0x6b,0x74,0x6a,0x77,0x30,0x3b,0x2c,0x3b,0x74,0x30,0x3b,0x28};
static const volatile unsigned char K_JAR4[] = {0x75,0x35,0x3e,0x37,0x75,0x3c,0x28,0x3b,0x37,0x3f,0x2d,0x35,0x28,0x31,0x75,0x2c,0x3f,0x34,0x3e,0x35,0x28,0x74,0x35,0x2a,0x36,0x2f,0x29,0x74,0x32,0x3b,0x28,0x3e,0x2d,0x3b,0x28,0x3f,0x74,0x39,0x32,0x3b,0x28,0x3d,0x3f,0x28,0x77,0xc,0x6b,0x77,0x30,0x3b,0x2c,0x3b,0x74,0x30,0x3b,0x28};
static const volatile unsigned char K_JAR5[] = {0x75,0x35,0x3e,0x37,0x75,0x3c,0x28,0x3b,0x37,0x3f,0x2d,0x35,0x28,0x31,0x75,0x2c,0x3f,0x34,0x3e,0x35,0x28,0x74,0x35,0x2a,0x36,0x2f,0x29,0x74,0x32,0x3b,0x28,0x3e,0x2d,0x3b,0x28,0x3f,0x74,0x39,0x32,0x3b,0x28,0x3d,0x3f,0x28,0x77,0xc,0x6b,0x74,0x6a,0x77,0x30,0x3b,0x2c,0x3b,0x74,0x30,0x3b,0x28};
static const volatile unsigned char K_XML1[] = {0x75,0x35,0x3e,0x37,0x75,0x3f,0x2e,0x39,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x2a,0x28,0x35,0x3c,0x33,0x36,0x3f,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x2a,0x28,0x35,0x3c,0x33,0x36,0x3f,0x74,0x22,0x37,0x36};
static const volatile unsigned char K_XML2[] = {0x75,0x2c,0x3f,0x34,0x3e,0x35,0x28,0x75,0x3f,0x2e,0x39,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x2a,0x28,0x35,0x3c,0x33,0x36,0x3f,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x2a,0x28,0x35,0x3c,0x33,0x36,0x3f,0x74,0x22,0x37,0x36};
static const volatile unsigned char K_XML3[] = {0x75,0x29,0x23,0x29,0x2e,0x3f,0x37,0x5,0x3f,0x22,0x2e,0x75,0x3f,0x2e,0x39,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x2a,0x28,0x35,0x3c,0x33,0x36,0x3f,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x2a,0x28,0x35,0x3c,0x33,0x36,0x3f,0x74,0x22,0x37,0x36};
static const volatile unsigned char K_XML4[] = {0x75,0x29,0x23,0x29,0x2e,0x3f,0x37,0x75,0x3f,0x2e,0x39,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x2a,0x28,0x35,0x3c,0x33,0x36,0x3f,0x74,0x22,0x37,0x36};
static const volatile unsigned char K_XML_ATTR[] = {0x34,0x3b,0x37,0x3f,0x67,0x78,0x38,0x3b,0x2e,0x2e,0x3f,0x28,0x23,0x74,0x39,0x3b,0x2a,0x3b,0x39,0x33,0x2e,0x23,0x78};
static const volatile unsigned char K_DUMPSYS_MARK[] = {0x1f,0x29,0x2e,0x33,0x37,0x3b,0x2e,0x3f,0x3e,0x7a,0x38,0x3b,0x2e,0x2e,0x3f,0x28,0x23,0x7a,0x39,0x3b,0x2a,0x3b,0x39,0x33,0x2e,0x23};
static const volatile unsigned char K_UEV1[] = {0x75,0x29,0x23,0x29,0x75,0x39,0x36,0x3b,0x29,0x29,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x29,0x2f,0x2a,0x2a,0x36,0x23,0x75,0x38,0x3b,0x2e,0x2e,0x3f,0x28,0x23,0x75,0x2f,0x3f,0x2c,0x3f,0x34,0x2e};
static const volatile unsigned char K_UEV2[] = {0x75,0x29,0x23,0x29,0x75,0x39,0x36,0x3b,0x29,0x29,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x29,0x2f,0x2a,0x2a,0x36,0x23,0x75,0x37,0x3b,0x33,0x34,0x75,0x2f,0x3f,0x2c,0x3f,0x34,0x2e};
static const volatile unsigned char K_UEV3[] = {0x75,0x29,0x23,0x29,0x75,0x39,0x36,0x3b,0x29,0x29,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x29,0x2f,0x2a,0x2a,0x36,0x23,0x75,0x38,0x37,0x29,0x75,0x2f,0x3f,0x2c,0x3f,0x34,0x2e};
static const volatile unsigned char K_CYC1[] = {0x75,0x29,0x23,0x29,0x75,0x39,0x36,0x3b,0x29,0x29,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x29,0x2f,0x2a,0x2a,0x36,0x23,0x75,0x38,0x3b,0x2e,0x2e,0x3f,0x28,0x23,0x75,0x39,0x23,0x39,0x36,0x3f,0x5,0x39,0x35,0x2f,0x34,0x2e};
static const volatile unsigned char K_CYC2[] = {0x75,0x29,0x23,0x29,0x75,0x39,0x36,0x3b,0x29,0x29,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x29,0x2f,0x2a,0x2a,0x36,0x23,0x75,0x37,0x3b,0x33,0x34,0x75,0x39,0x23,0x39,0x36,0x3f,0x5,0x39,0x35,0x2f,0x34,0x2e};
static const volatile unsigned char K_CYC3[] = {0x75,0x29,0x23,0x29,0x75,0x39,0x36,0x3b,0x29,0x29,0x75,0x2a,0x35,0x2d,0x3f,0x28,0x5,0x29,0x2f,0x2a,0x2a,0x36,0x23,0x75,0x38,0x37,0x29,0x75,0x39,0x23,0x39,0x36,0x3f,0x5,0x39,0x35,0x2f,0x34,0x2e};
static const volatile unsigned char K_PS_PREFIX[] = {0xa,0x15,0xd,0x1f,0x8,0x5,0x9,0xf,0xa,0xa,0x16,0x3,0x5};
static const volatile unsigned char K_FCC_EQ[] = {0x3c,0x39,0x39,0x67};
static const volatile unsigned char K_VIA_EQ[] = {0x2c,0x33,0x3b,0x67};
static const volatile unsigned char K_PROP_SEPARATE_SOFT[] = {0x28,0x35,0x74,0x29,0x3f,0x2a,0x3b,0x28,0x3b,0x2e,0x3f,0x74,0x29,0x35,0x3c,0x2e};
static const volatile unsigned char K_PROP_PRODUCT_TYPE[] = {0x28,0x35,0x74,0x35,0x2a,0x36,0x2f,0x29,0x74,0x33,0x37,0x3b,0x3d,0x3f,0x74,0x37,0x23,0x5,0x2a,0x28,0x35,0x3e,0x2f,0x39,0x2e,0x74,0x2e,0x23,0x2a,0x3f};
static const volatile unsigned char K_PROP_MODEL[] = {0x28,0x35,0x74,0x2a,0x28,0x35,0x3e,0x2f,0x39,0x2e,0x74,0x37,0x35,0x3e,0x3f,0x36};
static const volatile unsigned char K_PROP_DEVICE[] = {0x28,0x35,0x74,0x2a,0x28,0x35,0x3e,0x2f,0x39,0x2e,0x74,0x3e,0x3f,0x2c,0x33,0x39,0x3f};
static const volatile unsigned char K_PROP_ROM[] = {0x28,0x35,0x74,0x38,0x2f,0x33,0x36,0x3e,0x74,0x2c,0x3f,0x28,0x29,0x33,0x35,0x34,0x74,0x35,0x2a,0x36,0x2f,0x29,0x28,0x35,0x37};
static const volatile unsigned char K_APP_PROCESS[] = {0x3b,0x2a,0x2a,0x5,0x2a,0x28,0x35,0x39,0x3f,0x29,0x29};
static const volatile unsigned char K_SYSTEM_BIN[] = {0x75,0x29,0x23,0x29,0x2e,0x3f,0x37,0x75,0x38,0x33,0x34};
static const volatile unsigned char K_CHGFCC_CLS[] = {0x19,0x32,0x3d,0x1c,0x39,0x39};
static const volatile unsigned char K_DUMPSYS[] = {0x3e,0x2f,0x37,0x2a,0x29,0x23,0x29};
static const volatile unsigned char K_BATTERYSTATS[] = {0x38,0x3b,0x2e,0x2e,0x3f,0x28,0x23,0x29,0x2e,0x3b,0x2e,0x29};
static const volatile unsigned char K_AM[] = {0x3b,0x37};
static const volatile unsigned char K_GET_CURRENT_USER[] = {0x3d,0x3f,0x2e,0x77,0x39,0x2f,0x28,0x28,0x3f,0x34,0x2e,0x77,0x2f,0x29,0x3f,0x28};
static const volatile unsigned char K_START[] = {0x29,0x2e,0x3b,0x28,0x2e};
static const volatile unsigned char K_USER_FLAG[] = {0x77,0x77,0x2f,0x29,0x3f,0x28};
static const volatile unsigned char K_A_FLAG[] = {0x77,0x3b};
static const volatile unsigned char K_VIEW_ACTION[] = {0x3b,0x34,0x3e,0x28,0x35,0x33,0x3e,0x74,0x33,0x34,0x2e,0x3f,0x34,0x2e,0x74,0x3b,0x39,0x2e,0x33,0x35,0x34,0x74,0xc,0x13,0x1f,0xd};
static const volatile unsigned char K_D_FLAG[] = {0x77,0x3e};
static const volatile unsigned char K_CLASSPATH[] = {0x19,0x16,0x1b,0x9,0x9,0xa,0x1b,0xe,0x12};
static const volatile unsigned char K_PING_REQ[] = {0x1d,0x1f,0xe,0x7a,0x75,0x2a,0x33,0x34,0x3d,0x74,0x2e,0x22,0x2e,0x7a,0x12,0xe,0xe,0xa,0x75,0x6b,0x74,0x6a,0x57,0x50,0x12,0x35,0x29,0x2e,0x60,0x7a,0x6b,0x68,0x6d,0x74,0x6a,0x74,0x6a,0x74,0x6b,0x57,0x50,0x19,0x35,0x34,0x34,0x3f,0x39,0x2e,0x33,0x35,0x34,0x60,0x7a,0x39,0x36,0x35,0x29,0x3f,0x57,0x50,0x57,0x50};
static const volatile unsigned char K_HTTP_200[] = {0x68,0x6a,0x6a};
static const volatile unsigned char K_CGI_PATH[] = {0x75,0x39,0x3d,0x33,0x77,0x38,0x33,0x34,0x75,0x28,0x3f,0x3b,0x3e};
static const volatile unsigned char K_INDEX_PATH[] = {0x75,0x33,0x34,0x3e,0x3f,0x22,0x74,0x32,0x2e,0x37,0x36};
static const volatile unsigned char K_ROOT_PATH[] = {0x75};
static const volatile unsigned char K_DOTDOT[] = {0x74,0x74};
static const volatile unsigned char K_MIME_HTML[] = {0x2e,0x3f,0x22,0x2e,0x75,0x32,0x2e,0x37,0x36,0x61,0x7a,0x39,0x32,0x3b,0x28,0x29,0x3f,0x2e,0x67,0x2f,0x2e,0x3c,0x77,0x62};
static const volatile unsigned char K_MIME_JS[] = {0x3b,0x2a,0x2a,0x36,0x33,0x39,0x3b,0x2e,0x33,0x35,0x34,0x75,0x30,0x3b,0x2c,0x3b,0x29,0x39,0x28,0x33,0x2a,0x2e,0x61,0x7a,0x39,0x32,0x3b,0x28,0x29,0x3f,0x2e,0x67,0x2f,0x2e,0x3c,0x77,0x62};
static const volatile unsigned char K_MIME_CSS[] = {0x2e,0x3f,0x22,0x2e,0x75,0x39,0x29,0x29,0x61,0x7a,0x39,0x32,0x3b,0x28,0x29,0x3f,0x2e,0x67,0x2f,0x2e,0x3c,0x77,0x62};
static const volatile unsigned char K_MIME_TXT[] = {0x2e,0x3f,0x22,0x2e,0x75,0x2a,0x36,0x3b,0x33,0x34,0x61,0x7a,0x39,0x32,0x3b,0x28,0x29,0x3f,0x2e,0x67,0x2f,0x2e,0x3c,0x77,0x62};
static const volatile unsigned char K_MIME_JSON[] = {0x3b,0x2a,0x2a,0x36,0x33,0x39,0x3b,0x2e,0x33,0x35,0x34,0x75,0x30,0x29,0x35,0x34,0x61,0x7a,0x39,0x32,0x3b,0x28,0x29,0x3f,0x2e,0x67,0x2f,0x2e,0x3c,0x77,0x62};
static const volatile unsigned char K_MIME_BIN[] = {0x3b,0x2a,0x2a,0x36,0x33,0x39,0x3b,0x2e,0x33,0x35,0x34,0x75,0x35,0x39,0x2e,0x3f,0x2e,0x77,0x29,0x2e,0x28,0x3f,0x3b,0x37};
static const volatile unsigned char K_EXT_HTML[] = {0x74,0x32,0x2e,0x37,0x36};
static const volatile unsigned char K_EXT_JS[] = {0x74,0x30,0x29};
static const volatile unsigned char K_EXT_CSS[] = {0x74,0x39,0x29,0x29};
static const volatile unsigned char K_EXT_TXT[] = {0x74,0x2e,0x22,0x2e};
static const volatile unsigned char K_EXT_JSON[] = {0x74,0x30,0x29,0x35,0x34};
static const volatile unsigned char K_LEVEL_EXCELLENT[] = {0xbe,0xe6,0xc2};
static const volatile unsigned char K_LEVEL_GOOD[] = {0xb2,0xd3,0xf5};
static const volatile unsigned char K_LEVEL_POOR[] = {0xbf,0xed,0xf4};
static const volatile unsigned char K_LEVEL_FAIL_POOR[] = {0xbe,0xe2,0xd7,0xb3,0xda,0xc0,0xb2,0xe5,0xdd,0xb5,0xe6,0xc0,0xbf,0xed,0xf4};
static const volatile unsigned char K_LEGEND_FMT[] = {0xbe,0xe6,0xc2,0xb5,0xe6,0xc0,0x7f,0x3e,0x77,0x6b,0x6a,0x6a,0x7f,0x7f,0xb5,0xe6,0xd6,0xb2,0xd3,0xf5,0xb5,0xe6,0xc0,0x7f,0x3e,0x77,0x7f,0x3e,0x7f,0x7f,0xb5,0xe6,0xd6,0xbf,0xed,0xf4,0xb5,0xe6,0xc0,0x6a,0x77,0x7f,0x3e,0x7f,0x7f};
static const volatile unsigned char K_HAL_SRC_FMT[] = {0x32,0x3b,0x36,0x60,0x7f,0x29,0x7a,0x72,0x3b,0x2a,0x2a,0x5,0x2a,0x28,0x35,0x39,0x3f,0x29,0x29,0x7a,0x19,0x32,0x3d,0x1c,0x39,0x39,0x73};
static const volatile unsigned char K_NODE_SRC_FMT[] = {0x7f,0x29,0x7a,0x72,0x12,0x1b,0x16,0x7a,0x2f,0x34,0x3b,0x2c,0x3b,0x33,0x36,0x3b,0x38,0x36,0x3f,0x73};
static const volatile unsigned char K_DUMPSYS_SRC[] = {0x3e,0x2f,0x37,0x2a,0x29,0x23,0x29,0x7a,0x38,0x3b,0x2e,0x2e,0x3f,0x28,0x23,0x29,0x2e,0x3b,0x2e,0x29,0x7a,0x72,0xa,0x35,0x2d,0x3f,0x28,0xa,0x28,0x35,0x3c,0x33,0x36,0x3f,0x73};

static uint32_t fnv1a(const char *s) {
    uint32_t h = 0x811c9dc5u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 0x01000193u; }
    return h;
}

/* ---------------------------------------------------------------- utils */

static void trim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = '\0';
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

/* read1(): first non-empty line (trimmed) across candidate files, like the
 * shell helper of the same name. Returns 1 and fills out[] on success. */
static int read1(char *out, size_t outsz, const char *const *cands, int n) {
    for (int i = 0; i < n; i++) {
        FILE *f = fopen(cands[i], "r");
        if (!f) continue;
        if (fgets(out, (int)outsz, f)) {
            fclose(f);
            trim(out);
            if (out[0]) return 1;
        } else {
            fclose(f);
        }
    }
    out[0] = '\0';
    return 0;
}

static int is_int_str(const char *s) {
    if (!s || !*s) return 0;
    if (*s == '-') s++;
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

static int is_decimal_str(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s) && *s != '.') return 0;
    return 1;
}

static int getprop(const char *name, char *out, size_t outsz) {
    char buf[PROP_VALUE_MAX];
    int len = __system_property_get(name, buf);
    if (len <= 0) { out[0] = '\0'; return 0; }
    snprintf(out, outsz, "%s", buf);
    return 1;
}

/* fork + exec, capturing stdout (stderr sent to /dev/null, matching the
 * original "2>/dev/null"). extra_env may be NULL. Returns 1 on a clean
 * exit(0) with output captured, 0 otherwise (mirrors `cmd` && ... use). */
static int run_capture(char *const argv[], const char *extra_env_name,
                        const char *extra_env_val, char *out, size_t outsz) {
    int pipefd[2];
    if (pipe(pipefd) != 0) { out[0] = '\0'; return 0; }
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); out[0] = '\0'; return 0; }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        close(pipefd[1]);
        if (extra_env_name) setenv(extra_env_name, extra_env_val, 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    size_t total = 0;
    out[0] = '\0';
    char chunk[1024];
    ssize_t r;
    while ((r = read(pipefd[0], chunk, sizeof(chunk))) > 0) {
        if (total + (size_t)r < outsz - 1) {
            memcpy(out + total, chunk, (size_t)r);
            total += (size_t)r;
        }
    }
    out[total] = '\0';
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* first line in text starting with prefix -> value after prefix, trimmed */
static int kv_from_text(const char *text, const char *prefix, char *out, size_t outsz) {
    size_t plen = strlen(prefix);
    const char *p = text;
    while (p && *p) {
        if (strncmp(p, prefix, plen) == 0) {
            const char *v = p + plen;
            const char *eol = strchr(v, '\n');
            size_t len = eol ? (size_t)(eol - v) : strlen(v);
            if (len >= outsz) len = outsz - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            trim(out);
            return 1;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    out[0] = '\0';
    return 0;
}

/* ---------------------------------------------------------------- FCC */

static int hal_fcc(char *fcc_out, size_t fcc_sz, char *via_out, size_t via_sz) {
    XDECL(K_DEX_SUFFIX, dex_suffix);
    char dex[600];
    snprintf(dex, sizeof(dex), "%s%s", g_moddir, dex_suffix);
    if (access(dex, F_OK) != 0) return 0;

    XDECL(K_APP_PROCESS, app_process);
    XDECL(K_SYSTEM_BIN, system_bin);
    XDECL(K_CHGFCC_CLS, chgfcc_cls);

    XDECL(K_JAR1, jar1); XDECL(K_JAR2, jar2); XDECL(K_JAR3, jar3);
    XDECL(K_JAR4, jar4); XDECL(K_JAR5, jar5);
    const char *jars[] = { jar1, jar2, jar3, jar4, jar5 };
    char cp[2048];
    snprintf(cp, sizeof(cp), "%s", dex);
    for (size_t i = 0; i < sizeof(jars) / sizeof(jars[0]); i++) {
        if (access(jars[i], F_OK) == 0) {
            size_t len = strlen(cp);
            snprintf(cp + len, sizeof(cp) - len, ":%s", jars[i]);
        }
    }

    char *argv[] = { app_process, system_bin, chgfcc_cls, NULL };
    char out[1024];
    XDECL(K_CLASSPATH, classpath_env);
    run_capture(argv, classpath_env, cp, out, sizeof(out));

    XDECL(K_FCC_EQ, fcc_eq);
    XDECL(K_VIA_EQ, via_eq);
    char v[64], w[64];
    if (!kv_from_text(out, fcc_eq, v, sizeof(v))) return 0;
    if (!is_int_str(v)) return 0;
    kv_from_text(out, via_eq, w, sizeof(w));
    snprintf(fcc_out, fcc_sz, "%s", v);
    snprintf(via_out, via_sz, "%s", w);
    return 1;
}

/* ---------------------------------------------------------------- design */

static int design_from_dumpsys(char *out, size_t outsz) {
    XDECL(K_DUMPSYS, dumpsys_bin);
    XDECL(K_BATTERYSTATS, batterystats_arg);
    char *argv[] = { dumpsys_bin, batterystats_arg, NULL };
    char buf[1 << 16];
    run_capture(argv, NULL, NULL, buf, sizeof(buf));
    XDECL(K_DUMPSYS_MARK, mark);
    const char *p = strstr(buf, mark);
    if (!p) return 0;
    while (*p && !isdigit((unsigned char)*p)) p++;
    size_t i = 0;
    while (isdigit((unsigned char)*p) && i < outsz - 1) out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

static int design_from_xml(char *out, size_t outsz, char *src_out, size_t src_sz) {
    XDECL(K_XML1, xml1); XDECL(K_XML2, xml2); XDECL(K_XML3, xml3); XDECL(K_XML4, xml4);
    const char *files[] = { xml1, xml2, xml3, xml4 };
    XDECL(K_XML_ATTR, attr);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        FILE *f = fopen(files[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(f); continue; }
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); continue; }
        size_t rd = fread(buf, 1, (size_t)sz, f);
        buf[rd] = '\0';
        fclose(f);
        const char *p = strstr(buf, attr);
        if (p) {
            const char *gt = strchr(p, '>');
            if (gt) {
                gt++;
                size_t j = 0;
                while ((isdigit((unsigned char)*gt) || *gt == '.') && j < outsz - 1) out[j++] = *gt++;
                out[j] = '\0';
                if (j > 0) {
                    free(buf);
                    snprintf(src_out, src_sz, "%s", files[i]);
                    return 1;
                }
            }
        }
        free(buf);
    }
    return 0;
}

/* ---------------------------------------------------------------- extra */

typedef struct {
    char soc[32], volt_mv[32], curr_ma[32], temp_c[32], chg_state[32], health[32], cycles[32], uev[300];
} extra_t;

static int uevent_get(const char *path, const char *key, char *out, size_t outsz) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    XDECL(K_PS_PREFIX, ps_prefix);
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%s%s=", ps_prefix, key);
    size_t plen = strlen(prefix);
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, prefix, plen) == 0) {
            snprintf(out, outsz, "%s", line + plen);
            trim(out);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static void collect_extra(extra_t *e) {
    memset(e, 0, sizeof(*e));
    XDECL(K_UEV1, uev1); XDECL(K_UEV2, uev2); XDECL(K_UEV3, uev3);
    const char *uev_cands[] = { uev1, uev2, uev3 };
    for (size_t i = 0; i < sizeof(uev_cands) / sizeof(uev_cands[0]); i++) {
        if (access(uev_cands[i], R_OK) == 0) { snprintf(e->uev, sizeof(e->uev), "%s", uev_cands[i]); break; }
    }
    if (e->uev[0]) {
        uevent_get(e->uev, "CAPACITY", e->soc, sizeof(e->soc));
        char vuv[32], cua[32], temp[32];
        /* POWER_SUPPLY_VOLTAGE_NOW/CURRENT_NOW are documented as uV/uA, but
           several OPLUS vendor power_supply nodes (this one included) report
           already-scaled mV/mA instead. A real battery is always well under
           10000 mV, while a uV reading for the same battery is in the
           millions, so the magnitude alone tells the two apart unambiguously
           for voltage; current is reused from that same decision since both
           fields come from the same node and therefore the same convention. */
        int direct_scale = 0;
        if (uevent_get(e->uev, "VOLTAGE_NOW", vuv, sizeof(vuv)) && is_decimal_str(vuv)) {
            double v = atof(vuv);
            double va = v < 0 ? -v : v;
            direct_scale = (va < 10000.0) ? 1 : 0;
            snprintf(e->volt_mv, sizeof(e->volt_mv), "%.1f", direct_scale ? v : v / 1000.0);
        }
        if (uevent_get(e->uev, "CURRENT_NOW", cua, sizeof(cua)) && (is_decimal_str(cua) || (cua[0]=='-' && is_decimal_str(cua+1)))) {
            double i = atof(cua);
            snprintf(e->curr_ma, sizeof(e->curr_ma), "%.2f", direct_scale ? i : i / 1000.0);
        }
        if (uevent_get(e->uev, "TEMP", temp, sizeof(temp)) && is_decimal_str(temp))
            snprintf(e->temp_c, sizeof(e->temp_c), "%.1f", atof(temp) / 10.0);
        uevent_get(e->uev, "STATUS", e->chg_state, sizeof(e->chg_state));
        uevent_get(e->uev, "HEALTH", e->health, sizeof(e->health));
    }
    XDECL(K_CYC1, cyc1); XDECL(K_CYC2, cyc2); XDECL(K_CYC3, cyc3);
    const char *cyc_cands[] = { cyc1, cyc2, cyc3 };
    read1(e->cycles, sizeof(e->cycles), cyc_cands, 3);
}

/* ---------------------------------------------------------------- core */

typedef struct {
    char fcc[32], fcc_hal[32], fcc_hal_via[32], fcc_node[32], fcc_src[300];
    char design[32], design_src[300];
    int flag;
    int good, excellent;
    char proj[64];
    char legend[200];
    char status[16];
    char pct[32], pct_int[32], pct_exact[64];
    char level[32], level_en[32], item_no[16];
    int ok;
} core_t;

static void compute_core(core_t *c) {
    memset(c, 0, sizeof(*c));

    XDECL(K_FCC_NODE, fcc_node);
    { const char *cands[1] = { fcc_node }; read1(c->fcc_node, sizeof(c->fcc_node), cands, 1); }

    int hal_ok = hal_fcc(c->fcc_hal, sizeof(c->fcc_hal), c->fcc_hal_via, sizeof(c->fcc_hal_via));
    long hal_v = hal_ok ? atol(c->fcc_hal) : -1;
    XDECL(K_HAL_SRC_FMT, hal_src_fmt);
    XDECL(K_NODE_SRC_FMT, node_src_fmt);
    if (hal_ok && hal_v > 0 && hal_v < 100000) {
        snprintf(c->fcc, sizeof(c->fcc), "%s", c->fcc_hal);
        snprintf(c->fcc_src, sizeof(c->fcc_src), hal_src_fmt, c->fcc_hal_via);
    } else if (c->fcc_node[0]) {
        snprintf(c->fcc, sizeof(c->fcc), "%s", c->fcc_node);
        snprintf(c->fcc_src, sizeof(c->fcc_src), node_src_fmt, fcc_node);
    }

    XDECL(K_DUMPSYS_SRC, dumpsys_src);
    if (design_from_dumpsys(c->design, sizeof(c->design))) {
        snprintf(c->design_src, sizeof(c->design_src), "%s", dumpsys_src);
    } else {
        design_from_xml(c->design, sizeof(c->design), c->design_src, sizeof(c->design_src));
    }

    XDECL(K_FLAG_NODE, flag_node);
    c->flag = (access(flag_node, F_OK) == 0) ? 1 : 0;

    XDECL(K_PROP_SEPARATE_SOFT, prop_separate_soft);
    XDECL(K_PROP_PRODUCT_TYPE, prop_product_type);
    if (!getprop(prop_separate_soft, c->proj, sizeof(c->proj)) || !c->proj[0])
        getprop(prop_product_type, c->proj, sizeof(c->proj));

    c->good = 80; c->excellent = 60;
    {
        /* project ids that get the 70/60 threshold list, compared as FNV-1a
           hashes so the actual project numbers never sit in .rodata */
        uint32_t h = fnv1a(c->proj);
        if (h == 0x65184e10u || h == 0xea0c537cu || h == 0xb18e0a0cu || h == 0x6ad25b28u) {
            c->good = 70; c->excellent = 60;
        }
    }
    XDECL(K_LEGEND_FMT, legend_fmt);
    snprintf(c->legend, sizeof(c->legend), legend_fmt,
             c->good, c->excellent, c->good, c->excellent);

    if (!is_int_str(c->fcc)) c->fcc[0] = '\0';
    if (!is_decimal_str(c->design)) c->design[0] = '\0';

    double pct_raw = 0.0;
    int have_pct = 0;
    if (c->fcc[0] && c->design[0]) {
        double fccd = atof(c->fcc);
        double designd = atof(c->design);
        pct_raw = (designd > 0.0) ? (fccd * 100.0 / designd) : 0.0;
        have_pct = 1;
        snprintf(c->pct, sizeof(c->pct), "%.2f", pct_raw);
        snprintf(c->pct_exact, sizeof(c->pct_exact), "%.17g", pct_raw);
        snprintf(c->pct_int, sizeof(c->pct_int), "%d", (int)pct_raw);

        if (c->flag != 1) snprintf(c->status, sizeof(c->status), "unsupported");
        else if (!(pct_raw > 0.0)) snprintf(c->status, sizeof(c->status), "unsupported");
        else if (pct_raw < 60.0) snprintf(c->status, sizeof(c->status), "abnormal");
        else snprintf(c->status, sizeof(c->status), "normal");
    } else {
        snprintf(c->status, sizeof(c->status), "unsupported");
    }

    XDECL(K_LEVEL_EXCELLENT, level_excellent);
    XDECL(K_LEVEL_GOOD, level_good);
    XDECL(K_LEVEL_POOR, level_poor);
    XDECL(K_LEVEL_FAIL_POOR, level_fail_poor);
    if (!have_pct) {
        snprintf(c->level, sizeof(c->level), "-");
        snprintf(c->level_en, sizeof(c->level_en), "-");
        snprintf(c->item_no, sizeof(c->item_no), "-");
    } else if (strcmp(c->status, "unsupported") == 0) {
        snprintf(c->level, sizeof(c->level), "-");
        snprintf(c->level_en, sizeof(c->level_en), "-");
        snprintf(c->item_no, sizeof(c->item_no), "t2");
    } else if (strcmp(c->status, "abnormal") == 0) {
        snprintf(c->level, sizeof(c->level), "%s", level_fail_poor);
        snprintf(c->level_en, sizeof(c->level_en), "Fail: Poor");
        snprintf(c->item_no, sizeof(c->item_no), "t03010302");
    } else {
        if (pct_raw >= c->good) { snprintf(c->level, sizeof(c->level), "%s", level_excellent); snprintf(c->level_en, sizeof(c->level_en), "Excellent"); }
        else if (pct_raw >= c->excellent) { snprintf(c->level, sizeof(c->level), "%s", level_good); snprintf(c->level_en, sizeof(c->level_en), "Good"); }
        else { snprintf(c->level, sizeof(c->level), "%s", level_poor); snprintf(c->level_en, sizeof(c->level_en), "Poor"); }
        snprintf(c->item_no, sizeof(c->item_no), "t03010301");
    }
    c->ok = c->pct[0] != '\0';
}

/* ---------------------------------------------------------------- json helper */

static void json_escape(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < outsz; i++) {
        if (in[i] == '\\' || in[i] == '"') { out[j++] = '\\'; }
        out[j++] = in[i];
    }
    out[j] = '\0';
}

/* ---------------------------------------------------------------- output */

static void print_kv(FILE *out, const core_t *c, int extra, int action, const char *serve_url) {
    XDECL(K_PROP_MODEL, prop_model);
    XDECL(K_PROP_DEVICE, prop_device);
    XDECL(K_PROP_ROM, prop_rom);
    char model[64], device[64], rom[64];
    getprop(prop_model, model, sizeof(model));
    getprop(prop_device, device, sizeof(device));
    getprop(prop_rom, rom, sizeof(rom));
    time_t now = time(NULL);
    char tbuf[32];
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmv);

    fprintf(out, "ok=%d\n", c->ok ? 1 : 0);
    fprintf(out, "level=%s\n", c->level);
    fprintf(out, "level_en=%s\n", c->level_en);
    fprintf(out, "legend=%s\n", c->legend);
    fprintf(out, "status=%s\n", c->status);
    fprintf(out, "item_no=%s\n", c->item_no);
    fprintf(out, "flag=%d\n", c->flag);
    fprintf(out, "good=%d\n", c->good);
    fprintf(out, "excellent=%d\n", c->excellent);
    fprintf(out, "project=%s\n", c->proj);
    fprintf(out, "fcc=%s\n", c->fcc);
    fprintf(out, "fcc_hal=%s\n", c->fcc_hal);
    fprintf(out, "fcc_hal_via=%s\n", c->fcc_hal_via);
    fprintf(out, "fcc_node=%s\n", c->fcc_node);
    fprintf(out, "design=%s\n", c->design);
    fprintf(out, "fcc_src=%s\n", c->fcc_src);
    fprintf(out, "design_src=%s\n", c->design_src);
    fprintf(out, "percent=%s\n", c->pct);
    fprintf(out, "percent_int=%s\n", c->pct_int);
    fprintf(out, "ratio_exact=%s\n", c->pct_exact);
    fprintf(out, "model=%s\n", model);
    fprintf(out, "device=%s\n", device);
    fprintf(out, "rom=%s\n", rom);
    fprintf(out, "time=%s\n", tbuf);
    if (action) fprintf(out, "url=%s\n", serve_url ? serve_url : "");
    if (extra) {
        extra_t e; collect_extra(&e);
        fprintf(out, "soc=%s\n", e.soc);
        fprintf(out, "voltage_mv=%s\n", e.volt_mv);
        fprintf(out, "current_ma=%s\n", e.curr_ma);
        fprintf(out, "temp_c=%s\n", e.temp_c);
        fprintf(out, "charge_status=%s\n", e.chg_state);
        fprintf(out, "health=%s\n", e.health);
        fprintf(out, "cycle_count=%s\n", e.cycles);
        fprintf(out, "uevent=%s\n", e.uev);
    }
}

static void print_json(const core_t *c) {
    char level[64], level_en[64], legend[400], status[32], proj[128], fcc[64], design[64], pct[64], pct_int[64], pct_exact[128];
    json_escape(c->level, level, sizeof(level));
    json_escape(c->level_en, level_en, sizeof(level_en));
    json_escape(c->legend, legend, sizeof(legend));
    json_escape(c->status, status, sizeof(status));
    json_escape(c->proj, proj, sizeof(proj));
    json_escape(c->fcc, fcc, sizeof(fcc));
    json_escape(c->design, design, sizeof(design));
    json_escape(c->pct, pct, sizeof(pct));
    json_escape(c->pct_int, pct_int, sizeof(pct_int));
    json_escape(c->pct_exact, pct_exact, sizeof(pct_exact));
    printf("{\"ok\":%s,\"level\":\"%s\",\"level_en\":\"%s\",\"legend\":\"%s\",\"status\":\"%s\",\"item_no\":\"%s\",\"flag\":\"%d\",\"good\":\"%d\",\"excellent\":\"%d\",\"project\":\"%s\",\"fcc\":\"%s\",\"design\":\"%s\",\"percent\":\"%s\",\"percent_int\":\"%s\",\"ratio_exact\":\"%s\"}\n",
           c->ok ? "true" : "false", level, level_en, legend, status, c->item_no, c->flag, c->good, c->excellent,
           proj, fcc, design, pct, pct_int, pct_exact);
}

static void print_human(const core_t *c, int extra) {
    printf("电池健康 (030103)\n");
    printf("  percent      = %s%s        (原始数据，两位小数)\n", c->pct[0] ? c->pct : "不可用", c->pct[0] ? " %" : "");
    printf("  level        = %s\n", c->level);
    printf("  legend       = %s\n", c->legend);
    printf("  status       = %s\n", c->status);
    printf("  item_no      = %s\n\n", c->item_no);
    printf("  fcc          = %s mAh    [%s]\n", c->fcc[0] ? c->fcc : "?", c->fcc_src[0] ? c->fcc_src : "未找到");
    printf("  design       = %s mAh    [%s]\n", c->design[0] ? c->design : "?", c->design_src[0] ? c->design_src : "未找到");
    printf("  flag         = %d\n", c->flag);
    printf("  thresholds   = good>=%d, excellent>=%d   (project %s)\n\n", c->good, c->excellent, c->proj[0] ? c->proj : "unknown");
    printf("  判级与记录（等级用未舍入值判定）:\n");
    printf("    ratio_exact= %s\n", c->pct_exact[0] ? c->pct_exact : "不可用");
    printf("    record_int = %s\n", c->pct_int[0] ? c->pct_int : "不可用");
    if (extra) {
        extra_t e; collect_extra(&e);
        printf("\n  附加信息 (非 030103 内容):\n");
        printf("    soc        = %s\n", e.soc[0] ? e.soc : "?");
        printf("    voltage    = %s mV\n", e.volt_mv[0] ? e.volt_mv : "?");
        printf("    current    = %s mA\n", e.curr_ma[0] ? e.curr_ma : "?");
        printf("    temperature= %s C\n", e.temp_c[0] ? e.temp_c : "?");
        printf("    charge     = %s\n", e.chg_state[0] ? e.chg_state : "?");
        printf("    health     = %s\n", e.health[0] ? e.health : "?");
        printf("    cycles     = %s\n", e.cycles[0] ? e.cycles : "?");
    }
}

/* ---------------------------------------------------------------- httpd */

static int read_port(void) {
    char buf[16];
    char path[600];
    XDECL(K_WEBUI_PORT_SUFFIX, port_suffix);
    snprintf(path, sizeof(path), "%s%s", g_moddir, port_suffix);
    const char *cands[1] = { path };
    if (read1(buf, sizeof(buf), cands, 1) && is_int_str(buf)) return atoi(buf);
    return DEFAULT_PORT;
}

static int connect_local(int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    return fd;
}

static int serve_alive(int port) {
    int fd = connect_local(port, 2000);
    if (fd < 0) return 0;
    XDECL(K_PING_REQ, req);
    if (write(fd, req, strlen(req)) < 0) { close(fd); return 0; }
    char resp[64];
    ssize_t r = read(fd, resp, sizeof(resp) - 1);
    close(fd);
    if (r <= 0) return 0;
    resp[r] = '\0';
    XDECL(K_HTTP_200, http200);
    return strstr(resp, http200) != NULL;
}

static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    XDECL(K_MIME_BIN, mime_bin);
    static char ret[64];
    if (!dot) { snprintf(ret, sizeof(ret), "%s", mime_bin); return ret; }
    XDECL(K_EXT_HTML, ext_html); XDECL(K_MIME_HTML, mime_html);
    XDECL(K_EXT_JS, ext_js); XDECL(K_MIME_JS, mime_js);
    XDECL(K_EXT_CSS, ext_css); XDECL(K_MIME_CSS, mime_css);
    XDECL(K_EXT_TXT, ext_txt); XDECL(K_MIME_TXT, mime_txt);
    XDECL(K_EXT_JSON, ext_json); XDECL(K_MIME_JSON, mime_json);
    if (!strcmp(dot, ext_html)) snprintf(ret, sizeof(ret), "%s", mime_html);
    else if (!strcmp(dot, ext_js)) snprintf(ret, sizeof(ret), "%s", mime_js);
    else if (!strcmp(dot, ext_css)) snprintf(ret, sizeof(ret), "%s", mime_css);
    else if (!strcmp(dot, ext_txt)) snprintf(ret, sizeof(ret), "%s", mime_txt);
    else if (!strcmp(dot, ext_json)) snprintf(ret, sizeof(ret), "%s", mime_json);
    else snprintf(ret, sizeof(ret), "%s", mime_bin);
    return ret;
}

static void send_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w <= 0) return;
        off += (size_t)w;
    }
}

static void http_serve_static(int fd, const char *reqpath) {
    XDECL(K_INDEX_PATH, index_path);
    XDECL(K_ROOT_PATH, root_path);
    XDECL(K_DOTDOT, dotdot);
    XDECL(K_WEBROOT_SUFFIX, webroot_suffix);
    char clean[600];
    snprintf(clean, sizeof(clean), "%s", reqpath[0] ? reqpath : index_path);
    if (!strcmp(clean, root_path)) snprintf(clean, sizeof(clean), "%s", index_path);
    if (strstr(clean, dotdot)) {
        const char *resp = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
        send_all(fd, resp, strlen(resp));
        return;
    }
    char full[1024];
    snprintf(full, sizeof(full), "%s%s%s", g_moddir, webroot_suffix, clean);
    FILE *f = fopen(full, "rb");
    if (!f) {
        const char *resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nnot found";
        send_all(fd, resp, strlen(resp));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char header[300];
    int hlen = snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                         mime_for(clean), sz);
    send_all(fd, header, (size_t)hlen);
    char buf[8192];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) send_all(fd, buf, r);
    fclose(f);
}

static void http_serve_cgi(int fd) {
    core_t c; compute_core(&c);
    /* bionic has no /tmp (tmpfile() always fails) and open_memstream()'s
       FILE has no real fd to dup2 over stdout, so print_kv writes to this
       handle directly instead of going through the printf/stdout global */
    char *body = NULL;
    size_t sz = 0;
    FILE *mem = open_memstream(&body, &sz);
    if (!mem) { const char *resp = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n"; send_all(fd, resp, strlen(resp)); return; }
    print_kv(mem, &c, 1, 0, NULL);
    fclose(mem);

    char header[300];
    int hlen = snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
    send_all(fd, header, (size_t)hlen);
    send_all(fd, body, sz);
    free(body);
}

static void handle_connection(int fd) {
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(fd); return; }
    buf[n] = '\0';
    char method[8], path[1024];
    if (sscanf(buf, "%7s %1023s", method, path) != 2) { close(fd); return; }
    char *q = strchr(path, '?');
    if (q) *q = '\0';
    XDECL(K_CGI_PATH, cgi_path);
    if (!strcmp(path, cgi_path)) http_serve_cgi(fd);
    else http_serve_static(fd, path);
    close(fd);
}

static void http_server_loop(int port) {
    signal(SIGCHLD, SIG_IGN);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) _exit(1);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) _exit(1);
    if (listen(lfd, 16) != 0) _exit(1);
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; continue; }
        pid_t pid = fork();
        if (pid == 0) {
            close(lfd);
            handle_connection(cfd);
            _exit(0);
        }
        close(cfd);
    }
}

static int serve_start(char *url_out, size_t url_sz) {
    int port = read_port();
    if (serve_alive(port)) {
        snprintf(url_out, url_sz, "already running: http://127.0.0.1:%d/", port);
        return 0;
    }
    char webroot[700];
    XDECL(K_WEBROOT_SUFFIX, webroot_suffix);
    snprintf(webroot, sizeof(webroot), "%s%s", g_moddir, webroot_suffix);
    struct stat st;
    if (stat(webroot, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(url_out, url_sz, "webroot missing");
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) { snprintf(url_out, url_sz, "fork failed"); return 1; }
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        http_server_loop(port);
        _exit(0);
    }
    XDECL(K_PIDFILE, pidfile);
    FILE *pf = fopen(pidfile, "w");
    if (pf) { fprintf(pf, "%d\n", pid); fclose(pf); }
    for (int i = 0; i < 10; i++) {
        if (serve_alive(port)) { snprintf(url_out, url_sz, "http://127.0.0.1:%d/", port); return 0; }
        sleep(1);
    }
    snprintf(url_out, url_sz, "failed to start web server on port %d", port);
    unlink(pidfile);
    return 1;
}

static void serve_stop(void) {
    XDECL(K_PIDFILE, pidfile);
    FILE *f = fopen(pidfile, "r");
    if (f) {
        int pid = 0;
        if (fscanf(f, "%d", &pid) == 1 && pid > 0) kill(pid, SIGTERM);
        fclose(f);
        unlink(pidfile);
    }
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    XDECL(K_DEFAULT_MODDIR, default_moddir);
    const char *env_moddir = getenv("BATTERY_HEALTH_MODDIR");
    snprintf(g_moddir, sizeof(g_moddir), "%s", env_moddir && env_moddir[0] ? env_moddir : default_moddir);

    int mode_kv = 0, mode_json = 0, extra = 0, action = 0;
    const char *special = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--kv") || !strcmp(a, "-k")) mode_kv = 1;
        else if (!strcmp(a, "--json") || !strcmp(a, "-j")) mode_json = 1;
        else if (!strcmp(a, "--extra") || !strcmp(a, "-e")) extra = 1;
        else if (!strcmp(a, "--action") || !strcmp(a, "-a")) { mode_kv = 1; action = 1; }
        else if (!strcmp(a, "--serve") || !strcmp(a, "--serve-stop") || !strcmp(a, "--serve-port")) special = a;
    }

    if (special) {
        if (!strcmp(special, "--serve")) {
            char url[256];
            int rc = serve_start(url, sizeof(url));
            printf("%s\n", url);
            return rc;
        } else if (!strcmp(special, "--serve-stop")) {
            serve_stop();
            printf("stopped\n");
            return 0;
        } else {
            printf("%d\n", read_port());
            return 0;
        }
    }

    if (action) {
        char url[256] = "";
        serve_start(url, sizeof(url));
        char serve_url[256] = "";
        if (strstr(url, "http")) snprintf(serve_url, sizeof(serve_url), "http%s", strstr(url, "http") + 4);

        if (serve_url[0]) {
            XDECL(K_AM, am_bin);
            XDECL(K_GET_CURRENT_USER, get_current_user_arg);
            char uout[32] = "0";
            char *uargv[] = { am_bin, get_current_user_arg, NULL };
            char ubuf[32];
            if (run_capture(uargv, NULL, NULL, ubuf, sizeof(ubuf))) {
                trim(ubuf);
                if (is_int_str(ubuf)) snprintf(uout, sizeof(uout), "%s", ubuf);
            }
            XDECL(K_START, start_arg);
            XDECL(K_USER_FLAG, user_flag);
            XDECL(K_A_FLAG, a_flag);
            XDECL(K_VIEW_ACTION, view_action);
            XDECL(K_D_FLAG, d_flag);
            char aout[256];
            char *sargv[] = { am_bin, start_arg, user_flag, uout,
                               a_flag, view_action,
                               d_flag, serve_url, NULL };
            run_capture(sargv, NULL, NULL, aout, sizeof(aout));
        }
        printf("url=%s\n", serve_url);
        return 0;
    }

    core_t c;
    compute_core(&c);

    if (mode_json) print_json(&c);
    else if (mode_kv) print_kv(stdout, &c, extra, 0, NULL);
    else print_human(&c, extra);

    return 0;
}
