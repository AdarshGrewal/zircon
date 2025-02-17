// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2019 MediaTek Inc.

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/videodev2.h>
#include <linux/pinctrl/consumer.h>
#include <media/v4l2-subdev.h>
// #include <zircon_lm3644.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <linux/pm_runtime.h>
#include <linux/thermal.h>

#if IS_ENABLED(CONFIG_MTK_FLASHLIGHT)
#include "flashlight-core.h"

#include <linux/power_supply.h>
#endif
#define CIT_LED_NODE 1
#ifdef CIT_LED_NODE
#include <linux/leds.h>
static char ledon_flg = 0;
static struct i2c_client *i2client;
#endif

#define ZIRCON_LM3644_NAME	"zircon_lm3644"
#define ZIRCON_LM3644_I2C_ADDR	(0x63)

/* registers definitions */
#define REG_ENABLE		0x01
#define REG_LED0_FLASH_BR	0x03
#define REG_LED1_FLASH_BR	0x04
#define REG_LED0_TORCH_BR	0x05
#define REG_LED1_TORCH_BR	0x06
#define REG_FLASH_TOUT		0x08
#define REG_FLAG1		0x0A
#define REG_FLAG2		0x0B
#define REG_ID                  0x0C

/* fault mask */
#define FAULT_TIMEOUT	(1<<0)
#define FAULT_THERMAL_SHUTDOWN	(1<<2)
#define FAULT_LED0_SHORT_CIRCUIT	(1<<5)
#define FAULT_LED1_SHORT_CIRCUIT	(1<<4)

/*  FLASH Brightness
 *	min 10900uA, step 11725uA, max 1500000uA
 */
#define ZIRCON_LM3644_FLASH_BRT_MIN 10900
#define ZIRCON_LM3644_FLASH_BRT_STEP 11725
#define ZIRCON_LM3644_FLASH_BRT_MAX 1500000
#define ZIRCON_LM3644_FLASH_BRT_uA_TO_REG(a)	\
	((a) < ZIRCON_LM3644_FLASH_BRT_MIN ? 0 :	\
	 (((a) - ZIRCON_LM3644_FLASH_BRT_MIN) / ZIRCON_LM3644_FLASH_BRT_STEP))
#define ZIRCON_LM3644_FLASH_BRT_REG_TO_uA(a)		\
	((a) * ZIRCON_LM3644_FLASH_BRT_STEP + ZIRCON_LM3644_FLASH_BRT_MIN)

/*  FLASH TIMEOUT DURATION
 *	min 32ms, step 32ms, max 1024ms
 */
#define ZIRCON_LM3644_FLASH_TOUT_MIN 200
#define ZIRCON_LM3644_FLASH_TOUT_STEP 200
#define ZIRCON_LM3644_FLASH_TOUT_MAX 1600

/*  TORCH BRT
 *	min 1954uA, step 2800uA, max 357554uA
 */
#define ZIRCON_LM3644_TORCH_BRT_MIN 977
#define ZIRCON_LM3644_TORCH_BRT_STEP 1400
#define ZIRCON_LM3644_TORCH_BRT_MAX 179000
#define ZIRCON_LM3644_TORCH_BRT_uA_TO_REG(a)	\
	((a) < ZIRCON_LM3644_TORCH_BRT_MIN ? 0 :	\
	 (((a) - ZIRCON_LM3644_TORCH_BRT_MIN) / ZIRCON_LM3644_TORCH_BRT_STEP))
#define ZIRCON_LM3644_TORCH_BRT_REG_TO_uA(a)		\
	((a) * ZIRCON_LM3644_TORCH_BRT_STEP + ZIRCON_LM3644_TORCH_BRT_MIN)

#define ZIRCON_LM3644_COOLER_MAX_STATE 5
static const int flash_state_to_current_limit[ZIRCON_LM3644_COOLER_MAX_STATE] = {
	200000, 150000, 100000, 50000, 25000
};

enum zircon_lm3644_led_id {
	ZIRCON_LM3644_LED0 = 0,
	ZIRCON_LM3644_LED1,
	ZIRCON_LM3644_LED_MAX
};

/* struct zircon_lm3644_platform_data
 *
 * @max_flash_timeout: flash timeout
 * @max_flash_brt: flash mode led brightness
 * @max_torch_brt: torch mode led brightness
 */
struct zircon_lm3644_platform_data {
	u32 max_flash_timeout;
	u32 max_flash_brt[ZIRCON_LM3644_LED_MAX];
	u32 max_torch_brt[ZIRCON_LM3644_LED_MAX];
};


enum led_enable {
	MODE_SHDN = 0x0,
	MODE_TORCH = 0x08,
	MODE_FLASH = 0x0C,
};

/**
 * struct zircon_lm3644_flash
 *
 * @dev: pointer to &struct device
 * @pdata: platform data
 * @regmap: reg. map for i2c
 * @lock: muxtex for serial access.
 * @led_mode: V4L2 LED mode
 * @ctrls_led: V4L2 controls
 * @subdev_led: V4L2 subdev
 */
struct zircon_lm3644_flash {
	struct device *dev;
	struct zircon_lm3644_platform_data *pdata;
	struct regmap *regmap;
	struct mutex lock;

	enum v4l2_flash_led_mode led_mode;
	struct v4l2_ctrl_handler ctrls_led[ZIRCON_LM3644_LED_MAX];
	struct v4l2_subdev subdev_led[ZIRCON_LM3644_LED_MAX];
	struct device_node *dnode[ZIRCON_LM3644_LED_MAX];
	struct pinctrl *zircon_lm3644_hwen_pinctrl;
	struct pinctrl_state *zircon_lm3644_hwen_high;
	struct pinctrl_state *zircon_lm3644_hwen_low;
#if IS_ENABLED(CONFIG_MTK_FLASHLIGHT)
	struct flashlight_device_id flash_dev_id[ZIRCON_LM3644_LED_MAX];
#endif
	struct thermal_cooling_device *cdev;
	int need_cooler;
	unsigned long max_state;
	unsigned long target_state;
	unsigned long target_current[ZIRCON_LM3644_LED_MAX];
	unsigned long ori_current[ZIRCON_LM3644_LED_MAX];
};

/* define usage count */
static int use_count;

static struct zircon_lm3644_flash *zircon_lm3644_flash_data;

#define to_zircon_lm3644_flash(_ctrl, _no)	\
	container_of(_ctrl->handler, struct zircon_lm3644_flash, ctrls_led[_no])

