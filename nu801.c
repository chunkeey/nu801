// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace LED-Driver for the NumEn Tech. NU801 LED-Controller chip.
 * (3 channel 16 bit PWM Constant Current Driver)
 *
 * This code was based on gpio-utils + uledmon from the linux
 * kernel source... as well as leds-nu801.c from Kevin Paul Herbert.
 *
 * gcc -D_GNU_SOURCE -Os -o nu801 -std=gnu11 gpio-utils.c nu801.c
 *
 * For more information about the chip, visit: http://www.numen-tech.com
 *
 * by: Christian Lamparter <chunkeey@gmail.com>
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <getopt.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>

#include <sys/select.h>
#include <sys/ioctl.h>

#include <linux/gpio.h>
#include <linux/uleds.h>

#include "gpio-utils.h"

enum gpio_type { NUMBER };

/*
 * Here we describe our supported hardware
 * the "id" gets passed as the programs one and only parameter
 */
static const struct hardware_definitions {
	const char *id;
	const char *board;

	struct {
		const char *gpiochip;

		enum gpio_type type;
		union {
			struct {
				unsigned int cki;
				unsigned int sdi;
				unsigned int lei;
			} num;
		};
	} gpio;
	unsigned int ndelay;
	const char *colors[3];		/* nu801 has max. 3 channels */
	const char *functions[3];	/* likewise... 3 channels */
} supported_hardware[] = {
	{
		.id = "cisco-mx100-hw",
		.board = "mx100",
		.gpio = {
			.type = NUMBER,
			.gpiochip = "gpiochip0",
			.num = {
				.cki = 41,
				.sdi = 6,
				.lei = 5,
			},
		},
		.ndelay = 150,
		.colors = { "blue", "green", "red" },
		.functions = { "tricolor", "tricolor", "tricolor" },
	},


	{
		.id = "meraki,z1",
		.gpio = {
			.type = NUMBER,
			.gpiochip = "gpiochip0",
			.num = {
				.cki = 14,
				.sdi = 15,
				.lei = ~0,
			},
		},
		.ndelay = 500,
		.colors = { "blue", "green", "red" },
		.functions = { "tricolor", "tricolor", "tricolor" },
	},


	{
		.id = "meraki,mr18",
		.gpio = {
			.type = NUMBER,
			.gpiochip = "gpiochip0",
			.num = {
				.cki = 11,
				.sdi = 12,
				.lei = ~0,
			},
		},
		.ndelay = 500,
		.colors = { "red", "green", "blue" },
		.functions = { "tricolor", "tricolor", "tricolor" },
	},

	{ 0, } /* Sentinel */
};

struct nu801_led_struct {
	struct uleds_user_dev uleds_dev;
	int fd; /* /dev/uleds handle */
	int brightness; /* current brightness */
};

/* Program State */
static volatile sig_atomic_t fatal_error_in_progress = 0;
static struct gpio_v2_line_values values = { 0 };
static struct nu801_led_struct leds[3] = { 0 };
static const struct hardware_definitions *dev;
static unsigned int num_leds;
static int gpio_fd;
static bool daemonize = true;
static bool debug = false;

#define DPRINTF(fmt, ...) { if (debug) printf((fmt), ##__VA_ARGS__); }
#define PID_NOBODY 65534
#define GID_NOGROUP 65534
#define RUNFILE "/var/run/nu801.pid"

static int register_uled(struct nu801_led_struct *led,
			const char *board, const char *color,
			const char *function)
{
	int ret;

	/* sprintf_s would be cool... but alas */
	if (board)
		snprintf((char *)&led->uleds_dev.name, LED_MAX_NAME_SIZE-1, "%s:%s:%s",
			board, color, function);
	else
		snprintf((char *)&led->uleds_dev.name, LED_MAX_NAME_SIZE-1, "%s:%s",
			color, function);

	led->uleds_dev.max_brightness = 255;

	led->fd = open("/dev/uleds", O_RDWR);
	if (led->fd == -1) {
		perror("Failed to open /dev/uleds");
		return -errno;
	}

	ret = write(led->fd, &led->uleds_dev, sizeof(led->uleds_dev));
	if (ret < (int)sizeof(led->uleds_dev)) {
		perror("Failed to write to /dev/uleds");
		return -errno;
	}

	return 0;
}

