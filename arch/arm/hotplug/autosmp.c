/*
 * arch/arm/hotplug/autosmp.c
 *
 * automatically hotplug/unplug multiple cpu cores
 * based on cpu load and suspend state
 *
 * based on the msm_mpdecision code by
 * Copyright (c) 2012-2013, Dennis Rassmann <showp1984@gmail.com>
 *
 * Copyright (C) 2013-2014, Rauf Gungor, http://github.com/mrg666
 * rewrite to simplify and optimize, Jul. 2013, http://goo.gl/cdGw6x
 * optimize more, generalize for n cores, Sep. 2013, http://goo.gl/448qBz
 * generalize for all arch, rename as autosmp, Dec. 2013, http://goo.gl/x5oyhy
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. For more details, see the GNU
 * General Public License included with the Linux kernel or available
 * at www.gnu.org/licenses
 */

#include <linux/moduleparam.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#ifdef CONFIG_STATE_NOTIFIER
#include <linux/state_notifier.h>
#endif
#include <linux/mutex.h>

#define DEBUG 0

#define ASMP_TAG	"AutoSMP:"
#define ASMP_ENABLED	true

#if DEBUG
struct asmp_cpudata_t {
	long long unsigned int times_hotplugged;
};
static DEFINE_PER_CPU(struct asmp_cpudata_t, asmp_cpudata);
#endif

static struct delayed_work asmp_work;
static struct workqueue_struct *asmp_workq;

static struct asmp_param_struct {
	unsigned int delay;
	unsigned int suspended;
	unsigned int max_cpus;
	unsigned int min_cpus;
	unsigned int cpufreq_up;
	unsigned int cpufreq_down;
	unsigned int cycle_up;
	unsigned int cycle_down;
	struct notifier_block notif;
	bool enabled;
} asmp_param = {
	.delay = 100,
	.suspended = 0,
	.max_cpus = NR_CPUS,
	.min_cpus = 1,
	.cpufreq_up = 80,
	.cpufreq_down = 50,
	.cycle_up = 1,
	.cycle_down = 2,
	.enabled = ASMP_ENABLED,
};

static unsigned int cycle = 0;
static bool enable_switch = ASMP_ENABLED;

static void reschedule_hotplug_work(void)
{
	queue_delayed_work(asmp_workq, &asmp_work,
			msecs_to_jiffies(asmp_param.delay));
}

static void __cpuinit asmp_work_fn(struct work_struct *work)
{
	unsigned int cpu = 0, slow_cpu = 0;
	unsigned int rate, cpu0_rate, slow_rate = UINT_MAX, fast_rate;
	unsigned int max_rate, up_rate, down_rate;
	int nr_cpu_online;
	
	if (!asmp_param.enabled)
		return;

	cycle++;
	
	/* get maximum possible freq for cpu0 and
	   calculate up/down limits */
	max_rate  = cpufreq_quick_get_max(cpu);
	up_rate   = (asmp_param.cpufreq_up * max_rate) / 100;
	down_rate = (asmp_param.cpufreq_down * max_rate) / 100;

	/* find current max and min cpu freq to estimate load */
	nr_cpu_online = num_online_cpus();
	cpu0_rate = cpufreq_quick_get(cpu);
	fast_rate = cpu0_rate;

	for_each_online_cpu(cpu) {
		if (cpu) {
			rate = cpufreq_quick_get(cpu);
			if (rate <= slow_rate) {
				slow_cpu = cpu;
				slow_rate = rate;
			} else if (rate > fast_rate)
				fast_rate = rate;
		}
	}

	if (cpu0_rate < slow_rate)
		slow_rate = cpu0_rate;

	/* hotplug one core if all online cores are over up_rate limit */
	if (slow_rate > up_rate) {
		if ((nr_cpu_online < asmp_param.max_cpus) &&
		    (cycle >= asmp_param.cycle_up)) {
			cpu = cpumask_next_zero(0, cpu_online_mask);
			if (cpu_is_offline(cpu))
				cpu_up(cpu);
			cycle = 0;
#if DEBUG
			pr_info(ASMP_TAG"CPU [%d] On  | Mask [%d%d%d%d]\n",
				cpu, cpu_online(0), cpu_online(1), cpu_online(2), cpu_online(3));
#endif
		}
	/* unplug slowest core if all online cores are under down_rate limit */
	} else if (slow_cpu && (fast_rate < down_rate)) {
		if ((nr_cpu_online > asmp_param.min_cpus) &&
		    (cycle >= asmp_param.cycle_down)) {
 			if (cpu_online(slow_cpu))
	 			cpu_down(slow_cpu);
			cycle = 0;
#if DEBUG
			pr_info(ASMP_TAG"CPU [%d] Off | Mask [%d%d%d%d]\n",
				slow_cpu, cpu_online(0), cpu_online(1), cpu_online(2), cpu_online(3));
			per_cpu(asmp_cpudata, cpu).times_hotplugged += 1;
#endif
		}
	} /* else do nothing */

	reschedule_hotplug_work();
}

/* Bring online each possible CPU up to max_cpus cores */
static void __ref up_all(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu)
		if (cpu_is_offline(cpu) && num_online_cpus() < asmp_param.max_cpus)
			cpu_up(cpu);
}
	


#ifdef CONFIG_STATE_NOTIFIER
static void __ref asmp_suspend(void)
{
	unsigned int cpu = 0;
	
	cancel_delayed_work_sync(&asmp_work);
	
	/* unplug online cpu cores */
	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		cpu_down(cpu);
	}
	/*
	 * Enabled core 1 so we will have 0-1 online
	 * when screen is OFF to reduce system lags and reboots.
	 */
	cpu_up(1);
	
	pr_info(ASMP_TAG"Screen -> Off. Suspended.\n");
}

