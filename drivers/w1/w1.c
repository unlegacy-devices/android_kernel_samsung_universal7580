/*
 *	w1.c
 *
 * Copyright (c) 2004 Evgeniy Polyakov <zbr@ioremap.net>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/atomic.h>
#include <linux/input.h>
#include <linux/pm_qos.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>

#include "w1.h"
#include "w1_log.h"
#include "w1_int.h"
#include "w1_family.h"
#include "w1_netlink.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Evgeniy Polyakov <zbr@ioremap.net>");
MODULE_DESCRIPTION("Driver for 1-wire Dallas network protocol.");

#if defined(CONFIG_W1_SLAVE_DS28EL15) || defined(CONFIG_W1_SLAVE_DS28EL35)
static int w1_timeout = 2;
int w1_max_slave_count = 1;
#ifdef CONFIG_W1_KTHREAD
int w1_max_slave_ttl = 1;
#else
int w1_max_slave_ttl = 1;
#endif

static struct w1_master *master_dev = NULL;
extern int w1_ds28el15_verifymac(struct w1_slave *sl);
extern int w1_verification, w1_id, w1_color, w1_model, detect;
extern bool w1_is_suspended;
extern char w1_g_sn[14];
#ifdef CONFIG_W1_CF
extern int w1_cf_node;
#endif	/* CONFIG_W1_CF */
#else
static int w1_timeout = 10;
int w1_max_slave_count = 10;
int w1_max_slave_ttl = 10;
#endif

static struct pm_qos_request mif_handle;

module_param_named(timeout, w1_timeout, int, 0);
module_param_named(max_slave_count, w1_max_slave_count, int, 0);
module_param_named(slave_ttl, w1_max_slave_ttl, int, 0);

DEFINE_MUTEX(w1_mlock);
LIST_HEAD(w1_masters);

static int w1_attach_slave_device(struct w1_master *dev, struct w1_reg_num *rn);

static int w1_master_match(struct device *dev, struct device_driver *drv)
{
	return 1;
}

static int w1_master_probe(struct device *dev)
{
	return -ENODEV;
}

static void w1_master_release(struct device *dev)
{
	struct w1_master *md = dev_to_w1_master(dev);

	dev_dbg(dev, "%s: Releasing %s.\n", __func__, md->name);
	memset(md, 0, sizeof(struct w1_master) + sizeof(struct w1_bus_master));
	kfree(md);
}

static void w1_slave_release(struct device *dev)
{
	struct w1_slave *sl = dev_to_w1_slave(dev);

	dev_dbg(dev, "%s: Releasing %s.\n", __func__, sl->name);
	printk(KERN_ERR "%s: Releasing %s.\n", __func__, sl->name);

	/* add for sending uevent */
	pr_info("%s: uevent send 0\n", __func__);
	input_report_switch(sl->master->bus_master->input, SW_W1, 0);
	input_sync(sl->master->bus_master->input);
	/* add for sending uevent */

	while (atomic_read(&sl->refcnt)) {
		dev_dbg(dev, "Waiting for %s to become free: refcnt=%d.\n",
				sl->name, atomic_read(&sl->refcnt));
		if (msleep_interruptible(1000))
			flush_signals(current);
	}

	w1_family_put(sl->family);
	sl->master->slave_count--;

	complete(&sl->released);
}

static ssize_t w1_slave_read_name(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w1_slave *sl = dev_to_w1_slave(dev);

	return sprintf(buf, "%s\n", sl->name);
}

static ssize_t w1_slave_read_id(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct w1_slave *sl = dev_to_w1_slave(dev);
	ssize_t count = sizeof(sl->reg_num);

	memcpy(buf, (u8 *)&sl->reg_num, count);
	return count;
}

static struct device_attribute w1_slave_attr_name =
	__ATTR(name, S_IRUGO, w1_slave_read_name, NULL);
static struct device_attribute w1_slave_attr_id =
	__ATTR(id, S_IRUGO, w1_slave_read_id, NULL);

/* Default family */

static ssize_t w1_default_write(struct file *filp, struct kobject *kobj,
				struct bin_attribute *bin_attr,
				char *buf, loff_t off, size_t count)
{
	struct w1_slave *sl = kobj_to_w1_slave(kobj);

	mutex_lock(&sl->master->mutex);
	if (w1_reset_select_slave(sl)) {
		count = 0;
		goto out_up;
	}

	w1_write_block(sl->master, buf, count);

out_up:
	mutex_unlock(&sl->master->mutex);
	return count;
}

static ssize_t w1_default_read(struct file *filp, struct kobject *kobj,
			       struct bin_attribute *bin_attr,
			       char *buf, loff_t off, size_t count)
{
	struct w1_slave *sl = kobj_to_w1_slave(kobj);

	mutex_lock(&sl->master->mutex);
	w1_read_block(sl->master, buf, count);
	mutex_unlock(&sl->master->mutex);
	return count;
}

static struct bin_attribute w1_default_attr = {
      .attr = {
              .name = "rw",
              .mode = S_IRUGO | S_IWUSR,
      },
      .size = PAGE_SIZE,
      .read = w1_default_read,
      .write = w1_default_write,
};