enum nu801_gpio_t {
	NU801_CKI = 0,
	NU801_SDI = 1,
	NU801_LEI = 2
};

static int register_gpio(const struct hardware_definitions *dev)
{
	struct gpio_v2_line_config config = { 0 };
	unsigned int lines[3], num_lines, i;
	int _gpio_fd, ret;

	if (dev->gpio.type == NUMBER) {
		lines[NU801_CKI] = dev->gpio.num.cki;
		lines[NU801_SDI] = dev->gpio.num.sdi;
		lines[NU801_LEI] = dev->gpio.num.lei;
	}

	config.flags = GPIO_V2_LINE_FLAG_OUTPUT;

	/*
	 * the NU801 supports either a "2-" or a "3"-wire interface.
	 * This should not be confused with everyones favourite
	 * 2-Wire Protocol (I2C) or 3-Wire Protocol (SPI)... This
	 * interface is much simpler than those two. Heck, if this
	 * device would just have supported I2C, this wouldn't be
	 * worth all this hassle.
	 */
	num_lines = ((lines[NU801_LEI] ^ ~0) ? 3 : 2);
	DPRINTF("Registering '%u' gpio-lines.\n", num_lines);
	ret = gpiotools_request_line(dev->gpio.gpiochip, lines,
				num_lines, &config, "nu801");
	if (ret < 0) {
		perror("Failed to request chip lines");
		return ret;
	}
	_gpio_fd = ret;

	/*
	 * tell the kernel that we are interested in the following
	 * GPIOs by setting the bit in the .mask.
	 */
	for (i = 0; i < num_lines; i++)
		gpiotools_set_bit(&values.mask, i);

	/* get initial states ... not that this would matter */
	ret = gpiotools_get_values(_gpio_fd, &values);
	if (ret < 0) {
		perror("Failed to request initial states");
		return ret;
	}

	DPRINTF("Initial States: values.bits:%llx values.mask:%llx\n",
		values.bits, values.mask);

	return _gpio_fd;
}

static inline void gpio_set(const enum nu801_gpio_t gpio, const bool state)
{
	gpiotools_assign_bit(&values.bits, gpio, state);
}

static inline void gpio_commit(void)
{
	gpiotools_set_values(gpio_fd, &values);
}

/* yee, this are probably entirely cosmetic */
static void ndelay(const long nsec)
{
	struct timespec sleep = { 0, nsec };

	nanosleep(&sleep, NULL);
}

static void udelay(const unsigned short usec)
{
	/*
	 * let's assume the caller doesn't something stupid like
	 * udelay(1000000000000ULL); ... Not that he/she/it can
	 * because we only accept 65535 max... so at most we get
	 * 66ms.
	 */
	ndelay(usec * 1000);
}