/* define pinctrl */
#define ZIRCON_LM3644_PINCTRL_PIN_HWEN 0
#define ZIRCON_LM3644_PINCTRL_PINSTATE_LOW 0
#define ZIRCON_LM3644_PINCTRL_PINSTATE_HIGH 1
#define ZIRCON_LM3644_PINCTRL_STATE_HWEN_HIGH "lm3644_en_active"
#define ZIRCON_LM3644_PINCTRL_STATE_HWEN_LOW  "lm3644_en_deactive"
/******************************************************************************
 * Pinctrl configuration
 *****************************************************************************/
static int zircon_lm3644_pinctrl_init(struct zircon_lm3644_flash *flash)
{
	int ret = 0;

	/* get pinctrl */
	flash->zircon_lm3644_hwen_pinctrl = devm_pinctrl_get(flash->dev);
	if (IS_ERR(flash->zircon_lm3644_hwen_pinctrl)) {
		pr_info("Failed to get flashlight pinctrl.\n");
		ret = PTR_ERR(flash->zircon_lm3644_hwen_pinctrl);
		return ret;
	}

	/* Flashlight HWEN pin initialization */
	flash->zircon_lm3644_hwen_high = pinctrl_lookup_state(
			flash->zircon_lm3644_hwen_pinctrl,
			ZIRCON_LM3644_PINCTRL_STATE_HWEN_HIGH);
	if (IS_ERR(flash->zircon_lm3644_hwen_high)) {
		pr_info("Failed to init (%s)\n",
			ZIRCON_LM3644_PINCTRL_STATE_HWEN_HIGH);
		ret = PTR_ERR(flash->zircon_lm3644_hwen_high);
	}
	flash->zircon_lm3644_hwen_low = pinctrl_lookup_state(
			flash->zircon_lm3644_hwen_pinctrl,
			ZIRCON_LM3644_PINCTRL_STATE_HWEN_LOW);
	if (IS_ERR(flash->zircon_lm3644_hwen_low)) {
		ret = PTR_ERR(flash->zircon_lm3644_hwen_low);
	}

	return ret;
}

static int zircon_lm3644_pinctrl_set(struct zircon_lm3644_flash *flash, int pin, int state)
{
	int ret = 0;

	if (IS_ERR(flash->zircon_lm3644_hwen_pinctrl)) {
		pr_info("pinctrl is not available\n");
		return -1;
	}

	switch (pin) {
	case ZIRCON_LM3644_PINCTRL_PIN_HWEN:
		if (state == ZIRCON_LM3644_PINCTRL_PINSTATE_LOW &&
				!IS_ERR(flash->zircon_lm3644_hwen_low))
			pinctrl_select_state(flash->zircon_lm3644_hwen_pinctrl,
					flash->zircon_lm3644_hwen_low);
		else if (state == ZIRCON_LM3644_PINCTRL_PINSTATE_HIGH &&
				!IS_ERR(flash->zircon_lm3644_hwen_high))
			pinctrl_select_state(flash->zircon_lm3644_hwen_pinctrl,
					flash->zircon_lm3644_hwen_high);
		else
			pr_info("set err, pin(%d) state(%d)\n", pin, state);
		break;
	default:
		pr_info("set err, pin(%d) state(%d)\n", pin, state);
		break;
	}

	return ret;
}

/* enable mode control */
static int zircon_lm3644_mode_ctrl(struct zircon_lm3644_flash *flash)
{
	int rval = -EINVAL;

	pr_info_ratelimited("%s mode:%d", __func__, flash->led_mode);
	switch (flash->led_mode) {
	case V4L2_FLASH_LED_MODE_NONE:
		rval = regmap_update_bits(flash->regmap,
					  REG_ENABLE, 0x0C, MODE_SHDN);
		break;
	case V4L2_FLASH_LED_MODE_TORCH:
		rval = regmap_update_bits(flash->regmap,
					  REG_ENABLE, 0x0C, MODE_TORCH);
		break;
	case V4L2_FLASH_LED_MODE_FLASH:
		rval = regmap_update_bits(flash->regmap,
					  REG_ENABLE, 0x0C, MODE_FLASH);
		break;
	}
	return rval;
}

/* led1/2 enable/disable */
static int zircon_lm3644_enable_ctrl(struct zircon_lm3644_flash *flash,
			      enum zircon_lm3644_led_id led_no, bool on)
{
	int rval;

	if (led_no < 0 || led_no >= ZIRCON_LM3644_LED_MAX) {
		pr_info("led_no error\n");
		return -1;
	}
	pr_info_ratelimited("%s led:%d enable:%d", __func__, led_no, on);

	flashlight_kicker_pbm(on);
	if (flashlight_pt_is_low()) {
		pr_info_ratelimited("pt is low\n");
		return 0;
	}

	if (on){
		rval = regmap_update_bits(flash->regmap,
					  REG_ENABLE, 0x01, 0x01);
		rval = regmap_update_bits(flash->regmap,
					  REG_ENABLE, 0x02, 0x02);
	}else{
		rval = regmap_update_bits(flash->regmap,
					  REG_ENABLE, 0x01, 0x00);
		rval = regmap_update_bits(flash->regmap,
					  REG_ENABLE, 0x02, 0x00);
	}
	return rval;
}

/* torch1/2 brightness control */
static int zircon_lm3644_torch_brt_ctrl(struct zircon_lm3644_flash *flash,
				 enum zircon_lm3644_led_id led_no, unsigned int brt)
{
	int rval;
	u8 br_bits;

	if (led_no < 0 || led_no >= ZIRCON_LM3644_LED_MAX) {
		pr_info("led_no error\n");
		return -1;
	}
	pr_info_ratelimited("%s %d brt:%u\n", __func__, led_no, brt);
	if (brt < ZIRCON_LM3644_TORCH_BRT_MIN)
		return zircon_lm3644_enable_ctrl(flash, led_no, false);

	if (flash->need_cooler == 0) {
		flash->ori_current[led_no] = brt;
	} else {
		if (brt > flash->target_current[led_no]) {
			brt = flash->target_current[led_no];
			pr_info("thermal limit current:%d\n", brt);
		}
	}

	br_bits = ZIRCON_LM3644_TORCH_BRT_uA_TO_REG(brt);

	rval = regmap_update_bits(flash->regmap,
					  REG_LED0_TORCH_BR, 0x7f, br_bits);
	rval = regmap_update_bits(flash->regmap,
					  REG_LED1_TORCH_BR, 0x7f, br_bits);

	return rval;
}

