/*
 * V4L2 subdevice driver for OmniVision OV6650 Camera Sensor
 *
 * Copyright (C) 2010 Janusz Krzysztofik <jkrzyszt@tis.icnet.pl>
 *
 * Based on OmniVision OV96xx Camera Driver
 * Copyright (C) 2009 Marek Vasut <marek.vasut@gmail.com>
 *
 * Based on ov772x camera driver:
 * Copyright (C) 2008 Renesas Solutions Corp.
 * Kuninori Morimoto <morimoto.kuninori@renesas.com>
 *
 * Based on ov7670 and soc_camera_platform driver,
 * Copyright 2006-7 Jonathan Corbet <corbet@lwn.net>
 * Copyright (C) 2008 Magnus Damm
 * Copyright (C) 2008, Guennadi Liakhovetski <kernel@pengutronix.de>
 *
 * Hardware specific bits initialy based on former work by Matt Callow
 * drivers/media/video/omap/sensor_ov6650.c
 * Copyright (C) 2006 Matt Callow
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/v4l2-mediabus.h>
#include <linux/module.h>

#include <media/v4l2-clk.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

/* Register definitions */
#define REG_GAIN		0x00	/* range 00 - 3F */
#define REG_BLUE		0x01
#define REG_RED			0x02
#define REG_SAT			0x03	/* [7:4] saturation [0:3] reserved */
#define REG_HUE			0x04	/* [7:6] rsrvd [5] hue en [4:0] hue */

#define REG_BRT			0x06

#define REG_PIDH		0x0a
#define REG_PIDL		0x0b

#define REG_AECH		0x10
#define REG_CLKRC		0x11	/* Data Format and Internal Clock */
					/* [7:6] Input system clock (MHz)*/
					/*   00=8, 01=12, 10=16, 11=24 */
					/* [5:0]: Internal Clock Pre-Scaler */
#define REG_COMA		0x12	/* [7] Reset */
#define REG_COMB		0x13
#define REG_COMC		0x14
#define REG_COMD		0x15
#define REG_COML		0x16
#define REG_HSTRT		0x17
#define REG_HSTOP		0x18
#define REG_VSTRT		0x19
#define REG_VSTOP		0x1a
#define REG_PSHFT		0x1b
#define REG_MIDH		0x1c
#define REG_MIDL		0x1d
#define REG_HSYNS		0x1e
#define REG_HSYNE		0x1f
#define REG_COME		0x20
#define REG_YOFF		0x21
#define REG_UOFF		0x22
#define REG_VOFF		0x23
#define REG_AEW			0x24
#define REG_AEB			0x25
#define REG_COMF		0x26
#define REG_COMG		0x27
#define REG_COMH		0x28
#define REG_COMI		0x29

#define REG_FRARL		0x2b
#define REG_COMJ		0x2c
#define REG_COMK		0x2d
#define REG_AVGY		0x2e
#define REG_REF0		0x2f
#define REG_REF1		0x30
#define REG_REF2		0x31
#define REG_FRAJH		0x32
#define REG_FRAJL		0x33
#define REG_FACT		0x34
#define REG_L1AEC		0x35
#define REG_AVGU		0x36
#define REG_AVGV		0x37

#define REG_SPCB		0x60
#define REG_SPCC		0x61
#define REG_GAM1		0x62
#define REG_GAM2		0x63
#define REG_GAM3		0x64
#define REG_SPCD		0x65

#define REG_SPCE		0x68
#define REG_ADCL		0x69

#define REG_RMCO		0x6c
#define REG_GMCO		0x6d
#define REG_BMCO		0x6e


/* Register bits, values, etc. */
#define OV6650_PIDH		0x66	/* high byte of product ID number */
#define OV6650_PIDL		0x50	/* low byte of product ID number */
#define OV6650_MIDH		0x7F	/* high byte of mfg ID */
#define OV6650_MIDL		0xA2	/* low byte of mfg ID */

#define DEF_GAIN		0x00
#define DEF_BLUE		0x80
#define DEF_RED			0x80

#define SAT_SHIFT		4
#define SAT_MASK		(0xf << SAT_SHIFT)
#define SET_SAT(x)		(((x) << SAT_SHIFT) & SAT_MASK)

#define HUE_EN			BIT(5)
#define HUE_MASK		0x1f
#define DEF_HUE			0x10
#define SET_HUE(x)		(HUE_EN | ((x) & HUE_MASK))

#define DEF_AECH		0x4D