static void handle_leds(const struct hardware_definitions *dev)
{
	struct nu801_led_struct *led;
	uint16_t hwval, bit;
	unsigned int i;

	/*
	 * bit-bang the 3 x 16-Bit PWM values. There's no fancy protocol,
	 * just the raw values, one after the other and bit by bit...
	 *
	 * No, I don't think the ndelay will accomplish much, it's there
	 * "for show".
	 */

	for (i = 0, led = &leds[0]; i < num_leds; led++, i++) {

		/*
		 * Linux's defines the range for LED brightness from
		 * 0 = LED_OFF, 1 = LED_ON, 127 = LED_HALF and 255 = LED_FULL
		 *
		 * The LED_ON doesn't quite fit in this series. But
		 * since we want to provide bug-for-bug compatibility
		 * with the existing driver... we do what it did to
		 * convert these values to something the 16-bit PWM
		 * can better understand.
		 */
		hwval = led->brightness << 8;

		/* xmit each bit... starting from the MSB */
		for (bit = 0x8000; bit; bit >>= 1) {
			gpio_set(NU801_SDI, !!(hwval & bit));
			gpio_set(NU801_CKI, 1);
			gpio_commit();

			if (((i == (num_leds - 1)) && (bit == 1) &&
				   !(~0 ^ dev->gpio.num.lei))) {

				/*
				 * From the datasheet:
				 * "When clock signal keep high for more than
				 * 600us, NU801 will generate an internal
				 * pseudo LE signal. That will trigger the
				 * data latch circut to hold the
				 * luminance data.
				 */
				udelay(600);
			} else {
				/*
				 * Userspace is so slow that this nano-second
				 * delay are completely wasted cycles.
				 * ndelay(dev->ndelay);
				 */
			}
			gpio_set(NU801_CKI, 0);
			gpio_commit();

			ndelay(dev->ndelay);
		}
	}

	/*
	 * In case we have the latch connected through a GPIO,
	 * we can just trigger it, instead of wasting 600us.
	 */
	if ((~0 ^ dev->gpio.num.lei)) {
		gpio_set(NU801_LEI, 1);
		gpio_commit();

		ndelay(dev->ndelay);

		gpio_set(NU801_LEI, 0);
		gpio_commit();
	}
}

static void teardown(void)
{
	unsigned int i;

	if (gpio_fd > 0) {
		DPRINTF("turning off LEDs on shutdown\n");
		/* turn off the lights before exitting. */
		for (i = 0; i < num_leds; i++)
			leds[i].brightness = 0;

		handle_leds(dev);

		DPRINTF("releasing GPIOs back to the kernel.\n");
		gpiotools_release_line(gpio_fd);
		gpio_fd = -1;
	}

	for (i = 0; i < ARRAY_SIZE(leds); i++) {
		if (leds[i].fd > 0) {
			DPRINTF("unregistering LED %u\n", i);
			close(leds[i].fd);
			leds[i].fd = -1;
		}
	}
}

static void fatal_error_signal(int sig)
{
	/* catch cascading errors. if we end up here then elevate this */
	if (fatal_error_in_progress)
		raise(sig);

	fatal_error_in_progress = 1;

	teardown();

	signal(sig, SIG_DFL);
	raise(sig);
}

static int catch_fatal_errors(void)
{
        sigset_t sigs;

	/* block all signals */
	sigemptyset(&sigs);
	sigprocmask(SIG_BLOCK, &sigs, NULL);

	if ((signal(SIGTERM, fatal_error_signal) == SIG_ERR) ||
	    (signal(SIGALRM, fatal_error_signal) == SIG_ERR) ||
	    (signal(SIGABRT, fatal_error_signal) == SIG_ERR) ||
	    (signal(SIGPIPE, fatal_error_signal) == SIG_ERR) ||
	    (signal(SIGHUP,  fatal_error_signal) == SIG_ERR) ||
	    (signal(SIGILL,  fatal_error_signal) == SIG_ERR) ||
	    (signal(SIGINT,  fatal_error_signal) == SIG_ERR) ||
	    (signal(SIGFPE,  fatal_error_signal) == SIG_ERR))
		return -1;

	return 0;
}

