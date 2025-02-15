#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <asm/current.h>
#include <asm/ptrace.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <asm/unistd.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/syscalls.h>
#include "interceptor.h"

MODULE_DESCRIPTION("My kernel module");
MODULE_AUTHOR("Frederic Pun & Ralph Maamari");
MODULE_LICENSE("GPL");

//----- System Call Table Stuff ------------------------------------
/* Symbol that allows access to the kernel system call table */
extern void* sys_call_table[]; // Putting syscall in, or intercepting

/* The sys_call_table is read-only => must make it RW before replacing a syscall */
void set_addr_rw(unsigned long addr) {

	unsigned int level;
	pte_t *pte = lookup_address(addr, &level);

	if (pte->pte &~ _PAGE_RW) pte->pte |= _PAGE_RW;

}

/* Restores the sys_call_table as read-only */
void set_addr_ro(unsigned long addr) {

	unsigned int level;
	pte_t *pte = lookup_address(addr, &level);

	pte->pte = pte->pte &~_PAGE_RW;

}
//-------------------------------------------------------------


//----- Data structures and bookkeeping -----------------------
/**
 * This block contains the data structures needed for keeping track of
 * intercepted system calls (including their original calls), pid monitoring
 * synchronization on shared data, etc.
 * It's highly unlikely that you will need any globals other than these.
 */

/* List structure - each intercepted syscall may have a list of monitored pids */
struct pid_list {
	pid_t pid;
	struct list_head list;
};


/* Store info about intercepted/replaced system calls */
typedef struct {

	/* Original system call
	* asmlinkage - Keeps in it RAM.
	* Return Type: long
	* Field f which is type pointer (Function Pointer)
	* Arguments (Struct pt_regs)
    */
	asmlinkage long (*f)(struct pt_regs);

	/* Status: 1=intercepted, 0=not intercepted */
	int intercepted;

	/* Are any PIDs being monitored for this syscall? */
	int monitored;
	/* List of monitored PIDs */
	int listcount;
	struct list_head my_list;
}mytable;

/* An entry for each system call */
mytable table[NR_syscalls+1];

/* Access to the table and pid lists must be synchronized */
spinlock_t pidlist_lock = SPIN_LOCK_UNLOCKED;
spinlock_t calltable_lock = SPIN_LOCK_UNLOCKED;
//-------------------------------------------------------------

//----------LIST OPERATIONS------------------------------------
/**
 * These operations are meant for manipulating the list of pids
 * Nothing to do here, but please make sure to read over these functions
 * to understand their purpose, as you will need to use them!
 */

/**
 * Add a pid to a syscall's list of monitored pids.
 * Returns -ENOMEM if the operation is unsuccessful.
 */
static int add_pid_sysc(pid_t pid, int sysc)
{
	struct pid_list *ple=(struct pid_list*)kmalloc(sizeof(struct pid_list), GFP_KERNEL);

	if (!ple)
		return -ENOMEM;

	INIT_LIST_HEAD(&ple->list);
	ple->pid=pid;

	list_add(&ple->list, &(table[sysc].my_list));
	table[sysc].listcount++;

	return 0;
}

/**
 * Remove a pid from a system call's list of monitored pids.
 * Returns -EINVAL if no such pid was found in the list.
 */
static int del_pid_sysc(pid_t pid, int sysc)
{
	struct list_head *i;
	struct pid_list *ple;

	list_for_each(i, &(table[sysc].my_list)) {

		ple=list_entry(i, struct pid_list, list);
		if(ple->pid == pid) {

			list_del(i);
			kfree(ple);

			table[sysc].listcount--;
			/* If there are no more pids in sysc's list of pids, then
			 * stop the monitoring only if it's not for all pids (monitored=2) */
			if(table[sysc].listcount == 0 && table[sysc].monitored == 1) {
				table[sysc].monitored = 0;
			}

			return 0;
		}
	}

	return -EINVAL;
}

/**
 * Remove a pid from all the lists of monitored pids (for all intercepted syscalls).
 * Returns -1 if this process is not being monitored in any list.
 */
