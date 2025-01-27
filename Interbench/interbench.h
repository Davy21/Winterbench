/* Interbench.h */
#ifndef INTERBENCH_H
#define INTERBENCH_H

extern void *hackbench_thread(void *t);
extern void terminal_error(const char *name);
extern _inline void post_sem(HANDLE s);
extern _inline void wait_sem(HANDLE s);
extern _inline DWORD trywait_sem(HANDLE s);
extern _inline int Read(int fd, void *buf, size_t count);

#define THREADS	6	/* The total number of different loads */

struct sems {
	HANDLE ready;
	HANDLE start;
	HANDLE stop;
	HANDLE complete;
	HANDLE stopchild;
};

struct tk_thread {
	struct sems sem;
	unsigned long sleep_interval;
	unsigned long slept_interval;
};

struct data_table {
	unsigned long long total_latency;
	unsigned long long sum_latency_squared;
	unsigned long max_latency;
	unsigned long nr_samples;
	unsigned long deadlines_met;
	unsigned long missed_deadlines;
	unsigned long long missed_burns;
	unsigned long long achieved_burns;
};

struct thread {
	void(*name)(struct thread *);
	char *label;
	int bench;		/* This thread is suitable for benchmarking */
	int rtbench;		/* Suitable for real time benchmarking */
	int load;		/* Suitable as a background load */
	int rtload;		/* Suitable as a background load for rt benches */
	int nodeadlines;	/* Deadlines_met are meaningless for this load */
	unsigned long decasecond_deadlines;	/* Expected deadlines / 10s */
	DWORD pthread;
	DWORD tk_pthread;
	struct sems sem;
	struct data_table benchmarks[THREADS + 1], *dt;
	struct tk_thread tkthread;
	long threadno;
};
extern struct thread hackthread;
#endif