static void __attribute__ ((noreturn)) usage(int ret)
{
	fprintf(stderr, "Usage: nu801 [-P pidfile] [-F] [-d] [-h] device-id\n\n"
		"NU801 userspace controller\n\n"
		"\t-P\t- specify custom pidfile (default:'" RUNFILE "')\n"
		"\t-F\t- run in foreground.\n"
		"\t-h\t- shows this help.\n"
		"\n"
		"\tdevice-id - OF machine compatible/ACPI devicename\n");
	exit(ret ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	fd_set rfds;
	const char *const *color, *const *func;
	const char *runfile = RUNFILE;
	unsigned int i;
	int ret = -EINVAL, pidfd, highest_fd = -1, opt;
	pid_t pid;

	if (catch_fatal_errors())
		goto out;

	while ((opt = getopt(argc, argv, "P:Fdh")) != -1) {
		switch (opt) {
		case 'P':
			if (strnlen(optarg,1))
				runfile = optarg;
			else
				runfile = NULL;
			break;
		case 'F':
			daemonize = false;
			break;
		case 'd':
			debug = true;
			break;
		case 'h':
			usage(0);
			break;
		default:
			usage(ret);
			break;
		}
	}

	if (optind >= argc)
		usage(ret);

	for (dev = &supported_hardware[0]; dev->id; dev++) {
		if (!strcmp(argv[optind], dev->id))
			break;
	}

	if (!dev->id) {
		fprintf(stderr, "nu801: unsupported device '%s'\n", argv[optind]);
		goto out;
	}

	DPRINTF("Found supported device: '%s'\n", dev->id);
	DPRINTF("cki:%u sdi:%u lei:%u\n", dev->gpio.num.cki, dev->gpio.num.sdi,
		dev->gpio.num.lei);

	FD_ZERO(&rfds);
	for (i = 0, color = dev->colors, func = dev->functions;
	     *color && *func && i < ARRAY_SIZE(dev->colors);
	     color++, func++, i++) {
		DPRINTF("Registering LED %u %s:%s:%s\n", i, dev->board, *color, *func);
		ret = register_uled(&leds[i], dev->board, *color, *func);
		if (ret)
			goto out;

		FD_SET(leds[i].fd, &rfds);

		if (leds[i].fd > highest_fd)
			highest_fd = leds[i].fd;
	}
	num_leds = i;
	DPRINTF("Registered %u LEDs\n", num_leds);

	gpio_fd = register_gpio(dev);
	if (gpio_fd < 0) {
		perror("failed to register gpio");
		goto out;
	}

	if (daemonize) {
		DPRINTF("Summoning the daemon with a fork...\n");
		pid = fork();
		if (pid < 0) {
			perror("failed to fork/daemonize");
			ret = pid;
			goto out;
		} else if (pid > 0) {
			/* parent */
			goto goodbye;
		}
	}

	if (runfile) {
		DPRINTF("Setting up pid '%s'\n", runfile);
		/* remove stale pidfiles or nefarious symlinks - see dnsmasq. */
		unlink(runfile);
		pidfd = open(runfile, O_WRONLY|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
		if (pidfd < 0) {
			perror("failed to create pidfile");
			ret = pidfd;
			goto out;
		}
		dprintf(pidfd, "%d\n", getpid());
		fchown(pidfd, PID_NOBODY, GID_NOGROUP);
		close(pidfd);
	}

	/* no need for special permissions any more. Drop to nobody:nogroup */
	setgid(GID_NOGROUP);
	setuid(PID_NOBODY);

	highest_fd++; /* select needs highest_fd + 1 */
	for (;;) {
		DPRINTF("Polling LEDs...\n");
		ret = select(highest_fd, &rfds, NULL, NULL, NULL);
		DPRINTF("Got an LED event! ret=%d\n", ret);

		if (ret < 0)
			goto out;

		for (i = 0; i < num_leds; i++) {
			if (FD_ISSET(leds[i].fd, &rfds)) {
				int brightness;

				DPRINTF("LED %u has new data. (old brightness: %d)\n",
					i, leds[i].brightness);
				ret = read(leds[i].fd, &brightness,
					   sizeof(brightness));

				if (ret <= 0) {
					ret = ((ret < 0) ? -1 : ret);
					goto out;
				}

				DPRINTF("set LED %u to brightness %d\n", i, brightness);
				leds[i].brightness = brightness;
			}

			FD_SET(leds[i].fd, &rfds);
		}

		DPRINTF("Committing new brightness values to NU801.\n");
		handle_leds(dev);
	}

out:
	DPRINTF("Exiting... ret=%d\n", ret);

	teardown();
goodbye:
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
