#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/sched.h>

SYSCALL_DEFINE0(find_roots) {
    
	struct task_struct* new_curr = current;
	
	printk("find_roots system call called by process %d\n", new_curr->pid);
	
	while(1) {
		printk("id: %d, name: %s\n", new_curr->pid, new_curr->comm);
		if(new_curr->pid != 1) {
			new_curr = new_curr->real_parent;
		}
		else {
			break;
		}
	}
	
	return(0);
}
