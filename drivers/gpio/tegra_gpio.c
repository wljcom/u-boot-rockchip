/*
 * NVIDIA Tegra20 GPIO handling.
 *  (C) Copyright 2010-2012
 *  NVIDIA Corporation <www.nvidia.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

/*
 * Based on (mostly copied from) kw_gpio.c based Linux 2.6 kernel driver.
 * Tom Warren (twarren@nvidia.com)
 */

#include <common.h>
#include <dm.h>
#include <malloc.h>
#include <errno.h>
#include <fdtdec.h>
#include <asm/io.h>
#include <asm/bitops.h>
#include <asm/arch/tegra.h>
#include <asm/gpio.h>
#include <dm/device-internal.h>

DECLARE_GLOBAL_DATA_PTR;

enum {
	TEGRA_CMD_INFO,
	TEGRA_CMD_PORT,
	TEGRA_CMD_OUTPUT,
	TEGRA_CMD_INPUT,
};

struct tegra_gpio_platdata {
	struct gpio_ctlr_bank *bank;
	const char *port_name;	/* Name of port, e.g. "B" */
	int base_gpio;		/* Port number for this port (0, 1,.., n-1) */
};

/* Information about each port at run-time */
struct tegra_port_info {
	char label[TEGRA_GPIOS_PER_PORT][GPIO_NAME_SIZE];
	struct gpio_ctlr_bank *bank;
	int base_gpio;		/* Port number for this port (0, 1,.., n-1) */
};

/* Return config of pin 'gpio' as GPIO (1) or SFPIO (0) */
static int get_config(unsigned gpio)
{
	struct gpio_ctlr *ctlr = (struct gpio_ctlr *)NV_PA_GPIO_BASE;
	struct gpio_ctlr_bank *bank = &ctlr->gpio_bank[GPIO_BANK(gpio)];
	u32 u;
	int type;

	u = readl(&bank->gpio_config[GPIO_PORT(gpio)]);
	type =  (u >> GPIO_BIT(gpio)) & 1;

	debug("get_config: port = %d, bit = %d is %s\n",
		GPIO_FULLPORT(gpio), GPIO_BIT(gpio), type ? "GPIO" : "SFPIO");

	return type;
}

/* Config pin 'gpio' as GPIO or SFPIO, based on 'type' */
static void set_config(unsigned gpio, int type)
{
	struct gpio_ctlr *ctlr = (struct gpio_ctlr *)NV_PA_GPIO_BASE;
	struct gpio_ctlr_bank *bank = &ctlr->gpio_bank[GPIO_BANK(gpio)];
	u32 u;

	debug("set_config: port = %d, bit = %d, %s\n",
		GPIO_FULLPORT(gpio), GPIO_BIT(gpio), type ? "GPIO" : "SFPIO");

	u = readl(&bank->gpio_config[GPIO_PORT(gpio)]);
	if (type)				/* GPIO */
		u |= 1 << GPIO_BIT(gpio);
	else
		u &= ~(1 << GPIO_BIT(gpio));
	writel(u, &bank->gpio_config[GPIO_PORT(gpio)]);
}

/* Return GPIO pin 'gpio' direction - 0 = input or 1 = output */
static int get_direction(unsigned gpio)
{
	struct gpio_ctlr *ctlr = (struct gpio_ctlr *)NV_PA_GPIO_BASE;
	struct gpio_ctlr_bank *bank = &ctlr->gpio_bank[GPIO_BANK(gpio)];
	u32 u;
	int dir;

	u = readl(&bank->gpio_dir_out[GPIO_PORT(gpio)]);
	dir =  (u >> GPIO_BIT(gpio)) & 1;

	debug("get_direction: port = %d, bit = %d, %s\n",
		GPIO_FULLPORT(gpio), GPIO_BIT(gpio), dir ? "OUT" : "IN");

	return dir;
}

