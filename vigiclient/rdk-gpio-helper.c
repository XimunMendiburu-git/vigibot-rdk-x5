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
/* Defaults from the last "better" build — no extra anti-artifact features. */
#define SERVO_HYST_US_DEFAULT 60
#define SERVO_QUANTUM_US_DEFAULT 40
#define SERVO_MAX_VEL_US_DEFAULT 40
#define SERVO_MAX_ACCEL_US_DEFAULT 2
#define SERVO_HOLD_BREAK_US_DEFAULT 200
#define SERVO_HOLD_FRAMES_DEFAULT 10
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
    int value;               /* pulse currently emitted (µs) */
    int target;              /* commanded pulse (µs) */
    int vel_us;              /* signed slew speed (µs per frame) */
    int hold_us;             /* frozen pulse while hold_locked */
    int settled_frames;      /* consecutive settled frames */
    bool hold_locked;        /* ignore small target noise after settle */
    long period_ns;          /* motor soft-PWM period (pwmFrequency) */
    int64_t last_change_ns;  /* monotonic ns of last accepted target update */
    bool running;
    pthread_t thread;
    pthread_mutex_t lock;
} soft_channel_t;

static int servo_hyst_us = SERVO_HYST_US_DEFAULT;
static int servo_quantum_us = SERVO_QUANTUM_US_DEFAULT;
static int servo_max_vel_us = SERVO_MAX_VEL_US_DEFAULT;
static int servo_max_accel_us = SERVO_MAX_ACCEL_US_DEFAULT;
static int servo_hold_break_us = SERVO_HOLD_BREAK_US_DEFAULT;
static int servo_hold_frames = SERVO_HOLD_FRAMES_DEFAULT;
/* 0 = keep driving 50 Hz forever; >0 = stop pulses after idle (servo goes limp). */
static int servo_idle_detach_ms = 0;

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
static unsigned servo_glitch_count = 0;

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
    return ((pulse_us + servo_quantum_us / 2) / servo_quantum_us) *
           servo_quantum_us;
}

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
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

static int servo_step_toward_target(soft_channel_t *ch)
{
    int target;
    int value;
    int vel;
    int err;
    int desired_vel;
    int max_vel = servo_max_vel_us;
    int max_accel = servo_max_accel_us;

    if (max_vel < 1) {
        max_vel = 1;
    }
    if (max_accel < 1) {
        max_accel = 1;
    }

    /* Hold-lock: freeze emitted pulse; ignore micro-noise on target. */
    if (ch->hold_locked) {
        ch->value = ch->hold_us;
        ch->vel_us = 0;
        ch->target = ch->hold_us;
        return ch->hold_us;
    }

    target = ch->target;
    value = ch->value;
    vel = ch->vel_us;

    /* Immediate release / disable. */
    if (target <= 0) {
        ch->value = 0;
        ch->vel_us = 0;
        ch->settled_frames = 0;
        ch->hold_locked = false;
        return 0;
    }

    /* First command: snap (no hold yet — lock only after true settle). */
    if (value <= 0) {
        ch->value = target;
        ch->vel_us = 0;
        ch->settled_frames = 0;
        return target;
    }

    err = target - value;
    if (err == 0) {
        ch->vel_us = 0;
        if (ch->settled_frames < 1000000) {
            ch->settled_frames++;
        }
        if (ch->settled_frames >= servo_hold_frames) {
            ch->hold_locked = true;
            ch->hold_us = value;
            ch->target = value;
        }
        return value;
    }

    ch->settled_frames = 0;

    /* Desired velocity: close the gap, capped. */
    desired_vel = err;
    if (desired_vel > max_vel) {
        desired_vel = max_vel;
    } else if (desired_vel < -max_vel) {
        desired_vel = -max_vel;
    }

    /* Accelerate / decelerate toward desired_vel. */
    if (desired_vel > vel) {
        vel += max_accel;
        if (vel > desired_vel) {
            vel = desired_vel;
        }
    } else if (desired_vel < vel) {
        vel -= max_accel;
        if (vel < desired_vel) {
            vel = desired_vel;
        }
    }

    /* Do not overshoot the target in one frame. */
    if (err > 0 && vel > err) {
        vel = err;
    } else if (err < 0 && vel < err) {
        vel = err;
    }

    value += vel;
    if (value < 500 && target >= 500) {
        value = 500;
    }
    if (value > 2500) {
        value = 2500;
    }

    ch->value = value;
    ch->vel_us = vel;
    return value;
}

