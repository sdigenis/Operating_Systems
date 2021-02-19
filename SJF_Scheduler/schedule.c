/* schedule.c
 * This file contains the primary logic for the 
 * scheduler.
 */
#include "schedule.h"
#include "macros.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"

// #define GOODNESS 0
#define NEWTASKSLICE (NS_TO_JIFFIES(100000000))
#define alpha 0.5

/* Local Globals
 * rq - This is a pointer to the runqueue/readyqueue that the scheduler uses.
 * current - A pointer to the current running task.
 */
struct runqueue *rq;
struct task_struct *current;

/* External Globals
 * jiffies - A discrete unit of time used for scheduling.
 *			 There are HZ jiffies in a second, (HZ is 
 *			 declared in macros.h), and is usually
 *			 1 or 10 milliseconds.
 */
extern long long jiffies;
extern struct task_struct *idle;

/*-----------------Initilization/Shutdown Code-------------------*/
/* This code is not used by the scheduler, but by the virtual machine
 * to setup and destroy the scheduler cleanly.
 */
 
 /* initscheduler
  * Sets up and allocates memory for the scheduler, as well
  * as sets initial values. This function should also
  * set the initial effective priority for the "seed" task 
  * and enqueue it in the scheduler.
  * INPUT:
  * newrq - A pointer to an allocated rq to assign to your
  *			local rq.
  * seedTask - A pointer to a task to seed the scheduler and start
  * the simulation.
  */
void initschedule(struct runqueue *newrq, struct task_struct *seedTask)
{
	seedTask->next = seedTask->prev = seedTask;
	newrq->head = seedTask;
	newrq->nr_running++;
}

/* killschedule
 * This function should free any memory that 
 * was allocated when setting up the runqueue.
 * It SHOULD NOT free the runqueue itself.
 */
void killschedule()
{
	///

	///
	return;
}

/*
void print_rq () {
	struct task_struct *curr;
	
	printf("Rq: \n");
	curr = rq->head;
	if (curr)
		printf("%p", curr);
	while(curr->next != rq->head) {
		curr = curr->next;
		printf(", %p", curr);
	};
	printf("\n");
}
*/
void print_rq () {
	struct task_struct *curr;
	
	printf("Rq: \n");
	curr = rq->head;
	if (curr)
		printf("%p Exp_burst: %lf", curr,curr->exp_burst);
	while(curr->next != rq->head) {
		curr = curr->next;
		printf("\t %p Exp_burst: %lf", curr,curr->exp_burst);
	};
	printf("\n");
}

/*-------------Scheduler Code Goes Below------------*/
/* This is the beginning of the actual scheduling logic */

unsigned long long max_w_rq()
{
	struct task_struct *pos;
	unsigned long long max_waiting_in_rq = 0;
	list_for_each(pos,rq->head)
	{
		if(waiting_in_rq(pos) >= max_waiting_in_rq)
		{
			max_waiting_in_rq = waiting_in_rq(pos);
		} 
	}
	return (max_waiting_in_rq);
}

unsigned long long min_e_burst()
{
	struct task_struct *pos;
	unsigned long long min_exp_burst = rq->head->next->exp_burst;
	list_for_each(pos,rq->head)
	{
		if(pos->exp_burst <= min_exp_burst)
		{
			min_exp_burst = pos->exp_burst;
		} 
	}
	return (min_exp_burst);
}

/* schedule
 * Gets the next task in the queue
 */