#define CLKRC_6MHz		0x00
#define CLKRC_12MHz		0x40
#define CLKRC_16MHz		0x80
#define CLKRC_24MHz		0xc0
#define CLKRC_DIV_MASK		0x3f
#define GET_CLKRC_DIV(x)	(((x) & CLKRC_DIV_MASK) + 1)

#define COMA_RESET		BIT(7)
#define COMA_QCIF		BIT(5)
#define COMA_RAW_RGB		BIT(4)
#define COMA_RGB		BIT(3)
#define COMA_BW			BIT(2)
#define COMA_WORD_SWAP		BIT(1)
#define COMA_BYTE_SWAP		BIT(0)
#define DEF_COMA		0x00

#define COMB_FLIP_V		BIT(7)
#define COMB_FLIP_H		BIT(5)
#define COMB_BAND_FILTER	BIT(4)
#define COMB_AWB		BIT(2)
#define COMB_AGC		BIT(1)
#define COMB_AEC		BIT(0)
#define DEF_COMB		0x5f

#define COML_ONE_CHANNEL	BIT(7)

#define DEF_HSTRT		0x24
#define DEF_HSTOP		0xd4
#define DEF_VSTRT		0x04
#define DEF_VSTOP		0x94

#define COMF_HREF_LOW		BIT(4)

#define COMJ_PCLK_RISING	BIT(4)
#define COMJ_VSYNC_HIGH		BIT(0)

/* supported resolutions */
#define W_QCIF			(DEF_HSTOP - DEF_HSTRT)
#define W_CIF			(W_QCIF << 1)
#define H_QCIF			(DEF_VSTOP - DEF_VSTRT)
#define H_CIF			(H_QCIF << 1)

#define FRAME_RATE_MAX		30


struct ov6650_reg {
	u8	reg;
	u8	val;
};

struct ov6650 {
	struct v4l2_subdev	subdev;
	struct v4l2_ctrl_handler hdl;
	struct {
		/* exposure/autoexposure cluster */
		struct v4l2_ctrl *autoexposure;
		struct v4l2_ctrl *exposure;
	};
	struct {
		/* gain/autogain cluster */
		struct v4l2_ctrl *autogain;
		struct v4l2_ctrl *gain;
	};
	struct {
		/* blue/red/autowhitebalance cluster */
		struct v4l2_ctrl *autowb;
		struct v4l2_ctrl *blue;
		struct v4l2_ctrl *red;
	};
	struct v4l2_clk		*clk;
	bool			half_scale;	/* scale down output by 2 */
	struct v4l2_rect	rect;		/* sensor cropping window */
	unsigned long		pclk_limit;	/* from host */
	unsigned long		pclk_max;	/* from resolution and format */
	struct v4l2_fract	tpf;		/* as requested with s_frame_interval */
	u32 code;
	enum v4l2_colorspace	colorspace;
};


static u32 ov6650_codes[] = {
	MEDIA_BUS_FMT_YUYV8_2X8,
	MEDIA_BUS_FMT_UYVY8_2X8,
	MEDIA_BUS_FMT_YVYU8_2X8,
	MEDIA_BUS_FMT_VYUY8_2X8,
	MEDIA_BUS_FMT_SBGGR8_1X8,
	MEDIA_BUS_FMT_Y8_1X8,
};

/* read a register */
static int ov6650_reg_read(struct i2c_client *client, u8 reg, u8 *val)
{
	int ret;
	u8 data = reg;
	struct i2c_msg msg = {
		.addr	= client->addr,
		.flags	= 0,
		.len	= 1,
		.buf	= &data,
	};

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		goto err;

	msg.flags = I2C_M_RD;
	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		goto err;

	*val = data;
	return 0;

err:
	dev_err(&client->dev, "Failed reading register 0x%02x!\n", reg);
	return ret;
}

/* write a register */
static int ov6650_reg_write(struct i2c_client *client, u8 reg, u8 val)
{
	int ret;
	unsigned char data[2] = { reg, val };
	struct i2c_msg msg = {
		.addr	= client->addr,
		.flags	= 0,
		.len	= 2,
		.buf	= data,
	};

	ret = i2c_transfer(client->adapter, &msg, 1);
	udelay(100);

	if (ret < 0) {
		dev_err(&client->dev, "Failed writing register 0x%02x!\n", reg);
		return ret;
	}
	return 0;
}


