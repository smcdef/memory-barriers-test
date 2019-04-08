// SPDX-License-Identifier: GPL-2.0

/**
 * mb_test.c - memory barrier test.
 *
 * Copyright (c) 2019 songmuchun <smcdef@163.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "memory barrier test: %s " fmt, __func__

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/smpboot.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#ifndef DEFINE_SHOW_ATTRIBUTE
#define DEFINE_SHOW_ATTRIBUTE(__name)					\
static int __name ## _open(struct inode *inode, struct file *file)	\
{									\
	return single_open(file, __name ## _show, inode->i_private);	\
}									\
									\
static const struct file_operations __name ## _fops = {			\
	.owner		= THIS_MODULE,					\
	.open		= __name ## _open,				\
	.read		= seq_read,					\
	.llseek		= seq_lseek,					\
	.release	= single_release,				\
}
#endif

#ifndef DEFINE_SHOW_STORE_ATTRIBUTE
#define DEFINE_SHOW_STORE_ATTRIBUTE(__name)				\
static int __name ## _open(struct inode *inode, struct file *file)	\
{									\
	return single_open(file, __name ## _show, inode->i_private);	\
}									\
									\
static const struct file_operations __name ## _fops = {			\
	.owner		= THIS_MODULE,					\
	.open		= __name ## _open,				\
	.read		= seq_read,					\
	.write		= __name ## _store,				\
	.llseek		= seq_lseek,					\
	.release	= single_release,				\
}
#endif

#define MB_TEST_PROC_DIR	"mb_test"

static int values;
static atomic_t count = ATOMIC_INIT(0);
static int should_run = 1;

static DEFINE_PER_CPU(struct task_struct *, mb_test_tasks);

static int mb_test_should_run(unsigned int cpu)
{
	return should_run;
}

#if 1
static void mb_test_thread_fn(unsigned int cpu)
{
	static unsigned int a, b;

	if (cpu == 0) {
		atomic_inc(&count);
	} else if (cpu == 4) {
		int temp = atomic_read(&count);

		a = temp;
		smp_wmb();
		b = temp;
	} else {
		unsigned int c, d;
#if 0
		unsigned long temp = 0;

		asm volatile(
		"	ldr	%w1, %4\n"
		"	and	%w2, %w1, wzr\n"
		"	add	%2,  %2, %5\n"
		"	ldr	%w0, [%2]"
		: "=&r" (c), "=&r" (d), "+&r" (temp)
		: "Q" (a), "Q" (b), "r" (&a)
		: "memory");
#else
		d = b;
		smp_rmb();
		c = a;
#endif

		if ((int)(d - c) > 0) {
			should_run = 0;
			pr_info("a = %d, b = %d(cpu: %d)\n", c, d, cpu);
			msleep(1000);
			pr_info("after 1 second ...\n");
			pr_info("a = %d, b = %d(cpu: %d)\n", a, b, cpu);
		}
	}
}
#else
static void mb_test_thread_fn(unsigned int cpu)
{
	int i;
	static atomic_t lock = ATOMIC_INIT(0);

	for (i = 0; i < 100000000; i++) {
		while (atomic_read(&lock) ||
		       atomic_cmpxchg_acquire(&lock, 0, 1) != 0)
			;
		values++;
		atomic_set_release(&lock, 0);
	}
	should_run = 0;
	pr_info("values = %d(cpu: %d)\n", values, cpu);
}
#endif

static struct smp_hotplug_thread mb_smp_thread = {
	.store			= &mb_test_tasks,
	.thread_should_run	= mb_test_should_run,
	.thread_fn		= mb_test_thread_fn,
	.thread_comm		= "memory_barrier_test/%u",
};

static int values_show(struct seq_file *seq, void *offset)
{
	seq_printf(seq, "%d\n", values);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(values);

static int count_show(struct seq_file *seq, void *offset)
{
	seq_printf(seq, "%d\n", atomic_read(&count));

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(count);

static ssize_t should_run_store(struct file *file, const char __user *ubuf,
				size_t size, loff_t *ppos)
{
	char buf[4];

	if (size > sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, size))
		return -EFAULT;
	buf[sizeof(buf) - 1] = '\0';

	if (buf[0] == '1' || !strncmp(buf, "on", 2)) {
		unsigned int cpu;

		should_run = 1;
		for_each_online_cpu(cpu)
			wake_up_process(*per_cpu_ptr(mb_smp_thread.store, cpu));
	} else if (buf[0] == '0' || !strncmp(buf, "off", 3)) {
		should_run = 0;
	} else
		return -EINVAL;
	*ppos += size;

	return size;
}

static int should_run_show(struct seq_file *seq, void *offset)
{
	seq_printf(seq, "%d\n", should_run);

	return 0;
}

DEFINE_SHOW_STORE_ATTRIBUTE(should_run);

static inline void mb_procs_create(void)
{
	proc_mkdir(MB_TEST_PROC_DIR, NULL);
	proc_create_data(MB_TEST_PROC_DIR "/" "values", 0444, NULL,
			 &values_fops, NULL);
	proc_create_data(MB_TEST_PROC_DIR "/" "count", 0444, NULL,
			 &count_fops, NULL);
	proc_create_data(MB_TEST_PROC_DIR "/" "should_run", 0644, NULL,
			 &should_run_fops, NULL);
}

static inline void mb_procs_remove(void)
{
	remove_proc_entry(MB_TEST_PROC_DIR "/" "values", NULL);
	remove_proc_entry(MB_TEST_PROC_DIR "/" "count", NULL);
	remove_proc_entry(MB_TEST_PROC_DIR "/" "should_run", NULL);
	remove_proc_entry(MB_TEST_PROC_DIR, NULL);
}

static int __init mb_test_init(void)
{
	mb_procs_create();
	smpboot_register_percpu_thread(&mb_smp_thread);

	return 0;
}

static void __exit mb_test_exit(void)
{
	mb_procs_remove();
	smpboot_unregister_percpu_thread(&mb_smp_thread);
}

module_init(mb_test_init);
module_exit(mb_test_exit);
MODULE_AUTHOR("songmuchun <smcdef@163.com>");
MODULE_LICENSE("GPL v2");
