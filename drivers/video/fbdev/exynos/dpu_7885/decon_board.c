/*
 * Copyright (c) Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/regulator/consumer.h>
#include "../../../../pinctrl/core.h"

#include "decon_board.h"

/*
 *
0. There is a pre-defined property of the name of "decon_board".
decon_board has a phandle value that uniquely identifies the other node
containing subnodes to control gpio, regulator, delay and pinctrl.
If you want make new list, write control sequence list in dts, and call run_list function with subnode name.

1. type
There are 5 pre-defined types
- GPIO has 2 kinds of subtype: HIGH, LOW
- REGULATOR has 2 kinds of subtype: ENABLE, DISABLE
- DELAY has 3 kinds of subtype: MDELAY, MSLEEP, USLEEP
- PINCTRL has no pre-defined subtype
- TIMER has 3 kinds of subtype: START, CHECK, CLEAR

2. subinfo
- GPIO(HIGH, LOW) needs gpio name information. 1 at a time.
- REGULATOR(ENABLE, DISABLE) needs regulator name information. 1 at a time.
- DELAY(MDELAY, MSLEEP) needs delay information for duration. 1 at a time.
- DELAY(USLEEP) needs delay information. 1 at a time. or 2 at a time.
- PINCTRL needs pinctrl name information. 1 at a time.
- TIMER(START, DELAY, CLEAR) needs name which is used for identification keyword. 1 at a time.
- TIMER(START) also needs delay information. 1 at a time.

3. etc
- do not use timer for delay < 20ms
- do not use usleep for delay >= 20ms
- do not use msleep for delay < 20ms

- desc-property is for debugging message description. It's not essential.

4. example:
decon_board = <&node>;
node: node {
	compatible = "simple-bus"; <- add this when you need pinctrl to create platform_device with name 'node'

	pinctrl-names = "pin_off", "pin_on", "backlight_pin_only"; <- pinctrl position is here not in each subnode
	pinctrl-0 = <&backlight_pin_off &lcd_pin_off>;
	pinctrl-1 = <&backlight_pin_on &lcd_pin_on>;
	pinctrl-2 = <&backlight_pin_on>;

	gpio_lcd_en = <&gpf1 5 0x1>; <- gpio position is here not in each subnode

	subnode_1 {
		type =
		"regulator,enable",	"ldo1",
		"gpio,high",	"gpio_lcd_en",
		"delay,usleep",	"10000 11000",
		"delay,usleep",	"10000", <- fill automatically 2nd delay 15000 (1st delay + 1st delay >> 1)
		"pinctrl",	"pin_on",
		"delay,msleep",	"30";
	};
	subnode_2 {
		type =
		"timer,start",	"loading 300";
		desc = "keep timestamp when subnode_2 is called and we use 'loading' as identifier
	};
	subnode_3 {
		type =
		"timer,check",	"loading";
		desc = "if duration (start ~ check) < 300ms, wait. else if duration is enough, pass through. and then clear timestamp"
	};
	subnode_4 {
		type =
		"pinctrl",	"backlight_pin_only";
	};
};

run_list(dev, "subnode_1");
run_list(dev, "subnode_2"); at this time [80000.000000] <- keep timestamp under the name of 'loading'
run_list(dev, "subnode_3"); at this time [80000.290000] <- check duration subnode_2 ~ subnode_3 (0.290000 - 0.000000)
and we wait 10ms because duration between subnode_2 and subnode_3 is only 290ms not 300ms. and we clear timestap.
run_list(dev, "subnode_4"); pre-configured lcd_pin pinctrl at subnode_1 will be erased because one device has one pinctrl series at a time.

*/

/* #define CONFIG_BOARD_DEBUG */

#define DECON_BOARD_DTS_NAME	"decon_board"