void schedule()
{
	static struct task_struct *nxt = NULL;
	struct task_struct *curr;

	///
	double exp_burst_bk;

	struct task_struct *pos;
	///

//	printf("In schedule\n");
//	print_rq();
	
	current->need_reschedule = 0; /* Always make sure to reset that, in case *
								   * we entered the scheduler because current*
								   * had requested so by setting this flag   */
	
	if (rq->nr_running == 1) {
		context_switch(rq->head);
		nxt = rq->head->next;
	}
	else {	
		///
		exp_burst_bk = current->exp_burst;
		///					[			current burst			]			[current last exp burst]
		current->exp_burst = ( (sched_clock() - current->time_taken) + alpha * current->exp_burst) / \
							(1 + alpha);

		


		

		///choose next process
		// print_rq();
		#ifdef GOODNESS
			double min_goodness, goodness;
			unsigned long long max_waiting_in_rq;
			double min_exp_burst;

			max_waiting_in_rq = max_w_rq();
			min_exp_burst = min_e_burst();

			min_goodness = ( (1 + rq->head->next->exp_burst) * (1 + max_waiting_in_rq) ) / \
						( ( min_exp_burst + 1) * (1 + waiting_in_rq(rq->head->next)) );
			nxt = rq->head->next;

			for (pos = (rq->head)->next->next; pos != (rq->head); pos = pos->next)
			{
				goodness = ( (1 + pos->exp_burst) * (1 + max_waiting_in_rq) ) / \
						( ( min_exp_burst + 1) * (1 + waiting_in_rq(pos)) );
				if(goodness < min_goodness)
				{
					min_goodness = goodness;
					nxt = pos;
				}
			}
		#else
			double min_exp_burst;

			min_exp_burst = (rq->head->next->exp_burst);
			nxt = rq->head->next;

			for (pos = (rq->head)->next->next; pos != (rq->head); pos = pos->next)
			{
				if(pos->exp_burst < min_exp_burst)
				{
					min_exp_burst = pos->exp_burst;
					nxt = pos;
				}
			}
		#endif
		///
		/// 
		if(nxt != current){
			  current->exp_burst = exp_burst_bk;
		}
		///

		///
		// printf("\t**Idia diergasia\n");
		///

		curr = nxt;
		nxt = nxt->next;
		if (nxt == rq->head)    /* Do this to always skip init at the head */
			nxt = nxt->next;	/* of the queue, whenever there are other  */
								/* processes available					   */
		
		context_switch(curr);

	}
}


/* sched_fork
 * Sets up schedule info for a newly forked task
 */
void sched_fork(struct task_struct *p)
{
	p->time_taken = 0;
	p->time_lost = 0;
	p->last_burst = 0;
	p->exp_burst = 0;
	p->last_exp_burst = 0;
	p->last_rq_entry_time = 0;
	p->time_slice = 100;
}

double expected_burst(struct task_struct *p)
{
	return (p->last_burst + alpha * p->last_exp_burst) / \
						(1 + alpha);
}

unsigned long long waiting_in_rq(struct task_struct *p)
{
	return (sched_clock() - p->last_rq_entry_time); 
}

/* scheduler_tick
 * Updates information and priority
 * for the task that is currently running.
 */
void scheduler_tick(struct task_struct *p)
{
	if(p != rq->head)
	{
		p->time_slice--;
		if(p->time_slice == 0)
		{
			p->time_slice = 100;
			p->need_reschedule = 1;
		}
	}
	//unsigned long long tmp_burst = sched_clock() - current->time_taken;
	//print_rq();
	schedule();
	///
	if(current != p)
	{
		current->time_taken = sched_clock();
		p->time_lost = sched_clock();
		p->last_burst = p->time_lost - p->time_taken;
		p->last_exp_burst = p->exp_burst;
		p->exp_burst = expected_burst(p);
	}
	///
}

/* wake_up_new_task
 * Prepares information for a task
 * that is waking up for the first time
 * (being created).
 */
void wake_up_new_task(struct task_struct *p)
{	
	p->next = rq->head->next;
	p->prev = rq->head;
	p->next->prev = p;
	p->prev->next = p;
	///
	p->last_rq_entry_time = sched_clock();
	///
	rq->nr_running++;
}

/* activate_task
 * Activates a task that is being woken-up
 * from sleeping.
 */
void activate_task(struct task_struct *p)
{
	p->next = rq->head->next;
	p->prev = rq->head;
	p->next->prev = p;
	p->prev->next = p;
	
	///
	p->last_rq_entry_time = sched_clock();
	///

	rq->nr_running++;

	///
	schedule();
	///
}

/* deactivate_task
 * Removes a running task from the scheduler to
 * put it to sleep.
 */
void deactivate_task(struct task_struct *p)
{
	p->prev->next = p->next;
	p->next->prev = p->prev;
	p->next = p->prev = NULL; /* Make sure to set them to NULL *
							   * next is checked in cpu.c      */

	rq->nr_running--;
}