/* torch1/2 brightness control */
static int zircon_cit_lm3644_torch_brt_ctrl(struct zircon_lm3644_flash *flash,
				 enum zircon_lm3644_led_id led_no, unsigned int brt)
{
	int rval;
	u8 br_bits;

	if (led_no < 0 || led_no >= ZIRCON_LM3644_LED_MAX) {
		pr_info("led_no error\n");
		return -1;
	}
	pr_info_ratelimited("%s %d brt:%u\n", __func__, led_no, brt);
	if (brt < ZIRCON_LM3644_TORCH_BRT_MIN)
		return zircon_lm3644_enable_ctrl(flash, led_no, false);

	if (flash->need_cooler == 0) {
		flash->ori_current[led_no] = brt;
	} else {
		if (brt > flash->target_current[led_no]) {
			brt = flash->target_current[led_no];
			pr_info("thermal limit current:%d\n", brt);
		}
	}

	br_bits = ZIRCON_LM3644_TORCH_BRT_uA_TO_REG(brt);
	if (led_no == ZIRCON_LM3644_LED0)
		rval = regmap_update_bits(flash->regmap,
					  REG_LED0_TORCH_BR, 0x7f, br_bits);
	else
		rval = regmap_update_bits(flash->regmap,
					  REG_LED1_TORCH_BR, 0x7f, br_bits);

	return rval;
}

/* led1/2 enable/disable */
static int zircon_cit_lm3644_enable_ctrl(struct zircon_lm3644_flash *flash,
			      enum zircon_lm3644_led_id led_no, bool on)
{
	int rval;

	if (led_no < 0 || led_no >= ZIRCON_LM3644_LED_MAX) {
		pr_info("led_no error\n");
		return -1;
	}
	pr_info_ratelimited("%s led:%d enable:%d", __func__, led_no, on);

	flashlight_kicker_pbm(on);
	if (flashlight_pt_is_low()) {
		pr_info_ratelimited("pt is low\n");
		return 0;
	}

	if (led_no == ZIRCON_LM3644_LED0) {
		if (on)
			rval = regmap_update_bits(flash->regmap,
						  REG_ENABLE, 0x01, 0x01);
		else
			rval = regmap_update_bits(flash->regmap,
						  REG_ENABLE, 0x01, 0x00);
	} else {
		if (on)
			rval = regmap_update_bits(flash->regmap,
						  REG_ENABLE, 0x02, 0x02);
		else
			rval = regmap_update_bits(flash->regmap,
						  REG_ENABLE, 0x02, 0x00);
	}
	return rval;
}



/* flash1/2 brightness control */
static int zircon_lm3644_flash_brt_ctrl(struct zircon_lm3644_flash *flash,
				 enum zircon_lm3644_led_id led_no, unsigned int brt)
{
	int rval;
	u8 br_bits;

	if (led_no < 0 || led_no >= ZIRCON_LM3644_LED_MAX) {
		pr_info("led_no error\n");
		return -1;
	}
	pr_info("%s %d brt:%u", __func__, led_no, brt);
	if (brt < ZIRCON_LM3644_FLASH_BRT_MIN)
		return zircon_lm3644_enable_ctrl(flash, led_no, false);

	if (flash->need_cooler == 1 && brt > flash->target_current[led_no]) {
		brt = flash->target_current[led_no];
		pr_info("thermal limit current:%d\n", brt);
	}

	br_bits = ZIRCON_LM3644_FLASH_BRT_uA_TO_REG(brt);
	rval = regmap_update_bits(flash->regmap,
					  REG_LED0_FLASH_BR, 0x7f, br_bits);
	rval = regmap_update_bits(flash->regmap,
					  REG_LED1_FLASH_BR, 0x7f, br_bits);

	return rval;
}

/* flash1/2 timeout control */
static int zircon_lm3644_flash_tout_ctrl(struct zircon_lm3644_flash *flash,
				unsigned int tout)
{
	int rval;
	u8 tout_bits;

	pr_info("%s timeout:%u", __func__, tout);
	if (tout == 200)
		tout_bits = 0x04;
	else
		tout_bits = 0x07 + (tout / ZIRCON_LM3644_FLASH_TOUT_STEP);

	rval = regmap_update_bits(flash->regmap,
				  REG_FLASH_TOUT, 0x1f, tout_bits);

	return rval;
}

/* v4l2 controls  */
static int zircon_lm3644_get_ctrl(struct v4l2_ctrl *ctrl, enum zircon_lm3644_led_id led_no)
{
	struct zircon_lm3644_flash *flash = to_zircon_lm3644_flash(ctrl, led_no);
	int rval = -EINVAL;

	mutex_lock(&flash->lock);

	if (ctrl->id == V4L2_CID_FLASH_FAULT) {
		s32 fault = 0;
		unsigned int reg_val = 0;

		rval = regmap_read(flash->regmap, REG_FLAG1, &reg_val);
		if (rval < 0)
			goto out;
		if (reg_val & FAULT_LED0_SHORT_CIRCUIT)
			fault |= V4L2_FLASH_FAULT_SHORT_CIRCUIT;
		if (reg_val & FAULT_LED1_SHORT_CIRCUIT)
			fault |= V4L2_FLASH_FAULT_SHORT_CIRCUIT;
		if (reg_val & FAULT_THERMAL_SHUTDOWN)
			fault |= V4L2_FLASH_FAULT_OVER_TEMPERATURE;
		if (reg_val & FAULT_TIMEOUT)
			fault |= V4L2_FLASH_FAULT_TIMEOUT;
		ctrl->cur.val = fault;
	}

out:
	mutex_unlock(&flash->lock);
	return rval;
}