#if defined(CONFIG_BOARD_DEBUG)
#define dbg_dbg(fmt, ...)		pr_debug(pr_fmt("%s: %3d: %s: " fmt), DECON_BOARD_DTS_NAME, __LINE__, __func__, ##__VA_ARGS__)
#else
#define dbg_dbg(fmt, ...)
#endif
#define dbg_info(fmt, ...)		pr_info(pr_fmt("%s: %3d: %s: " fmt), DECON_BOARD_DTS_NAME, __LINE__, __func__, ##__VA_ARGS__)
#define dbg_warn(fmt, ...)		pr_warn(pr_fmt("%s: %3d: %s: " fmt), DECON_BOARD_DTS_NAME, __LINE__, __func__, ##__VA_ARGS__)

#define STREQ(a, b)			(*(a) == *(b) && strcmp((a), (b)) == 0)
#define STRNEQ(a, b)			(strncmp((a), (b), (strlen(a))) == 0)

#define MSEC_TO_USEC(ms)		(ms * USEC_PER_MSEC)
#define USEC_TO_MSEC(us)		(us / USEC_PER_MSEC)
#define SMALL_MSECS			(20)

struct dt_node_info {
	char				*name;
	struct list_head		node;
};

struct timer_info {
	const char			*name;
	u64				start;
	u64				end;
	u64				now;
	unsigned int			delay;
};

struct action_info {
	const char			*type;
	const char			*subinfo;

	const char			*desc;

	unsigned int			idx;
	int				gpio;
	unsigned int			delay[2];
	struct regulator_bulk_data	*supply;
	struct pinctrl			*pins;
	struct pinctrl_state		*state;
	struct timer_info		*timer;
	struct list_head		node;
};

enum {
	ACTION_DUMMY,
	ACTION_GPIO_HIGH,
	ACTION_GPIO_LOW,
	ACTION_REGULATOR_ENABLE,
	ACTION_REGULATOR_DISABLE,
	ACTION_DELAY_MDELAY,
	ACTION_DELAY_MSLEEP,
	ACTION_DELAY_USLEEP,	/* usleep_range */
	ACTION_PINCTRL,
	ACTION_TIMER_START,
	ACTION_TIMER_DELAY,
	ACTION_TIMER_CLEAR,
	ACTION_MAX
};

const char *action_list[ACTION_MAX] = {
	[ACTION_GPIO_HIGH] = "gpio,high",
	"gpio,low",
	"regulator,enable",
	"regulator,disable",
	"delay,mdelay",
	"delay,msleep",
	"delay,usleep",
	"pinctrl",
	"timer,start",
	"timer,delay",
	"timer,clear"
};

static struct dt_node_info	*dt_nodes[10];

static int print_action(struct action_info *action)
{
	if (!IS_ERR_OR_NULL(action->desc))
		dbg_dbg("[%2d] %s\n", action->idx, action->desc);

	switch (action->idx) {
	case ACTION_GPIO_HIGH:
		dbg_dbg("[%2d] gpio(%d) high\n", action->idx, action->gpio);
		break;
	case ACTION_GPIO_LOW:
		dbg_dbg("[%2d] gpio(%d) low\n", action->idx, action->gpio);
		break;
	case ACTION_REGULATOR_ENABLE:
		dbg_dbg("[%2d] regulator(%s) enable\n", action->idx, action->supply->supply);
		break;
	case ACTION_REGULATOR_DISABLE:
		dbg_dbg("[%2d] regulator(%s) disable\n", action->idx, action->supply->supply);
		break;
	case ACTION_DELAY_MDELAY:
		dbg_dbg("[%2d] mdelay(%d)\n", action->idx, action->delay[0]);
		break;
	case ACTION_DELAY_MSLEEP:
		dbg_dbg("[%2d] msleep(%d)\n", action->idx, action->delay[0]);
		break;
	case ACTION_DELAY_USLEEP:
		dbg_dbg("[%2d] usleep(%d %d)\n", action->idx, action->delay[0], action->delay[1]);
		break;
	case ACTION_PINCTRL:
		dbg_dbg("[%2d] pinctrl(%s)\n", action->idx, action->state->name);
		break;
	case ACTION_TIMER_START:
		dbg_dbg("[%2d] timer,start(%s %d)\n", action->idx, action->timer->name, action->timer->delay);
		break;
	case ACTION_TIMER_DELAY:
		dbg_dbg("[%2d] timer,delay(%s %d)\n", action->idx, action->timer->name, action->timer->delay);
		break;
	case ACTION_TIMER_CLEAR:
		dbg_dbg("[%2d] timer,clear(%s %d)\n", action->idx, action->timer->name, action->timer->delay);
		break;
	default:
		dbg_info("[%2d] unknown idx\n", action->idx);
		break;
	}

	return 0;
}

