// SPDX-License-Identifier: GPL-2.0
// E404 - Universal SPI driven IR device driver for LOS based or OEM ROMs

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/miscdevice.h>
#include <media/rc-core.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <linux/e404_attributes.h>

#define IR_SPI_DRIVER_NAME "ir-spi"

#define IR_SPI_LED_DEFAULT_FREQUENCY 38000
#define IR_SPI_LED_MAX_BUFSIZE 32768

#define IR_SPI_MI_DEFAULT_FREQUENCY 1920000
#define IR_SPI_MI_BIT_PER_WORD 32
#define IR_SPI_MI_DATA_BUFFER 150000

__u32 p = 0;

struct ir_spi_data {
    struct spi_device *spi;
    struct regulator *regulator;
    struct mutex mutex;

    u32 freq;
    bool negated;
    u16 tx_buf[IR_SPI_LED_MAX_BUFSIZE];
    u16 pulse;
    u16 space;
    struct rc_dev *rc;

    u16 nusers;
    int power_gpio;
    int buffer_size;
    u8 *buffer;
    dev_t devt;
    struct spi_transfer xfer;
};

static struct ir_spi_data *ir_spi_data_g;

static int ir_spi_led_tx(struct rc_dev *dev,
             unsigned int *buffer, unsigned int count)
{
    int i, ret;
    unsigned int len = 0;
    struct ir_spi_data *idata = dev->priv;
    struct spi_transfer xfer;

    for (i = 0; i < count; i++) {
        unsigned int periods;
        int j;
        u16 val;

        periods = DIV_ROUND_CLOSEST(buffer[i] * idata->freq, 1000000);

        if (len + periods >= IR_SPI_LED_MAX_BUFSIZE)
            return -EINVAL;

        val = (i % 2) ? idata->space : idata->pulse;
        for (j = 0; j < periods; j++)
            idata->tx_buf[len++] = val;
    }

    memset(&xfer, 0, sizeof(xfer));
    xfer.speed_hz = idata->freq * 16;
    xfer.len = len * sizeof(*idata->tx_buf);
    xfer.tx_buf = idata->tx_buf;

    ret = regulator_enable(idata->regulator);
    if (ret)
        return ret;

    ret = spi_sync_transfer(idata->spi, &xfer, 1);
    if (ret)
        dev_err(&idata->spi->dev, "unable to deliver the signal\n");

    regulator_disable(idata->regulator);

    return ret ? ret : count;
}

static int ir_spi_led_set_tx_carrier(struct rc_dev *dev, u32 carrier)
{
    struct ir_spi_data *idata = dev->priv;
    if (!carrier)
        return -EINVAL;
    idata->freq = carrier;
    return 0;
}

static int ir_spi_led_set_duty_cycle(struct rc_dev *dev, u32 duty_cycle)
{
    struct ir_spi_data *idata = dev->priv;
    int bits = (duty_cycle * 15) / 100;

    idata->pulse = GENMASK(bits, 0);

    if (idata->negated) {
        idata->pulse = ~idata->pulse;
        idata->space = 0xffff;
    } else {
        idata->space = 0;
    }
    return 0;
}

static ssize_t ir_spi_mi_write(struct file *file,
                    const char __user *buffer, size_t length,
                    loff_t *offset)
{
    struct ir_spi_data *idata = file->private_data;
    bool please_free = false;
    int ret = 0;

    if (idata->xfer.len && (idata->xfer.len != length))
        return -EINVAL;

    mutex_lock(&idata->mutex);

    if (!idata->xfer.len) {
        idata->buffer = kmalloc(length, GFP_DMA);

        if (!idata->buffer) {
            ret = -ENOMEM;
            goto out_unlock;
        }

        idata->xfer.len = length;
        please_free = true;
    }

    if (copy_from_user(idata->buffer, buffer, length)) {
        ret = -EFAULT;
        goto out_free;
    }
    idata->xfer.tx_buf = idata->buffer;
    dev_warn(&idata->spi->dev, "xfer.len%d buffer_size %d\n",
         (int)idata->xfer.len, idata->buffer_size);
    ret = spi_sync_transfer(idata->spi, &idata->xfer, 1);
    if (ret)
        dev_err(&idata->spi->dev, "unable to deliver the signal\n");
out_free:
    if (please_free) {
        kfree(idata->buffer);
        idata->xfer.len = 0;
        idata->buffer = NULL;
    }

out_unlock:
    mutex_unlock(&idata->mutex);

    return ret ? ret : length;
}

static int ir_spi_mi_open(struct inode *inode, struct file *file)
{
    struct ir_spi_data *idata = ir_spi_data_g;

    if (unlikely(idata->nusers >= SHRT_MAX)) {
        dev_err(&idata->spi->dev, "device busy\n");
        return -EBUSY;
    }

    file->private_data = idata;

    mutex_lock(&idata->mutex);
    idata->nusers++;
    mutex_unlock(&idata->mutex);

    return 0;
}

static int ir_spi_mi_close(struct inode *inode, struct file *file)
{
    struct ir_spi_data *idata = ir_spi_data_g;

    mutex_lock(&idata->mutex);
    idata->nusers--;

    if (!idata->nusers) {
        idata->xfer.len = 0;
        idata->xfer.speed_hz = IR_SPI_MI_DEFAULT_FREQUENCY;
    }

    mutex_unlock(&idata->mutex);

    return 0;
}