static int w1_default_add_slave(struct w1_slave *sl)
{
	return sysfs_create_bin_file(&sl->dev.kobj, &w1_default_attr);
}

static void w1_default_remove_slave(struct w1_slave *sl)
{
	sysfs_remove_bin_file(&sl->dev.kobj, &w1_default_attr);
}

static struct w1_family_ops w1_default_fops = {
	.add_slave	= w1_default_add_slave,
	.remove_slave	= w1_default_remove_slave,
};

static struct w1_family w1_default_family = {
	.fops = &w1_default_fops,
};

static int w1_uevent(struct device *dev, struct kobj_uevent_env *env);

static struct bus_type w1_bus_type = {
	.name = "w1",
	.match = w1_master_match,
	.uevent = w1_uevent,
};

struct device_driver w1_master_driver = {
	.name = "w1_master_driver",
	.bus = &w1_bus_type,
	.probe = w1_master_probe,
};

struct device w1_master_device = {
	.parent = NULL,
	.bus = &w1_bus_type,
	.init_name = "w1 bus master",
	.driver = &w1_master_driver,
	.release = &w1_master_release
};

static struct device_driver w1_slave_driver = {
	.name = "w1_slave_driver",
	.bus = &w1_bus_type,
};

#if 0
struct device w1_slave_device = {
	.parent = NULL,
	.bus = &w1_bus_type,
	.init_name = "w1 bus slave",
	.driver = &w1_slave_driver,
	.release = &w1_slave_release
};
#endif  /*  0  */

static ssize_t w1_master_attribute_show_name(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w1_master *md = dev_to_w1_master(dev);
	ssize_t count;

	mutex_lock(&md->mutex);
	count = sprintf(buf, "%s\n", md->name);
	mutex_unlock(&md->mutex);

	return count;
}

static ssize_t w1_master_attribute_store_search(struct device * dev,
						struct device_attribute *attr,
						const char * buf, size_t count)
{
	long tmp;
	struct w1_master *md = dev_to_w1_master(dev);

	if (strict_strtol(buf, 0, &tmp) == -EINVAL)
		return -EINVAL;

	mutex_lock(&md->mutex);
	md->search_count = tmp;
	mutex_unlock(&md->mutex);
	wake_up_process(md->thread);

	return count;
}

static ssize_t w1_master_attribute_show_search(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct w1_master *md = dev_to_w1_master(dev);
	ssize_t count;

	mutex_lock(&md->mutex);
	count = sprintf(buf, "%d\n", md->search_count);
	mutex_unlock(&md->mutex);

	return count;
}

static ssize_t w1_master_attribute_store_pullup(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t count)
{
	long tmp;
	struct w1_master *md = dev_to_w1_master(dev);

	if (strict_strtol(buf, 0, &tmp) == -EINVAL)
		return -EINVAL;

	mutex_lock(&md->mutex);
	md->enable_pullup = tmp;
	mutex_unlock(&md->mutex);
	wake_up_process(md->thread);

	return count;
}

static ssize_t w1_master_attribute_show_pullup(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct w1_master *md = dev_to_w1_master(dev);
	ssize_t count;

	mutex_lock(&md->mutex);
	count = sprintf(buf, "%d\n", md->enable_pullup);
	mutex_unlock(&md->mutex);

	return count;
}

static ssize_t w1_master_attribute_show_pointer(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w1_master *md = dev_to_w1_master(dev);
	ssize_t count;

	mutex_lock(&md->mutex);
	count = sprintf(buf, "0x%p\n", md->bus_master);
	mutex_unlock(&md->mutex);
	return count;
}

static ssize_t w1_master_attribute_show_timeout(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t count;
	count = sprintf(buf, "%d\n", w1_timeout);
	return count;
}

static ssize_t w1_master_attribute_show_max_slave_count(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w1_master *md = dev_to_w1_master(dev);
	ssize_t count;

	mutex_lock(&md->mutex);
	count = sprintf(buf, "%d\n", md->max_slave_count);
	mutex_unlock(&md->mutex);
	return count;
}

static ssize_t w1_master_attribute_show_attempts(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w1_master *md = dev_to_w1_master(dev);
	ssize_t count;

	mutex_lock(&md->mutex);
	count = sprintf(buf, "%lu\n", md->attempts);
	mutex_unlock(&md->mutex);
	return count;
}

static ssize_t w1_master_attribute_show_slave_count(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w1_master *md = dev_to_w1_master(dev);
	ssize_t count;

	mutex_lock(&md->mutex);
	count = sprintf(buf, "%d\n", md->slave_count);
	mutex_unlock(&md->mutex);
	return count;
}