/* Read a register, alter its bits, write it back */
static int ov6650_reg_rmw(struct i2c_client *client, u8 reg, u8 set, u8 mask)
{
	u8 val;
	int ret;

	ret = ov6650_reg_read(client, reg, &val);
	if (ret) {
		dev_err(&client->dev,
			"[Read]-Modify-Write of register 0x%02x failed!\n",
			reg);
		return ret;
	}

	val &= ~mask;
	val |= set;

	ret = ov6650_reg_write(client, reg, val);
	if (ret)
		dev_err(&client->dev,
			"Read-Modify-[Write] of register 0x%02x failed!\n",
			reg);

	return ret;
}

static struct ov6650 *to_ov6650(const struct i2c_client *client)
{
	return container_of(i2c_get_clientdata(client), struct ov6650, subdev);
}

/* Start/Stop streaming from the device */
static int ov6650_s_stream(struct v4l2_subdev *sd, int enable)
{
	return 0;
}

/* Get status of additional camera capabilities */
static int ov6550_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov6650 *priv = container_of(ctrl->handler, struct ov6650, hdl);
	struct v4l2_subdev *sd = &priv->subdev;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	uint8_t reg, reg2;
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_AUTOGAIN:
		ret = ov6650_reg_read(client, REG_GAIN, &reg);
		if (!ret)
			priv->gain->val = reg;
		return ret;
	case V4L2_CID_AUTO_WHITE_BALANCE:
		ret = ov6650_reg_read(client, REG_BLUE, &reg);
		if (!ret)
			ret = ov6650_reg_read(client, REG_RED, &reg2);
		if (!ret) {
			priv->blue->val = reg;
			priv->red->val = reg2;
		}
		return ret;
	case V4L2_CID_EXPOSURE_AUTO:
		ret = ov6650_reg_read(client, REG_AECH, &reg);
		if (!ret)
			priv->exposure->val = reg;
		return ret;
	}
	return -EINVAL;
}

/* Set status of additional camera capabilities */
static int ov6550_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov6650 *priv = container_of(ctrl->handler, struct ov6650, hdl);
	struct v4l2_subdev *sd = &priv->subdev;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_AUTOGAIN:
		ret = ov6650_reg_rmw(client, REG_COMB,
				ctrl->val ? COMB_AGC : 0, COMB_AGC);
		if (!ret && !ctrl->val)
			ret = ov6650_reg_write(client, REG_GAIN, priv->gain->val);
		return ret;
	case V4L2_CID_AUTO_WHITE_BALANCE:
		ret = ov6650_reg_rmw(client, REG_COMB,
				ctrl->val ? COMB_AWB : 0, COMB_AWB);
		if (!ret && !ctrl->val) {
			ret = ov6650_reg_write(client, REG_BLUE, priv->blue->val);
			if (!ret)
				ret = ov6650_reg_write(client, REG_RED,
							priv->red->val);
		}
		return ret;
	case V4L2_CID_SATURATION:
		return ov6650_reg_rmw(client, REG_SAT, SET_SAT(ctrl->val),
				SAT_MASK);
	case V4L2_CID_HUE:
		return ov6650_reg_rmw(client, REG_HUE, SET_HUE(ctrl->val),
				HUE_MASK);
	case V4L2_CID_BRIGHTNESS:
		return ov6650_reg_write(client, REG_BRT, ctrl->val);
	case V4L2_CID_EXPOSURE_AUTO:
		ret = ov6650_reg_rmw(client, REG_COMB, ctrl->val ==
				V4L2_EXPOSURE_AUTO ? COMB_AEC : 0, COMB_AEC);
		if (!ret && ctrl->val == V4L2_EXPOSURE_MANUAL)
			ret = ov6650_reg_write(client, REG_AECH,
						priv->exposure->val);
		return ret;
	case V4L2_CID_GAMMA:
		return ov6650_reg_write(client, REG_GAM1, ctrl->val);
	case V4L2_CID_VFLIP:
		return ov6650_reg_rmw(client, REG_COMB,
				ctrl->val ? COMB_FLIP_V : 0, COMB_FLIP_V);
	case V4L2_CID_HFLIP:
		return ov6650_reg_rmw(client, REG_COMB,
				ctrl->val ? COMB_FLIP_H : 0, COMB_FLIP_H);
	}

	return -EINVAL;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int ov6650_get_register(struct v4l2_subdev *sd,
				struct v4l2_dbg_register *reg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;
	u8 val;

	if (reg->reg & ~0xff)
		return -EINVAL;

	reg->size = 1;

	ret = ov6650_reg_read(client, reg->reg, &val);
	if (!ret)
		reg->val = (__u64)val;

	return ret;
}

