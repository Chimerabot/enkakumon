/*
 * test_main.c — unit tests for enkakumond's hardware-free logic.
 * Run from the repo root by make test, make asan and make memcheck.
 *
 * Covers: CRC-8 vectors, SCD40 and SGP40 conversions, motion, the dB formula
 * (including RMS = 0), config parsing (including bad lines), the JSON line
 * format with nulls, and IPC tap parsing / dropping of broken clients.
 * Config error messages printed during the run are expected.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "conf.h"
#include "ipc.h"
#include "mic.h"
#include "sensors.h"

static int checks, failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        checks++;                                                                   \
        if (!(cond)) {                                                              \
            failures++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))

/* ---------- sensors ---------- */

static uint8_t crc_of(uint8_t hi, uint8_t lo)
{
    uint8_t w[2] = { hi, lo };
    return sensirion_crc8(w, 2);
}

static void test_crc(void)
{
    CHECK(crc_of(0xBE, 0xEF) == 0x92);
    CHECK(crc_of(0x66, 0x66) == 0x93);
    CHECK(crc_of(0x80, 0x00) == 0xA2);
    CHECK(crc_of(0xFF, 0xFF) == 0xAC);
}

static void test_scd40(void)
{
    CHECK_NEAR(scd40_temp_c(0), -45.0, 1e-9);
    CHECK_NEAR(scd40_temp_c(65535), 130.0, 1e-9);
    CHECK_NEAR(scd40_temp_c(0x6666), 25.0, 0.01);
    CHECK_NEAR(scd40_rh(0), 0.0, 1e-9);
    CHECK_NEAR(scd40_rh(65535), 100.0, 1e-9);
    CHECK_NEAR(scd40_rh(0x8000), 50.0, 0.01);
}

static void test_sgp40(void)
{
    CHECK(sgp40_temp_ticks(25.0) == 0x6666);
    CHECK(sgp40_rh_ticks(50.0) == 0x8000);
    CHECK(sgp40_temp_ticks(130.0) == 0xFFFF);
    CHECK(sgp40_rh_ticks(100.0) == 0xFFFF);
    CHECK(sgp40_temp_ticks(-45.0) == 0);
    CHECK(sgp40_rh_ticks(0.0) == 0);
    /* out of range clamps instead of wrapping */
    CHECK(sgp40_temp_ticks(500.0) == 0xFFFF);
    CHECK(sgp40_temp_ticks(-100.0) == 0);
    CHECK(sgp40_rh_ticks(150.0) == 0xFFFF);
    CHECK(sgp40_rh_ticks(-5.0) == 0);
}

static void test_motion(void)
{
    /* 16393 LSB * 0.061 mg/LSB = 1 g */
    CHECK(lsm6dsox_motion(0, 0, 16393) < 0.01);
    CHECK(lsm6dsox_motion(0, -16393, 0) < 0.01);
    CHECK_NEAR(lsm6dsox_motion(0, 0, 0), 9.81, 1e-9);
    CHECK_NEAR(lsm6dsox_motion(16393, 0, 16393), 4.058, 0.01);
}

/* ---------- mic ---------- */

static void test_db(void)
{
    const double full = 2147483648.0;

    CHECK_NEAR(mic_db_from_mean_square(full * full, 120.0), 120.0, 1e-9);
    CHECK_NEAR(mic_db_from_mean_square(full * full / 4.0, 120.0), 120.0 - 6.0206, 1e-3);
    CHECK(isnan(mic_db_from_mean_square(0.0, 120.0)));
    CHECK(isnan(mic_db_from_mean_square(NAN, 120.0)));

    int32_t dc[4] = { 1000, 1000, 1000, 1000 };
    int32_t sq[4] = { 1100, 900, 1100, 900 };
    int32_t big[2] = { INT32_MAX, INT32_MIN };
    CHECK(mic_mean_square(dc, 4) == 0.0);
    CHECK(isnan(mic_db_from_mean_square(mic_mean_square(dc, 4), 120.0)));
    CHECK_NEAR(mic_mean_square(sq, 4), 10000.0, 1e-9);
    CHECK(mic_mean_square(dc, 0) == 0.0);
    CHECK(mic_mean_square(big, 2) > 0.0);
}