static ssize_t w1_master_attribute_show_slaves(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct w1_master *md = dev_to_w1_master(dev);
	int c = PAGE_SIZE;

	mutex_lock(&md->mutex);

	if (md->slave_count == 0)
		c -= snprintf(buf + PAGE_SIZE - c, c, "not found.\n");
	else {
		struct list_head *ent, *n;
		struct w1_slave *sl;

		list_for_each_safe(ent, n, &md->slist) {
			sl = list_entry(ent, struct w1_slave, w1_slave_entry);

			c -= snprintf(buf + PAGE_SIZE - c, c, "%s\n", sl->name);
		}
	}

	mutex_unlock(&md->mutex);

	return PAGE_SIZE - c;
}

static ssize_t w1_master_attribute_show_add(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int c = PAGE_SIZE;
	c -= snprintf(buf+PAGE_SIZE - c, c,
		"write device id xx-xxxxxxxxxxxx to add slave\n");
	return PAGE_SIZE - c;
}

static int w1_atoreg_num(struct device *dev, const char *buf, size_t count,
	struct w1_reg_num *rn)
{
	unsigned int family;
	unsigned long long id;
	int i;
	u64 rn64_le;

	/* The CRC value isn't read from the user because the sysfs directory
	 * doesn't include it and most messages from the bus search don't
	 * print it either.  It would be unreasonable for the user to then
	 * provide it.
	 */
	const char *error_msg = "bad slave string format, expecting "
		"ff-dddddddddddd\n";

	if (buf[2] != '-') {
		dev_err(dev, "%s", error_msg);
		return -EINVAL;
	}
	i = sscanf(buf, "%02x-%012llx", &family, &id);
	if (i != 2) {
		dev_err(dev, "%s", error_msg);
		return -EINVAL;
	}
	rn->family = family;
	rn->id = id;

	rn64_le = cpu_to_le64(*(u64 *)rn);
	rn->crc = w1_calc_crc8((u8 *)&rn64_le, 7);

#if 0
	dev_info(dev, "With CRC device is %02x.%012llx.%02x.\n",
		  rn->family, (unsigned long long)rn->id, rn->crc);
#endif

	return 0;
}

/* Searches the slaves in the w1_master and returns a pointer or NULL.
 * Note: must hold the mutex
 */

static bool w1_kthread_flag = true;
static bool w1_check_id_flag = false;

static struct w1_slave *w1_slave_search_device(struct w1_master *dev,
	struct w1_reg_num *rn)
{
	struct w1_slave *sl = NULL;
	list_for_each_entry(sl, &dev->slist, w1_slave_entry) {
		if (w1_check_id_flag) {
			pr_info("%s: w1 check_id_flag true\n", __func__);
			w1_check_id_flag = false;

			if (sl->reg_num.family == rn->family &&
					sl->reg_num.id == rn->id &&
					sl->reg_num.crc == rn->crc) {
				return sl;
			} else {
				printk("%s: fid(%x) id(%llx) crc(%x)\n",
					__func__,
					(unsigned int) rn->family,
					(unsigned long long) rn->id,
					(unsigned int) rn->crc);

				w1_slave_detach(sl);
				return NULL;
			}
		} else {
			return sl;
		}
	}
	return NULL;
}

static ssize_t w1_master_attribute_store_add(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct w1_master *md = dev_to_w1_master(dev);
	struct w1_reg_num rn = {0,};
	struct w1_slave *sl;
	ssize_t result = count;

	if (w1_atoreg_num(dev, buf, count, &rn))
		return -EINVAL;

	mutex_lock(&md->mutex);
	sl = w1_slave_search_device(md, &rn);
	/* It would be nice to do a targeted search one the one-wire bus
	 * for the new device to see if it is out there or not.  But the
	 * current search doesn't support that.
	 */
	if (sl) {
		dev_info(dev, "Device %s already exists\n", sl->name);
		result = -EINVAL;
	} else {
		w1_attach_slave_device(md, &rn);
	}
	mutex_unlock(&md->mutex);

	return result;
}

static ssize_t w1_master_attribute_show_remove(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int c = PAGE_SIZE;
	c -= snprintf(buf+PAGE_SIZE - c, c,
		"write device id xx-xxxxxxxxxxxx to remove slave\n");
	return PAGE_SIZE - c;
}

static ssize_t w1_master_attribute_store_remove(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct w1_master *md = dev_to_w1_master(dev);
	struct w1_reg_num rn ={0,};
	struct w1_slave *sl;
	ssize_t result = count;

	if (w1_atoreg_num(dev, buf, count, &rn))
		return -EINVAL;

	mutex_lock(&md->mutex);
	sl = w1_slave_search_device(md, &rn);
	if (sl) {
		w1_slave_detach(sl);
	} else {
		dev_info(dev, "Device %02x-%012llx doesn't exists\n", rn.family,
			(unsigned long long)rn.id);
		result = -EINVAL;
	}
	mutex_unlock(&md->mutex);

	return result;
}