static int secprintf(char *buf, size_t size, s64 nsec)
{
	struct timeval tv = ns_to_timeval(nsec);

	return scnprintf(buf, size, "%lu.%06lu", (unsigned long)tv.tv_sec, tv.tv_usec);
}

static void print_timer(struct timer_info *timer)
{
	s64 elapse, remain;
	char buf[70] = {0, };
	int len = 0;

	elapse = timer->now - timer->start;
	remain = abs(timer->end - timer->now);

	len += secprintf(buf + len, sizeof(buf) - len, timer->start);
	len += scnprintf(buf + len, sizeof(buf) - len, " - ");
	len += secprintf(buf + len, sizeof(buf) - len, timer->now);
	len += scnprintf(buf + len, sizeof(buf) - len, " = ");
	len += secprintf(buf + len, sizeof(buf) - len, elapse);
	len += scnprintf(buf + len, sizeof(buf) - len, ", remain: %s", timer->end < timer->now ? "-" : "");
	len += secprintf(buf + len, sizeof(buf) - len, remain);

	dbg_info("%s: delay: %d, %s\n", timer->name, timer->delay, buf);
}

static void dump_list(struct list_head *lh)
{
	struct action_info *action;
	unsigned int gpio = 0, regulator = 0, delay = 0, pinctrl = 0, timer = 0;

	list_for_each_entry(action, lh, node) {
		print_action(action);
	}

	list_for_each_entry(action, lh, node) {
		switch (action->idx) {
		case ACTION_GPIO_HIGH:
		case ACTION_GPIO_LOW:
			gpio++;
			break;
		case ACTION_REGULATOR_ENABLE:
		case ACTION_REGULATOR_DISABLE:
			regulator++;
			break;
		case ACTION_DELAY_MDELAY:
		case ACTION_DELAY_MSLEEP:
		case ACTION_DELAY_USLEEP:
			delay++;
			break;
		case ACTION_PINCTRL:
			pinctrl++;
			break;
		case ACTION_TIMER_START:
		case ACTION_TIMER_DELAY:
		case ACTION_TIMER_CLEAR:
			timer++;
			break;
		}
	}

	dbg_info("gpio: %d, regulator: %d, delay: %d, pinctrl: %d, timer: %d\n", gpio, regulator, delay, pinctrl, timer);
}

static struct timer_info *find_timer(const char *name)
{
	struct dt_node_info *dt_node = NULL;
	struct list_head *lh = NULL;
	struct timer_info *timer = NULL;
	struct action_info *action;
	int idx = 0;

	dbg_dbg("%s\n", name);
	while (!IS_ERR_OR_NULL(dt_nodes[idx])) {
		dt_node = dt_nodes[idx];
		lh = &dt_node->node;
		dbg_dbg("%dth dt_node name is %s\n", idx, dt_node->name);
		list_for_each_entry(action, lh, node) {
			if (STRNEQ("timer", action->type)) {
				if (action->timer && action->timer->name && STREQ(action->timer->name, name)) {
					dbg_dbg("%s is found in %s\n", action->timer->name, dt_node->name);
					return action->timer;
				}
			}
		}
		idx++;
		BUG_ON(idx == ARRAY_SIZE(dt_nodes));
	};

	dbg_info("%s is not exist, so create it\n", name);
	timer = kzalloc(sizeof(struct timer_info), GFP_KERNEL);
	timer->name = kstrdup(name, GFP_KERNEL);

	return timer;
}