static int del_pid(pid_t pid)
{
	struct list_head *i, *n;
	struct pid_list *ple;
	int ispid = 0, s = 0;

	for(s = 1; s < NR_syscalls; s++) {

		list_for_each_safe(i, n, &(table[s].my_list)) {

			ple=list_entry(i, struct pid_list, list);
			if(ple->pid == pid) {

				list_del(i);
				ispid = 1;
				kfree(ple);

				table[s].listcount--;
				/* If there are no more pids in sysc's list of pids, then
				 * stop the monitoring only if it's not for all pids (monitored=2) */
				if(table[s].listcount == 0 && table[s].monitored == 1) {
					table[s].monitored = 0;
				}
			}
		}
	}

	if (ispid) return 0;
	return -1;
}

/**
 * Clear the list of monitored pids for a specific syscall.
 */
static void destroy_list(int sysc) {

	struct list_head *i, *n;
	struct pid_list *ple;

	list_for_each_safe(i, n, &(table[sysc].my_list)) {

		ple=list_entry(i, struct pid_list, list);
		list_del(i);
		kfree(ple);
	}

	table[sysc].listcount = 0;
	table[sysc].monitored = 0;
}

/**
 * Check if two pids have the same owner - useful for checking if a pid
 * requested to be monitored is owned by the requesting process.
 * Remember that when requesting to start monitoring for a pid, only the
 * owner of that pid is allowed to request that.
 */
static int check_pid_from_list(pid_t pid1, pid_t pid2) {

	struct task_struct *p1 = pid_task(find_vpid(pid1), PIDTYPE_PID);
	struct task_struct *p2 = pid_task(find_vpid(pid2), PIDTYPE_PID);
	if(p1->real_cred->uid != p2->real_cred->uid)
		return -EPERM;
	return 0;
}

/**
 * Check if a pid is already being monitored for a specific syscall.
 * Returns 1 if it already is, or 0 if pid is not in sysc's list.
 */
static int check_pid_monitored(int sysc, pid_t pid) {

	struct list_head *i;
	struct pid_list *ple;

	list_for_each(i, &(table[sysc].my_list)) {

		ple=list_entry(i, struct pid_list, list);
		if(ple->pid == pid)
			return 1;

	}
	return 0;
}
//----------------------------------------------------------------

//----- Intercepting exit_group ----------------------------------
/**
 * Since a process can exit without its owner specifically requesting
 * to stop monitoring it, we must intercept the exit_group system call
 * so that we can remove the exiting process's pid from *all* syscall lists.
 */

/**
 * Stores original exit_group function - after all, we must restore it
 * when our kernel module exits.
 */
void (*orig_exit_group)(int);

/**
 * Our custom exit_group system call.
 *
 * TODO: When a process exits, make sure to remove that pid from all lists.
 * The exiting process's PID can be retrieved using the current variable (current->pid).
 * Don't forget to call the original exit_group.
 */
void my_exit_group(int status)
{
	// Lock Access
    spin_lock(&calltable_lock);
    spin_lock(&pidlist_lock);

	// Delete the pid from all list of monitored pids.
	del_pid(current->pid);

	// Unlock Access
    spin_unlock(&pidlist_lock);
    spin_unlock(&calltable_lock);

	// Original Exit Group Call
	orig_exit_group(status);
}
//----------------------------------------------------------------



/**
 * This is the generic interceptor function.
 * It should just log a message and call the original syscall.
 *
 * TODO: Implement this function.
 * (1) Check first to see if the syscall is being monitored for the current->pid.
 * (2) Recall the convention for the "monitored" flag in the mytable struct:
 *     monitored=0 => not monitored
 *     monitored=1 => some pids are monitored, check the corresponding my_list
 *     monitored=2 => all pids are monitored for this syscall
 * (3) Use the log_message macro, to log the system call parameters!
 *     Remember that the parameters are passed in the pt_regs registers.
 *     The syscall parameters are found (in order) in the
 *     ax, bx, cx, dx, si, di, and bp registers (see the pt_regs struct).
 * (4) Don't forget to call the original system call, so we allow processes to proceed as normal.
 */