static ssize_t w1_master_attribute_show_verify_mac(struct device *dev, struct device_attribute *attr, char *buf)
{
	int result = -1;

	struct w1_master *md = dev_to_w1_master(dev);

#if defined(CONFIG_W1_WORKQUEUE)
	cancel_delayed_work_sync(&w1_gdev->w1_dwork);

	schedule_delayed_work(&w1_gdev->w1_dwork, 0);

	msleep(10);
#endif /* W1_WORKQUEUE */

#if (defined(CONFIG_SEC_FACTORY) && defined(CONFIG_W1_DETECT_PIN)) || defined(CONFIG_W1_DETECT_HALL)
	w1_master_search();
#endif
	/* there is a slave or not */
	if (md->slave_count > 0)
		result = 0;
	else
		result = -1;

	pr_info("COVER ACT 1 %s %d, slave_count: %d\n", __func__, result, md->slave_count);

	return sprintf(buf, "%d\n", result);
}

#ifdef CONFIG_W1_CF
static ssize_t w1_master_attribute_show_cf(struct device *dev, struct device_attribute *attr, char *buf)
{
	pr_info("%s: cf_node = %d\n", __func__, w1_cf_node);
	return sprintf(buf, "%d\n", w1_cf_node);
}
#endif	/* CONFIG_W1_CF */

static ssize_t w1_master_attribute_show_check_id(struct device *dev, struct device_attribute *attr, char *buf)
{
	pr_info("COVER ACT 2 %s %d\n", __func__, w1_id);
	return sprintf(buf, "%d\n", w1_id);
}

static ssize_t w1_master_attribute_show_check_color(struct device *dev, struct device_attribute *attr, char *buf)
{
	pr_info("COVER ACT 2 %s %d\n", __func__, w1_color);
	return sprintf(buf, "%d\n", w1_color);
}

static ssize_t w1_master_attribute_show_check_model(struct device *dev, struct device_attribute *attr, char *buf)
{
	pr_info("COVER ACT 2 %s %d\n", __func__, w1_model);
	return sprintf(buf, "%d\n", w1_model);
}

static ssize_t w1_master_attribute_show_check_detect(struct device *dev, struct device_attribute *attr, char *buf)
{
#if defined(CONFIG_W1_SLAVE_DS28EL35)
	detect =  w1_read_detect_state();
#else
	detect = -1;
#endif
	pr_info("COVER ACT 2 %s %d\n", __func__, detect);
	return sprintf(buf, "%d\n", detect);
}

static ssize_t w1_master_attribute_show_check_sn(struct device *dev, struct device_attribute *attr, char *buf)
{
	pr_info("COVER ACT 3 %s\n", w1_g_sn);

	if (w1_g_sn[0])
		return snprintf(buf, 15, "%s\n", w1_g_sn);
	else
		return snprintf(buf, 1, "%s", "");
}

/* need to add */