static int decide_type(struct action_info *action)
{
	int i, ret = 0;
	int idx = ACTION_DUMMY;
	const char *type = action->type;

	if (type == NULL || *type == '\0')
		return ret;

	if (STRNEQ("pinctrl", type)) {
		idx = ACTION_PINCTRL;
		goto exit;
	}

	for (i = ACTION_GPIO_HIGH; i < ACTION_MAX; i++) {
		if (STRNEQ(action_list[i], type)) {
			idx = i;
			break;
		}
	}

exit:
	if (idx == ACTION_DUMMY || idx == ACTION_MAX) {
		dbg_warn("there is no valid idx for %s\n", type);
		idx = ACTION_DUMMY;
		ret = -EINVAL;
	}

	action->idx = idx;

	return ret;
}

static int decide_subinfo(struct device_node *np, struct action_info *action)
{
	int ret = 0;
	const char *subinfo = NULL;
	struct platform_device *pdev = NULL;
	char *timer_name = NULL;
	unsigned int delay = 0;

	if (!action) {
		dbg_warn("invalid action\n");
		ret = -EINVAL;
		goto exit;
	}

	subinfo = action->subinfo;

	if (!subinfo || !strlen(subinfo)) {
		dbg_warn("invalid subinfo\n");
		ret = -EINVAL;
		goto exit;
	}

	switch (action->idx) {
	case ACTION_GPIO_HIGH:
	case ACTION_GPIO_LOW:
		action->gpio = of_get_named_gpio(np->parent, subinfo, 0);
		if (!gpio_is_valid(action->gpio)) {
			dbg_warn("of_get_named_gpio fail %d %s\n", action->gpio, subinfo);
			ret = -EINVAL;
		}
		break;
	case ACTION_REGULATOR_ENABLE:
	case ACTION_REGULATOR_DISABLE:
		action->supply = kzalloc(sizeof(struct regulator_bulk_data), GFP_KERNEL);
		action->supply->supply = subinfo;
		ret = regulator_bulk_get(NULL, 1, action->supply);
		if (ret < 0)
			dbg_warn("regulator_bulk_get fail %d %s\n", ret, subinfo);
		break;
	case ACTION_DELAY_MDELAY:
	case ACTION_DELAY_MSLEEP:
		if (!isdigit(subinfo[0])) {
			dbg_warn("delay need digit parameter %s\n", subinfo);
			ret = -EINVAL;
			goto exit;
		}

		ret = kstrtouint(subinfo, 0, &action->delay[0]);
		if (ret < 0)
			dbg_warn("kstrtouint for delay fail %d %s\n", ret, subinfo);
		break;
	case ACTION_DELAY_USLEEP:
		if (!isdigit(subinfo[0])) {
			dbg_warn("delay need digit parameter %s\n", subinfo);
			ret = -EINVAL;
			goto exit;
		}

		ret = sscanf(subinfo, "%8d %8d", &action->delay[0], &action->delay[1]);
		if (ret < 0) {
			dbg_warn("sscanf for delay fail %d %s\n", ret, subinfo);
			ret = -EINVAL;
		} else if (ret < 2) {
			action->delay[1] = action->delay[0] + (action->delay[0] >> 1);
			action->delay[1] = (action->delay[0] == action->delay[1]) ? action->delay[1] + 1 : action->delay[1];
			dbg_warn("usleep need two parameters. 2nd delay is %d\n", action->delay[1]);
		} else if (ret > 2) {
			dbg_warn("usleep need only two parameters\n");
			ret = -EINVAL;
		}

		if (!action->delay[0] || !action->delay[1]) {
			dbg_warn("usleep parameter (%d %d) invalid\n", action->delay[0], action->delay[1]);
			ret = -EINVAL;
		} else if (action->delay[0] > action->delay[1]) {
			dbg_warn("usleep parameter (%d %d) invalid\n", action->delay[0], action->delay[1]);
			ret = -EINVAL;
		} else if (action->delay[0] >= MSEC_TO_USEC(SMALL_MSECS)) {
			dbg_warn("use msleep instead of usleep for (%d)us\n", action->delay[0]);
			ret = -EINVAL;
		}
		break;
	case ACTION_PINCTRL:
		pdev = of_find_device_by_node(np->parent);
		if (!pdev) {
			dbg_warn("of_find_device_by_node fail\n");
			ret = -EINVAL;
			goto exit;
		} else
			dbg_info("of_find_device_by_node %s for pinctrl %s\n", dev_name(&pdev->dev), subinfo);

		action->pins = devm_pinctrl_get(&pdev->dev);
		if (IS_ERR(action->pins)) {
			dbg_warn("devm_pinctrl_get fail\n");
			ret = -EINVAL;
		}
		action->state = pinctrl_lookup_state(action->pins, subinfo);
		if (IS_ERR(action->state)) {
			dbg_warn("pinctrl_lookup_state fail %s\n", subinfo);
			ret = -EINVAL;
		}
		break;
	case ACTION_TIMER_START:
		timer_name = kzalloc(strlen(subinfo) + 1, GFP_KERNEL);
		ret = sscanf(subinfo, "%s %8d\n", timer_name, &delay);
		if (ret != 2) {
			dbg_warn("timer start parameter invalid %d %s\n", ret, subinfo);
			ret = -EINVAL;
		} else {
			action->timer = find_timer(timer_name);
			action->timer->delay = delay;
		}

		if (action->timer->delay < SMALL_MSECS) {
			dbg_warn("use usleep instead of timer for (%d)ms\n", action->timer->delay);
			ret = -EINVAL;
		}
		kfree(timer_name);
		break;
	case ACTION_TIMER_DELAY:
	case ACTION_TIMER_CLEAR:
		action->timer = find_timer(subinfo);
		break;
	default:
		dbg_warn("idx: %d, type: %s is invalid\n", action->idx, action->type);
		ret = -EINVAL;
		break;
	}

	dbg_info("idx: %d, type: %s, subinfo: %s\n", action->idx, action->type, action->subinfo);
exit:

	return ret;
}