/* ---------- conf ---------- */

static int load_text(const char *text, struct conf *c)
{
    char path[] = "/tmp/enkakumon-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        exit(2);
    }
    size_t len = strlen(text);
    if (write(fd, text, len) != (ssize_t)len || close(fd) != 0) {
        perror("write temp conf");
        exit(2);
    }
    conf_defaults(c);
    int rc = conf_load(c, path);
    if (unlink(path) != 0)
        perror("unlink temp conf");
    return rc;
}

static void test_conf(void)
{
    struct conf c;

    CHECK(load_text("# comment\n"
                    "\n"
                    "   \t\n"
                    "SAMPLE_HZ = 1\n"
                    "RETRY_S = 7      # inline comment\n"
                    "I2C_DEV = /dev/i2c-3\n"
                    "TEMP_OFFSET_C = 1.5\n"
                    "DEFAULT_TEMP_C=20\n"
                    "DEFAULT_RH = 40\n"
                    "MIC_NAME = foo\n"
                    "MIC_RATE = 16000\n"
                    "MIC_PERIOD_FRAMES = 1600\n"
                    "DB_OFFSET = 110.5\n"
                    "SOCKET_PATH = /tmp/x.sock\n"
                    "MAX_CLIENTS = 2\n"
                    "HEARTBEAT_PATH = /tmp/hb\n"
                    "DC_PIN = 25\n",
                    &c) == 0);
    CHECK(c.retry_s == 7);
    CHECK(strcmp(c.i2c_dev, "/dev/i2c-3") == 0);
    CHECK_NEAR(c.temp_offset_c, 1.5, 1e-12);
    CHECK_NEAR(c.default_temp_c, 20.0, 1e-12);
    CHECK_NEAR(c.default_rh, 40.0, 1e-12);
    CHECK(strcmp(c.mic_name, "foo") == 0);
    CHECK(c.mic_rate == 16000);
    CHECK(c.mic_period_frames == 1600);
    CHECK_NEAR(c.db_offset, 110.5, 1e-12);
    CHECK(strcmp(c.socket_path, "/tmp/x.sock") == 0);
    CHECK(c.max_clients == 2);
    CHECK(strcmp(c.heartbeat_path, "/tmp/hb") == 0);

    /* missing keys keep their defaults; CRLF endings are accepted */
    CHECK(load_text("RETRY_S = 9\r\n", &c) == 0);
    CHECK(c.retry_s == 9);
    CHECK(c.max_clients == 4);
    CHECK(strcmp(c.socket_path, "/tmp/enkakumon.sock") == 0);

    /* the shipped config loads */
    conf_defaults(&c);
    CHECK(conf_load(&c, "enkakumon.conf") == 0);
    CHECK(c.retry_s == 30);
    CHECK(c.max_clients == 4);
    CHECK(strcmp(c.mic_name, "googlevoicehat") == 0);

    fprintf(stderr, "--- expected config errors follow ---\n");
    CHECK(load_text("RETRY_S 30\n", &c) != 0);
    CHECK(load_text("RETRY_S = abc\n", &c) != 0);
    CHECK(load_text("RETRY_S = 3x\n", &c) != 0);
    CHECK(load_text("RETRY_S = 99999999999\n", &c) != 0);
    CHECK(load_text("MAX_CLIENTS =\n", &c) != 0);
    CHECK(load_text("MAX_CLIENTS = # nothing\n", &c) != 0);
    CHECK(load_text("= 5\n", &c) != 0);
    CHECK(load_text("TEMP_OFFSET_C = nan\n", &c) != 0);
    CHECK(load_text("SAMPLE_HZ = 2\n", &c) != 0);
    CHECK(load_text("MAX_CLIENTS = 0\n", &c) != 0);
    CHECK(load_text("MIC_PERIOD_FRAMES = 0\n", &c) != 0);

    char longline[700];
    memset(longline, 'A', sizeof(longline) - 2);
    longline[sizeof(longline) - 2] = '\n';
    longline[sizeof(longline) - 1] = '\0';
    CHECK(load_text(longline, &c) != 0);

    char longval[200] = "SOCKET_PATH = /";
    memset(longval + strlen(longval), 'a', 150);
    strcat(longval, "\n");
    CHECK(load_text(longval, &c) != 0);

    conf_defaults(&c);
    CHECK(conf_load(&c, "/nonexistent/enkakumon.conf") != 0);
    fprintf(stderr, "--- end of expected config errors ---\n");
}