#define W1_MASTER_ATTR_RO(_name, _mode)				\
	struct device_attribute w1_master_attribute_##_name =	\
		__ATTR(w1_master_##_name, _mode,		\
		       w1_master_attribute_show_##_name, NULL)

#define W1_MASTER_ATTR_RW(_name, _mode)				\
	struct device_attribute w1_master_attribute_##_name =	\
		__ATTR(w1_master_##_name, _mode,		\
		       w1_master_attribute_show_##_name,	\
		       w1_master_attribute_store_##_name)

static W1_MASTER_ATTR_RO(name, S_IRUGO);
static W1_MASTER_ATTR_RO(slaves, S_IRUGO);
static W1_MASTER_ATTR_RO(slave_count, S_IRUGO);
static W1_MASTER_ATTR_RO(max_slave_count, S_IRUGO);
static W1_MASTER_ATTR_RO(attempts, S_IRUGO);
static W1_MASTER_ATTR_RO(timeout, S_IRUGO);
static W1_MASTER_ATTR_RO(pointer, S_IRUGO);
static W1_MASTER_ATTR_RW(search, S_IRUGO | S_IWUSR | S_IWGRP);
static W1_MASTER_ATTR_RW(pullup, S_IRUGO | S_IWUSR | S_IWGRP);
static W1_MASTER_ATTR_RW(add, S_IRUGO | S_IWUSR | S_IWGRP);
static W1_MASTER_ATTR_RW(remove, S_IRUGO | S_IWUSR | S_IWGRP);
static W1_MASTER_ATTR_RO(verify_mac, S_IRUGO);
#ifdef CONFIG_W1_CF
static W1_MASTER_ATTR_RO(cf, S_IRUGO);
#endif	/* CONFIG_W1_CF */
static W1_MASTER_ATTR_RO(check_id, S_IRUGO);
static W1_MASTER_ATTR_RO(check_color, S_IRUGO);
static W1_MASTER_ATTR_RO(check_model, S_IRUGO);
static W1_MASTER_ATTR_RO(check_detect, S_IRUGO);
static W1_MASTER_ATTR_RO(check_sn, S_IRUGO);

static struct attribute *w1_master_default_attrs[] = {
	&w1_master_attribute_name.attr,
	&w1_master_attribute_slaves.attr,
	&w1_master_attribute_slave_count.attr,
	&w1_master_attribute_max_slave_count.attr,
	&w1_master_attribute_attempts.attr,
	&w1_master_attribute_timeout.attr,
	&w1_master_attribute_pointer.attr,
	&w1_master_attribute_search.attr,
	&w1_master_attribute_pullup.attr,
	&w1_master_attribute_add.attr,
	&w1_master_attribute_remove.attr,
	&w1_master_attribute_verify_mac.attr,
#ifdef CONFIG_W1_CF
	&w1_master_attribute_cf.attr,
#endif	/* CONFIG_W1_CF */
	&w1_master_attribute_check_id.attr,
	&w1_master_attribute_check_color.attr,
	&w1_master_attribute_check_model.attr,
	&w1_master_attribute_check_sn.attr,
	NULL
};

static struct attribute_group w1_master_defattr_group = {
	.attrs = w1_master_default_attrs,
};

int w1_create_master_attributes(struct w1_master *master)
{
	return sysfs_create_group(&master->dev.kobj, &w1_master_defattr_group);
}

void w1_destroy_master_attributes(struct w1_master *master)
{
	sysfs_remove_group(&master->dev.kobj, &w1_master_defattr_group);
}

int w1_create_additional_attributes(struct w1_master *master)
{
	return sysfs_create_file(&master->dev.kobj,
			&w1_master_attribute_check_detect.attr);
}

void w1_destroy_additional_attributes(struct w1_master *master)
{
	sysfs_remove_file(&master->dev.kobj,
			&w1_master_attribute_check_detect.attr);
}

#ifdef CONFIG_HOTPLUG
static int w1_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct w1_master *md = NULL;
	struct w1_slave *sl = NULL;
	char *event_owner, *name;
	int err = 0;

	if (dev->driver == &w1_master_driver) {
		md = container_of(dev, struct w1_master, dev);
		event_owner = "master";
		name = md->name;
#if defined(CONFIG_W1_SLAVE_DS28EL15) || defined(CONFIG_W1_SLAVE_DS28EL35)
		master_dev = md; //container_of(dev, struct w1_master, dev);
		printk(KERN_ERR "%s master_dev name = %s\n", __func__, master_dev->name);
#endif
	} else if (dev->driver == &w1_slave_driver) {
		sl = container_of(dev, struct w1_slave, dev);
		event_owner = "slave";
		name = sl->name;
	} else {
		dev_dbg(dev, "Unknown event.\n");
		return -EINVAL;
	}

	dev_dbg(dev, "Hotplug event for %s %s, bus_id=%s.\n",
			event_owner, name, dev_name(dev));

	if (dev->driver != &w1_slave_driver || !sl)
		goto end;

	err = add_uevent_var(env, "W1_FID=%02X", sl->reg_num.family);
	if (err)
		goto end;

	err = add_uevent_var(env, "W1_SLAVE_ID=%024LX",
			     (unsigned long long)sl->reg_num.id);
end:
	return err;
}
#else
static int w1_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	return 0;
}
#endif

#if defined(CONFIG_W1_SLAVE_DS28EL15) || defined(CONFIG_W1_SLAVE_DS28EL35)
static void w1_search_process(struct w1_master *dev, u8 search_type);

void w1_master_search(void)
{
	w1_search_process(master_dev, W1_SEARCH);
}
#endif

static int __w1_attach_slave_device(struct w1_slave *sl)
{
	int err;

	sl->dev.parent = &sl->master->dev;
	sl->dev.driver = &w1_slave_driver;
	sl->dev.bus = &w1_bus_type;
	sl->dev.release = &w1_slave_release;

	dev_set_name(&sl->dev, "%02x-%012llx",
		 (unsigned int) sl->reg_num.family,
		 (unsigned long long) sl->reg_num.id);
	snprintf(&sl->name[0], sizeof(sl->name),
		 "%02x-%012llx",
		 (unsigned int) sl->reg_num.family,
		 (unsigned long long) sl->reg_num.id);

	dev_dbg(&sl->dev, "%s: registering %s as %p.\n", __func__,
		dev_name(&sl->dev), sl);

	err = device_register(&sl->dev);
	if (err < 0) {
		dev_err(&sl->dev,
			"Device registration [%s] failed. err=%d\n",
			dev_name(&sl->dev), err);
		return err;
	}

	sl->master->slave_count++;

	/* Create "name" entry */
	err = device_create_file(&sl->dev, &w1_slave_attr_name);
	if (err < 0) {
		dev_err(&sl->dev,
			"sysfs file creation for [%s] failed. err=%d\n",
			dev_name(&sl->dev), err);
		goto out_unreg;
	}

	/* Create "id" entry */
	err = device_create_file(&sl->dev, &w1_slave_attr_id);
	if (err < 0) {
		dev_err(&sl->dev,
			"sysfs file creation for [%s] failed. err=%d\n",
			dev_name(&sl->dev), err);
		goto out_rem1;
	}

	/* if the family driver needs to initialize something... */
	if (sl->family->fops && sl->family->fops->add_slave &&
	    ((err = sl->family->fops->add_slave(sl)) < 0)) {
		dev_err(&sl->dev,
			"sysfs file creation for [%s] failed. err=%d\n",
			dev_name(&sl->dev), err);
		goto out_rem2;
	}

	list_add_tail(&sl->w1_slave_entry, &sl->master->slist);

	return 0;

out_rem2:
	device_remove_file(&sl->dev, &w1_slave_attr_id);
out_rem1:
	device_remove_file(&sl->dev, &w1_slave_attr_name);
out_unreg:
	device_unregister(&sl->dev);
	return err;
}