/* Config GPIO pin 'gpio' as input or output (OE) as per 'output' */
static void set_direction(unsigned gpio, int output)
{
	struct gpio_ctlr *ctlr = (struct gpio_ctlr *)NV_PA_GPIO_BASE;
	struct gpio_ctlr_bank *bank = &ctlr->gpio_bank[GPIO_BANK(gpio)];
	u32 u;

	debug("set_direction: port = %d, bit = %d, %s\n",
		GPIO_FULLPORT(gpio), GPIO_BIT(gpio), output ? "OUT" : "IN");

	u = readl(&bank->gpio_dir_out[GPIO_PORT(gpio)]);
	if (output)
		u |= 1 << GPIO_BIT(gpio);
	else
		u &= ~(1 << GPIO_BIT(gpio));
	writel(u, &bank->gpio_dir_out[GPIO_PORT(gpio)]);
}

/* set GPIO pin 'gpio' output bit as 0 or 1 as per 'high' */
static void set_level(unsigned gpio, int high)
{
	struct gpio_ctlr *ctlr = (struct gpio_ctlr *)NV_PA_GPIO_BASE;
	struct gpio_ctlr_bank *bank = &ctlr->gpio_bank[GPIO_BANK(gpio)];
	u32 u;

	debug("set_level: port = %d, bit %d == %d\n",
		GPIO_FULLPORT(gpio), GPIO_BIT(gpio), high);

	u = readl(&bank->gpio_out[GPIO_PORT(gpio)]);
	if (high)
		u |= 1 << GPIO_BIT(gpio);
	else
		u &= ~(1 << GPIO_BIT(gpio));
	writel(u, &bank->gpio_out[GPIO_PORT(gpio)]);
}

static int check_reserved(struct udevice *dev, unsigned offset,
			  const char *func)
{
	struct tegra_port_info *state = dev_get_priv(dev);
	struct gpio_dev_priv *uc_priv = dev->uclass_priv;

	if (!*state->label[offset]) {
		printf("tegra_gpio: %s: error: gpio %s%d not reserved\n",
		       func, uc_priv->bank_name, offset);
		return -EBUSY;
	}

	return 0;
}

/* set GPIO pin 'gpio' as an output, with polarity 'value' */
int tegra_spl_gpio_direction_output(int gpio, int value)
{
	/* Configure as a GPIO */
	set_config(gpio, 1);

	/* Configure GPIO output value. */
	set_level(gpio, value);

	/* Configure GPIO direction as output. */
	set_direction(gpio, 1);

	return 0;
}

/*
 * Generic_GPIO primitives.
 */

static int tegra_gpio_request(struct udevice *dev, unsigned offset,
			      const char *label)
{
	struct tegra_port_info *state = dev_get_priv(dev);

	if (*state->label[offset])
		return -EBUSY;

	strncpy(state->label[offset], label, GPIO_NAME_SIZE);
	state->label[offset][GPIO_NAME_SIZE - 1] = '\0';

	/* Configure as a GPIO */
	set_config(state->base_gpio + offset, 1);

	return 0;
}

static int tegra_gpio_free(struct udevice *dev, unsigned offset)
{
	struct tegra_port_info *state = dev_get_priv(dev);
	int ret;

	ret = check_reserved(dev, offset, __func__);
	if (ret)
		return ret;
	state->label[offset][0] = '\0';

	return 0;
}

/* read GPIO OUT value of pin 'gpio' */
static int tegra_gpio_get_output_value(unsigned gpio)
{
	struct gpio_ctlr *ctlr = (struct gpio_ctlr *)NV_PA_GPIO_BASE;
	struct gpio_ctlr_bank *bank = &ctlr->gpio_bank[GPIO_BANK(gpio)];
	int val;

	debug("gpio_get_output_value: pin = %d (port %d:bit %d)\n",
		gpio, GPIO_FULLPORT(gpio), GPIO_BIT(gpio));

	val = readl(&bank->gpio_out[GPIO_PORT(gpio)]);

	return (val >> GPIO_BIT(gpio)) & 1;
}