/*
 * Drive one servo pulse with width measured from the ACTUAL rising edge.
 * Absolute "cursor + pulse" deadlines stretch/shorten the pulse when the frame
 * wakes late — that looks like uncommanded jerks with a fixed target.
 */
static void emit_servo_pulse(int physical, int pulse_us)
{
    int64_t t0;
    int64_t tend;
    int64_t t;
    int64_t width_ns;
    int64_t overshoot_ns;

    pulse_us = pulse_us < 500 ? 500 : pulse_us;
    pulse_us = pulse_us > 2500 ? 2500 : pulse_us;
    width_ns = (int64_t)pulse_us * 1000LL;

    digitalWrite(physical, HIGH);
    t0 = mono_ns();
    tend = t0 + width_ns;

    do {
        t = mono_ns();
    } while (!stopping && t < tend);

    digitalWrite(physical, LOW);

    overshoot_ns = mono_ns() - tend;
    if (overshoot_ns > 100000LL) { /* >100 µs late falling edge */
        servo_glitch_count++;
        if ((servo_glitch_count % 25u) == 1u) {
            fprintf(stderr,
                    "servo pulse glitch phys=%d want=%dus overshoot=%lldus count=%u\n",
                    physical, pulse_us, (long long)(overshoot_ns / 1000LL),
                    servo_glitch_count);
        }
    }
}

static void *servo_loop(void *arg)
{
    struct timespec cycle;
    (void)arg;

    set_realtime_priority(80);
    mlockall(MCL_CURRENT | MCL_FUTURE);
    clock_gettime(CLOCK_MONOTONIC, &cycle);

    while (!stopping) {
        soft_channel_t *servos[MAX_SOFT_CHANNELS];
        int values[MAX_SOFT_CHANNELS];
        int servo_count = 0;

        pthread_mutex_lock(&channels_lock);
        for (int i = 0; i < soft_count; i++) {
            soft_channel_t *ch = &soft_channels[i];
            int pulse;
            int64_t last_change;
            int settled = 0;
            if (ch->mode != SOFT_SERVO) {
                continue;
            }
            pthread_mutex_lock(&ch->lock);
            pulse = servo_step_toward_target(ch);
            last_change = ch->last_change_ns;
            settled = (ch->target == ch->value && ch->vel_us == 0 &&
                       ch->target > 0);
            pthread_mutex_unlock(&ch->lock);
            if (servo_idle_detach_ms > 0 && settled) {
                int64_t idle_ns =
                    (int64_t)servo_idle_detach_ms * 1000000LL;
                if (mono_ns() - last_change >= idle_ns) {
                    pulse = 0;
                }
            }
            values[servo_count] = pulse;
            servos[servo_count++] = ch;
        }
        pthread_mutex_unlock(&channels_lock);

        for (int i = 0; i < servo_count && !stopping; i++) {
            int pulse_us = values[i];
            struct timespec slot;

            if (pulse_us <= 0) {
                digitalWrite(servos[i]->physical, LOW);
                continue;
            }

            if (servo_count > 1) {
                slot = offset_from(cycle, SERVO_PERIOD_NS * i / servo_count);
                sleep_until(&slot);
            }

            emit_servo_pulse(servos[i]->physical, pulse_us);
        }

        add_ns(&cycle, SERVO_PERIOD_NS);
        sleep_until(&cycle);

        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            while (now.tv_sec > cycle.tv_sec ||
                   (now.tv_sec == cycle.tv_sec &&
                    now.tv_nsec > cycle.tv_nsec + SERVO_PERIOD_NS)) {
                add_ns(&cycle, SERVO_PERIOD_NS);
            }
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
        period_ns = ch->period_ns > 0 ? ch->period_ns : MOTOR_PERIOD_NS;
        pthread_mutex_unlock(&ch->lock);

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
    ch->value = 0;
    ch->target = 0;
    ch->vel_us = 0;
    ch->hold_us = 0;
    ch->settled_frames = 0;
    ch->hold_locked = false;
    ch->period_ns = MOTOR_PERIOD_NS;
    ch->last_change_ns = mono_ns();
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
        long period = hw->period_ns > 0 ? hw->period_ns : MOTOR_PERIOD_NS;
        return hardware_enable(hw, period, period * (long)duty / 255L);
    }

    soft_channel_t *ch = get_soft_channel(bcm, SOFT_MOTOR);
    if (!ch) return -1;
    pthread_mutex_lock(&ch->lock);
    ch->value = duty;
    pthread_mutex_unlock(&ch->lock);
    return 0;
}