static int w1_attach_slave_device(struct w1_master *dev, struct w1_reg_num *rn)
{
	struct w1_slave *sl;
	struct w1_family *f;
	int err;
	struct w1_netlink_msg msg;

	sl = kzalloc(sizeof(struct w1_slave), GFP_KERNEL);
	if (!sl) {
		dev_err(&dev->dev,
			 "%s: failed to allocate new slave device.\n",
			 __func__);
		return -ENOMEM;
	}


	sl->owner = THIS_MODULE;
	sl->master = dev;
	set_bit(W1_SLAVE_ACTIVE, (long *)&sl->flags);

	memset(&msg, 0, sizeof(msg));
	memcpy(&sl->reg_num, rn, sizeof(sl->reg_num));
	atomic_set(&sl->refcnt, 0);
	init_completion(&sl->released);
	/* slave modules need to be loaded in a context with unlocked mutex */
	mutex_unlock(&dev->mutex);
	request_module("w1-family-0x%0x", rn->family);
	mutex_lock(&dev->mutex);

	spin_lock(&w1_flock);
	f = w1_family_registered(rn->family);
	if (!f) {
		f= &w1_default_family;
		dev_info(&dev->dev, "Family %x for %02x.%012llx.%02x is not registered.\n",
			  rn->family, rn->family,
			  (unsigned long long)rn->id, rn->crc);
	}
	__w1_family_get(f);
	spin_unlock(&w1_flock);

	sl->family = f;


	err = __w1_attach_slave_device(sl);
	if (err < 0) {
		dev_err(&dev->dev, "%s: Attaching %s failed.\n", __func__,
			 sl->name);
		w1_family_put(sl->family);
		kfree(sl);
		return err;
	}

	sl->ttl = dev->slave_ttl;

	pr_info("%s: uevent send 1\n", __func__);
	input_report_switch(sl->master->bus_master->input, SW_W1, 1);
	input_sync(sl->master->bus_master->input);

	memcpy(msg.id.id, rn, sizeof(msg.id));
	msg.type = W1_SLAVE_ADD;
	w1_netlink_send(dev, &msg);

	return 0;
}

void w1_slave_detach(struct w1_slave *sl)
{
	struct w1_netlink_msg msg;

	dev_dbg(&sl->dev, "%s: detaching %s [%p].\n", __func__, sl->name, sl);
	printk(KERN_ERR "%s: detaching %s [%p].\n", __func__, sl->name, sl);

	list_del(&sl->w1_slave_entry);

	if (sl->family->fops && sl->family->fops->remove_slave)
		sl->family->fops->remove_slave(sl);

	memset(&msg, 0, sizeof(msg));
	memcpy(msg.id.id, &sl->reg_num, sizeof(msg.id));
	msg.type = W1_SLAVE_REMOVE;
	w1_netlink_send(sl->master, &msg);

	device_remove_file(&sl->dev, &w1_slave_attr_id);
	device_remove_file(&sl->dev, &w1_slave_attr_name);
	device_unregister(&sl->dev);
	wait_for_completion(&sl->released);
	kfree(sl);
}

struct w1_master *w1_search_master_id(u32 id)
{
	struct w1_master *dev;
	int found = 0;

	mutex_lock(&w1_mlock);
	list_for_each_entry(dev, &w1_masters, w1_master_entry) {
		if (dev->id == id) {
			found = 1;
			atomic_inc(&dev->refcnt);
			break;
		}
	}
	mutex_unlock(&w1_mlock);

	return (found)?dev:NULL;
}

struct w1_slave *w1_search_slave(struct w1_reg_num *id)
{
	struct w1_master *dev;
	struct w1_slave *sl = NULL;
	int found = 0;

	mutex_lock(&w1_mlock);
	list_for_each_entry(dev, &w1_masters, w1_master_entry) {
		mutex_lock(&dev->mutex);
		list_for_each_entry(sl, &dev->slist, w1_slave_entry) {
			if (sl->reg_num.family == id->family &&
					sl->reg_num.id == id->id &&
					sl->reg_num.crc == id->crc) {
				found = 1;
				atomic_inc(&dev->refcnt);
				atomic_inc(&sl->refcnt);
				break;
			}
		}
		mutex_unlock(&dev->mutex);

		if (found)
			break;
	}
	mutex_unlock(&w1_mlock);

	return (found)?sl:NULL;
}