static void __ref asmp_resume(void)
{
	up_all();
	queue_delayed_work_on(0, asmp_workq, &asmp_work, msecs_to_jiffies(asmp_param.delay));
}

static int state_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	if (!asmp_param.enabled)
		return NOTIFY_OK;

	switch (event) {
		case STATE_NOTIFIER_ACTIVE:
			asmp_resume();
			break;
		case STATE_NOTIFIER_SUSPEND:
			asmp_suspend();
			break;
		default:
			break;
	}

	return NOTIFY_OK;
}
#endif

static int hotplug_start(void)
{
	int ret = 0;

#ifdef CONFIG_STATE_NOTIFIER
	asmp_param.notif.notifier_call = state_notifier_callback;
	if (state_register_client(&asmp_param.notif)) {
		pr_err("%s: Failed to register State notifier callback\n",
			ASMP_TAG);
		goto quit;
	}
#endif

	asmp_workq = alloc_workqueue("autosmp_wq",
					WQ_HIGHPRI | WQ_FREEZABLE, 0);
	if (!asmp_workq) {
		pr_err("%s: Failed to allocate hotplug workqueue\n",
					ASMP_TAG);
		ret = -ENOMEM;
		goto quit;
	}

	INIT_DELAYED_WORK(&asmp_work, asmp_work_fn);
	reschedule_hotplug_work();

	return ret;
quit:
	destroy_workqueue(asmp_workq);
	asmp_param.enabled = false;

	return ret;
}

static void hotplug_stop(void)
{
	unsigned int cpu = 0;

#ifdef CONFIG_STATE_NOTIFIER
	state_unregister_client(&asmp_param.notif);
#endif

	flush_workqueue(asmp_workq);
	cancel_delayed_work_sync(&asmp_work);
	destroy_workqueue(asmp_workq);

	/* Wake up all the sibling cores */
	for_each_possible_cpu(cpu)
		if (cpu_is_offline(cpu))
			cpu_up(cpu);
}

static int __cpuinit set_enabled(const char *val, const struct kernel_param *kp)
{
	int ret = 0;

	ret = param_set_bool(val, kp);
	if (ret)
		return -EINVAL;

	if (enable_switch == asmp_param.enabled)
		return ret;

	enable_switch = asmp_param.enabled;
	if (asmp_param.enabled)
		hotplug_start();
	else
		hotplug_stop();

	return ret;
}

static struct kernel_param_ops module_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

module_param_cb(enabled, &module_ops, &asmp_param.enabled, 0644);
MODULE_PARM_DESC(enabled, "hotplug/unplug cpu cores based on cpu load");

/***************************** SYSFS START *****************************/
#define define_one_global_ro(_name)					\
static struct global_attr _name =					\
__ATTR(_name, 0444, show_##_name, NULL)

#define define_one_global_rw(_name)					\
static struct global_attr _name =					\
__ATTR(_name, 0644, show_##_name, store_##_name)

struct kobject *asmp_kobject;

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", asmp_param.object);			\
}
show_one(cpufreq_up, cpufreq_up);
show_one(cpufreq_down, cpufreq_down);

#define store_one(file_name, object)					\
static ssize_t store_##file_name					\
(struct kobject *a, struct attribute *b, const char *buf, size_t count)	\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	asmp_param.object = input;					\
	return count;							\
}									\
define_one_global_rw(file_name);
store_one(cpufreq_up, cpufreq_up);
store_one(cpufreq_down, cpufreq_down);

static struct attribute *asmp_attributes[] = {
	&cpufreq_up.attr,
	&cpufreq_down.attr,
	NULL
};

static struct attribute_group asmp_attr_group = {
	.attrs = asmp_attributes,
	.name = "conf",
};

#if DEBUG
static ssize_t show_times_hotplugged(struct kobject *a,
					struct attribute *b, char *buf)
{
	ssize_t len = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		len += sprintf(buf + len, "%i %llu\n", cpu,
			per_cpu(asmp_cpudata, cpu).times_hotplugged);
	}
	return len;
}
define_one_global_ro(times_hotplugged);

static struct attribute *asmp_stats_attributes[] = {
	&times_hotplugged.attr,
	NULL
};

static struct attribute_group asmp_stats_attr_group = {
	.attrs = asmp_stats_attributes,
	.name = "stats",
};
#endif
/****************************** SYSFS END ******************************/

static int __init asmp_init(void)
{
	unsigned int cpu = 0;
	int ret = 0;

	asmp_param.max_cpus = NR_CPUS;

#if DEBUG
	for_each_possible_cpu(cpu)
		per_cpu(asmp_cpudata, cpu).times_hotplugged = 0;
#endif

	asmp_kobject = kobject_create_and_add("autosmp", kernel_kobj);
	if (asmp_kobject) {
		ret = sysfs_create_group(asmp_kobject, &asmp_attr_group);
		if (ret) {
			pr_warn(ASMP_TAG "sysfs: ERROR, create sysfs group.");
			goto err_dev;
		}
#if DEBUG
		ret = sysfs_create_group(asmp_kobject, &asmp_stats_attr_group);
		if (ret) {
			pr_warn(ASMP_TAG "sysfs: ERROR, create sysfs stats group.");
			goto err_dev;
		}
#endif
	} else {
		pr_warn(ASMP_TAG "sysfs: ERROR, create sysfs kobj");
		goto err_dev;
	}

	if (asmp_param.enabled)
		ret = hotplug_start();

	pr_info(ASMP_TAG "Init complete.\n");

err_dev:
	return ret;
}

late_initcall(asmp_init);