asmlinkage long interceptor(struct pt_regs reg) {

	int hasPid;
	spin_lock(&calltable_lock);

	// Read pid
	spin_lock(&pidlist_lock);	
	hasPid = check_pid_monitored(reg.ax, current->pid);
	spin_unlock(&pidlist_lock);

	// Read monitored
	spin_unlock(&calltable_lock);

	// If monitoring all and not blacklisted, or is not monitoring all but whitelisted
	if (((table[reg.ax].monitored == 2) && (hasPid == 0)) || ((table[reg.ax].monitored == 1) && (hasPid == 1))) {
		log_message(current->pid, reg.ax, reg.bx, reg.cx, reg.dx, reg.si, reg.di, reg.bp);
	}
	// Returns the original custom syscall.
	return table[reg.ax].f(reg);

	// return 0; // Just a placeholder, so it compiles with no warnings!
}

static long request_syscall_intercept(int syscall) {

	// Check if root
	if (current_uid() != 0) {
		return -EPERM;
	}

	spin_lock(&calltable_lock);

	// Check if call is intercepted
	if (table[syscall].intercepted == 1) {
		spin_unlock(&calltable_lock);
		return -EBUSY;
	}

	// Flag to intercept syscall
	set_addr_rw((unsigned long) sys_call_table);
	// Replacing kernal syscall with our intercepted function
	sys_call_table[syscall] = interceptor;
	table[syscall].intercepted = 1;
	set_addr_ro((unsigned long) sys_call_table);
	spin_unlock(&calltable_lock);
	return 0;
}

static long request_syscall_release(int syscall) {

	// Check if root
	if (current_uid() != 0) {
		return -EPERM;
	}

	spin_lock(&calltable_lock);

	// Check if call is unintercepted
	if (table[syscall].intercepted == 0) {
		spin_unlock(&calltable_lock);
		return -EINVAL;
	}
	set_addr_rw((unsigned long) sys_call_table);
	// Replacing kernal syscall with the original function
	sys_call_table[syscall] = table[syscall].f;
	// Flag to intercept syscall
	table[syscall].intercepted = 0;
	set_addr_ro((unsigned long) sys_call_table);
	spin_unlock(&calltable_lock);
	return 0;
}

static long request_start_monitoring(int syscall, int pid) {
	int status = 0;
	int hasPid;

	// Check if root user, or if monitoring own process
	if (
		current_uid() != 0 &&
		(pid == 0 || check_pid_from_list(pid, current->pid) != 0)
	) {
		return -EPERM;
	}

	spin_lock(&calltable_lock);

	if (pid == 0) {
		// If already monitoring all, no good
		if (table[syscall].monitored == 2) {
			status = -EBUSY;
		} else {
			// Reset list to blacklist and set to monitor all
			spin_lock(&pidlist_lock);
			destroy_list(syscall);
			spin_unlock(&pidlist_lock);
			table[syscall].monitored = 2;
		}
	} else {
		spin_lock(&pidlist_lock);

		// If not monitoring all, try to add to whitelist
		if (table[syscall].monitored != 2) {
			hasPid = check_pid_monitored(syscall, pid);
			status = hasPid ? -EBUSY : add_pid_sysc(pid, syscall);

			if (status == 0) {
				table[syscall].monitored = 1;
			}

		// If not, try to remove from whitelist
		} else {
			status = del_pid_sysc(pid, syscall);
		}

		spin_unlock(&pidlist_lock);
	}

	spin_unlock(&calltable_lock);
	return status;
}