void w1_reconnect_slaves(struct w1_family *f, int attach)
{
	struct w1_slave *sl, *sln;
	struct w1_master *dev;

	mutex_lock(&w1_mlock);
	list_for_each_entry(dev, &w1_masters, w1_master_entry) {
		dev_dbg(&dev->dev, "Reconnecting slaves in device %s "
			"for family %02x.\n", dev->name, f->fid);
		mutex_lock(&dev->mutex);
		list_for_each_entry_safe(sl, sln, &dev->slist, w1_slave_entry) {
			/* If it is a new family, slaves with the default
			 * family driver and are that family will be
			 * connected.  If the family is going away, devices
			 * matching that family are reconneced.
			 */
			if ((attach && sl->family->fid == W1_FAMILY_DEFAULT
				&& sl->reg_num.family == f->fid) ||
				(!attach && sl->family->fid == f->fid)) {
				struct w1_reg_num rn;

				memcpy(&rn, &sl->reg_num, sizeof(rn));
				w1_slave_detach(sl);

				w1_attach_slave_device(dev, &rn);
			}
		}
		dev_dbg(&dev->dev, "Reconnecting slaves in device %s "
			"has been finished.\n", dev->name);
		mutex_unlock(&dev->mutex);
	}
	mutex_unlock(&w1_mlock);
}

void w1_slave_found(struct w1_master *dev, u64 rn)
{
	struct w1_slave *sl;
	struct w1_reg_num *tmp;
	u64 rn_le = cpu_to_le64(rn);

	atomic_inc(&dev->refcnt);

	tmp = (struct w1_reg_num *) &rn;

	sl = w1_slave_search_device(dev, tmp);
	if (sl) {
		set_bit(W1_SLAVE_ACTIVE, (long *)&sl->flags);
#if 0	/* if using polling, it will log too much */
		printk(KERN_ERR "%s : (ELSE) fid(%x) id(%llx) crc(%x)\n", __func__,
			(unsigned int) tmp->family,
			(unsigned long long) tmp->id,
			(unsigned int) tmp->crc);
#endif
	} else {
		printk("%s: fid(%x) id(%llx) crc(%x)\n", __func__,
			(unsigned int) tmp->family,
			(unsigned long long) tmp->id,
			(unsigned int) tmp->crc);

		if (rn && tmp->crc == w1_calc_crc8((u8 *)&rn_le, 7))
			w1_attach_slave_device(dev, tmp);
	}

	atomic_dec(&dev->refcnt);
}

/**
 * Performs a ROM Search & registers any devices found.
 * The 1-wire search is a simple binary tree search.
 * For each bit of the address, we read two bits and write one bit.
 * The bit written will put to sleep all devies that don't match that bit.
 * When the two reads differ, the direction choice is obvious.
 * When both bits are 0, we must choose a path to take.
 * When we can scan all 64 bits without having to choose a path, we are done.
 *
 * See "Application note 187 1-wire search algorithm" at www.maxim-ic.com
 *
 * @dev        The master device to search
 * @cb         Function to call when a device is found
 */
void w1_search(struct w1_master *dev, u8 search_type, w1_slave_found_callback cb)
{
	u64 last_rn, rn, tmp64;
	int i, slave_count = 0;
	int last_zero, last_device;
	int search_bit, desc_bit;
	u8  triplet_ret = 0;

	search_bit = 0;
	rn = last_rn = 0;
	last_device = 0;
	last_zero = -1;

	desc_bit = 64;

	while ( !last_device && (slave_count++ < dev->max_slave_count) ) {
		last_rn = rn;
		rn = 0;

		/*
		 * Reset bus and all 1-wire device state machines
		 * so they can respond to our requests.
		 *
		 * Return 0 - device(s) present, 1 - no devices present.
		 */
		mutex_lock(&dev->bus_mutex);
		if (w1_reset_bus(dev)) {
			mutex_unlock(&dev->bus_mutex);
			dev_dbg(&dev->dev, "No devices present on the wire.\n");
			break;
		}

		/* Do fast search on single slave bus */
		if (dev->max_slave_count == 1) {
			int rv;
			w1_write_8(dev, W1_READ_ROM);
			rv = w1_read_block(dev, (u8 *)&rn, 8);
			mutex_unlock(&dev->bus_mutex);

			if (rv == 8 && rn)
				cb(dev, rn);

			break;
		}

		/* Start the search */
		w1_write_8(dev, search_type);
		for (i = 0; i < 64; ++i) {
			/* Determine the direction/search bit */
			if (i == desc_bit)
				search_bit = 1;	  /* took the 0 path last time, so take the 1 path */
			else if (i > desc_bit)
				search_bit = 0;	  /* take the 0 path on the next branch */
			else
				search_bit = ((last_rn >> i) & 0x1);

			/** Read two bits and write one bit */
			triplet_ret = w1_triplet(dev, search_bit);

			/* quit if no device responded */
			if ( (triplet_ret & 0x03) == 0x03 ) {
				pr_info("%s: quit if no device responded\n", __func__);
				break;
			}

			/* If both directions were valid, and we took the 0 path... */
			if (triplet_ret == 0)
				last_zero = i;

			/* extract the direction taken & update the device number */
			tmp64 = (triplet_ret >> 2);
			rn |= (tmp64 << i);

			/* ensure we're called from kthread and not by netlink callback */
			if (!dev->priv && kthread_should_stop()) {
				mutex_unlock(&dev->bus_mutex);
				dev_dbg(&dev->dev, "Abort w1_search\n");
				return;
			}
		}
		mutex_unlock(&dev->bus_mutex);

		if ( (triplet_ret & 0x03) != 0x03 ) {
			if ( (desc_bit == last_zero) || (last_zero < 0))
				last_device = 1;
			desc_bit = last_zero;
			cb(dev, rn);
		}
	}
}

