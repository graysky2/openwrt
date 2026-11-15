// SPDX-License-Identifier: GPL-2.0-only

/*
 * UBNT EtherLighting — per-port RGBW status LED driver
 * ======================================================
 *
 * Hardware overview
 * ------------------
 * Each switch port has an RGBW status LED. These are not driven directly
 * by the switch ASIC or this driver; they are controlled by a satellite
 * microcontroller, reachable over its own I2C bus, that this driver talks
 * to directly. This driver has no knowledge of the switch ASIC itself.
 *
 * The MCU's I2C application protocol is a small, undocumented binary
 * command set, recovered by reverse-engineering the MCU firmware image
 * and cross-checking against real traffic captured from the vendor
 * userspace daemon ("etherlightd"). Two commands matter here:
 *
 *   0x20 <port> <flags> <Rhi Rlo> <Ghi Glo> <Bhi Blo> <Whi Wlo> <flags2> <crc>
 *       Stages a 16-bit-per-channel RGBW color for one port into a
 *       shadow buffer. Takes effect only after the next 0x1f commit.
 *       `flags` additionally selects the display behavior for that
 *       port (static vs. one of several breathing/animation variants —
 *       see ETHERLIGHT_FLAGS_* below); this driver always requests
 *       static output, since the LED-pattern/trigger ecosystem is the
 *       right layer for animation, not this driver.
 *
 *   0x1f 0x00
 *       Commits every port's shadow buffer into its active (visible)
 *       buffer in one shot. There is no per-port commit used here.
 *
 * Both commands are sent as raw byte sequences with a trailing CRC-8
 * (poly 0x07, MSB-first, no reflection, computed over the command's
 * own data bytes — i.e. the kernel's crc8_populate_msb() table style).
 * There is no length byte on the wire, which means this protocol is
 * *not* SMBus block-data shaped; it is sent via the I2C-block-data
 * SMBus transaction subtype (i2c_smbus_write_i2c_block_data()), which
 * the kernel emulates over master_xfer-only adapters and maps to
 * native hardware support on SMBus-only controllers — either way, no
 * extra framing byte is added, matching what the MCU expects exactly.
 *
 * What this driver deliberately does NOT do
 * ------------------------------------------
 * Whether a port's LED is lit at all (as opposed to its configured
 * color) is decided by a completely separate mechanism: the switch
 * ASIC has a hardware LED-serial-scan output (clock + data) that feeds
 * live per-port link state directly into the MCU, which the MCU uses
 * to gate LED output independently of anything sent over I2C. That
 * feed is configured by the switch/DSA driver and its devicetree
 * (clock setup etc.), not by this driver. If a port's LED never lights
 * up regardless of the color commands below, check that configuration
 * first — it is a prerequisite for any of this to be visible at all.
 */

#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/led-class-multicolor.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>

#define ETHERLIGHT_CMD_SET_COLOR	0x20
#define ETHERLIGHT_CMD_COMMIT		0x1f

/*
 * Effect values for the flags byte, found by sweeping it on a USW Pro Max
 * 16 PoE (MCU fw 1.19.4):
 *   0x00  breathing, Ubiquiti's name: ports fade out of phase and the
 *         brightness drifts across the panel
 *   0x01  all lit ports pulse dim-bright-dim in sync, 1 s period
 *   0x02  same, 2 s period
 *   0x03  same, 3 s period
 *   0x08  static
 * 0x04-0x07 and 0x09-0x0f look static, and any of the upper four bits turns
 * the LED off. Activity blinking from the switch ASIC is overlaid in every
 * lit mode.
 */
#define ETHERLIGHT_FLAGS_BREATHING	0x00
#define ETHERLIGHT_FLAGS_PULSE_1S	0x01
#define ETHERLIGHT_FLAGS_PULSE_2S	0x02
#define ETHERLIGHT_FLAGS_PULSE_3S	0x03
#define ETHERLIGHT_FLAGS_STATIC		0x08

#define ETHERLIGHT_MAX_PORTS		26
#define ETHERLIGHT_NUM_COLORS		4	/* R, G, B, W */

#define ETHERLIGHT_CRC8_POLY		0x07

struct etherlight_port {
	struct etherlight *priv;
	struct led_classdev_mc mc_cdev;
	struct mc_subled subled[ETHERLIGHT_NUM_COLORS];
	u8 port;
	u8 flags;
};

struct etherlight {
	struct i2c_client *client;
	struct mutex lock;              /* serializes the set-color + commit sequence */
	unsigned int num_ports;
	struct etherlight_port ports[];
};

DECLARE_CRC8_TABLE(etherlight_crc8_table);

static int etherlight_write_color(struct etherlight *priv, u8 port, u8 flags,
				  u16 r, u16 g, u16 b, u16 w)
{
	u8 buf[12];

	buf[0] = port;
	buf[1] = flags;
	buf[2] = r >> 8;
	buf[3] = r & 0xff;
	buf[4] = g >> 8;
	buf[5] = g & 0xff;
	buf[6] = b >> 8;
	buf[7] = b & 0xff;
	buf[8] = w >> 8;
	buf[9] = w & 0xff;
	buf[10] = 0x00; /* flags2 */
	buf[11] = crc8(etherlight_crc8_table, buf, 11, 0);

	return i2c_smbus_write_i2c_block_data(priv->client, ETHERLIGHT_CMD_SET_COLOR,
					      sizeof(buf), buf);
}