static long request_stop_monitoring(int syscall, int pid) {
	int status = 0;
	int hasPid;

	// Check if root user, or if monitoring own process
	if (
		current_uid() != 0 &&
		(pid == 0 || check_pid_from_list(pid, current->pid) != 0)
	) {
		return -EPERM;
	}

	spin_lock(&calltable_lock);

	if (pid == 0) {
		// If already monitoring all, no good
		if (table[syscall].monitored != 2) {
			status = -EINVAL;
		} else {
			// Reset list to whitelist
			spin_lock(&pidlist_lock);
			destroy_list(syscall);
			spin_unlock(&pidlist_lock);
		}
	} else {
		spin_lock(&pidlist_lock);

		// If monitoring all, try to add to blacklist
		if (table[syscall].monitored == 2) {
			hasPid = check_pid_monitored(syscall, pid);
			status = hasPid ? -EBUSY : add_pid_sysc(pid, syscall);

			if (status == 0) {
				table[syscall].monitored = 1;
			}

		// If not, try to remove from blacklist
		} else {
			status = del_pid_sysc(pid, syscall);
		}

		spin_unlock(&pidlist_lock);
	}

	spin_unlock(&calltable_lock);
	return status;
}

/**
 * My system call - this function is called whenever a user issues a MY_CUSTOM_SYSCALL system call.
 * When that happens, the parameters for this system call indicate one of 4 actions/commands:
 *      - REQUEST_SYSCALL_INTERCEPT to intercept the 'syscall' argument
 *      - REQUEST_SYSCALL_RELEASE to de-intercept the 'syscall' argument
 *      - REQUEST_START_MONITORING to start monitoring for 'pid' whenever it issues 'syscall'
 *      - REQUEST_STOP_MONITORING to stop monitoring for 'pid'
 *      For the last two, if pid=0, that translates to "all pids".
 *
 * TODO: Implement this function, to handle all 4 commands correctly.
 *
 * (1) For each of the commands, check that the arguments are valid (-EINVAL):
 *   a) the syscall must be valid (not negative, not > NR_syscalls, and not MY_CUSTOM_SYSCALL itself)
 *   b) the pid must be valid for the last two commands. It cannot be a negative integer,
 *      and it must be an existing pid (except for the case when it's 0, indicating that we want
 *      to start/stop monitoring for "all pids").
 *      If a pid belongs to a valid process, then the following expression is non-NULL:
 *           pid_task(find_vpid(pid), PIDTYPE_PID)
 * (2) Check that the caller has the right permissions (-EPERM)
 *      For the first two commands, we must be root (see the current_uid() macro).
 *      For the last two commands, the following logic applies:
 *        - is the calling process root? if so, all is good, no doubts about permissions.
 *        - if not, then check if the 'pid' requested is owned by the calling process
 *        - also, if 'pid' is 0 and the calling process is not root, then access is denied
 *          (monitoring all pids is allowed only for root, obviously).
 *      To determine if two pids have the same owner, use the helper function provided above in this file.
 * (3) Check for correct context of commands (-EINVAL):
 *     a) Cannot de-intercept a system call that has not been intercepted yet.
 *     b) Cannot stop monitoring for a pid that is not being monitored, or if the
 *        system call has not been intercepted yet.
 * (4) Check for -EBUSY conditions:
 *     a) If intercepting a system call that is already intercepted.
 *     b) If monitoring a pid that is already being monitored.
 * - If a pid cannot be added to a monitored list, due to no memory being available,
 *   an -ENOMEM error code should be returned.
 *
 * - Make sure to keep track of all the metadata on what is being intercepted and monitored.
 *   Use the helper functions provided above for dealing with list operations.
 *
 * - Whenever altering the sys_call_table, make sure to use the set_addr_rw/set_addr_ro functions
 *   to make the system call table writable, then set it back to read-only.
 *   For example: set_addr_rw((unsigned long)sys_call_table);
 *   Also, make sure to save the original system call (you'll need it for 'interceptor' to work correctly).
 *
 * - Make sure to use synchronization to ensure consistency of shared data structures.
 *   Use the calltable_spinlock and pidlist_spinlock to ensure mutual exclusion for accesses
 *   to the system call table and the lists of monitored pids. Be careful to unlock any spinlocks
 *   you might be holding, before you exit the function (including error cases!).
 */
