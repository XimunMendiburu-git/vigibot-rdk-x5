#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <wiringPi.h>

#define MOTOR_HZ 250
#define SERVO_HZ 50
#define MOTOR_PERIOD_NS (1000000000L / MOTOR_HZ)
#define SERVO_PERIOD_NS (1000000000L / SERVO_HZ)
#define MAX_SOFT_CHANNELS 32
#define MAX_SERVO_EVENTS (MAX_SOFT_CHANNELS * 2)
/* Dampen stick noise / Node spam harder than the old Python defaults. */
#define SERVO_HYST_US 80
#define SERVO_QUANTUM_US 40
/* Busy-wait the whole high phase for accurate pulse width under load. */
#define SERVO_BUSY_NS 2500000L

typedef enum {
    SOFT_NONE = 0,
    SOFT_MOTOR,
    SOFT_SERVO,
} soft_mode_t;

typedef struct {
    int bcm;
    int physical;
    soft_mode_t mode;
    int value;
    bool running;
    pthread_t thread;
    pthread_mutex_t lock;
} soft_channel_t;

typedef struct {
    int bcm;
    int group;
    int channel;
    char chip[PATH_MAX];
    bool exported;
    bool enabled;
    long period_ns;
} hardware_channel_t;

typedef struct {
    long offset_ns;
    int physical;
    int level;
} servo_event_t;

static volatile sig_atomic_t stopping = 0;
static soft_channel_t soft_channels[MAX_SOFT_CHANNELS];
static int soft_count = 0;
static bool hardware_pwm_enabled = false;
static pthread_t servo_thread;
static bool servo_thread_running = false;
static pthread_mutex_t channels_lock = PTHREAD_MUTEX_INITIALIZER;
static bool left_ir_prepared = false;

/*
 * Official Vigibot Raspberry Pi BCM numbering mapped to the physical RDK X5
 * header. wiringPiSetupPhys() lets the C helper keep this mapping explicit.
 */
static int bcm_to_physical(int bcm)
{
    switch (bcm) {
    case 1: return 28;
    case 4: return 7;
    case 5: return 29;
    case 6: return 31;
    case 7: return 26;
    case 8: return 24;
    case 9: return 21;
    case 13: return 33;
    case 16: return 36;
    case 17: return 11;
    case 18: return 12;
    case 19: return 35;
    case 20: return 38;
    case 21: return 40;
    case 22: return 15;
    case 23: return 16;
    case 24: return 18;
    case 25: return 22;
    case 26: return 37;
    case 27: return 13;
    default: return -1;
    }
}

/*
 * Experimental hardware PWM routing. It remains disabled unless explicitly
 * requested after the board pinmux has been validated independently.
 * BCM7 / physical 26 has no hardware PWM and always stays software.
 */
static bool hardware_route(int bcm, int *group, int *channel)
{
    if (!hardware_pwm_enabled) {
        return false;
    }
    switch (bcm) {
    case 5:  *group = 0; *channel = 0; return true;
    case 6:  *group = 0; *channel = 1; return true;
    case 26: *group = 1; *channel = 0; return true;
    case 8:  *group = 1; *channel = 1; return true;
    default: return false;
    }
}

static void add_ns(struct timespec *ts, long ns)
{
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

static void sleep_until(const struct timespec *deadline)
{
    while (!stopping &&
           clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL) == EINTR) {
    }
}

/*
 * Sleep until shortly before the deadline, then spin for the last busy window.
 * Improves falling-edge accuracy under video/BPU load vs pure nanosleep.
 */
static void sleep_until_edge(const struct timespec *deadline, long busy_ns)
{
    struct timespec now;
    struct timespec soft = *deadline;

    if (busy_ns > 0) {
        soft.tv_nsec -= busy_ns;
        while (soft.tv_nsec < 0) {
            soft.tv_nsec += 1000000000L;
            soft.tv_sec--;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec < soft.tv_sec ||
            (now.tv_sec == soft.tv_sec && now.tv_nsec < soft.tv_nsec)) {
            sleep_until(&soft);
        }
    }

    while (!stopping) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline->tv_sec ||
            (now.tv_sec == deadline->tv_sec &&
             now.tv_nsec >= deadline->tv_nsec)) {
            break;
        }
    }
}

static void set_realtime_priority(int priority)
{
    struct sched_param param = {.sched_priority = priority};
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
}

static int quantize_servo_us(int pulse_us)
{
    if (pulse_us <= 0) {
        return 0;
    }
    return ((pulse_us + SERVO_QUANTUM_US / 2) / SERVO_QUANTUM_US) *
           SERVO_QUANTUM_US;
}