static int zircon_lm3644_set_ctrl(struct v4l2_ctrl *ctrl, enum zircon_lm3644_led_id led_no)
{
	struct zircon_lm3644_flash *flash = to_zircon_lm3644_flash(ctrl, led_no);
	int rval = -EINVAL;

	pr_info("%s led:%d ID:%d", __func__, led_no, ctrl->id);
	mutex_lock(&flash->lock);

	switch (ctrl->id) {
	case V4L2_CID_FLASH_LED_MODE:
		flash->led_mode = ctrl->val;
		if (flash->led_mode != V4L2_FLASH_LED_MODE_FLASH)
			rval = zircon_lm3644_mode_ctrl(flash);
		else
			rval = 0;
		if (flash->led_mode == V4L2_FLASH_LED_MODE_NONE)
			zircon_lm3644_enable_ctrl(flash, led_no, false);
		else if (flash->led_mode == V4L2_FLASH_LED_MODE_TORCH)
			rval = zircon_lm3644_enable_ctrl(flash, led_no, true);
		break;

	case V4L2_CID_FLASH_STROBE_SOURCE:
		if (ctrl->val == V4L2_FLASH_STROBE_SOURCE_SOFTWARE) {
			pr_info("sw ctrl\n");
			rval = regmap_update_bits(flash->regmap,
					REG_ENABLE, 0x2C, 0x00);
		} else if (ctrl->val == V4L2_FLASH_STROBE_SOURCE_EXTERNAL) {
			pr_info("hw trigger\n");
			rval = regmap_update_bits(flash->regmap,
					REG_ENABLE, 0x2C, 0x24);
			rval = zircon_lm3644_enable_ctrl(flash, led_no, true);
		}
		if (rval < 0)
			goto err_out;
		break;

	case V4L2_CID_FLASH_STROBE:
		if (flash->led_mode != V4L2_FLASH_LED_MODE_FLASH) {
			rval = -EBUSY;
			goto err_out;
		}
		flash->led_mode = V4L2_FLASH_LED_MODE_FLASH;
		rval = zircon_lm3644_mode_ctrl(flash);
		rval = zircon_lm3644_enable_ctrl(flash, led_no, true);
		break;

	case V4L2_CID_FLASH_STROBE_STOP:
		if (flash->led_mode != V4L2_FLASH_LED_MODE_FLASH) {
			rval = -EBUSY;
			goto err_out;
		}
		zircon_lm3644_enable_ctrl(flash, led_no, false);
		flash->led_mode = V4L2_FLASH_LED_MODE_NONE;
		rval = zircon_lm3644_mode_ctrl(flash);
		break;

	case V4L2_CID_FLASH_TIMEOUT:
		rval = zircon_lm3644_flash_tout_ctrl(flash, ctrl->val);
		break;

	case V4L2_CID_FLASH_INTENSITY:
		rval = zircon_lm3644_flash_brt_ctrl(flash, led_no, ctrl->val);
		break;

	case V4L2_CID_FLASH_TORCH_INTENSITY:
		rval = zircon_lm3644_torch_brt_ctrl(flash, led_no, ctrl->val);
		break;
	}

err_out:
	mutex_unlock(&flash->lock);
	return rval;
}

static int zircon_lm3644_led1_get_ctrl(struct v4l2_ctrl *ctrl)
{
	return zircon_lm3644_get_ctrl(ctrl, ZIRCON_LM3644_LED1);
}

static int zircon_lm3644_led1_set_ctrl(struct v4l2_ctrl *ctrl)
{
	return zircon_lm3644_set_ctrl(ctrl, ZIRCON_LM3644_LED1);
}

static int zircon_lm3644_led0_get_ctrl(struct v4l2_ctrl *ctrl)
{
	return zircon_lm3644_get_ctrl(ctrl, ZIRCON_LM3644_LED0);
}

static int zircon_lm3644_led0_set_ctrl(struct v4l2_ctrl *ctrl)
{
	return zircon_lm3644_set_ctrl(ctrl, ZIRCON_LM3644_LED0);
}

static const struct v4l2_ctrl_ops zircon_lm3644_led_ctrl_ops[ZIRCON_LM3644_LED_MAX] = {
	[ZIRCON_LM3644_LED0] = {
			.g_volatile_ctrl = zircon_lm3644_led0_get_ctrl,
			.s_ctrl = zircon_lm3644_led0_set_ctrl,
			},
	[ZIRCON_LM3644_LED1] = {
			.g_volatile_ctrl = zircon_lm3644_led1_get_ctrl,
			.s_ctrl = zircon_lm3644_led1_set_ctrl,
			}
};

static int zircon_lm3644_init_controls(struct zircon_lm3644_flash *flash,
				enum zircon_lm3644_led_id led_no)
{
	struct v4l2_ctrl *fault;
	u32 max_flash_brt = flash->pdata->max_flash_brt[led_no];
	u32 max_torch_brt = flash->pdata->max_torch_brt[led_no];
	struct v4l2_ctrl_handler *hdl = &flash->ctrls_led[led_no];
	const struct v4l2_ctrl_ops *ops = &zircon_lm3644_led_ctrl_ops[led_no];

	v4l2_ctrl_handler_init(hdl, 8);

	/* flash mode */
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_FLASH_LED_MODE,
			       V4L2_FLASH_LED_MODE_TORCH, ~0x7,
			       V4L2_FLASH_LED_MODE_NONE);
	flash->led_mode = V4L2_FLASH_LED_MODE_NONE;

	/* flash source */
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_FLASH_STROBE_SOURCE,
			       0x1, ~0x3, V4L2_FLASH_STROBE_SOURCE_SOFTWARE);

	/* flash strobe */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_STROBE, 0, 0, 0, 0);

	/* flash strobe stop */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_STROBE_STOP, 0, 0, 0, 0);

	/* flash strobe timeout */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_TIMEOUT,
			  ZIRCON_LM3644_FLASH_TOUT_MIN,
			  flash->pdata->max_flash_timeout,
			  ZIRCON_LM3644_FLASH_TOUT_STEP,
			  flash->pdata->max_flash_timeout);

	/* flash brt */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_INTENSITY,
			  ZIRCON_LM3644_FLASH_BRT_MIN, max_flash_brt,
			  ZIRCON_LM3644_FLASH_BRT_STEP, max_flash_brt);

	/* torch brt */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_TORCH_INTENSITY,
			  ZIRCON_LM3644_TORCH_BRT_MIN, max_torch_brt,
			  ZIRCON_LM3644_TORCH_BRT_STEP, max_torch_brt);

	/* fault */
	fault = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_FAULT, 0,
				  V4L2_FLASH_FAULT_OVER_VOLTAGE
				  | V4L2_FLASH_FAULT_OVER_TEMPERATURE
				  | V4L2_FLASH_FAULT_SHORT_CIRCUIT
				  | V4L2_FLASH_FAULT_TIMEOUT, 0, 0);
	if (fault != NULL)
		fault->flags |= V4L2_CTRL_FLAG_VOLATILE;

	if (hdl->error)
		return hdl->error;

	if (led_no < 0 || led_no >= ZIRCON_LM3644_LED_MAX) {
		pr_info("led_no error\n");
		return -1;
	}

	flash->subdev_led[led_no].ctrl_handler = hdl;
	return 0;
}

/* initialize device */
static const struct v4l2_subdev_ops zircon_lm3644_ops = {
	.core = NULL,
};

static const struct regmap_config zircon_lm3644_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xFF,
};