static int ov6650_set_register(struct v4l2_subdev *sd,
				const struct v4l2_dbg_register *reg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (reg->reg & ~0xff || reg->val & ~0xff)
		return -EINVAL;

	return ov6650_reg_write(client, reg->reg, reg->val);
}
#endif

static int ov6650_s_power(struct v4l2_subdev *sd, int on)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov6650 *priv = to_ov6650(client);
	int ret = 0;

	if (on)
		ret = v4l2_clk_enable(priv->clk);
	else
		v4l2_clk_disable(priv->clk);

	return ret;
}

static int ov6650_get_selection(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_selection *sel)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov6650 *priv = to_ov6650(client);

	if (sel->which != V4L2_SUBDEV_FORMAT_ACTIVE)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.left = DEF_HSTRT << 1;
		sel->r.top = DEF_VSTRT << 1;
		sel->r.width = W_CIF;
		sel->r.height = H_CIF;
		return 0;
	case V4L2_SEL_TGT_CROP:
		sel->r = priv->rect;
		return 0;
	default:
		return -EINVAL;
	}
}

static int ov6650_set_selection(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_selection *sel)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov6650 *priv = to_ov6650(client);
	struct v4l2_rect rect = sel->r;
	int ret;

	if (sel->which != V4L2_SUBDEV_FORMAT_ACTIVE ||
	    sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	v4l_bound_align_image(&rect.width, 2, W_CIF, 1,
			      &rect.height, 2, H_CIF, 1, 0);
	v4l_bound_align_image(&rect.left, DEF_HSTRT << 1,
			      (DEF_HSTRT << 1) + W_CIF - (__s32)rect.width, 1,
			      &rect.top, DEF_VSTRT << 1,
			      (DEF_VSTRT << 1) + H_CIF - (__s32)rect.height, 1,
			      0);

	ret = ov6650_reg_write(client, REG_HSTRT, rect.left >> 1);
	if (!ret) {
		priv->rect.left = rect.left;
		ret = ov6650_reg_write(client, REG_HSTOP,
				(rect.left + rect.width) >> 1);
	}
	if (!ret) {
		priv->rect.width = rect.width;
		ret = ov6650_reg_write(client, REG_VSTRT, rect.top >> 1);
	}
	if (!ret) {
		priv->rect.top = rect.top;
		ret = ov6650_reg_write(client, REG_VSTOP,
				(rect.top + rect.height) >> 1);
	}
	if (!ret)
		priv->rect.height = rect.height;

	return ret;
}

static int ov6650_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov6650 *priv = to_ov6650(client);

	if (format->pad)
		return -EINVAL;

	mf->width	= priv->rect.width >> priv->half_scale;
	mf->height	= priv->rect.height >> priv->half_scale;
	mf->code	= priv->code;
	mf->colorspace	= priv->colorspace;
	mf->field	= V4L2_FIELD_NONE;

	return 0;
}

static bool is_unscaled_ok(int width, int height, struct v4l2_rect *rect)
{
	return width > rect->width >> 1 || height > rect->height >> 1;
}

static u8 to_clkrc(struct v4l2_fract *timeperframe,
		unsigned long pclk_limit, unsigned long pclk_max)
{
	unsigned long pclk;

	if (timeperframe->numerator && timeperframe->denominator)
		pclk = pclk_max * timeperframe->denominator /
				(FRAME_RATE_MAX * timeperframe->numerator);
	else
		pclk = pclk_max;

	if (pclk_limit && pclk_limit < pclk)
		pclk = pclk_limit;

	return (pclk_max - 1) / pclk;
}