static struct timespec offset_from(struct timespec base, long ns)
{
    add_ns(&base, ns);
    return base;
}

static int compare_servo_events(const void *left, const void *right)
{
    const servo_event_t *a = left;
    const servo_event_t *b = right;
    if (a->offset_ns < b->offset_ns) return -1;
    if (a->offset_ns > b->offset_ns) return 1;
    return a->level - b->level;
}

static void *servo_loop(void *arg)
{
    struct timespec cycle;
    (void)arg;

    set_realtime_priority(80);
    /* Avoid page faults mid-pulse when video/BPU thrash memory. */
    mlockall(MCL_CURRENT | MCL_FUTURE);
    clock_gettime(CLOCK_MONOTONIC, &cycle);

    while (!stopping) {
        soft_channel_t *servos[MAX_SOFT_CHANNELS];
        int values[MAX_SOFT_CHANNELS];
        int servo_count = 0;
        struct timespec cursor;

        pthread_mutex_lock(&channels_lock);
        for (int i = 0; i < soft_count; i++) {
            soft_channel_t *ch = &soft_channels[i];
            if (ch->mode != SOFT_SERVO) {
                continue;
            }
            pthread_mutex_lock(&ch->lock);
            values[servo_count] = ch->value;
            pthread_mutex_unlock(&ch->lock);
            servos[servo_count++] = ch;
        }
        pthread_mutex_unlock(&channels_lock);

        /*
         * Emit pulses sequentially with a full busy-wait on the high phase.
         * Absolute multi-deadline schedules lose pulse width under load when
         * the rising edge wakes late from nanosleep; width is what servos track.
         */
        cursor = cycle;
        for (int i = 0; i < servo_count && !stopping; i++) {
            int pulse_us = values[i];
            struct timespec falling;

            pulse_us = pulse_us < 0 ? 0 : pulse_us;
            pulse_us = pulse_us > 2500 ? 2500 : pulse_us;
            if (pulse_us == 0) {
                digitalWrite(servos[i]->physical, LOW);
                continue;
            }

            /* Stagger starts across the 20 ms frame to spread current draw. */
            if (servo_count > 1) {
                cursor = offset_from(cycle, SERVO_PERIOD_NS * i / servo_count);
                sleep_until(&cursor);
            }

            digitalWrite(servos[i]->physical, HIGH);
            falling = cursor;
            add_ns(&falling, (long)pulse_us * 1000L);
            /* Busy-wait the entire high window (capped by SERVO_BUSY_NS). */
            sleep_until_edge(&falling, SERVO_BUSY_NS);
            digitalWrite(servos[i]->physical, LOW);
            cursor = falling;
        }

        add_ns(&cycle, SERVO_PERIOD_NS);
        sleep_until(&cycle);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > cycle.tv_sec ||
            (now.tv_sec == cycle.tv_sec &&
             now.tv_nsec > cycle.tv_nsec + SERVO_PERIOD_NS)) {
            cycle = now;
        }
    }

    pthread_mutex_lock(&channels_lock);
    for (int i = 0; i < soft_count; i++) {
        if (soft_channels[i].mode == SOFT_SERVO) {
            digitalWrite(soft_channels[i].physical, LOW);
        }
    }
    pthread_mutex_unlock(&channels_lock);
    return NULL;
}

static void *soft_loop(void *arg)
{
    soft_channel_t *ch = arg;
    struct timespec cycle;

    set_realtime_priority(50);
    clock_gettime(CLOCK_MONOTONIC, &cycle);

    while (!stopping) {
        int value;
        soft_mode_t mode;
        long period_ns;
        long high_ns;

        pthread_mutex_lock(&ch->lock);
        value = ch->value;
        mode = ch->mode;
        pthread_mutex_unlock(&ch->lock);

        period_ns = MOTOR_PERIOD_NS;
        add_ns(&cycle, period_ns);

        value = value < 0 ? 0 : value;
        value = value > 255 ? 255 : value;
        high_ns = period_ns * value / 255L;

        if (mode == SOFT_MOTOR && value >= 255) {
            digitalWrite(ch->physical, HIGH);
            sleep_until(&cycle);
            continue;
        }

        if (high_ns > 0) {
            struct timespec falling;
            digitalWrite(ch->physical, HIGH);
            clock_gettime(CLOCK_MONOTONIC, &falling);
            add_ns(&falling, high_ns);
            sleep_until(&falling);
        }
        digitalWrite(ch->physical, LOW);
        sleep_until(&cycle);

        /*
         * Recover from a long scheduler delay without emitting catch-up pulses.
         */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > cycle.tv_sec ||
            (now.tv_sec == cycle.tv_sec && now.tv_nsec > cycle.tv_nsec + period_ns)) {
            cycle = now;
        }
    }

    digitalWrite(ch->physical, LOW);
    return NULL;
}