static int etherlight_commit(struct etherlight *priv)
{
	static const u8 buf[1] = { 0x00 };

	return i2c_smbus_write_i2c_block_data(priv->client, ETHERLIGHT_CMD_COMMIT,
					      sizeof(buf), buf);
}

static inline u16 etherlight_scale16(u8 val)
{
	/* expand 0..255 to 0..65535 by replicating into both bytes:
	 * 0 -> 0x0000, 255 -> 0xffff, matching the verified-good 0x20 protocol
	 */
	return val | ((u16)val << 8);
}

static int etherlight_brightness_set(struct led_classdev *cdev,
				     enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(cdev);
	struct etherlight_port *port = container_of(mc_cdev, struct etherlight_port, mc_cdev);
	struct etherlight *priv = port->priv;
	int ret;

	led_mc_calc_color_components(mc_cdev, brightness);
	mutex_lock(&priv->lock);

	ret = etherlight_write_color(priv, port->port, port->flags,
				     etherlight_scale16(mc_cdev->subled_info[0].brightness),
				     etherlight_scale16(mc_cdev->subled_info[1].brightness),
				     etherlight_scale16(mc_cdev->subled_info[2].brightness),
				     etherlight_scale16(mc_cdev->subled_info[3].brightness));
	if (!ret)
		ret = etherlight_commit(priv);

	mutex_unlock(&priv->lock);
	return ret;
}

static const struct {
	const char *name;
	u8 flags;
} etherlight_effects[] = {
	{ "static",	ETHERLIGHT_FLAGS_STATIC },
	{ "breathing",	ETHERLIGHT_FLAGS_BREATHING },
	{ "pulse-1s",	ETHERLIGHT_FLAGS_PULSE_1S },
	{ "pulse-2s",	ETHERLIGHT_FLAGS_PULSE_2S },
	{ "pulse-3s",	ETHERLIGHT_FLAGS_PULSE_3S },
};

static struct etherlight_port *etherlight_dev_to_port(struct device *dev)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);

	return container_of(lcdev_to_mccdev(cdev), struct etherlight_port, mc_cdev);
}

static ssize_t effect_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct etherlight_port *port = etherlight_dev_to_port(dev);
	int len = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(etherlight_effects); i++)
		len += sysfs_emit_at(buf, len,
				     etherlight_effects[i].flags == port->flags ?
				     "[%s] " : "%s ", etherlight_effects[i].name);
	buf[len - 1] = '\n';

	return len;
}

static ssize_t effect_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t size)
{
	struct etherlight_port *port = etherlight_dev_to_port(dev);
	struct led_classdev *cdev = &port->mc_cdev.led_cdev;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(etherlight_effects); i++)
		if (sysfs_streq(buf, etherlight_effects[i].name))
			break;
	if (i == ARRAY_SIZE(etherlight_effects))
		return -EINVAL;

	mutex_lock(&cdev->led_access);
	port->flags = etherlight_effects[i].flags;
	ret = etherlight_brightness_set(cdev, cdev->brightness);
	mutex_unlock(&cdev->led_access);

	return ret ? ret : size;
}
static DEVICE_ATTR_RW(effect);

static struct attribute *etherlight_port_attrs[] = {
	&dev_attr_effect.attr,
	NULL
};

/* added after registration, the multicolor core overwrites led_cdev->groups */
static const struct attribute_group etherlight_port_group = {
	.attrs = etherlight_port_attrs,
};

static int etherlight_stop_welcome(struct etherlight *priv)
{
	static const u8 buf[1] = { 0x01 };

	return i2c_smbus_write_i2c_block_data(priv->client, 0x02, sizeof(buf), buf);
}

static int etherlight_read_fw_version(struct etherlight *priv, u8 ver[3])
{
	return i2c_smbus_read_i2c_block_data(priv->client, 0x70, 3, ver);
}

static int etherlight_wait_ready(struct etherlight *priv)
{
	int ret, tries = 15;
	u8 val;

	/*
	 * The MCU needs ~4s time to transition from welcome mode to normal
	 * operation. Within this time, any color setting commands are silently
	 * ignored. Transition is done when register 0x3 has value 0x0.
	 */

	do {
		msleep(500);
		ret = i2c_smbus_read_i2c_block_data(priv->client, 0x03, 1, &val);
		if (ret == 1 && val == 0x00)
			return 0;
	} while (--tries);

	return -ETIMEDOUT;
}