/* set GPIO pin 'gpio' as an input */
static int tegra_gpio_direction_input(struct udevice *dev, unsigned offset)
{
	struct tegra_port_info *state = dev_get_priv(dev);
	int ret;

	ret = check_reserved(dev, offset, __func__);
	if (ret)
		return ret;

	/* Configure GPIO direction as input. */
	set_direction(state->base_gpio + offset, 0);

	return 0;
}

/* set GPIO pin 'gpio' as an output, with polarity 'value' */
static int tegra_gpio_direction_output(struct udevice *dev, unsigned offset,
				       int value)
{
	struct tegra_port_info *state = dev_get_priv(dev);
	int gpio = state->base_gpio + offset;
	int ret;

	ret = check_reserved(dev, offset, __func__);
	if (ret)
		return ret;

	/* Configure GPIO output value. */
	set_level(gpio, value);

	/* Configure GPIO direction as output. */
	set_direction(gpio, 1);

	return 0;
}

/* read GPIO IN value of pin 'gpio' */
static int tegra_gpio_get_value(struct udevice *dev, unsigned offset)
{
	struct tegra_port_info *state = dev_get_priv(dev);
	int gpio = state->base_gpio + offset;
	int ret;
	int val;

	ret = check_reserved(dev, offset, __func__);
	if (ret)
		return ret;

	debug("%s: pin = %d (port %d:bit %d)\n", __func__,
	      gpio, GPIO_FULLPORT(gpio), GPIO_BIT(gpio));

	val = readl(&state->bank->gpio_in[GPIO_PORT(gpio)]);

	return (val >> GPIO_BIT(gpio)) & 1;
}

/* write GPIO OUT value to pin 'gpio' */
static int tegra_gpio_set_value(struct udevice *dev, unsigned offset, int value)
{
	struct tegra_port_info *state = dev_get_priv(dev);
	int gpio = state->base_gpio + offset;
	int ret;

	ret = check_reserved(dev, offset, __func__);
	if (ret)
		return ret;

	debug("gpio_set_value: pin = %d (port %d:bit %d), value = %d\n",
	      gpio, GPIO_FULLPORT(gpio), GPIO_BIT(gpio), value);

	/* Configure GPIO output value. */
	set_level(gpio, value);

	return 0;
}

void gpio_config_table(const struct tegra_gpio_config *config, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		switch (config[i].init) {
		case TEGRA_GPIO_INIT_IN:
			gpio_direction_input(config[i].gpio);
			break;
		case TEGRA_GPIO_INIT_OUT0:
			gpio_direction_output(config[i].gpio, 0);
			break;
		case TEGRA_GPIO_INIT_OUT1:
			gpio_direction_output(config[i].gpio, 1);
			break;
		}
		set_config(config[i].gpio, 1);
	}
}

static int tegra_gpio_get_function(struct udevice *dev, unsigned offset)
{
	struct tegra_port_info *state = dev_get_priv(dev);
	int gpio = state->base_gpio + offset;

	if (!*state->label[offset])
		return GPIOF_UNUSED;
	if (!get_config(gpio))
		return GPIOF_FUNC;
	else if (get_direction(gpio))
		return GPIOF_OUTPUT;
	else
		return GPIOF_INPUT;
}

static int tegra_gpio_get_state(struct udevice *dev, unsigned int offset,
				char *buf, int bufsize)
{
	struct gpio_dev_priv *uc_priv = dev->uclass_priv;
	struct tegra_port_info *state = dev_get_priv(dev);
	int gpio = state->base_gpio + offset;
	const char *label;
	int is_output;
	int is_gpio;
	int size;

	label = state->label[offset];
	is_gpio = get_config(gpio); /* GPIO, not SFPIO */
	size = snprintf(buf, bufsize, "%s%d: ",
			uc_priv->bank_name ? uc_priv->bank_name : "", offset);
	buf += size;
	bufsize -= size;
	if (is_gpio) {
		is_output = get_direction(gpio);

		snprintf(buf, bufsize, "%s: %d [%c]%s%s",
			 is_output ? "out" : " in",
			 is_output ?
				tegra_gpio_get_output_value(gpio) :
				tegra_gpio_get_value(dev, offset),
			 *label ? 'x' : ' ',
			 *label ? " " : "",
			 label);
	} else {
		snprintf(buf, bufsize, "sfpio");
	}

	return 0;
}