static soft_channel_t *get_soft_channel(int bcm, soft_mode_t mode)
{
    int physical = bcm_to_physical(bcm);
    soft_channel_t *ch;

    if (physical < 0) {
        return NULL;
    }

    pthread_mutex_lock(&channels_lock);
    for (int i = 0; i < soft_count; i++) {
        if (soft_channels[i].bcm == bcm) {
            ch = &soft_channels[i];
            pthread_mutex_unlock(&channels_lock);
            if (ch->mode != mode) {
                return NULL;
            }
            return ch;
        }
    }

    if (soft_count >= MAX_SOFT_CHANNELS) {
        pthread_mutex_unlock(&channels_lock);
        return NULL;
    }

    ch = &soft_channels[soft_count];
    memset(ch, 0, sizeof(*ch));
    ch->bcm = bcm;
    ch->physical = physical;
    ch->mode = mode;
    pthread_mutex_init(&ch->lock, NULL);
    pinMode(physical, OUTPUT);
    digitalWrite(physical, LOW);
    soft_count++;

    if (mode == SOFT_SERVO) {
        if (!servo_thread_running) {
            servo_thread_running =
                pthread_create(&servo_thread, NULL, servo_loop, NULL) == 0;
        }
        ch->running = servo_thread_running;
    } else {
        ch->running = pthread_create(&ch->thread, NULL, soft_loop, ch) == 0;
    }
    pthread_mutex_unlock(&channels_lock);
    return ch->running ? ch : NULL;
}

static int write_text(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    ssize_t len;
    ssize_t written;

    if (fd < 0) {
        return -1;
    }
    len = (ssize_t)strlen(value);
    written = write(fd, value, (size_t)len);
    close(fd);
    return written == len ? 0 : -1;
}

static int wait_for_path(const char *path);

/*
 * BOARD33 / BCM13 is GPIO357 on the second LSIO bank. The RDK WiringPi fork
 * cannot drive this line correctly and PWM3 owns its pinmux after boot.
 * Requesting GPIO357 through gpiolib selects the GPIO mux reliably.
 */
static int set_left_ir(int value)
{
    const char *gpio_path = "/sys/class/gpio/gpio357";

    if (!left_ir_prepared) {
        if (access("/sys/bus/platform/drivers/drobot-pwm/34170000.pwm",
                   F_OK) == 0 &&
            write_text("/sys/bus/platform/drivers/drobot-pwm/unbind",
                       "34170000.pwm") < 0) {
            return -1;
        }

        if (access(gpio_path, F_OK) != 0) {
            if (write_text("/sys/class/gpio/export", "357") < 0 &&
                errno != EBUSY) {
                return -1;
            }
            if (wait_for_path(gpio_path) < 0) {
                return -1;
            }
        }
        if (write_text("/sys/class/gpio/gpio357/direction", "out") < 0) {
            return -1;
        }
        left_ir_prepared = true;
    }

    return write_text("/sys/class/gpio/gpio357/value", value ? "1" : "0");
}

static int read_alias(const char *chip, char *alias, size_t alias_size)
{
    char path[PATH_MAX];
    FILE *fp;
    char line[256];

    snprintf(path, sizeof(path), "%s/device/uevent", chip);
    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "OF_ALIAS_0=", 11) == 0) {
            char *value = line + 11;
            value[strcspn(value, "\r\n")] = '\0';
            size_t length = strlen(value);
            if (length >= alias_size) {
                length = alias_size - 1;
            }
            memcpy(alias, value, length);
            alias[length] = '\0';
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    return -1;
}