asmlinkage long my_syscall(int cmd, int syscall, int pid) {

	// Check if syscall is valid
	if (syscall <= 0 || syscall > NR_syscalls) {
		return -EINVAL;
	}

	switch(cmd) {
		case REQUEST_SYSCALL_INTERCEPT:
			return request_syscall_intercept(syscall);

		case REQUEST_SYSCALL_RELEASE:
			return request_syscall_release(syscall);

		case REQUEST_START_MONITORING:
			// Check if valid pid
			if (pid != 0 && !pid_task(find_vpid(pid), PIDTYPE_PID)) {
				return -EINVAL;
			}
			return request_start_monitoring(syscall, pid);

		case REQUEST_STOP_MONITORING:
			// Check if valid pid
			if (pid != 0 && !pid_task(find_vpid(pid), PIDTYPE_PID)) {
				return -EINVAL;
			}
			return request_stop_monitoring(syscall, pid);

		default:
			return -EINVAL;
	}
}

/**
 *
 */
long (*orig_custom_syscall)(void); // STORES FUNCTION OF ORGINAL SYSCALL


/**
 * Module initialization.
 *
 * TODO: Make sure to:
 * (1) Hijack MY_CUSTOM_SYSCALL and save the original in orig_custom_syscall.
 * (2) Hijack the exit_group system call (__NR_exit_group) and save the original
 *   in orig_exit_group.
 * (3) Make sure to set the system call table to writable when making changes,
 *   then set it back to read only once done.
 * (4) Perform any necessary initializations for bookkeeping data structures.
 *   To initialize a list, use
 *        INIT_LIST_HEAD (&some_list);
 *   where some_list is a "struct list_head".
 * (5) Ensure synchronization as needed.
 */
static int init_function(void) {

	int syscall;
    spin_lock(&calltable_lock);
    spin_lock(&pidlist_lock);

	set_addr_rw((unsigned long) sys_call_table);

	// * Original Custom System Call Step *
	// Save the current custom system call in a holder pointer and replace kernal.
	orig_custom_syscall = sys_call_table[MY_CUSTOM_SYSCALL];
	sys_call_table[MY_CUSTOM_SYSCALL] = my_syscall;

	// * Original NR_EXIT System Call Step *
	// Save the current exit system call in a holder pointer and replace kernal.
	orig_exit_group = sys_call_table[__NR_exit_group];
	sys_call_table[__NR_exit_group] = my_exit_group;

	// Map all the kernal syscall commands to our abstract data structure for conditional behaviour.
	for (syscall = 0; syscall < NR_syscalls; syscall++) {
		table[syscall].listcount = 0;
		table[syscall].intercepted = 0;
		table[syscall].monitored = 0;
		table[syscall].f = sys_call_table[syscall];
	 	INIT_LIST_HEAD(&(table[syscall].my_list));
	}

    set_addr_ro((unsigned long) sys_call_table);

    spin_unlock(&pidlist_lock);
    spin_unlock(&calltable_lock);


	return 0;
}

/**
 * Module exits.
 *
 * TODO: Make sure to:
 * (1) Restore MY_CUSTOM_SYSCALL to the original syscall.
 * (2) Restore __NR_exit_group to its original syscall.
 * (3) Make sure to set the system call table to writable when making changes,
 *   then set it back to read only once done.
 * (4) Ensure synchronization, if needed.
 */
static void exit_function(void)
{

	spin_lock(&calltable_lock);
	spin_lock(&pidlist_lock);

	set_addr_rw((unsigned long) sys_call_table);

	// Restoring MY_CUSTOM_SYSCALL to the original syscall.
	sys_call_table[MY_CUSTOM_SYSCALL] = orig_custom_syscall;
	// Restoring __NR_exit_group (SYSCALL) to its original syscall.
    sys_call_table[__NR_exit_group] = orig_exit_group;

	set_addr_ro((unsigned long) sys_call_table);

	spin_unlock(&pidlist_lock);
    spin_unlock(&calltable_lock);

}

module_init(init_function);
module_exit(exit_function);