static int set_freq(int bcm, int hz)
{
    int group;
    int channel;
    long period_ns;

    if (hz < 1) {
        hz = 1;
    }
    if (hz > 20000) {
        hz = 20000;
    }
    period_ns = 1000000000L / hz;

    if (hardware_route(bcm, &group, &channel)) {
        hardware_channel_t *hw = get_hardware(bcm);
        if (!hw) return -1;
        hw->period_ns = period_ns;
        if (hw->enabled) {
            long high = hw->period_ns; /* keep prior duty roughly full if unknown */
            (void)high;
            /* Re-apply with new period at 0 duty until next pwmWrite. */
            return hardware_enable(hw, period_ns, 0);
        }
        return 0;
    }

    soft_channel_t *ch = get_soft_channel(bcm, SOFT_MOTOR);
    if (!ch) return -1;
    pthread_mutex_lock(&ch->lock);
    ch->period_ns = period_ns;
    pthread_mutex_unlock(&ch->lock);
    return 0;
}

static int set_mode(int bcm, int input)
{
    int physical = bcm_to_physical(bcm);

    if (bcm == 13) {
        /* Left IR via sysfs only supports driven output. */
        return input ? -1 : 0;
    }
    if (physical < 0) {
        return -1;
    }

    if (input) {
        pinMode(physical, INPUT);
    } else {
        pinMode(physical, OUTPUT);
        digitalWrite(physical, LOW);
    }
    return 0;
}

static int do_read(int bcm)
{
    int physical = bcm_to_physical(bcm);

    if (bcm == 13) {
        int fd = open("/sys/class/gpio/gpio357/value", O_RDONLY);
        char c = '0';
        if (fd < 0) {
            return -1;
        }
        if (read(fd, &c, 1) != 1) {
            close(fd);
            return -1;
        }
        close(fd);
        return c == '1' ? 1 : 0;
    }
    if (physical < 0) {
        return -1;
    }
    return digitalRead(physical) ? 1 : 0;
}