static int make_list(struct device *dev, struct list_head *lh, const char *name)
{
	struct device_node *np = NULL;
	struct action_info *action;
	int i, count, ret = 0;
	const char *type = NULL;
	const char *subinfo = NULL;

	np = (dev && dev->of_node) ? dev->of_node : of_find_node_with_property(NULL, DECON_BOARD_DTS_NAME);
	if (!np) {
		dbg_warn("%s property does not exist, so create dummy\n", DECON_BOARD_DTS_NAME);
		action = kzalloc(sizeof(struct action_info), GFP_KERNEL);
		list_add_tail(&action->node, lh);
		return -EINVAL;
	}

	np = of_parse_phandle(np, DECON_BOARD_DTS_NAME, 0);
	if (!np)
		dbg_warn("%s node does not exist, so create dummy\n", DECON_BOARD_DTS_NAME);

	np = of_find_node_by_name(np, name);
	if (!np) {
		dbg_warn("%s node does not exist in %s, so create dummy\n", name, DECON_BOARD_DTS_NAME);
		action = kzalloc(sizeof(struct action_info), GFP_KERNEL);
		list_add_tail(&action->node, lh);
		return -EINVAL;
	}

	count = of_property_count_strings(np, "type");

	if (count < 0 || !count || count % 2) {
		dbg_info("%s node type count %d invalid\n", name, count);
		action = kzalloc(sizeof(struct action_info), GFP_KERNEL);
		list_add_tail(&action->node, lh);
		return -EINVAL;
	}

	count /= 2;

	for (i = 0; i < count; i++) {
		of_property_read_string_index(np, "type", i * 2, &type);
		of_property_read_string_index(np, "type", i * 2 + 1, &subinfo);

		if (!lcdtype && !STRNEQ("delay", type) && !STRNEQ("timer", type)) {
			dbg_info("lcdtype is zero, so skip to add %s: %2d: %s\n", name, count, type);
			continue;
		}

		action = kzalloc(sizeof(struct action_info), GFP_KERNEL);
		action->type = type;
		action->subinfo = subinfo;

		ret = decide_type(action);
		if (ret < 0)
			break;
		ret = decide_subinfo(np, action);
		if (ret < 0)
			break;

		if (of_property_count_strings(np, "desc") == count)
			of_property_read_string_index(np, "desc", i, &action->desc);

		list_add_tail(&action->node, lh);
	}

	if (ret < 0)
		kfree(action);

	if (ret < 0)
		BUG();

	return ret;
}