static long ir_spi_mi_ioctl(struct file *file, unsigned int cmd,
                 unsigned long arg)
{
    __u32 p;
    s32 ret;
    struct ir_spi_data *idata = file->private_data;

    switch (cmd) {
    case LIRC_SET_SEND_MODE: {
        void *new;

        ret = get_user(p, (__u32 __user *)arg);
        if (ret)
            return ret;

        if (idata->xfer.len == p)
            return 0;

        if (idata->nusers > 1)
            return -EPERM;

        if (idata->xfer.len) {
            new = krealloc(idata->buffer, p, GFP_DMA);
        } else {
            if ((p > idata->buffer_size) ||
                (idata->buffer == NULL)) {
                if (idata->buffer != NULL) {
                    kfree(idata->buffer);
                    idata->buffer = NULL;
                }
                new = kmalloc(p, GFP_DMA);
                if (!new)
                    return -ENOMEM;
                idata->buffer = new;
                idata->buffer_size = p;
            }
        }

        mutex_lock(&idata->mutex);
        idata->xfer.len = p;
        mutex_unlock(&idata->mutex);

        return 0;
    }
    }

    return -EINVAL;
}

static const struct file_operations ir_spi_mi_fops = {
    .owner = THIS_MODULE,
    .write = ir_spi_mi_write,
    .open = ir_spi_mi_open,
    .release = ir_spi_mi_close,
    .llseek = noop_llseek,
    .unlocked_ioctl = ir_spi_mi_ioctl,
    .compat_ioctl = ir_spi_mi_ioctl,
};

static struct miscdevice ir_spi_mi_dev_drv = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "ir_spi",
    .fops = &ir_spi_mi_fops,
    .nodename = "ir_spi",
    .mode = 0666,
};

static int ir_spi_universal_probe(struct spi_device *spi)
{
    struct ir_spi_data *idata;
    int ret = 0;
    u8 dc;

    idata = devm_kzalloc(&spi->dev, sizeof(*idata), GFP_KERNEL);
    if (!idata)
        return -ENOMEM;

    mutex_init(&idata->mutex);
    idata->spi = spi;
    spi_set_drvdata(spi, idata);

    if (e404_data.e404_ir_type == 2) {
        dev_alert(&spi->dev, "E404: Using IR SPI MI\n");
        ir_spi_data_g = idata;
        idata->xfer.bits_per_word = IR_SPI_MI_BIT_PER_WORD;
        idata->xfer.speed_hz = IR_SPI_MI_DEFAULT_FREQUENCY;
        idata->buffer = kmalloc(IR_SPI_MI_DATA_BUFFER, GFP_DMA);
        if (!idata->buffer)
            return -ENOMEM;
        idata->buffer_size = IR_SPI_MI_DATA_BUFFER;
        misc_register(&ir_spi_mi_dev_drv);
        return 0;
    } else if (e404_data.e404_ir_type == 1) {
        dev_alert(&spi->dev, "E404: Using IR SPI LED\n");
        idata->regulator = devm_regulator_get(&spi->dev, "irda_regulator");
        if (IS_ERR(idata->regulator))
            return PTR_ERR(idata->regulator);

        idata->rc = devm_rc_allocate_device(&spi->dev, RC_DRIVER_IR_RAW_TX);
        if (!idata->rc)
            return -ENOMEM;

        idata->rc->tx_ir           = ir_spi_led_tx;
        idata->rc->s_tx_carrier    = ir_spi_led_set_tx_carrier;
        idata->rc->s_tx_duty_cycle = ir_spi_led_set_duty_cycle;
        idata->rc->device_name	   = "IR SPI";
        idata->rc->driver_name     = IR_SPI_DRIVER_NAME;
        idata->rc->priv            = idata;

        idata->negated = of_property_read_bool(spi->dev.of_node, "led-active-low");
        ret = of_property_read_u8(spi->dev.of_node, "duty-cycle", &dc);
        if (ret)
            dc = 50;
        ir_spi_led_set_duty_cycle(idata->rc, dc);

        idata->freq = IR_SPI_LED_DEFAULT_FREQUENCY;

        return devm_rc_register_device(&spi->dev, idata->rc);
    } else {
        dev_alert(&spi->dev, "E404: Unknown IR type, probe cancelled\n");
        return -ENODEV;
    }
}

static int ir_spi_universal_remove(struct spi_device *spi)
{
    struct ir_spi_data *idata = spi_get_drvdata(spi);
    if (e404_data.e404_ir_type == 2) {
        if (idata->buffer)
            kfree(idata->buffer);
        misc_deregister(&ir_spi_mi_dev_drv);
    }
    return 0;
}

static const struct of_device_id ir_spi_universal_of_match[] = {
    { .compatible = "ir-spi" },
    {},
};
MODULE_DEVICE_TABLE(of, ir_spi_universal_of_match);

static struct spi_driver ir_spi_universal_driver = {
    .probe = ir_spi_universal_probe,
    .remove = ir_spi_universal_remove,
    .driver = {
        .name = IR_SPI_DRIVER_NAME,
        .of_match_table = ir_spi_universal_of_match,
    },
};

module_spi_driver(ir_spi_universal_driver);

MODULE_AUTHOR("kvsnr113 <kvsnrprojkt113@gmail.com>");
MODULE_DESCRIPTION("E404 SPI IR driver");
MODULE_LICENSE("GPL v2");