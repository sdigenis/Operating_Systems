#include <linux/module.h>
#include <linux/printk.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/sched.h>

static struct kobject *example_kobject;
volatile int roots = 0;

static ssize_t foo_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{

	struct task_struct* new_curr = current;
	
	pid_t caller_pid = current->pid;	

	printk("find_roots sysfs opened by process %d\n", new_curr->pid);
	
	while(1) {
		printk("id: %d, name: %s\n", new_curr->pid, new_curr->comm);
		if(new_curr->pid != 1) {
			new_curr = new_curr->real_parent;
		}
		else {
			break;
		}
	}


	return sprintf(buf, "%d\n", caller_pid);
}

struct kobj_attribute foo_attribute = __ATTR(find_roots, 0444, foo_show, NULL);

static int __init mymodule_init (void)
{
	int error = 0;
	example_kobject = kobject_create_and_add("teamXX", kernel_kobj);
	if (!example_kobject){
		return -ENOMEM;
	}

	error = sysfs_create_file(example_kobject, &foo_attribute.attr);
	if (error) {
		printk("failed to create the foo file in /sys/kernel/teamXX/find_roots \n");
	}
	
	return error;
}

static void __exit mymodule_exit (void)
{
	printk("Module uninitialized successfully \n");
	kobject_put(example_kobject);
}

module_init(mymodule_init);
module_exit(mymodule_exit);
MODULE_LICENSE("GPL");