static int do_list(struct list_head *lh)
{
	struct action_info *action;
	int ret = 0;
	u64 us_delta;

	list_for_each_entry(action, lh, node) {
		switch (action->idx) {
		case ACTION_GPIO_HIGH:
			ret = gpio_request_one(action->gpio, GPIOF_OUT_INIT_HIGH, NULL);
			if (ret < 0)
				dbg_warn("gpio_request_one fail %d, %d, %s\n", ret, action->gpio, action->subinfo);
			gpio_free(action->gpio);
			break;
		case ACTION_GPIO_LOW:
			ret = gpio_request_one(action->gpio, GPIOF_OUT_INIT_LOW, NULL);
			if (ret < 0)
				dbg_warn("gpio_request_one fail %d, %d, %s\n", ret, action->gpio, action->subinfo);
			gpio_free(action->gpio);
			break;
		case ACTION_REGULATOR_ENABLE:
			ret = regulator_enable(action->supply->consumer);
			if (ret < 0)
				dbg_warn("regulator_enable fail %d, %s\n", ret, action->supply->supply);
			break;
		case ACTION_REGULATOR_DISABLE:
			ret = regulator_disable(action->supply->consumer);
			if (ret < 0)
				dbg_warn("regulator_disable fail %d, %s\n", ret, action->supply->supply);
			break;
		case ACTION_DELAY_MDELAY:
			mdelay(action->delay[0]);
			break;
		case ACTION_DELAY_MSLEEP:
			msleep(action->delay[0]);
			break;
		case ACTION_DELAY_USLEEP:
			usleep_range(action->delay[0], action->delay[1]);
			break;
		case ACTION_PINCTRL:
			pinctrl_select_state(action->pins, action->state);
			break;
		case ACTION_TIMER_START:
			action->timer->start = local_clock();
			action->timer->end = action->timer->start + (action->timer->delay * NSEC_PER_MSEC);
			break;
		case ACTION_TIMER_DELAY:
			action->timer->now = local_clock();
			print_timer(action->timer);

			if (!action->timer->end)
				msleep(action->timer->delay);
			else if (action->timer->end > action->timer->now) {
				us_delta = ktime_us_delta(ns_to_ktime(action->timer->end), ns_to_ktime(action->timer->now));

				if (!us_delta || us_delta > UINT_MAX)
					break;

				if (us_delta < MSEC_TO_USEC(SMALL_MSECS))
					usleep_range(us_delta, us_delta + (us_delta >> 1));
				else
					msleep(USEC_TO_MSEC(us_delta));
			}
		case ACTION_TIMER_CLEAR:
			action->timer->end = 0;
			break;
		case ACTION_DUMMY:
			break;
		default:
			dbg_warn("unknown idx(%d)\n", action->idx);
			ret = -EINVAL;
			break;
		}
	}

	if (ret < 0)
		BUG();

	return ret;
}

static inline struct list_head *find_list(const char *name)
{
	struct dt_node_info *dt_node = NULL;
	int idx = 0;

	dbg_dbg("%s\n", name);
	while (!IS_ERR_OR_NULL(dt_nodes[idx])) {
		dt_node = dt_nodes[idx];
		dbg_dbg("%dth list name is %s\n", idx, dt_node->name);
		if (STREQ(dt_node->name, name))
			return &dt_node->node;
		idx++;
		BUG_ON(idx == ARRAY_SIZE(dt_nodes));
	};

	dbg_info("%s is not exist, so create it\n", name);
	dt_node = kzalloc(sizeof(struct dt_node_info), GFP_KERNEL);
	dt_node->name = kstrdup(name, GFP_KERNEL);
	INIT_LIST_HEAD(&dt_node->node);

	dt_nodes[idx] = dt_node;

	return &dt_node->node;
}