void w1_search_process_cb(struct w1_master *dev, u8 search_type,
	w1_slave_found_callback cb)
{
	struct w1_slave *sl, *sln;

	list_for_each_entry(sl, &dev->slist, w1_slave_entry)
		clear_bit(W1_SLAVE_ACTIVE, (long *)&sl->flags);

	w1_search_devices(dev, search_type, cb);

	list_for_each_entry_safe(sl, sln, &dev->slist, w1_slave_entry) {
		if (!test_bit(W1_SLAVE_ACTIVE, (unsigned long *)&sl->flags) && !--sl->ttl)
			w1_slave_detach(sl);
		else if (test_bit(W1_SLAVE_ACTIVE, (unsigned long *)&sl->flags))
			sl->ttl = dev->slave_ttl;
	}

	if (dev->search_count > 0)
		dev->search_count--;
}

static void w1_search_process(struct w1_master *dev, u8 search_type)
{
	w1_search_process_cb(dev, search_type, w1_slave_found);
}

int w1_process(void *data)
{
	struct w1_master *dev = (struct w1_master *) data;
	/* As long as w1_timeout is only set by a module parameter the sleep
	 * time can be calculated in jiffies once.
	 */
	const unsigned long jtime = msecs_to_jiffies(w1_timeout * 1000);

	while (!kthread_should_stop()) {
		if (dev->search_count && w1_kthread_flag) {
			pm_qos_add_request(&mif_handle,
					PM_QOS_BUS_THROUGHPUT, 825000);
			mutex_lock(&dev->mutex);
			w1_search_process(dev, W1_SEARCH);
			mutex_unlock(&dev->mutex);
			pm_qos_remove_request(&mif_handle);
		}

		try_to_freeze();
		__set_current_state(TASK_INTERRUPTIBLE);

		if (kthread_should_stop())
			break;

		/* Only sleep when the search is active. */
		if (dev->search_count)
			schedule_timeout(jtime);
		else
			schedule();
	}

	atomic_dec(&dev->refcnt);

	return 0;
}

#ifdef CONFIG_W1_WORKQUEUE
void w1_work(struct work_struct *work)
{
	struct w1_master *dev =
		container_of(work, struct w1_master, w1_dwork.work);

	if (dev->search_count) {
		mutex_lock(&dev->mutex);
		w1_search_process(dev, W1_SEARCH);
		mutex_unlock(&dev->mutex);
	}

	schedule_delayed_work(&dev->w1_dwork, HZ * 2);
}
#endif

static int exynos_w1_notifier_event(struct notifier_block *this,
					unsigned long event,
					void *ptr)
{
	switch (event) {
	case PM_SUSPEND_PREPARE:
		printk("%s: kthread flag clear\n", __func__);
		w1_kthread_flag = false;
		return NOTIFY_OK;
	case PM_POST_SUSPEND:
		printk("%s: kthread & check id flag set\n", __func__);
		w1_kthread_flag = true;
		w1_check_id_flag = true;
		return NOTIFY_OK;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block exynos_w1_notifier = {
	.notifier_call = exynos_w1_notifier_event,
};

static int __init w1_init(void)
{
	int retval;

	printk(KERN_INFO "Driver for 1-wire Dallas network protocol.\n");

	w1_init_netlink();

	retval = bus_register(&w1_bus_type);
	if (retval) {
		printk(KERN_ERR "Failed to register bus. err=%d.\n", retval);
		goto err_out_exit_init;
	}

	retval = driver_register(&w1_master_driver);
	if (retval) {
		printk(KERN_ERR
			"Failed to register master driver. err=%d.\n",
			retval);
		goto err_out_bus_unregister;
	}

	retval = driver_register(&w1_slave_driver);
	if (retval) {
		printk(KERN_ERR
			"Failed to register slave driver. err=%d.\n",
			retval);
		goto err_out_master_unregister;
	}

	register_pm_notifier(&exynos_w1_notifier);
	printk("%s: Register pm notifier\n", __func__);

	return 0;

#if 0
/* For undoing the slave register if there was a step after it. */
err_out_slave_unregister:
	driver_unregister(&w1_slave_driver);
#endif

err_out_master_unregister:
	driver_unregister(&w1_master_driver);

err_out_bus_unregister:
	bus_unregister(&w1_bus_type);

err_out_exit_init:
	return retval;
}

static void __exit w1_fini(void)
{
	struct w1_master *dev;

	/* Set netlink removal messages and some cleanup */
	list_for_each_entry(dev, &w1_masters, w1_master_entry)
		__w1_remove_master_device(dev);

	w1_fini_netlink();

	driver_unregister(&w1_slave_driver);
	driver_unregister(&w1_master_driver);
	bus_unregister(&w1_bus_type);
}

late_initcall(w1_init);
module_exit(w1_fini);