/* set the format we will capture in */
static int ov6650_s_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov6650 *priv = to_ov6650(client);
	bool half_scale = !is_unscaled_ok(mf->width, mf->height, &priv->rect);
	struct v4l2_subdev_selection sel = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.target = V4L2_SEL_TGT_CROP,
		.r.left = priv->rect.left + (priv->rect.width >> 1) -
			(mf->width >> (1 - half_scale)),
		.r.top = priv->rect.top + (priv->rect.height >> 1) -
			(mf->height >> (1 - half_scale)),
		.r.width = mf->width << half_scale,
		.r.height = mf->height << half_scale,
	};
	u32 code = mf->code;
	unsigned long mclk, pclk;
	u8 coma_set = 0, coma_mask = 0, coml_set, coml_mask, clkrc;
	int ret;

	/* select color matrix configuration for given color encoding */
	switch (code) {
	case MEDIA_BUS_FMT_Y8_1X8:
		dev_dbg(&client->dev, "pixel format GREY8_1X8\n");
		coma_mask |= COMA_RGB | COMA_WORD_SWAP | COMA_BYTE_SWAP;
		coma_set |= COMA_BW;
		break;
	case MEDIA_BUS_FMT_YUYV8_2X8:
		dev_dbg(&client->dev, "pixel format YUYV8_2X8_LE\n");
		coma_mask |= COMA_RGB | COMA_BW | COMA_BYTE_SWAP;
		coma_set |= COMA_WORD_SWAP;
		break;
	case MEDIA_BUS_FMT_YVYU8_2X8:
		dev_dbg(&client->dev, "pixel format YVYU8_2X8_LE (untested)\n");
		coma_mask |= COMA_RGB | COMA_BW | COMA_WORD_SWAP |
				COMA_BYTE_SWAP;
		break;
	case MEDIA_BUS_FMT_UYVY8_2X8:
		dev_dbg(&client->dev, "pixel format YUYV8_2X8_BE\n");
		if (half_scale) {
			coma_mask |= COMA_RGB | COMA_BW | COMA_WORD_SWAP;
			coma_set |= COMA_BYTE_SWAP;
		} else {
			coma_mask |= COMA_RGB | COMA_BW;
			coma_set |= COMA_BYTE_SWAP | COMA_WORD_SWAP;
		}
		break;
	case MEDIA_BUS_FMT_VYUY8_2X8:
		dev_dbg(&client->dev, "pixel format YVYU8_2X8_BE (untested)\n");
		if (half_scale) {
			coma_mask |= COMA_RGB | COMA_BW;
			coma_set |= COMA_BYTE_SWAP | COMA_WORD_SWAP;
		} else {
			coma_mask |= COMA_RGB | COMA_BW | COMA_WORD_SWAP;
			coma_set |= COMA_BYTE_SWAP;
		}
		break;
	case MEDIA_BUS_FMT_SBGGR8_1X8:
		dev_dbg(&client->dev, "pixel format SBGGR8_1X8 (untested)\n");
		coma_mask |= COMA_BW | COMA_BYTE_SWAP | COMA_WORD_SWAP;
		coma_set |= COMA_RAW_RGB | COMA_RGB;
		break;
	default:
		dev_err(&client->dev, "Pixel format not handled: 0x%x\n", code);
		return -EINVAL;
	}
	priv->code = code;

	if (code == MEDIA_BUS_FMT_Y8_1X8 ||
			code == MEDIA_BUS_FMT_SBGGR8_1X8) {
		coml_mask = COML_ONE_CHANNEL;
		coml_set = 0;
		priv->pclk_max = 4000000;
	} else {
		coml_mask = 0;
		coml_set = COML_ONE_CHANNEL;
		priv->pclk_max = 8000000;
	}

	if (code == MEDIA_BUS_FMT_SBGGR8_1X8)
		priv->colorspace = V4L2_COLORSPACE_SRGB;
	else if (code != 0)
		priv->colorspace = V4L2_COLORSPACE_JPEG;

	if (half_scale) {
		dev_dbg(&client->dev, "max resolution: QCIF\n");
		coma_set |= COMA_QCIF;
		priv->pclk_max /= 2;
	} else {
		dev_dbg(&client->dev, "max resolution: CIF\n");
		coma_mask |= COMA_QCIF;
	}
	priv->half_scale = half_scale;

	clkrc = CLKRC_12MHz;
	mclk = 12000000;
	priv->pclk_limit = 1334000;
	dev_dbg(&client->dev, "using 12MHz input clock\n");

	clkrc |= to_clkrc(&priv->tpf, priv->pclk_limit, priv->pclk_max);

	pclk = priv->pclk_max / GET_CLKRC_DIV(clkrc);
	dev_dbg(&client->dev, "pixel clock divider: %ld.%ld\n",
			mclk / pclk, 10 * mclk % pclk / pclk);

	ret = ov6650_set_selection(sd, NULL, &sel);
	if (!ret)
		ret = ov6650_reg_rmw(client, REG_COMA, coma_set, coma_mask);
	if (!ret)
		ret = ov6650_reg_write(client, REG_CLKRC, clkrc);
	if (!ret)
		ret = ov6650_reg_rmw(client, REG_COML, coml_set, coml_mask);

	if (!ret) {
		mf->colorspace	= priv->colorspace;
		mf->width = priv->rect.width >> half_scale;
		mf->height = priv->rect.height >> half_scale;
	}
	return ret;
}