static int set_servo(int bcm, int pulse_us)
{
    int group;
    int channel;
    int cur_target;

    pulse_us = pulse_us < 0 ? 0 : pulse_us;
    pulse_us = pulse_us > 2500 ? 2500 : pulse_us;
    pulse_us = quantize_servo_us(pulse_us);

    if (hardware_route(bcm, &group, &channel)) {
        hardware_channel_t *hw = get_hardware(bcm);
        if (!hw) return -1;
        return hardware_enable(hw, SERVO_PERIOD_NS, (long)pulse_us * 1000L);
    }

    soft_channel_t *ch = get_soft_channel(bcm, SOFT_SERVO);
    if (!ch) return -1;
    pthread_mutex_lock(&ch->lock);

    /* While hold-locked, ignore noise; only a real move unlocks. */
    if (ch->hold_locked && pulse_us != 0) {
        if (abs(pulse_us - ch->hold_us) < servo_hold_break_us) {
            pthread_mutex_unlock(&ch->lock);
            return 0;
        }
        ch->hold_locked = false;
        ch->settled_frames = 0;
        ch->vel_us = 0;
        fprintf(stderr, "servo hold break bcm=%d hold=%d -> %d\n",
                bcm, ch->hold_us, pulse_us);
    }

    cur_target = ch->target;
    /* Always accept pulse=0 (release). Ignore tiny stick/noise on the target. */
    if (pulse_us != 0 && cur_target != 0 &&
        abs(pulse_us - cur_target) < servo_hyst_us) {
        pthread_mutex_unlock(&ch->lock);
        return 0;
    }
    ch->target = pulse_us;
    ch->last_change_ns = mono_ns();
    if (pulse_us == 0) {
        ch->hold_locked = false;
        ch->settled_frames = 0;
        ch->vel_us = 0;
    }
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
    const char *env;

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

    env = getenv("VIGI_SERVO_HYST_US");
    if (env && *env) {
        servo_hyst_us = atoi(env);
        if (servo_hyst_us < 0) {
            servo_hyst_us = 0;
        }
    }
    env = getenv("VIGI_SERVO_QUANTUM_US");
    if (env && *env) {
        servo_quantum_us = atoi(env);
        if (servo_quantum_us < 1) {
            servo_quantum_us = 1;
        }
    }
    env = getenv("VIGI_SERVO_MAX_VEL_US");
    if (env && *env) {
        servo_max_vel_us = atoi(env);
        if (servo_max_vel_us < 1) {
            servo_max_vel_us = 1;
        }
    }
    env = getenv("VIGI_SERVO_MAX_ACCEL_US");
    if (env && *env) {
        servo_max_accel_us = atoi(env);
        if (servo_max_accel_us < 1) {
            servo_max_accel_us = 1;
        }
    }
    env = getenv("VIGI_SERVO_HOLD_BREAK_US");
    if (env && *env) {
        servo_hold_break_us = atoi(env);
        if (servo_hold_break_us < 1) {
            servo_hold_break_us = 1;
        }
    }
    env = getenv("VIGI_SERVO_HOLD_FRAMES");
    if (env && *env) {
        servo_hold_frames = atoi(env);
        if (servo_hold_frames < 1) {
            servo_hold_frames = 1;
        }
    }
    env = getenv("VIGI_SERVO_IDLE_DETACH_MS");
    if (env && *env) {
        servo_idle_detach_ms = atoi(env);
        if (servo_idle_detach_ms < 0) {
            servo_idle_detach_ms = 0;
        }
    }

    printf("rdk-gpio-helper ready backend=wiringpi-c hardware-pwm=%s "
           "servo hyst=%d q=%d vel=%d accel=%d hold_break=%d hold_frames=%d "
           "busy_us=%ld idle_detach_ms=%d\n",
           hardware_pwm_enabled ? "on" : "off", servo_hyst_us,
           servo_quantum_us, servo_max_vel_us, servo_max_accel_us,
           servo_hold_break_us, servo_hold_frames,
           SERVO_BUSY_NS / 1000L, servo_idle_detach_ms);

    while (!stopping && fgets(line, sizeof(line), stdin)) {
        char command[16];
        char arg[16];
        int bcm = 0;
        int value = 0;
        int rc = -1;
        int n;

        if (sscanf(line, "%15s", command) != 1) {
            printf("err invalid\n");
            continue;
        }
        if (strcmp(command, "ping") == 0) {
            printf("ok\n");
            continue;
        }

        /* read <bcm> */
        if (strcmp(command, "read") == 0) {
            if (sscanf(line, "%15s %d", command, &bcm) != 2) {
                printf("err invalid\n");
                continue;
            }
            rc = do_read(bcm);
            if (rc < 0) {
                printf("err read bcm=%d errno=%d\n", bcm, errno);
            } else {
                printf("val %d\n", rc);
            }
            continue;
        }

        /* mode <bcm> in|out|0|1 */
        if (strcmp(command, "mode") == 0) {
            if (sscanf(line, "%15s %d %15s", command, &bcm, arg) != 3) {
                printf("err invalid\n");
                continue;
            }
            value = (strcmp(arg, "in") == 0 || strcmp(arg, "1") == 0 ||
                     strcmp(arg, "input") == 0)
                        ? 1
                        : 0;
            rc = set_mode(bcm, value);
            if (rc == 0) {
                printf("ok\n");
            } else {
                printf("err mode bcm=%d errno=%d\n", bcm, errno);
            }
            continue;
        }

        n = sscanf(line, "%15s %d %d", command, &bcm, &value);
        if (n != 3) {
            printf("err invalid\n");
            continue;
        }

        if (strcmp(command, "out") == 0) {
            rc = set_out(bcm, value);
        } else if (strcmp(command, "pwm") == 0) {
            rc = set_pwm(bcm, value);
        } else if (strcmp(command, "servo") == 0) {
            rc = set_servo(bcm, value);
        } else if (strcmp(command, "freq") == 0) {
            rc = set_freq(bcm, value);
        } else {
            printf("err unknown %s\n", command);
            continue;
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