static const struct dm_gpio_ops gpio_tegra_ops = {
	.request		= tegra_gpio_request,
	.free			= tegra_gpio_free,
	.direction_input	= tegra_gpio_direction_input,
	.direction_output	= tegra_gpio_direction_output,
	.get_value		= tegra_gpio_get_value,
	.set_value		= tegra_gpio_set_value,
	.get_function		= tegra_gpio_get_function,
	.get_state		= tegra_gpio_get_state,
};

/**
 * Returns the name of a GPIO port
 *
 * GPIOs are named A, B, C, ..., Z, AA, BB, CC, ...
 *
 * @base_port: Base port number (0, 1..n-1)
 * @return allocated string containing the name
 */
static char *gpio_port_name(int base_port)
{
	char *name, *s;

	name = malloc(3);
	if (name) {
		s = name;
		*s++ = 'A' + (base_port % 26);
		if (base_port >= 26)
			*s++ = *name;
		*s = '\0';
	}

	return name;
}

static const struct udevice_id tegra_gpio_ids[] = {
	{ .compatible = "nvidia,tegra30-gpio" },
	{ .compatible = "nvidia,tegra20-gpio" },
	{ }
};

static int gpio_tegra_probe(struct udevice *dev)
{
	struct gpio_dev_priv *uc_priv = dev->uclass_priv;
	struct tegra_port_info *priv = dev->priv;
	struct tegra_gpio_platdata *plat = dev->platdata;

	/* Only child devices have ports */
	if (!plat)
		return 0;

	priv->bank = plat->bank;
	priv->base_gpio = plat->base_gpio;

	uc_priv->gpio_count = TEGRA_GPIOS_PER_PORT;
	uc_priv->bank_name = plat->port_name;

	return 0;
}

/**
 * We have a top-level GPIO device with no actual GPIOs. It has a child
 * device for each Tegra port.
 */
static int gpio_tegra_bind(struct udevice *parent)
{
	struct tegra_gpio_platdata *plat = parent->platdata;
	struct gpio_ctlr *ctlr;
	int bank_count;
	int bank;
	int ret;
	int len;

	/* If this is a child device, there is nothing to do here */
	if (plat)
		return 0;

	/*
	 * This driver does not make use of interrupts, other than to figure
	 * out the number of GPIO banks
	 */
	if (!fdt_getprop(gd->fdt_blob, parent->of_offset, "interrupts", &len))
		return -EINVAL;
	bank_count = len / 3 / sizeof(u32);
	ctlr = (struct gpio_ctlr *)fdtdec_get_addr(gd->fdt_blob,
						   parent->of_offset, "reg");
	for (bank = 0; bank < bank_count; bank++) {
		int port;

		for (port = 0; port < TEGRA_PORTS_PER_BANK; port++) {
			struct tegra_gpio_platdata *plat;
			struct udevice *dev;
			int base_port;

			plat = calloc(1, sizeof(*plat));
			if (!plat)
				return -ENOMEM;
			plat->bank = &ctlr->gpio_bank[bank];
			base_port = bank * TEGRA_PORTS_PER_BANK + port;
			plat->base_gpio = TEGRA_GPIOS_PER_PORT * base_port;
			plat->port_name = gpio_port_name(base_port);

			ret = device_bind(parent, parent->driver,
					  plat->port_name, plat, -1, &dev);
			if (ret)
				return ret;
			dev->of_offset = parent->of_offset;
		}
	}

	return 0;
}

U_BOOT_DRIVER(gpio_tegra) = {
	.name	= "gpio_tegra",
	.id	= UCLASS_GPIO,
	.of_match = tegra_gpio_ids,
	.bind	= gpio_tegra_bind,
	.probe = gpio_tegra_probe,
	.priv_auto_alloc_size = sizeof(struct tegra_port_info),
	.ops	= &gpio_tegra_ops,
};