static int ov6650_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov6650 *priv = to_ov6650(client);

	if (format->pad)
		return -EINVAL;

	if (is_unscaled_ok(mf->width, mf->height, &priv->rect))
		v4l_bound_align_image(&mf->width, 2, W_CIF, 1,
				&mf->height, 2, H_CIF, 1, 0);

	mf->field = V4L2_FIELD_NONE;

	switch (mf->code) {
	case MEDIA_BUS_FMT_Y10_1X10:
		mf->code = MEDIA_BUS_FMT_Y8_1X8;
		/* fall through */
	case MEDIA_BUS_FMT_Y8_1X8:
	case MEDIA_BUS_FMT_YVYU8_2X8:
	case MEDIA_BUS_FMT_YUYV8_2X8:
	case MEDIA_BUS_FMT_VYUY8_2X8:
	case MEDIA_BUS_FMT_UYVY8_2X8:
		mf->colorspace = V4L2_COLORSPACE_JPEG;
		break;
	default:
		mf->code = MEDIA_BUS_FMT_SBGGR8_1X8;
		/* fall through */
	case MEDIA_BUS_FMT_SBGGR8_1X8:
		mf->colorspace = V4L2_COLORSPACE_SRGB;
		break;
	}

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		return ov6650_s_fmt(sd, mf);
	cfg->try_fmt = *mf;

	return 0;
}

static int ov6650_enum_mbus_code(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index >= ARRAY_SIZE(ov6650_codes))
		return -EINVAL;

	code->code = ov6650_codes[code->index];
	return 0;
}

static int ov6650_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *ival)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov6650 *priv = to_ov6650(client);

	ival->interval.numerator = GET_CLKRC_DIV(to_clkrc(&priv->tpf,
			priv->pclk_limit, priv->pclk_max));
	ival->interval.denominator = FRAME_RATE_MAX;

	dev_dbg(&client->dev, "Frame interval: %u/%u s\n",
		ival->interval.numerator, ival->interval.denominator);

	return 0;
}

static int ov6650_s_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *ival)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov6650 *priv = to_ov6650(client);
	struct v4l2_fract *tpf = &ival->interval;
	int div, ret;
	u8 clkrc;

	if (tpf->numerator == 0 || tpf->denominator == 0)
		div = 1;  /* Reset to full rate */
	else
		div = (tpf->numerator * FRAME_RATE_MAX) / tpf->denominator;

	if (div == 0)
		div = 1;
	else if (div > GET_CLKRC_DIV(CLKRC_DIV_MASK))
		div = GET_CLKRC_DIV(CLKRC_DIV_MASK);

	/*
	 * Keep result to be used as tpf limit
	 * for subseqent clock divider calculations
	 */
	priv->tpf.numerator = div;
	priv->tpf.denominator = FRAME_RATE_MAX;

	clkrc = to_clkrc(&priv->tpf, priv->pclk_limit, priv->pclk_max);

	ret = ov6650_reg_rmw(client, REG_CLKRC, clkrc, CLKRC_DIV_MASK);
	if (!ret) {
		tpf->numerator = GET_CLKRC_DIV(clkrc);
		tpf->denominator = FRAME_RATE_MAX;
	}

	return ret;
}

/* Soft reset the camera. This has nothing to do with the RESET pin! */
static int ov6650_reset(struct i2c_client *client)
{
	int ret;

	dev_dbg(&client->dev, "reset\n");

	ret = ov6650_reg_rmw(client, REG_COMA, COMA_RESET, 0);
	if (ret)
		dev_err(&client->dev,
			"An error occurred while entering soft reset!\n");

	return ret;
}

/* program default register values */
static int ov6650_prog_dflt(struct i2c_client *client)
{
	int ret;

	dev_dbg(&client->dev, "initializing\n");

	ret = ov6650_reg_write(client, REG_COMA, 0);	/* ~COMA_RESET */
	if (!ret)
		ret = ov6650_reg_rmw(client, REG_COMB, 0, COMB_BAND_FILTER);

	return ret;
}