void run_list(struct device *dev, const char *name)
{
	struct list_head *lh = find_list(name);

	if (unlikely(list_empty(lh))) {
		dbg_info("%s is empty, so make list\n", name);
		make_list(dev, lh, name);
		dump_list(lh);
	}

	do_list(lh);
}

int of_gpio_get_active(const char *gpioname)
{
	int ret = 0, gpio = 0, gpio_level, active_level;
	struct device_node *np = NULL;
	enum of_gpio_flags flags = {0, };

	np = of_find_node_with_property(NULL, gpioname);
	if (!np) {
		dbg_info("of_find_node_with_property fail for %s\n", gpioname);
		ret = -EINVAL;
		goto exit;
	}

	dbg_dbg("%s property find in node %s\n", gpioname, np->name);

	gpio = of_get_named_gpio_flags(np, gpioname, 0, &flags);
	if (!gpio_is_valid(gpio)) {
		dbg_warn("of_get_named_gpio fail %d %s\n", gpio, gpioname);
		ret = -EINVAL;
		goto exit;
	}
	of_node_put(np);

	active_level = !(flags & OF_GPIO_ACTIVE_LOW);
	gpio_level = gpio_get_value(gpio);
	ret = (gpio_level == active_level) ? 1 : 0;
exit:
	return ret;
}

int of_gpio_get_value(const char *gpioname)
{
	int ret = 0, gpio = 0, gpio_level, active_level;
	struct device_node *np = NULL;
	enum of_gpio_flags flags = {0, };

	np = of_find_node_with_property(NULL, gpioname);
	if (!np) {
		dbg_info("of_find_node_with_property fail for %s\n", gpioname);
		ret = -EINVAL;
		goto exit;
	}

	dbg_dbg("%s property find in node %s\n", gpioname, np->name);

	gpio = of_get_named_gpio_flags(np, gpioname, 0, &flags);
	if (!gpio_is_valid(gpio)) {
		dbg_warn("of_get_named_gpio fail %d %s\n", gpio, gpioname);
		of_node_put(np);
		ret = -EINVAL;
		goto exit;
	}
	of_node_put(np);

	active_level = !(flags & OF_GPIO_ACTIVE_LOW);
	gpio_level = gpio_get_value(gpio);
	ret = gpio_level;

exit:
	return ret;
}

int of_gpio_set_value(const char *gpioname, int value)
{
	int ret = 0, gpio = 0;
	struct device_node *np = NULL;
	enum of_gpio_flags flags = {0, };

	np = of_find_node_with_property(NULL, gpioname);
	if (!np) {
		dbg_info("of_find_node_with_property fail for %s\n", gpioname);
		ret = -EINVAL;
		goto exit;
	}

	dbg_dbg("%s property find in node %s\n", gpioname, np->name);

	gpio = of_get_named_gpio_flags(np, gpioname, 0, &flags);
	if (!gpio_is_valid(gpio)) {
		dbg_warn("of_get_named_gpio fail %d %s\n", gpio, gpioname);
		of_node_put(np);
		ret = -EINVAL;
		goto exit;
	}
	of_node_put(np);

	ret = gpio_request_one(gpio, value ? GPIOF_OUT_INIT_HIGH : GPIOF_OUT_INIT_LOW, NULL);
	if (ret < 0)
		dbg_warn("gpio_request_one fail %d, %d, %s\n", ret, gpio, gpioname);
	gpio_free(gpio);
exit:
	return ret;
}

int of_get_gpio_with_name(const char *gpioname)
{
	int ret = 0, gpio = 0;
	struct device_node *np = NULL;
	enum of_gpio_flags flags = {0, };

	np = of_find_node_with_property(NULL, gpioname);
	if (!np) {
		dbg_info("of_find_node_with_property fail for %s\n", gpioname);
		ret = -EINVAL;
		goto exit;
	}

	dbg_dbg("%s property find in node %s\n", gpioname, np->name);

	gpio = of_get_named_gpio_flags(np, gpioname, 0, &flags);
	if (!gpio_is_valid(gpio)) {
		dbg_warn("of_get_named_gpio fail %d %s\n", gpio, gpioname);
		ret = -EINVAL;
		goto exit;
	}
	of_node_put(np);

	ret = gpio;
exit:
	return ret;
}