static int etherlight_reset_mcu(struct etherlight *priv)
{
	struct gpio_desc *reset_gpio, *boot_mode_gpio;
	int ret = 0;

	reset_gpio = gpiod_get_optional(&priv->client->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset_gpio))
		return dev_err_probe(&priv->client->dev, PTR_ERR(reset_gpio),
				     "failed to get reset GPIO\n");
	if (!reset_gpio)
		return 0;

	boot_mode_gpio = gpiod_get_optional(&priv->client->dev, "boot-mode", GPIOD_OUT_HIGH);
	if (IS_ERR(boot_mode_gpio)) {
		ret = dev_err_probe(&priv->client->dev, PTR_ERR(boot_mode_gpio),
				    "failed to get boot-mode GPIO\n");
		goto put_reset;
	}

	/*
	 * Both lines are only needed transiently, to bring the MCU out of
	 * reset into a known application-firmware state.
	 *
	 * Pulse width and post-reset boot delay are conservative guesses,
	 * not measured minimums -- tighten if you get hard numbers.
	 */
	gpiod_set_value_cansleep(reset_gpio, 1);
	msleep(10);
	gpiod_set_value_cansleep(reset_gpio, 0);
	msleep(1000);

	gpiod_put(boot_mode_gpio);
put_reset:
	gpiod_put(reset_gpio);
	return ret;
}

static int etherlight_probe_port(struct etherlight *priv, struct device_node *np,
				 struct etherlight_port *port)
{
	struct led_init_data init_data = { .fwnode = of_fwnode_handle(np) };
	int ret, tries = 5;
	u32 reg;

	ret = of_property_read_u32(np, "reg", &reg);
	if (ret)
		return ret;
	if (reg >= priv->num_ports)
		return -EINVAL;

	port->priv = priv;
	port->port = reg;
	port->flags = ETHERLIGHT_FLAGS_STATIC;

	/*
	 * establish a known (off) baseline for this port; the actual commit
	 * covering every port happens once, after the whole probe loop
	 */
	do {
		ret = etherlight_write_color(priv, port->port, ETHERLIGHT_FLAGS_STATIC,
					     0, 0, 0, 0);
		if (!ret)
			break;
		msleep(50);
	} while (--tries);

	if (ret) {
		dev_err_probe(&priv->client->dev, ret, "port %d: init write failed\n", port->port);
		return ret;
	}

	port->subled[0].color_index = LED_COLOR_ID_RED;
	port->subled[1].color_index = LED_COLOR_ID_GREEN;
	port->subled[2].color_index = LED_COLOR_ID_BLUE;
	port->subled[3].color_index = LED_COLOR_ID_WHITE;

	port->mc_cdev.led_cdev.brightness_set_blocking = etherlight_brightness_set;
	port->mc_cdev.led_cdev.max_brightness = 255;
	port->mc_cdev.num_colors = ETHERLIGHT_NUM_COLORS;
	port->mc_cdev.subled_info = port->subled;

	ret = devm_led_classdev_multicolor_register_ext(&priv->client->dev,
							&port->mc_cdev, &init_data);
	if (ret)
		return ret;

	return device_add_group(port->mc_cdev.led_cdev.dev, &etherlight_port_group);
}

static int etherlight_probe(struct i2c_client *client)
{
	struct device_node *np = client->dev.of_node;
	unsigned int i = 0, num_ports = 0;
	struct etherlight *priv;
	u8 ver[3];
	int ret;

	num_ports = of_get_child_count(np);
	if (!num_ports || num_ports > ETHERLIGHT_MAX_PORTS)
		return -EINVAL;

	priv = devm_kzalloc(&client->dev, struct_size(priv, ports, num_ports),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->num_ports = num_ports;
	mutex_init(&priv->lock);
	crc8_populate_msb(etherlight_crc8_table, ETHERLIGHT_CRC8_POLY);

	ret = etherlight_reset_mcu(priv);
	if (ret)
		return ret;

	ret = etherlight_read_fw_version(priv, ver);
	if (ret != 3)
		dev_warn(&client->dev, "could not read MCU firmware version: %d\n", ret);

	ret = etherlight_stop_welcome(priv);
	if (ret)
		dev_warn(&client->dev, "failed to send welcome-exit command: %d\n", ret);
	
	ret = etherlight_wait_ready(priv);
	if (ret)
		return ret;

	for_each_child_of_node_scoped(np, child) {
		ret = etherlight_probe_port(priv, child, &priv->ports[i++]);
		if (ret) {
			pr_info("UBNT: port probe failed\n");
			return ret;
		}
	}
	
	ret = etherlight_commit(priv);
	if (ret) {
		dev_err_probe(&client->dev, ret, "failed to commit init settings\n");
		return ret;
	}

	dev_info(&client->dev, "MCU firmware v%u.%u.%u\n", ver[0], ver[1], ver[2]);

	i2c_set_clientdata(client, priv);
	return 0;
}

static const struct of_device_id etherlight_of_match[] = {
	{ .compatible = "ubnt,etherlighting" },
	{ }
};
MODULE_DEVICE_TABLE(of, etherlight_of_match);

static struct i2c_driver etherlight_driver = {
	.driver = {
		.name = "leds-ubnt-etherlighting",
		.of_match_table = etherlight_of_match,
	},
	.probe = etherlight_probe,
};
module_i2c_driver(etherlight_driver);

MODULE_DESCRIPTION("UBNT EtherLighting per-port RGBW LED driver");
MODULE_LICENSE("GPL");