static int ov6650_video_probe(struct i2c_client *client)
{
	struct ov6650 *priv = to_ov6650(client);
	u8		pidh, pidl, midh, midl;
	int		ret;

	ret = ov6650_s_power(&priv->subdev, 1);
	if (ret < 0)
		return ret;

	msleep(20);

	/*
	 * check and show product ID and manufacturer ID
	 */
	ret = ov6650_reg_read(client, REG_PIDH, &pidh);
	if (!ret)
		ret = ov6650_reg_read(client, REG_PIDL, &pidl);
	if (!ret)
		ret = ov6650_reg_read(client, REG_MIDH, &midh);
	if (!ret)
		ret = ov6650_reg_read(client, REG_MIDL, &midl);

	if (ret)
		goto done;

	if ((pidh != OV6650_PIDH) || (pidl != OV6650_PIDL)) {
		dev_err(&client->dev, "Product ID error 0x%02x:0x%02x\n",
				pidh, pidl);
		ret = -ENODEV;
		goto done;
	}

	dev_info(&client->dev,
		"ov6650 Product ID 0x%02x:0x%02x Manufacturer ID 0x%02x:0x%02x\n",
		pidh, pidl, midh, midl);

	ret = ov6650_reset(client);
	if (!ret)
		ret = ov6650_prog_dflt(client);
	if (!ret)
		ret = v4l2_ctrl_handler_setup(&priv->hdl);

done:
	ov6650_s_power(&priv->subdev, 0);
	return ret;
}

static const struct v4l2_ctrl_ops ov6550_ctrl_ops = {
	.g_volatile_ctrl = ov6550_g_volatile_ctrl,
	.s_ctrl = ov6550_s_ctrl,
};

static const struct v4l2_subdev_core_ops ov6650_core_ops = {
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register		= ov6650_get_register,
	.s_register		= ov6650_set_register,
#endif
	.s_power		= ov6650_s_power,
};

/* Request bus settings on camera side */
static int ov6650_g_mbus_config(struct v4l2_subdev *sd,
				struct v4l2_mbus_config *cfg)
{

	cfg->flags = V4L2_MBUS_MASTER |
		V4L2_MBUS_PCLK_SAMPLE_RISING | V4L2_MBUS_PCLK_SAMPLE_FALLING |
		V4L2_MBUS_HSYNC_ACTIVE_HIGH | V4L2_MBUS_HSYNC_ACTIVE_LOW |
		V4L2_MBUS_VSYNC_ACTIVE_HIGH | V4L2_MBUS_VSYNC_ACTIVE_LOW |
		V4L2_MBUS_DATA_ACTIVE_HIGH;
	cfg->type = V4L2_MBUS_PARALLEL;

	return 0;
}

/* Alter bus settings on camera side */
static int ov6650_s_mbus_config(struct v4l2_subdev *sd,
				const struct v4l2_mbus_config *cfg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	if (cfg->flags & V4L2_MBUS_PCLK_SAMPLE_RISING)
		ret = ov6650_reg_rmw(client, REG_COMJ, COMJ_PCLK_RISING, 0);
	else
		ret = ov6650_reg_rmw(client, REG_COMJ, 0, COMJ_PCLK_RISING);
	if (ret)
		return ret;

	if (cfg->flags & V4L2_MBUS_HSYNC_ACTIVE_LOW)
		ret = ov6650_reg_rmw(client, REG_COMF, COMF_HREF_LOW, 0);
	else
		ret = ov6650_reg_rmw(client, REG_COMF, 0, COMF_HREF_LOW);
	if (ret)
		return ret;

	if (cfg->flags & V4L2_MBUS_VSYNC_ACTIVE_HIGH)
		ret = ov6650_reg_rmw(client, REG_COMJ, COMJ_VSYNC_HIGH, 0);
	else
		ret = ov6650_reg_rmw(client, REG_COMJ, 0, COMJ_VSYNC_HIGH);

	return ret;
}

static const struct v4l2_subdev_video_ops ov6650_video_ops = {
	.s_stream	= ov6650_s_stream,
	.g_frame_interval = ov6650_g_frame_interval,
	.s_frame_interval = ov6650_s_frame_interval,
	.g_mbus_config	= ov6650_g_mbus_config,
	.s_mbus_config	= ov6650_s_mbus_config,
};

static const struct v4l2_subdev_pad_ops ov6650_pad_ops = {
	.enum_mbus_code = ov6650_enum_mbus_code,
	.get_selection	= ov6650_get_selection,
	.set_selection	= ov6650_set_selection,
	.get_fmt	= ov6650_get_fmt,
	.set_fmt	= ov6650_set_fmt,
};

static const struct v4l2_subdev_ops ov6650_subdev_ops = {
	.core	= &ov6650_core_ops,
	.video	= &ov6650_video_ops,
	.pad	= &ov6650_pad_ops,
};

/*
 * i2c_driver function
 */