static void zircon_lm3644_v4l2_i2c_subdev_init(struct v4l2_subdev *sd,
		struct i2c_client *client,
		const struct v4l2_subdev_ops *ops)
{
	int ret = 0;

	v4l2_subdev_init(sd, ops);
	sd->flags |= V4L2_SUBDEV_FL_IS_I2C;
	/* the owner is the same as the i2c_client's driver owner */
	sd->owner = client->dev.driver->owner;
	sd->dev = &client->dev;
	/* i2c_client and v4l2_subdev point to one another */
	v4l2_set_subdevdata(sd, client);
	i2c_set_clientdata(client, sd);
	/* initialize name */
	ret = snprintf(sd->name, sizeof(sd->name), "%s %d-%04x",
		client->dev.driver->name, i2c_adapter_id(client->adapter),
		client->addr);
	if (ret < 0)
		pr_info("snprintf failed\n");
}

static int zircon_lm3644_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	int ret;

	ret = pm_runtime_get_sync(sd->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(sd->dev);
		return ret;
	}

	return 0;
}

static int zircon_lm3644_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(sd->dev);

	return 0;
}

static const struct v4l2_subdev_internal_ops zircon_lm3644_int_ops = {
	.open = zircon_lm3644_open,
	.close = zircon_lm3644_close,
};

static int zircon_lm3644_subdev_init(struct zircon_lm3644_flash *flash,
			      enum zircon_lm3644_led_id led_no, char *led_name)
{
	struct i2c_client *client = to_i2c_client(flash->dev);
	struct device_node *np = flash->dev->of_node, *child;
	const char *fled_name = "flash";
	int rval;

	// pr_info("%s %d", __func__, led_no);
	if (led_no < 0 || led_no >= ZIRCON_LM3644_LED_MAX) {
		pr_info("led_no error\n");
		return -1;
	}

	zircon_lm3644_v4l2_i2c_subdev_init(&flash->subdev_led[led_no],
				client, &zircon_lm3644_ops);
	flash->subdev_led[led_no].flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	flash->subdev_led[led_no].internal_ops = &zircon_lm3644_int_ops;
	strscpy(flash->subdev_led[led_no].name, led_name,
		sizeof(flash->subdev_led[led_no].name));

	for (child = of_get_child_by_name(np, fled_name); child;
			child = of_find_node_by_name(child, fled_name)) {
		int rv;
		u32 reg = 0;

		rv = of_property_read_u32(child, "reg", &reg);
		if (rv)
			continue;

		if (reg == led_no) {
			flash->dnode[led_no] = child;
			flash->subdev_led[led_no].fwnode =
				of_fwnode_handle(flash->dnode[led_no]);
		}
	}

	rval = zircon_lm3644_init_controls(flash, led_no);
	if (rval)
		goto err_out;
	rval = media_entity_pads_init(&flash->subdev_led[led_no].entity, 0, NULL);
	if (rval < 0)
		goto err_out;
	flash->subdev_led[led_no].entity.function = MEDIA_ENT_F_FLASH;

	rval = v4l2_async_register_subdev(&flash->subdev_led[led_no]);
	if (rval < 0)
		goto err_out;

	return rval;

err_out:
	v4l2_ctrl_handler_free(&flash->ctrls_led[led_no]);
	return rval;
}

/* flashlight init */
static int zircon_lm3644_init(struct zircon_lm3644_flash *flash)
{
	int rval = 0;
	unsigned int reg_val;

	zircon_lm3644_pinctrl_set(flash, ZIRCON_LM3644_PINCTRL_PIN_HWEN, ZIRCON_LM3644_PINCTRL_PINSTATE_HIGH);

	/* set timeout */
	rval = zircon_lm3644_flash_tout_ctrl(flash, 1600);
	if (rval < 0)
		return rval;
	/* output disable */
	flash->led_mode = V4L2_FLASH_LED_MODE_NONE;
	rval = zircon_lm3644_mode_ctrl(flash);

	zircon_lm3644_torch_brt_ctrl(flash, ZIRCON_LM3644_LED0,
				flash->ori_current[ZIRCON_LM3644_LED0]);
	zircon_lm3644_torch_brt_ctrl(flash, ZIRCON_LM3644_LED1,
				flash->ori_current[ZIRCON_LM3644_LED1]);
	zircon_lm3644_flash_brt_ctrl(flash, ZIRCON_LM3644_LED0,
				flash->ori_current[ZIRCON_LM3644_LED0]);
	zircon_lm3644_flash_brt_ctrl(flash, ZIRCON_LM3644_LED1,
				flash->ori_current[ZIRCON_LM3644_LED1]);

	/* reset faults */
	rval = regmap_read(flash->regmap, REG_FLAG1, &reg_val);
	return rval;
}

/* flashlight uninit */
static int zircon_lm3644_uninit(struct zircon_lm3644_flash *flash)
{
	zircon_lm3644_pinctrl_set(flash,
			ZIRCON_LM3644_PINCTRL_PIN_HWEN, ZIRCON_LM3644_PINCTRL_PINSTATE_LOW);

	return 0;
}

static int zircon_lm3644_flash_open(void)
{
	return 0;
}

static int zircon_lm3644_flash_release(void)
{
	return 0;
}