static int find_pwm_chip(int group, char *chip, size_t chip_size)
{
    DIR *dir = opendir("/sys/class/pwm");
    struct dirent *entry;
    char wanted[32];

    if (!dir) {
        return -1;
    }
    snprintf(wanted, sizeof(wanted), "pwm%d", group);
    while ((entry = readdir(dir))) {
        char candidate[PATH_MAX];
        char alias[64];

        if (strncmp(entry->d_name, "pwmchip", 7) != 0) {
            continue;
        }
        snprintf(candidate, sizeof(candidate), "/sys/class/pwm/%s", entry->d_name);
        if (read_alias(candidate, alias, sizeof(alias)) == 0 &&
            strcmp(alias, wanted) == 0) {
            size_t length = strlen(candidate);
            if (length >= chip_size) {
                closedir(dir);
                return -1;
            }
            memcpy(chip, candidate, length + 1);
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return -1;
}

static int wait_for_path(const char *path)
{
    for (int i = 0; i < 100; i++) {
        if (access(path, F_OK) == 0) {
            return 0;
        }
        usleep(10000);
    }
    return -1;
}

static int prepare_hardware(hardware_channel_t *hw, int bcm)
{
    char export_path[PATH_MAX];
    char channel_text[16];
    char pwm_path[PATH_MAX];

    memset(hw, 0, sizeof(*hw));
    hw->bcm = bcm;
    if (!hardware_route(bcm, &hw->group, &hw->channel)) {
        return -1;
    }
    if (find_pwm_chip(hw->group, hw->chip, sizeof(hw->chip)) < 0) {
        return -1;
    }

    snprintf(pwm_path, sizeof(pwm_path), "%s/pwm%d", hw->chip, hw->channel);
    if (access(pwm_path, F_OK) != 0) {
        snprintf(export_path, sizeof(export_path), "%s/export", hw->chip);
        snprintf(channel_text, sizeof(channel_text), "%d", hw->channel);
        if (write_text(export_path, channel_text) < 0 && errno != EBUSY) {
            return -1;
        }
        if (wait_for_path(pwm_path) < 0) {
            return -1;
        }
    }
    hw->exported = true;
    return 0;
}

static int hardware_enable(hardware_channel_t *hw, long period_ns, long duty_ns)
{
    char path[PATH_MAX];
    char value[64];

    if (!hw->exported) {
        return -1;
    }
    if (duty_ns < 0) duty_ns = 0;
    if (duty_ns > period_ns) duty_ns = period_ns;

    snprintf(path, sizeof(path), "%s/pwm%d/enable", hw->chip, hw->channel);
    if (hw->enabled && hw->period_ns != period_ns) {
        write_text(path, "0");
        hw->enabled = false;
    }

    /*
     * Lower duty first: sysfs rejects a new period below the current duty.
     */
    snprintf(path, sizeof(path), "%s/pwm%d/duty_cycle", hw->chip, hw->channel);
    write_text(path, "0");

    snprintf(path, sizeof(path), "%s/pwm%d/period", hw->chip, hw->channel);
    snprintf(value, sizeof(value), "%ld", period_ns);
    if (write_text(path, value) < 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/pwm%d/duty_cycle", hw->chip, hw->channel);
    snprintf(value, sizeof(value), "%ld", duty_ns);
    if (write_text(path, value) < 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/pwm%d/enable", hw->chip, hw->channel);
    if (!hw->enabled && write_text(path, "1") < 0) {
        return -1;
    }
    hw->enabled = true;
    hw->period_ns = period_ns;
    return 0;
}

static hardware_channel_t hardware_channels[] = {
    {.bcm = 5}, {.bcm = 6}, {.bcm = 8}, {.bcm = 26},
};

static hardware_channel_t *get_hardware(int bcm)
{
    for (size_t i = 0; i < sizeof(hardware_channels) / sizeof(hardware_channels[0]); i++) {
        hardware_channel_t *hw = &hardware_channels[i];
        if (hw->bcm != bcm) {
            continue;
        }
        if (!hw->exported && prepare_hardware(hw, bcm) < 0) {
            return NULL;
        }
        return hw;
    }
    return NULL;
}

static int set_out(int bcm, int value)
{
    hardware_channel_t *hw;
    int physical;

    if (bcm == 13) {
        return set_left_ir(value);
    }

    if (hardware_route(bcm, &(int){0}, &(int){0})) {
        hw = get_hardware(bcm);
        if (!hw) return -1;
        long period = hw->period_ns > 0 ? hw->period_ns : MOTOR_PERIOD_NS;
        return hardware_enable(hw, period, value ? period : 0);
    }

    for (int i = 0; i < soft_count; i++) {
        soft_channel_t *ch = &soft_channels[i];
        if (ch->bcm != bcm) continue;
        pthread_mutex_lock(&ch->lock);
        ch->value = ch->mode == SOFT_MOTOR ? (value ? 255 : 0) : 0;
        pthread_mutex_unlock(&ch->lock);
        return 0;
    }

    physical = bcm_to_physical(bcm);
    if (physical < 0) return -1;
    pinMode(physical, OUTPUT);
    digitalWrite(physical, value ? HIGH : LOW);
    return 0;
}

static int set_pwm(int bcm, int duty)
{
    int group;
    int channel;

    duty = duty < 0 ? 0 : duty;
    duty = duty > 255 ? 255 : duty;
    if (hardware_route(bcm, &group, &channel)) {
        hardware_channel_t *hw = get_hardware(bcm);
        if (!hw) return -1;
        return hardware_enable(hw, MOTOR_PERIOD_NS,
                               MOTOR_PERIOD_NS * (long)duty / 255L);
    }

    soft_channel_t *ch = get_soft_channel(bcm, SOFT_MOTOR);
    if (!ch) return -1;
    pthread_mutex_lock(&ch->lock);
    ch->value = duty;
    pthread_mutex_unlock(&ch->lock);
    return 0;
}

static int set_servo(int bcm, int pulse_us)
{
    int group;
    int channel;
    int cur;

    pulse_us = pulse_us < 0 ? 0 : pulse_us;
    pulse_us = pulse_us > 2500 ? 2500 : pulse_us;
    pulse_us = quantize_servo_us(pulse_us);

    if (hardware_route(bcm, &group, &channel)) {
        hardware_channel_t *hw = get_hardware(bcm);
        if (!hw) return -1;
        return hardware_enable(hw, SERVO_PERIOD_NS, (long)pulse_us * 1000L);
    }

    /*
     * BCM7 is the expected software-servo route. Other non-hardware servo
     * pins also use this backend so a configuration error remains recoverable.
     */
    soft_channel_t *ch = get_soft_channel(bcm, SOFT_SERVO);
    if (!ch) return -1;
    pthread_mutex_lock(&ch->lock);
    cur = ch->value;
    /* Always accept pulse=0 (release). Ignore tiny stick/noise updates. */
    if (pulse_us != 0 && cur != 0 && abs(pulse_us - cur) < SERVO_HYST_US) {
        pthread_mutex_unlock(&ch->lock);
        return 0;
    }
    ch->value = pulse_us;
    pthread_mutex_unlock(&ch->lock);
    return 0;
}

static void cleanup(void)
{
    stopping = 1;
    if (servo_thread_running) {
        pthread_join(servo_thread, NULL);
    }
    for (int i = 0; i < soft_count; i++) {
        if (soft_channels[i].running &&
            soft_channels[i].mode == SOFT_MOTOR) {
            pthread_join(soft_channels[i].thread, NULL);
        }
        digitalWrite(soft_channels[i].physical, LOW);
    }
    for (size_t i = 0; i < sizeof(hardware_channels) / sizeof(hardware_channels[0]); i++) {
        hardware_channel_t *hw = &hardware_channels[i];
        if (hw->enabled) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/pwm%d/enable", hw->chip, hw->channel);
            write_text(path, "0");
        }
    }
    if (left_ir_prepared) {
        write_text("/sys/class/gpio/gpio357/value", "0");
    }
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

int main(void)
{
    char line[256];
    const char *hardware_env;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    if (wiringPiSetupPhys() < 0) {
        fprintf(stderr, "wiringPiSetupPhys failed\n");
        return 1;
    }

    hardware_env = getenv("VIGI_GPIO_HARDWARE_PWM");
    hardware_pwm_enabled =
        hardware_env != NULL && strcmp(hardware_env, "1") == 0;

    printf("rdk-gpio-helper ready backend=wiringpi-c hardware-pwm=%s "
           "servo hyst=%d q=%d busy_us=%ld\n",
           hardware_pwm_enabled ? "on" : "off", SERVO_HYST_US,
           SERVO_QUANTUM_US, SERVO_BUSY_NS / 1000L);

    while (!stopping && fgets(line, sizeof(line), stdin)) {
        char command[16];
        int bcm;
        int value;
        int rc = -1;

        if (sscanf(line, "%15s %d %d", command, &bcm, &value) < 1) {
            printf("err invalid\n");
            continue;
        }
        if (strcmp(command, "ping") == 0) {
            printf("ok\n");
            continue;
        }
        if (sscanf(line, "%15s %d %d", command, &bcm, &value) != 3) {
            printf("err invalid\n");
            continue;
        }

        if (strcmp(command, "out") == 0) {
            rc = set_out(bcm, value);
        } else if (strcmp(command, "pwm") == 0) {
            rc = set_pwm(bcm, value);
        } else if (strcmp(command, "servo") == 0) {
            rc = set_servo(bcm, value);
        }

        if (rc == 0) {
            printf("ok\n");
        } else {
            printf("err %s bcm=%d errno=%d\n", command, bcm, errno);
        }
    }

    cleanup();
    return 0;
}