static int ov6650_probe(struct i2c_client *client,
			const struct i2c_device_id *did)
{
	struct ov6650 *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&priv->subdev, client, &ov6650_subdev_ops);
	v4l2_ctrl_handler_init(&priv->hdl, 13);
	v4l2_ctrl_new_std(&priv->hdl, &ov6550_ctrl_ops,
			V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&priv->hdl, &ov6550_ctrl_ops,
			V4L2_CID_HFLIP, 0, 1, 1, 0);
	priv->autogain = v4l2_ctrl_new_std(&priv->hdl, &ov6550_ctrl_ops,
			V4L2_CID_AUTOGAIN, 0, 1, 1, 1);
	priv->gain = v4l2_ctrl_new_std(&priv->hdl, &ov6550_ctrl_ops,
			V4L2_CID_GAIN, 0, 0x3f, 1, DEF_GAIN);
	priv->autowb = v4l2_ctrl_new_std(&priv->hdl, &ov6550_ctrl_ops,
			V4L2_CID_AUTO_WHITE_BALANCE, 0, 1, 1, 1);
	priv->blue = v4l2_ctrl_new_std(&priv->hdl, &ov6550_ctrl_ops,
			V4L2_CID_BLUE_BALANCE, 0, 0xff, 1, DEF_BLUE);
	priv->red = v4l2_ctrl_new_std(&priv->hdl, &ov6550_ctrl_ops,
			V4L2_CID_RED_BALANCE, 0, 0xff, 1, DEF_RED);
	v4l2_ctrl_new_std(&priv->hdl, &ov6550_ctrl_ops,
			V4L2_CID_SATURATION, 0, 0xf, 1, 0x8);
	v4l2_ctrl_new_std(&priv->hdl, &ov6550_ctrl_ops,
			V4L2_CID_HUE, 0, HUE_MASK, 1, DEF_HUE);
	v4l2_ctrl_new_std(&priv->hdl, &ov6550_ctrl_ops,
			V4L2_CID_BRIGHTNESS, 0, 0xff, 1, 0x80);
	priv->autoexposure = v4l2_ctrl_new_std_menu(&priv->hdl,
			&ov6550_ctrl_ops, V4L2_CID_EXPOSURE_AUTO,
			V4L2_EXPOSURE_MANUAL, 0, V4L2_EXPOSURE_AUTO);
	priv->exposure = v4l2_ctrl_new_std(&priv->hdl, &ov6550_ctrl_ops,
			V4L2_CID_EXPOSURE, 0, 0xff, 1, DEF_AECH);
	v4l2_ctrl_new_std(&priv->hdl, &ov6550_ctrl_ops,
			V4L2_CID_GAMMA, 0, 0xff, 1, 0x12);

	priv->subdev.ctrl_handler = &priv->hdl;
	if (priv->hdl.error)
		return priv->hdl.error;

	v4l2_ctrl_auto_cluster(2, &priv->autogain, 0, true);
	v4l2_ctrl_auto_cluster(3, &priv->autowb, 0, true);
	v4l2_ctrl_auto_cluster(2, &priv->autoexposure,
				V4L2_EXPOSURE_MANUAL, true);

	priv->rect.left	  = DEF_HSTRT << 1;
	priv->rect.top	  = DEF_VSTRT << 1;
	priv->rect.width  = W_CIF;
	priv->rect.height = H_CIF;
	priv->half_scale  = false;
	priv->code	  = MEDIA_BUS_FMT_YUYV8_2X8;
	priv->colorspace  = V4L2_COLORSPACE_JPEG;

	priv->clk = v4l2_clk_get(&client->dev, NULL);
	if (IS_ERR(priv->clk)) {
		ret = PTR_ERR(priv->clk);
		goto eclkget;
	}

	ret = ov6650_video_probe(client);
	if (ret) {
		v4l2_clk_put(priv->clk);
eclkget:
		v4l2_ctrl_handler_free(&priv->hdl);
	}

	return ret;
}

static int ov6650_remove(struct i2c_client *client)
{
	struct ov6650 *priv = to_ov6650(client);

	v4l2_clk_put(priv->clk);
	v4l2_device_unregister_subdev(&priv->subdev);
	v4l2_ctrl_handler_free(&priv->hdl);
	return 0;
}

static const struct i2c_device_id ov6650_id[] = {
	{ "ov6650", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ov6650_id);

static struct i2c_driver ov6650_i2c_driver = {
	.driver = {
		.name = "ov6650",
	},
	.probe    = ov6650_probe,
	.remove   = ov6650_remove,
	.id_table = ov6650_id,
};

module_i2c_driver(ov6650_i2c_driver);

MODULE_DESCRIPTION("SoC Camera driver for OmniVision OV6650");
MODULE_AUTHOR("Janusz Krzysztofik <jkrzyszt@tis.icnet.pl>");
MODULE_LICENSE("GPL v2");