static int zircon_lm3644_ioctl(unsigned int cmd, unsigned long arg)
{
	struct flashlight_dev_arg *fl_arg;
	int channel;

	fl_arg = (struct flashlight_dev_arg *)arg;
	channel = fl_arg->channel;

	switch (cmd) {
	case FLASH_IOC_SET_ONOFF:
		pr_info_ratelimited("FLASH_IOC_SET_ONOFF(%d): %d\n",
				channel, (int)fl_arg->arg);
		if ((int)fl_arg->arg) {
			zircon_lm3644_torch_brt_ctrl(zircon_lm3644_flash_data, channel, 25000);
			zircon_lm3644_flash_data->led_mode = V4L2_FLASH_LED_MODE_TORCH;
			zircon_lm3644_mode_ctrl(zircon_lm3644_flash_data);
			zircon_lm3644_enable_ctrl(zircon_lm3644_flash_data, channel, true);
		} else {
			if (zircon_lm3644_flash_data->led_mode != V4L2_FLASH_LED_MODE_NONE) {
				zircon_lm3644_flash_data->led_mode = V4L2_FLASH_LED_MODE_NONE;
				zircon_lm3644_mode_ctrl(zircon_lm3644_flash_data);
				zircon_lm3644_enable_ctrl(zircon_lm3644_flash_data, channel, false);
			}
		}
		break;
	case XIAOMI_IOC_SET_ONOFF:
		pr_err("XIAOMI_IOC_SET_ONOFF(%d): %d\n",
				channel, (int)fl_arg->arg);
		if ((int)fl_arg->arg) {
			zircon_cit_lm3644_enable_ctrl(zircon_lm3644_flash_data, channel, true);
		} else {
			zircon_cit_lm3644_enable_ctrl(zircon_lm3644_flash_data, channel, false);
		}
		break;
	case XIAOMI_IOC_SET_FLASH_CUR:
		pr_err("XIAOMI_IOC_SET_FLASH_CUR(%d): %d\n",
				channel, (int)fl_arg->arg);
		zircon_lm3644_flash_brt_ctrl(zircon_lm3644_flash_data, channel, fl_arg->arg);
		break;
	case XIAOMI_IOC_SET_TORCH_CUR:
		pr_err("XIAOMI_IOC_SET_TORCH_CUR(%d): %d\n",
				channel, (int)fl_arg->arg);
		zircon_cit_lm3644_torch_brt_ctrl(zircon_lm3644_flash_data, channel, fl_arg->arg);
		break;
	case XIAOMI_IOC_SET_MODE:
		pr_err("XIAOMI_IOC_SET_MODE(%d): %d\n",
				channel, (int)fl_arg->arg);
		zircon_lm3644_flash_data->led_mode = fl_arg->arg;
		zircon_lm3644_mode_ctrl(zircon_lm3644_flash_data);
		break;
	case XIAOMI_IOC_SET_HW_TIMEOUT:
		pr_err("XIAOMI_IOC_SET_HW_TIMEOUT(%d): %d\n",
				channel, (int)fl_arg->arg);
		zircon_lm3644_flash_tout_ctrl(zircon_lm3644_flash_data, fl_arg->arg);
		break;
	default:
		pr_info("No such command and arg(%d): (%d, %d)\n",
				channel, _IOC_NR(cmd), (int)fl_arg->arg);
		return -ENOTTY;
	}

	return 0;
}

#ifdef CIT_LED_NODE
static void zircon_lm3644_en_ctrl(enum zircon_lm3644_led_id led_id, bool on)
{
	unsigned char enable;
	enable = i2c_smbus_read_byte_data(i2client, REG_ENABLE);
	enable &= 0xf3;
	enable |= 0x08;//torch mode
	if(led_id == ZIRCON_LM3644_LED0){
		if(on)
			enable |= 0xff & (0x01 << ZIRCON_LM3644_LED0);
		else
			enable &= 0xff & (~(0x01 << ZIRCON_LM3644_LED0));
	}else if(led_id == ZIRCON_LM3644_LED1){
		if(on)
			enable |= 0xff & (0x01 << ZIRCON_LM3644_LED1);
		else
			enable &= 0xff & (~(0x01 << ZIRCON_LM3644_LED1));
	}
 	i2c_smbus_write_byte_data(i2client, REG_ENABLE, enable);
}

static void zircon_torch0brightness_set(struct led_classdev *led_cdev,
		enum led_brightness value)
{
	int ret;

	if(value > 14)
		value = 14;
	if (value) {
		if(!ledon_flg)
			ret = zircon_lm3644_init(zircon_lm3644_flash_data);
		ledon_flg |= 0x01 << ZIRCON_LM3644_LED0;
		zircon_lm3644_torch_brt_ctrl(zircon_lm3644_flash_data, ZIRCON_LM3644_LED0, value * 25000);
		zircon_lm3644_en_ctrl(ZIRCON_LM3644_LED0, true);
		zircon_lm3644_flash_data->led_mode = V4L2_FLASH_LED_MODE_TORCH;
		zircon_lm3644_mode_ctrl(zircon_lm3644_flash_data);
	} else {
		ledon_flg &= ~(0x01 << ZIRCON_LM3644_LED0);
		zircon_lm3644_flash_data->led_mode = V4L2_FLASH_LED_MODE_NONE;
		zircon_lm3644_mode_ctrl(zircon_lm3644_flash_data);
		zircon_lm3644_en_ctrl(ZIRCON_LM3644_LED0, false);
		if(!ledon_flg)
			ret = zircon_lm3644_uninit(zircon_lm3644_flash_data);
	}
}

static void zircon_torch1brightness_set(struct led_classdev *led_cdev,
		enum led_brightness value)
{
	int ret;

	if(value > 14)
		value = 14;
	if (value) {
		if(!ledon_flg)
			ret = zircon_lm3644_init(zircon_lm3644_flash_data);
		ledon_flg |= 0x01 << ZIRCON_LM3644_LED1;
		zircon_lm3644_torch_brt_ctrl(zircon_lm3644_flash_data, ZIRCON_LM3644_LED1, value * 25000);
		zircon_lm3644_en_ctrl(ZIRCON_LM3644_LED1, true);
		zircon_lm3644_flash_data->led_mode = V4L2_FLASH_LED_MODE_TORCH;
		zircon_lm3644_mode_ctrl(zircon_lm3644_flash_data);
	} else {
		ledon_flg &= ~(0x01 << ZIRCON_LM3644_LED1);
		zircon_lm3644_flash_data->led_mode = V4L2_FLASH_LED_MODE_NONE;
		zircon_lm3644_mode_ctrl(zircon_lm3644_flash_data);
		zircon_lm3644_en_ctrl(ZIRCON_LM3644_LED1, false);
		if(!ledon_flg)
			ret = zircon_lm3644_uninit(zircon_lm3644_flash_data);
	}
}

static struct led_classdev mtk_flash_led[ZIRCON_LM3644_LED_MAX] = {
	{
		.name = "zircon_torch0",
		.brightness_set = zircon_torch0brightness_set,
		.brightness_get = NULL,
		.brightness = LED_OFF,
	},
	{
		.name = "zircon_torch1",
		.brightness_set = zircon_torch1brightness_set,
		.brightness_get = NULL,
		.brightness = LED_OFF,
	},
};

static int32_t mtk_flashlight_create_flash_classdev(struct device *pdev, int lednum)
{
	int32_t rc = 0;
	int32_t i = 0;
	for (i = 0; i < lednum; i++) {
		rc = led_classdev_register(pdev,
			&mtk_flash_led[i]);
		if (rc) {
			pr_err("Failed to register %d led dev. rc = %d\n",
					i, rc);
			return rc;
		}
	}
	return 0;
};
#endif