/**
 * of_update_phandle_property - update a phandle property to a device_node pointer
 * @phandle_name: to. Name of property holding a phandle value which will be updated
 * @node_name: from. Name of node which has new phandle value
 *
 * Example:
 *
 * phandle1: node1 {
 * }
 *
 * phandle2: node2 {
 * }
 *
 * node3 {
 *	phandle_name = <&phandle1>;
 * }
 *
 * To change a device_node using phandle_name like below:
 *	phandle_name = <&phandle2>;
 *
 * you may call this:
 * of_update_phandle_property("phandle_name", "node2");
 */
int of_update_phandle_property(const char *phandle_name, const char *node_name)
{
	struct device_node *parent = NULL, *node_new = NULL, *node = NULL;
	struct property *prop_org, *prop_new;
	int len = 0, ret = 0;
	__be32 *pphandle_new = NULL;
	const __be32 *pphandle_org;
	phandle phandle_org = 0;

	parent = of_find_node_with_property(NULL, phandle_name);
	if (!parent) {
		dbg_info("of_find_node_with_property fail with %s\n", phandle_name);
		ret = -EINVAL;
		goto exit;
	}

	len = of_count_phandle_with_args(parent, phandle_name, NULL);
	if (len != 1) {
		dbg_info("of_count_phandle_with_args fail, count: %d\n", len);
		ret = -EINVAL;
		goto exit;
	}

	pphandle_org = of_get_property(parent, phandle_name, &len);
	if (!pphandle_org) {
		dbg_info("of_get_property fail with %s, len(%d)\n", phandle_name, len);
		ret = -EINVAL;
		goto exit;
	}

	phandle_org = be32_to_cpup(pphandle_org);
	if (!phandle_org) {
		dbg_info("%s property has invalid phandle(%d)\n", phandle_name, phandle_org);
		ret = -EINVAL;
		goto exit;
	}

	node = of_find_node_by_phandle(phandle_org);
	if (!node) {
		dbg_info("of_find_node_by_phandle fail with %s(%d)\n", phandle_name, phandle_org);
		ret = -EINVAL;
		goto exit;
	}

	node_new = of_find_node_by_name(NULL, node_name);
	if (!node_new) {
		dbg_info("of_find_node_by_name fail with %s\n", node_name);
		ret = -EINVAL;
		goto exit;
	}

	if (!node_new->phandle) {
		dbg_info("%s node has no label for phandle\n", node_new->full_name);
		ret = -EINVAL;
		goto exit;
	}

	if (phandle_org == node_new->phandle) {
		dbg_info("phandle is same(%d, %d)\n", phandle_org, node_new->phandle);
		ret = -EINVAL;
		goto exit;
	}

	prop_org = of_find_property(parent, phandle_name, &len);

	prop_new = kzalloc(sizeof(struct property), GFP_KERNEL);
	prop_new->name = kstrdup(prop_org->name, GFP_KERNEL);
	prop_new->value = kzalloc(prop_org->length, GFP_KERNEL);
	prop_new->length = prop_org->length;

	pphandle_new = prop_new->value;
	*pphandle_new = be32_to_cpu(node_new->phandle);

	ret = of_update_property(parent, prop_new);
	if (ret) {
		dbg_info("of_update_property fail: %d\n", ret);
		kfree(prop_new->value);
		kfree(prop_new->name);
		kfree(prop_new);
		ret = -EINVAL;
		goto exit;
	}

	dbg_info("%s %s phandle is changed. %d(%s)->%d(%s)\n", of_node_full_name(parent), phandle_name,
		phandle_org, of_node_full_name(of_find_node_by_phandle(phandle_org)),
		node_new->phandle, of_node_full_name(of_parse_phandle(parent, phandle_name, 0)));

exit:
	return ret;
}