/* ---------- JSON ---------- */

static void test_json(void)
{
    char buf[512];
    struct readings r = {
        .co2 = 612, .temp_c = 22.4, .rh = 41.0, .voc = 98, .light = 530,
        .proximity = 3, .motion = 0.04, .steps = 120, .db = 41.3,
    };
    const char *want = "{\"t\":1700000000,\"co2\":612,\"temp_c\":22.4,\"rh\":41.0,\"voc\":98,"
                       "\"light\":530,\"proximity\":3,\"motion\":0.04,\"steps\":120,\"db\":41.3}\n";
    int n = ipc_format_json(buf, sizeof(buf), 1700000000, &r);
    CHECK(n == (int)strlen(want));
    CHECK(strcmp(buf, want) == 0);

    readings_clear(&r);
    const char *nulls = "{\"t\":5,\"co2\":null,\"temp_c\":null,\"rh\":null,\"voc\":null,"
                        "\"light\":null,\"proximity\":null,\"motion\":null,\"steps\":null,\"db\":null}\n";
    n = ipc_format_json(buf, sizeof(buf), 5, &r);
    CHECK(n == (int)strlen(nulls));
    CHECK(strcmp(buf, nulls) == 0);

    r.co2 = 400;
    r.db = INFINITY;
    n = ipc_format_json(buf, sizeof(buf), 5, &r);
    CHECK(n > 0 && strstr(buf, "\"co2\":400,") != NULL);
    CHECK(n > 0 && strstr(buf, "\"db\":null}") != NULL);

    CHECK(ipc_format_json(buf, 20, 5, &r) == -1);
}

/* ---------- IPC ---------- */

static void send_all(int fd, const char *s)
{
    size_t len = strlen(s);
    if (write(fd, s, len) != (ssize_t)len) {
        perror("write");
        exit(2);
    }
}

static void test_ipc(void)
{
    static struct ipc p;
    int sv[2];
    char buf[16];

    ipc_reset(&p, 4);
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        perror("socketpair");
        exit(2);
    }
    p.clients[0].fd = sv[0];

    send_all(sv[1], "tap\nhello\ntap\r\nta");
    ipc_client_input(&p, 0);
    CHECK(p.taps == 2);
    send_all(sv[1], "p\n"); /* "tap" split across two reads */
    ipc_client_input(&p, 0);
    CHECK(p.taps == 3);
    send_all(sv[1], "tapx\nTAP\n\n");
    ipc_client_input(&p, 0);
    CHECK(p.taps == 3);

    char flood[IPC_LINE_MAX + 20];
    memset(flood, 'x', sizeof(flood) - 1);
    flood[sizeof(flood) - 1] = '\0';
    send_all(sv[1], flood);
    send_all(sv[1], "tap\ntap\n"); /* the overlong line ends at the first \n */
    ipc_client_input(&p, 0);
    CHECK(p.taps == 4);

    ipc_publish(&p, "x\n", 2);
    CHECK(p.clients[0].fd == sv[0]);
    CHECK(read(sv[1], buf, sizeof(buf)) == 2 && memcmp(buf, "x\n", 2) == 0);

    /* a broken client is dropped on publish, never waited on */
    CHECK(close(sv[1]) == 0);
    ipc_publish(&p, "x\n", 2);
    CHECK(p.clients[0].fd == -1);

    /* a client that hangs up is dropped on input */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        perror("socketpair");
        exit(2);
    }
    p.clients[1].fd = sv[0];
    CHECK(close(sv[1]) == 0);
    ipc_client_input(&p, 1);
    CHECK(p.clients[1].fd == -1);

    ipc_close(&p); /* no listener: only clients, all already closed */
}

int main(void)
{
    test_crc();
    test_scd40();
    test_sgp40();
    test_motion();
    test_db();
    test_conf();
    test_json();
    test_ipc();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