static int zircon_lm3644_set_driver(int set)
{
	int ret = 0;

	/* set chip and usage count */
	if (set) {
		if (!use_count)
			ret = zircon_lm3644_init(zircon_lm3644_flash_data);
		use_count++;
		pr_debug("Set driver: %d\n", use_count);
	} else {
		use_count--;
		if (!use_count)
			ret = zircon_lm3644_uninit(zircon_lm3644_flash_data);
		if (use_count < 0)
			use_count = 0;
		pr_debug("Unset driver: %d\n", use_count);
	}

	return 0;
}

static ssize_t zircon_lm3644_strobe_store(struct flashlight_arg arg)
{
	zircon_lm3644_set_driver(1);
	if(arg.level == 0 || arg.level > 80){
		return 0;
	}else if(arg.level < 15){
		zircon_lm3644_torch_brt_ctrl(zircon_lm3644_flash_data, arg.channel, arg.level * 25000);
		zircon_lm3644_enable_ctrl(zircon_lm3644_flash_data, arg.channel, true);
		zircon_lm3644_flash_data->led_mode = V4L2_FLASH_LED_MODE_TORCH;
		zircon_lm3644_mode_ctrl(zircon_lm3644_flash_data);
		msleep(arg.dur);
		zircon_lm3644_flash_data->led_mode = V4L2_FLASH_LED_MODE_NONE;
		zircon_lm3644_mode_ctrl(zircon_lm3644_flash_data);
		zircon_lm3644_enable_ctrl(zircon_lm3644_flash_data, arg.channel, false);
		zircon_lm3644_set_driver(0);
	}else{
		if(arg.level < 21)
			return 0;
		arg.level -= 20;
		zircon_lm3644_flash_brt_ctrl(zircon_lm3644_flash_data, arg.channel, arg.level * 25000);
		zircon_lm3644_enable_ctrl(zircon_lm3644_flash_data, arg.channel, true);
		zircon_lm3644_flash_data->led_mode = V4L2_FLASH_LED_MODE_FLASH;
		zircon_lm3644_mode_ctrl(zircon_lm3644_flash_data);
		msleep(arg.dur);
		zircon_lm3644_flash_data->led_mode = V4L2_FLASH_LED_MODE_NONE;
		zircon_lm3644_mode_ctrl(zircon_lm3644_flash_data);
		zircon_lm3644_enable_ctrl(zircon_lm3644_flash_data, arg.channel, false);
		zircon_lm3644_set_driver(0);
	}
	return 0;
}

static int zircon_lm3644_cooling_get_max_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct zircon_lm3644_flash *flash = cdev->devdata;

	*state = flash->max_state;

	return 0;
}

static int zircon_lm3644_cooling_get_cur_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct zircon_lm3644_flash *flash = cdev->devdata;

	*state = flash->target_state;

	return 0;
}

static int zircon_lm3644_cooling_set_cur_state(struct thermal_cooling_device *cdev,
					unsigned long state)
{
	struct zircon_lm3644_flash *flash = cdev->devdata;
	int ret = 0;

	/* Request state should be less than max_state */
	if (state > flash->max_state)
		state = flash->max_state;

	if (flash->target_state == state)
		return 0;

	flash->target_state = state;
	pr_info("set thermal current:%lu\n", flash->target_state);

	if (flash->target_state == 0) {
		flash->need_cooler = 0;
		flash->target_current[ZIRCON_LM3644_LED0] = ZIRCON_LM3644_FLASH_BRT_MAX;
		flash->target_current[ZIRCON_LM3644_LED1] = ZIRCON_LM3644_FLASH_BRT_MAX;
		ret = zircon_lm3644_torch_brt_ctrl(flash, ZIRCON_LM3644_LED0,
					ZIRCON_LM3644_TORCH_BRT_MAX);
		ret = zircon_lm3644_torch_brt_ctrl(flash, ZIRCON_LM3644_LED1,
					ZIRCON_LM3644_TORCH_BRT_MAX);
	} else {
		flash->need_cooler = 1;
		flash->target_current[ZIRCON_LM3644_LED0] =
			flash_state_to_current_limit[flash->target_state - 1];
		flash->target_current[ZIRCON_LM3644_LED1] =
			flash_state_to_current_limit[flash->target_state - 1];
		ret = zircon_lm3644_torch_brt_ctrl(flash, ZIRCON_LM3644_LED0,
					flash->target_current[ZIRCON_LM3644_LED0]);
		ret = zircon_lm3644_torch_brt_ctrl(flash, ZIRCON_LM3644_LED1,
					flash->target_current[ZIRCON_LM3644_LED1]);
	}
	return ret;
}

static struct thermal_cooling_device_ops zircon_lm3644_cooling_ops = {
	.get_max_state		= zircon_lm3644_cooling_get_max_state,
	.get_cur_state		= zircon_lm3644_cooling_get_cur_state,
	.set_cur_state		= zircon_lm3644_cooling_set_cur_state,
};

static struct flashlight_operations zircon_lm3644_flash_ops = {
	zircon_lm3644_flash_open,
	zircon_lm3644_flash_release,
	zircon_lm3644_ioctl,
	zircon_lm3644_strobe_store,
	zircon_lm3644_set_driver
};

static int zircon_lm3644_parse_dt(struct zircon_lm3644_flash *flash)
{
	struct device_node *np = NULL, *cnp = NULL;
	struct device *dev = flash->dev;
	u32 decouple = 0;
	int i = 0, ret = 0;

	if (!dev || !dev->of_node)
		return -ENODEV;

	np = dev->of_node;
	for_each_child_of_node(np, cnp) {
		if (of_property_read_u32(cnp, "type",
					&flash->flash_dev_id[i].type))
			goto err_node_put;
		if (of_property_read_u32(cnp,
					"ct", &flash->flash_dev_id[i].ct))
			goto err_node_put;
		if (of_property_read_u32(cnp,
					"part", &flash->flash_dev_id[i].part))
			goto err_node_put;
		ret = snprintf(flash->flash_dev_id[i].name,
				FLASHLIGHT_NAME_SIZE,
				flash->subdev_led[i].name);
		if (ret < 0)
			pr_info("snprintf failed\n");
		flash->flash_dev_id[i].channel = i;
		flash->flash_dev_id[i].decouple = decouple;

		pr_info("Parse dt (type,ct,part,name,channel,decouple)=(%d,%d,%d,%s,%d,%d).\n",
				flash->flash_dev_id[i].type,
				flash->flash_dev_id[i].ct,
				flash->flash_dev_id[i].part,
				flash->flash_dev_id[i].name,
				flash->flash_dev_id[i].channel,
				flash->flash_dev_id[i].decouple);
		if (flashlight_dev_register_by_device_id(&flash->flash_dev_id[i],
			&zircon_lm3644_flash_ops))
			return -EFAULT;
		i++;
	}

	return 0;

err_node_put:
	of_node_put(cnp);
	return -EINVAL;
}

