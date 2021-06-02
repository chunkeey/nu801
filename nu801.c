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

#include <sys/select.h>
#include <sys/ioctl.h>

#include <linux/gpio.h>
#include <linux/uleds.h>

#include "gpio-utils.h"

#ifdef NDEBUG
#define DPRINTF(fmt, ...)
#else
#define DPRINTF(fmt, ...) printf((fmt), ##__VA_ARGS__)
#endif

enum gpio_type { NUMBER };

/*
 * Here we describe our supported hardware
 * the "id" gets passed as the programs one and only parameter
 */
struct hardware_definitions {
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
		.id = "Cisco MX100-HW",
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

	/*
	 * {
	 *	.id = "Meraki Z1",
	 * },
	 * {
	 *	.id = "Meraki MR18",
	 * }
	 */

	{ },
};

struct nu801_led_struct {
	struct uleds_user_dev uleds_dev;
	int fd; /* /dev/uleds handle */
	int brightness; /* current brightness */
};

/* Program State */

static struct gpio_v2_line_values values = { };
static struct nu801_led_struct leds[3] = { };
static size_t num_leds;
static int gpio_fd;

static int register_uled(struct nu801_led_struct *led,
			const char *board, const char *color,
			const char *function)
{
	int ret;

	/* sprintf_s would be cool... but alas */
	snprintf((char *)&led->uleds_dev.name, LED_MAX_NAME_SIZE-1, "%s:%s:%s",
		board, color, function);

	led->uleds_dev.max_brightness = 255;

	led->fd = open("/dev/uleds", O_RDWR);
	if (led->fd == -1) {
		perror("Failed to open /dev/uleds");
		return -errno;
	}

	ret = write(led->fd, &led->uleds_dev, sizeof(led->uleds_dev));
	if (ret < sizeof(led->uleds_dev)) {
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
	struct gpio_v2_line_config config = { };
	unsigned int lines[3];
	int gpio_fd, ret, i, num_lines;

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
	num_lines = lines[NU801_LEI] >= 0 ? 3 : 2;
	DPRINTF("Registering '%d' gpio-lines.\n", num_lines);
	ret = gpiotools_request_line(dev->gpio.gpiochip, lines,
				num_lines, &config, "nu801");
	if (ret < 0) {
		perror("Failed to request chip lines.");
		return ret;
	}
	gpio_fd = ret;

	/*
	 * tell the kernel that we are interested in the following
	 * GPIOs by setting the bit in the .mask.
	 */
	for (i = 0; i < num_lines; i++)
		gpiotools_set_bit(&values.mask, i);

	/* get initial states ... not that this would matter */
	ret = gpiotools_get_values(gpio_fd, &values);
	if (ret < 0) {
		perror("Failed to request initial states...");
		return ret;
	}

	DPRINTF("Initial States: values.bits:%xl values.mask:%xl",
		values.bits, values.mask);

	return gpio_fd;
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

void udelay(const unsigned short usec)
{
	/*
	 * let's assume the caller doesn't something stupid like
	 * udelay(1000000000000ULL); ... Not that he/she/it can
	 * because we only accept 65535 max... so at most we get
	 * 66ms.
	 */
	ndelay(usec * 1000);
}

void handle_leds(struct hardware_definitions *dev)
{
	struct nu801_led_struct *led;
	uint16_t hwval, bit;
	int i;

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
				   (dev->gpio.num.lei < 0))) {

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

			/* ndelay(dev->ndelay); */
		}
	}

	/*
	 * In case we have the latch connected through a GPIO,
	 * we can just trigger it, instead of wasting 600us.
	 */
	if (dev->gpio.num.lei >= 0) {
		gpio_set(NU801_LEI, 1);
		gpio_commit();

		/* ndelay(dev->ndelay); */

		gpio_set(NU801_LEI, 0);
		gpio_commit();
	}
}

int main(int argc, char **args)
{
	struct hardware_definitions *dev;
	fd_set rfds;
	const char **color, **func;
	int i, ret = -EINVAL, highest_fd = -1;

	if (argc != 2) {
		fprintf(stderr, "%s: device-id\n", args[0]);
		goto out;
	}

	for (dev = &supported_hardware[0]; dev->id; dev++) {
		if (!strcmp(args[1], dev->id))
			break;
	}

	if (!dev->id) {
		fprintf(stderr, "%s: unsupported device '%s'\n",
			args[0], args[1]);
		goto out;
	}

	printf("Found supported device: '%s'\n", dev->id);
	DPRINTF("cki:%d sdi:%d lei:%d\n", dev->gpio.num.cki, dev->gpio.num.sdi,
		dev->gpio.num.lei);

	FD_ZERO(&rfds);
	for (i = 0, color = dev->colors, func = dev->functions;
	     *color && *func && i < ARRAY_SIZE(dev->colors);
	     color++, func++, i++) {
		DPRINTF("Registering LED %d %s:%s:%s\n", i, dev->board, *color, *func);
		ret = register_uled(&leds[i], dev->board, *color, *func);
		if (ret)
			goto out;

		FD_SET(leds[i].fd, &rfds);

		if (leds[i].fd > highest_fd)
			highest_fd = leds[i].fd;
	}
	num_leds = i;
	DPRINTF("Registered %d LEDs\n", num_leds);

	gpio_fd = register_gpio(dev);
	if (gpio_fd < 0) {
		perror("failed to register gpio.");
		goto out;
	}

	highest_fd++; /* select needs highest_fd + 1 */
	for (;;) {
		DPRINTF("Polling LEDs...\n");
		ret = select(highest_fd, &rfds, NULL, NULL, NULL);
		DPRINTF(" Got an LED event!\n ret=%d\n", ret);

		if (ret < 0)
			goto out;

		for (i = 0; i < num_leds; i++) {
			if (FD_ISSET(leds[i].fd, &rfds)) {
				int brightness;

				DPRINTF("LED %d has new data. (old brightness: %d)\n",
					i, leds[i].brightness);
				ret = read(leds[i].fd, &brightness,
					   sizeof(brightness));

				if (ret <= 0) {
					ret = ret ? : -1;
					goto out;
				}

				DPRINTF("set LED %d to brightness %d\n", i, brightness);
				leds[i].brightness = brightness;
			}

			FD_SET(leds[i].fd, &rfds);
		}

		DPRINTF("Committing new brightness values to NU801.\n");
		handle_leds(dev);
	}

out:
	DPRINTF("Exiting... ret=%d\n", ret);

	if (gpio_fd > 0)
		gpiotools_release_line(gpio_fd);
	for (i = 0; i < ARRAY_SIZE(leds); i++) {
		if (leds[i].fd > 0)
			close(leds[i].fd);
	}

	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
