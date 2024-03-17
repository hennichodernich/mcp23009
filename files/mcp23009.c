// SPDX-License-Identifier: GPL-2.0-only
/*
 * mcp23009.c - Support for Microchip MCP23009 as one-hot GPO
 *
 * Copyright (C) 2022 Henning Paul <hnch@gmx.net>
 *
 * Based on MCP4725.c by Peter Meerwald <pmeerw@pmeerw.net>
 *
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/of_device.h>
#include <linux/of.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include "mcp23009.h"

#define MCP23009_DRV_NAME "mcp23009"

struct mcp23009_data {
	struct i2c_client *client;
	int id;
	unsigned num_out;
	u8 out_value;
	u8 inout_mask;
};

/*
static IIO_DEVICE_ATTR(store_eeprom, S_IWUSR, NULL, mcp23009_store_eeprom, 0);
*/

static struct attribute *mcp23009_attributes[] = {
//	&iio_dev_attr_store_eeprom.dev_attr.attr,
	NULL,
};

static const struct attribute_group mcp23009_attribute_group = {
	.attrs = mcp23009_attributes,
};


enum chip_id {
	MCP23009,
};

static const struct iio_chan_spec mcp23009_channel[] = {
	[MCP23009] = {
		.type		= IIO_VOLTAGE,
		.indexed	= 1,
		.output		= 1,
		.channel	= 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int mcp23009_set_value(struct iio_dev *indio_dev, int val)
{
	struct mcp23009_data *data = iio_priv(indio_dev);
	u8 outbuf[2];
	int ret;

	if (val > 8 || val < 0)
		return -EINVAL;

	outbuf[0] = 0x09;

	if (val==0){
		outbuf[1] = 0;
	}
	else{
		outbuf[1] = 1 << (val-1);
	}

	ret = i2c_master_send(data->client, outbuf, 2);
	if (ret < 0)
		return ret;
	else if (ret != 2)
		return -EIO;
	else
		return 0;
}

static int mcp23009_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct mcp23009_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		*val = data->out_value;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 1;
		return IIO_VAL_INT;
	}
	return -EINVAL;
}

static int mcp23009_write_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int val, int val2, long mask)
{
	struct mcp23009_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if ((val>=0) && (val<=data->num_out)){
			ret = mcp23009_set_value(indio_dev, val);
			data->out_value = val;
		}
		else
		{
			ret = -EINVAL;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct iio_info mcp23009_info = {
	.read_raw = mcp23009_read_raw,
	.write_raw = mcp23009_write_raw,
	.attrs = &mcp23009_attribute_group,
};

#ifdef CONFIG_OF
static int mcp23009_probe_dt(struct device *dev,
			    struct mcp23009_platform_data *pdata)
{
	struct device_node *np = dev->of_node;
	int ret;

	if (!np)
		return -ENODEV;

	ret = of_property_read_u32(np, "num-out", &pdata->num_out);
	if(ret)
		pdata->num_out = 8;

	return 0;
}
#else
static int mcp23009_probe_dt(struct device *dev,
			    struct mcp23009_platform_data *platform_data)
{
	return -ENODEV;
}
#endif

static int mcp23009_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct mcp23009_data *data;
	struct iio_dev *indio_dev;
	struct mcp23009_platform_data *pdata, pdata_dt;
	u8 outbuf[2];
	int err,ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (indio_dev == NULL)
		return -ENOMEM;
	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->client = client;
	if (client->dev.of_node)
		data->id = (enum chip_id)of_device_get_match_data(&client->dev);
	else
		data->id = id->driver_data;
	pdata = dev_get_platdata(&client->dev);

	if (!pdata) {
		err = mcp23009_probe_dt(&client->dev, &pdata_dt);
		if (err) {
			dev_err(&client->dev,
				"invalid platform or devicetree data");
			return err;
		}
		pdata = &pdata_dt;
	}

	if((pdata->num_out) > 8){
		dev_err(&client->dev,
				"invalid devicetree data (number of outputs)");
			return -EINVAL;
	}
	data->num_out = pdata->num_out;
	data->inout_mask = ~((1UL << data->num_out) - 1) & 0xFF;

	printk(KERN_INFO "mcp23009: configured for %d outputs, mask %02x\n",data->num_out,data->inout_mask);

	outbuf[0] = 0x00;
	outbuf[1] = data->inout_mask;
	ret = i2c_master_send(data->client, outbuf, 2);
	if ((ret < 0)||(ret != 2)){
		dev_err(&client->dev, "failed to configure MCP23009");
		return -EIO;
	}
	
	outbuf[0] = 0x06;
	outbuf[1] = 0xff;
	ret = i2c_master_send(data->client, outbuf, 2);
	if ((ret < 0)||(ret != 2)){
		dev_err(&client->dev, "failed to configure MCP23009");
		return -EIO;
	}

	indio_dev->dev.parent = &client->dev;
	indio_dev->name = id->name;
	indio_dev->info = &mcp23009_info;
	indio_dev->channels = &mcp23009_channel[id->driver_data];
	indio_dev->num_channels = 1;
	indio_dev->modes = INDIO_DIRECT_MODE;

	err = iio_device_register(indio_dev);
	if (err)
        return err;

	return 0;
}

static int mcp23009_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct mcp23009_data *data = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);

	return 0;
}

static const struct i2c_device_id mcp23009_id[] = {
	{ "mcp23009", MCP23009 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mcp23009_id);

#ifdef CONFIG_OF
static const struct of_device_id mcp23009_of_match[] = {
	{
		.compatible = "microchip,mcp23009",
		.data = (void *)MCP23009
	},
	{ }
};
MODULE_DEVICE_TABLE(of, mcp23009_of_match);
#endif

static struct i2c_driver mcp23009_driver = {
	.driver = {
		.name	= MCP23009_DRV_NAME,
		.of_match_table = of_match_ptr(mcp23009_of_match),
//		.pm	= &mcp23009_pm_ops,
	},
	.probe		= mcp23009_probe,
	.remove		= mcp23009_remove,
	.id_table	= mcp23009_id,
};
module_i2c_driver(mcp23009_driver);

MODULE_AUTHOR("Henning Paul <hnch@gmx.net>");
MODULE_DESCRIPTION("MCP23009 one-hot GPIO");
MODULE_LICENSE("GPL");