static int zircon_lm3644_probe(struct i2c_client *client,
			const struct i2c_device_id *devid)
{
	struct zircon_lm3644_flash *flash;
	struct zircon_lm3644_platform_data *pdata = dev_get_platdata(&client->dev);
	int rval;
	unsigned int reg_val;

	pr_info("%s:%d", __func__, __LINE__);

	flash = devm_kzalloc(&client->dev, sizeof(*flash), GFP_KERNEL);
	if (flash == NULL)
		return -ENOMEM;

	flash->regmap = devm_regmap_init_i2c(client, &zircon_lm3644_regmap);
	if (IS_ERR(flash->regmap)) {
		rval = PTR_ERR(flash->regmap);
		return rval;
	}

	/* if there is no platform data, use chip default value */
	if (pdata == NULL) {
		pdata = devm_kzalloc(&client->dev, sizeof(*pdata), GFP_KERNEL);
		if (pdata == NULL)
			return -ENODEV;
		pdata->max_flash_timeout = ZIRCON_LM3644_FLASH_TOUT_MAX;
		/* led 1 */
		pdata->max_flash_brt[ZIRCON_LM3644_LED0] = ZIRCON_LM3644_FLASH_BRT_MAX;
		pdata->max_torch_brt[ZIRCON_LM3644_LED0] = ZIRCON_LM3644_TORCH_BRT_MAX;
		/* led 2 */
		pdata->max_flash_brt[ZIRCON_LM3644_LED1] = ZIRCON_LM3644_FLASH_BRT_MAX;
		pdata->max_torch_brt[ZIRCON_LM3644_LED1] = ZIRCON_LM3644_TORCH_BRT_MAX;
	}
	flash->pdata = pdata;
	flash->dev = &client->dev;
	mutex_init(&flash->lock);
	zircon_lm3644_flash_data = flash;

	rval = zircon_lm3644_pinctrl_init(flash);
	if (rval < 0)
		return rval;

	rval = zircon_lm3644_subdev_init(flash, ZIRCON_LM3644_LED0, "zircon_lm3644-led0");
	if (rval < 0)
		return rval;

	rval = zircon_lm3644_subdev_init(flash, ZIRCON_LM3644_LED1, "zircon_lm3644-led1");
	if (rval < 0)
		return rval;

	pm_runtime_enable(flash->dev);

	rval = zircon_lm3644_parse_dt(flash);

	i2c_set_clientdata(client, flash);

	flash->max_state = ZIRCON_LM3644_COOLER_MAX_STATE;
	flash->target_state = 0;
	flash->need_cooler = 0;
	flash->target_current[ZIRCON_LM3644_LED0] = ZIRCON_LM3644_FLASH_BRT_MAX;
	flash->target_current[ZIRCON_LM3644_LED1] = ZIRCON_LM3644_FLASH_BRT_MAX;
	flash->ori_current[ZIRCON_LM3644_LED0] = ZIRCON_LM3644_TORCH_BRT_MIN;
	flash->ori_current[ZIRCON_LM3644_LED1] = ZIRCON_LM3644_TORCH_BRT_MIN;
	flash->cdev = thermal_of_cooling_device_register(client->dev.of_node,
			"flashlight_cooler", flash, &zircon_lm3644_cooling_ops);
	if (IS_ERR(flash->cdev))
		pr_info("register thermal failed\n");

#ifdef CIT_LED_NODE
	rval = mtk_flashlight_create_flash_classdev(&client->dev, ZIRCON_LM3644_LED_MAX);
	if(rval)
		pr_err("torchbrightness node creat failed\n");
	else
		pr_err("torchbrightness node creat success\n");
	i2client = client;
#endif

	rval = regmap_read(flash->regmap, REG_ID, &reg_val);
	pr_info("LM3644 ID %xn", reg_val);
	pr_info("%s:%d", __func__, __LINE__);
	return 0;
}

static int zircon_lm3644_remove(struct i2c_client *client)
{
	struct zircon_lm3644_flash *flash = i2c_get_clientdata(client);
	unsigned int i;

	thermal_cooling_device_unregister(flash->cdev);
	for (i = ZIRCON_LM3644_LED0; i < ZIRCON_LM3644_LED_MAX; i++) {
		v4l2_device_unregister_subdev(&flash->subdev_led[i]);
		v4l2_ctrl_handler_free(&flash->ctrls_led[i]);
		media_entity_cleanup(&flash->subdev_led[i].entity);
	}

	pm_runtime_disable(&client->dev);

	pm_runtime_set_suspended(&client->dev);
	return 0;
}

static int __maybe_unused zircon_lm3644_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct zircon_lm3644_flash *flash = i2c_get_clientdata(client);

	pr_info("%s %d", __func__, __LINE__);

	return zircon_lm3644_uninit(flash);
}

static int __maybe_unused zircon_lm3644_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct zircon_lm3644_flash *flash = i2c_get_clientdata(client);

	pr_info("%s %d", __func__, __LINE__);

	return zircon_lm3644_init(flash);
}

static const struct i2c_device_id zircon_lm3644_id_table[] = {
	{ZIRCON_LM3644_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, zircon_lm3644_id_table);

static const struct of_device_id zircon_lm3644_of_table[] = {
	{ .compatible = "mediatek,zircon_lm3644" },
	{ },
};
MODULE_DEVICE_TABLE(of, zircon_lm3644_of_table);

static const struct dev_pm_ops zircon_lm3644_pm_ops = {
	// SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
	// 			pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(zircon_lm3644_suspend, zircon_lm3644_resume, NULL)
};

static struct i2c_driver zircon_lm3644_i2c_driver = {
	.driver = {
		   .name = ZIRCON_LM3644_NAME,
		   .pm = &zircon_lm3644_pm_ops,
		   .of_match_table = zircon_lm3644_of_table,
		   },
	.probe = zircon_lm3644_probe,
	.remove = zircon_lm3644_remove,
	.id_table = zircon_lm3644_id_table,
};

module_i2c_driver(zircon_lm3644_i2c_driver);

MODULE_AUTHOR("Roger-HY Wang <roger-hy.wang@mediatek.com>");
MODULE_DESCRIPTION("Texas Instruments ZIRCON_LM3644 LED flash driver");
MODULE_LICENSE("GPL");
