#define _CRT_SECURE_NO_DEPRECATE
#include <Windows.h>
#include <intrin.h>
#include <stdio.h>
#include <errno.h>
#include <io.h>
#include <math.h>
#include "interbench.h"
#include "getopt.h"

#define MAX_UNAME_LENGTH	100
#define MAX_LOG_LENGTH		((MAX_UNAME_LENGTH) + 4)
#define MIN_BLK_SIZE		1024
#define DEFAULT_RESERVE		64
#define MB			(1024 * 1024)	/* 2^20 bytes */
#define KB			1024
#define MAX_MEM_IN_MB		(1024 * 64)	/* 64 GB */

struct user_data {
	unsigned long loops_per_ms;
	unsigned long ram, swap;
	int duration;
	int do_rt;
	int bench_nice;
	int load_nice;
	unsigned long custom_run;
	unsigned long custom_interval;
	unsigned long cpu_load;
	char logfilename[MAX_LOG_LENGTH];
	int log;
	char unamer[MAX_UNAME_LENGTH];
	char datestamp[13];
	FILE *logfile;
} ud = {
	.duration = 30,
	.cpu_load = 4,
	.log = 1,
};

/* Pipes main to/from load and bench processes */
static HANDLE m2l, l2m, m2b, b2m;

/* Which member of becnhmarks is used when not benchmarking */
#define NOT_BENCHING	(THREADS)
#define CUSTOM		(THREADS - 1)

/*
* To add another load or a benchmark you need to increment the value of
* THREADS, add a function prototype for your function and add an entry to
* the threadlist. To specify whether the function is a benchmark or a load
* set the benchmark and/or load flag as appropriate. The basic requirements
* of a new load can be seen by using emulate_none as a template.
*/
void emulate_none(struct thread *th);
void emulate_audio(struct thread *th);
void emulate_video(struct thread *th);
void emulate_x(struct thread *th);
void emulate_game(struct thread *th);
void emulate_burn(struct thread *th);

struct thread threadlist[THREADS] = {
	{ .label = "None", .name = emulate_none, .load = 1, .rtload = 1 },
	{ .label = "Audio", .name = emulate_audio, .bench = 1, .rtbench = 1 },
	{ .label = "Video", .name = emulate_video, .bench = 1, .rtbench = 1, .load = 1, .rtload = 1 },
	{ .label = "X", .name = emulate_x, .bench = 1, .load = 1, .rtload = 1 },
	{ .label = "Gaming", .name = emulate_game, .nodeadlines = 1, .bench = 1 },
	{ .label = "Burn", .name = emulate_burn, .load = 1, .rtload = 1 },
};

HANDLE init_sem(HANDLE sem);
void init_all_sems(struct sems *s);
void initialise_thread(int i);
void start_thread(struct thread *th);
void stop_thread(struct thread *th);

void terminal_error(const char *name)
{
	fprintf(stderr, "\n");
	perror(name);
	exit(1);
}

LARGE_INTEGER getFILETIMEoffset()
{
	SYSTEMTIME s;
	FILETIME f;
	LARGE_INTEGER t;

	s.wYear = 1970;
	s.wMonth = 1;
	s.wDay = 1;
	s.wHour = 0;
	s.wMinute = 0;
	s.wSecond = 0;
	s.wMilliseconds = 0;
	SystemTimeToFileTime(&s, &f);
	t.QuadPart = f.dwHighDateTime;
	t.QuadPart <<= 32;
	t.QuadPart |= f.dwLowDateTime;
	return t;
}

int clock_gettime(struct timeval *tv)
{
	LARGE_INTEGER           t;
	FILETIME                f;
	double                  microseconds;
	static LARGE_INTEGER    offset;
	static double           frequencyToMicroseconds;
	static int              initialized = 0;
	static BOOL             usePerformanceCounter = 0;

	if (!initialized) {
		LARGE_INTEGER performanceFrequency;
		initialized = 1;
		usePerformanceCounter = QueryPerformanceFrequency(&performanceFrequency);
		if (usePerformanceCounter) {
			QueryPerformanceCounter(&offset);
			frequencyToMicroseconds = (double)performanceFrequency.QuadPart / 1000000.;
		}
		else {
			offset = getFILETIMEoffset();
			frequencyToMicroseconds = 10.;
		}
	}
	if (usePerformanceCounter) QueryPerformanceCounter(&t);
	else {
		GetSystemTimeAsFileTime(&f);
		t.QuadPart = f.dwHighDateTime;
		t.QuadPart <<= 32;
		t.QuadPart |= f.dwLowDateTime;
	}

	t.QuadPart -= offset.QuadPart;
	microseconds = (double)t.QuadPart / frequencyToMicroseconds;
	t.QuadPart = microseconds;
	tv->tv_sec = t.QuadPart / 1000000;
	tv->tv_usec = t.QuadPart % 1000000;
	return 0;
}

unsigned long get_nsecs(struct timeval *myts)
{
	if (clock_gettime(myts))
		terminal_error("clock_gettime");

	return (myts->tv_sec * 1000000000 + myts->tv_usec * 1000);
}

unsigned long get_usecs(struct timeval *myts)
{
	if (clock_gettime(myts))
		terminal_error("clock_gettime");

	return (myts->tv_sec * 1000000 + myts->tv_usec);
}

void burn_loops(unsigned long loops)
{
	unsigned long i;

	/*
	* We need some magic here to prevent the compiler from optimising
	* this loop away. Otherwise trying to emulate a fixed cpu load
	* with this loop will not work.
	*/

	for (i = 0; i < loops; i++)
		_ReadWriteBarrier();

}

/* Use this many usecs of cpu time */
void burn_usecs(unsigned long usecs)
{
	unsigned long ms_loops;

	ms_loops = ud.loops_per_ms / 1000 * usecs;
	burn_loops(ms_loops);
}

void set_realtime()
{
	HANDLE pHandle, tHandle;
	pHandle = GetCurrentProcess();
	tHandle = GetCurrentThread();

	memset(&pHandle, 0, sizeof(pHandle));
	memset(&tHandle, 0, sizeof(tHandle));

	SetPriorityClass(pHandle, REALTIME_PRIORITY_CLASS);
	SetThreadPriority(tHandle, THREAD_PRIORITY_TIME_CRITICAL);

}

void set_normal()
{
	HANDLE pHandle, tHandle;
	pHandle = GetCurrentProcess();
	tHandle = GetCurrentThread();

	memset(&pHandle, 0, sizeof(pHandle));
	memset(&tHandle, 0, sizeof(tHandle));

	SetPriorityClass(pHandle, NORMAL_PRIORITY_CLASS);
	SetThreadPriority(tHandle, THREAD_PRIORITY_IDLE);
}

void sync_flush(void)
{
	if ((fflush(NULL)) == EOF)
		terminal_error("fflush");
	FlushFileBuffers(NULL);
	FlushFileBuffers(NULL);
	FlushFileBuffers(NULL);
}

void set_nice(int prio)
{
	HANDLE pHandle;
	pHandle = GetCurrentThread();

	memset(&pHandle, 0, sizeof(pHandle));
	SetPriorityClass(pHandle, HIGH_PRIORITY_CLASS);
	SetThreadPriority(pHandle, prio);

}

void microsleep(DWORD waitTime){

	Sleep(waitTime / 1000);
}

_inline void post_sem(HANDLE s)
{
retry:
	if ((ReleaseSemaphore(s, 1, NULL)) == 0) {
		if (errno == EINTR)
			goto retry;
		terminal_error("sem_post");
	}
}

_inline void wait_sem(HANDLE s)
{
	int ret;
retry:
	if ((ret = WaitForSingleObject(s, INFINITE)) != WAIT_OBJECT_0) {
		if (errno == EINTR)
			goto retry;
		terminal_error("sem_wait");
	}
}

_inline DWORD trywait_sem(HANDLE s)
{
	DWORD ret;

retry:
	if ((ret = WaitForSingleObject(s, 0)) != WAIT_OBJECT_0) {
		if (errno == EINTR)
			goto retry;
		if (errno != EAGAIN){}
	}
	return ret;
}

_inline int Read(int fd, void *buf, size_t count)
{
	int retval;

retry:
	retval = _read(fd, buf, count);
	if (retval == -1) {
		if (errno == EINTR)
			goto retry;
		terminal_error("read");
	}
	return retval;
}

_inline int Write(int fd, const void *buf, size_t count)
{
	int retval;

retry:
	retval = _write(fd, &buf, count);
	if (retval == -1) {
		if (errno == EINTR)
			goto retry;
		terminal_error("write\n");
	}
	return retval;
}

unsigned long periodic_schedule(struct thread *th, unsigned long run_usecs,
	unsigned long interval_usecs, unsigned long long deadline)
{
	unsigned long long latency, missed_latency;
	unsigned long long current_time;
	struct tk_thread *tk;
	struct data_table *tb;
	struct timeval myts;

	latency = 0;
	tb = th->dt;
	tk = &th->tkthread;

	current_time = get_usecs(&myts);
	if (current_time > deadline + tk->slept_interval)
		latency = current_time - deadline - tk->slept_interval;

	/* calculate the latency for missed frames */
	missed_latency = 0;

	current_time = get_usecs(&myts);
	if (interval_usecs && current_time > deadline + interval_usecs) {
		/* We missed the deadline even before we consumed cpu */
		unsigned long intervals;

		deadline += interval_usecs;
		intervals = (current_time - deadline) /
			interval_usecs + 1;

		tb->missed_deadlines += intervals;
		missed_latency = intervals * interval_usecs;
		deadline += intervals * interval_usecs;
		tb->missed_burns += intervals;
		goto bypass_burn;
	}

	burn_usecs(run_usecs);
	current_time = get_usecs(&myts);
	tb->achieved_burns++;

	/*
	* If we meet the deadline we move the deadline forward, otherwise
	* we consider it a missed deadline and dropped frame etc.
	*/
	deadline += interval_usecs;
	if (deadline >= current_time) {
		tb->deadlines_met++;
	}
	else {
		if (interval_usecs) {
			unsigned long intervals = (current_time - deadline) /
				interval_usecs + 1;

			tb->missed_deadlines += intervals;
			missed_latency = intervals * interval_usecs;
			deadline += intervals * interval_usecs;
			if (intervals > 1)
				tb->missed_burns += intervals;
		}
		else {
			deadline = current_time;
			goto out_nosleep;
		}
	}
bypass_burn:
	tk->sleep_interval = deadline - current_time;

	post_sem(tk->sem.start);
	wait_sem(tk->sem.complete);
out_nosleep:
	/*
	* Must add missed_latency to total here as this function may not be
	* called again and the missed latency can be lost
	*/
	latency += missed_latency;
	if (latency > tb->max_latency)
		tb->max_latency = latency;
	tb->total_latency += latency;
	tb->sum_latency_squared += latency * latency;
	tb->nr_samples++;

	return deadline;
}

void initialise_thread_data(struct data_table *tb)
{
	tb->max_latency =
		tb->total_latency =
		tb->sum_latency_squared =
		tb->deadlines_met =
		tb->missed_deadlines =
		tb->missed_burns =
		tb->nr_samples = 0;
}

void create_pthread(DWORD WINAPI start_routine, LPVOID arg, DWORD thread)
{
	HANDLE cThread;
	if ((cThread = CreateThread(NULL, 0, start_routine, arg, 0, &thread)) == NULL)
		terminal_error("pthread_create");

}

void join_pthread(DWORD th)
{
	HANDLE currentThread = OpenThread(THREAD_ALL_ACCESS, TRUE, th);
	if (WaitForSingleObject(currentThread, INFINITE) == 0)
		terminal_error("pthread_join");
}

void emulate_none(struct thread *th)
{
	HANDLE s = th->sem.stop;
	wait_sem(s);
}

#define AUDIO_INTERVAL	(50000)
#define AUDIO_RUN	(AUDIO_INTERVAL / 20)
/* We emulate audio by using 5% cpu and waking every 50ms */
void emulate_audio(struct thread *th)
{

	unsigned long long deadline;
	HANDLE s = th->sem.stop;
	struct timeval myts;

	th->decasecond_deadlines = 1000000 / AUDIO_INTERVAL * 10;
	deadline = get_usecs(&myts);

	while (1) {
		deadline = periodic_schedule(th, AUDIO_RUN, AUDIO_INTERVAL, deadline);
		if (trywait_sem(s) == WAIT_OBJECT_0)
			return;
	}
}

/* We emulate video by using 40% cpu and waking for 60fps */
#define VIDEO_INTERVAL	(1000000 / 60)
#define VIDEO_RUN	(VIDEO_INTERVAL * 40 / 100)
void emulate_video(struct thread *th)
{

	unsigned long long deadline;
	HANDLE s = th->sem.stop;
	struct timeval myts;

	th->decasecond_deadlines = 1000000 / VIDEO_INTERVAL * 10;
	deadline = get_usecs(&myts);

	while (1) {
		deadline = periodic_schedule(th, VIDEO_RUN, VIDEO_INTERVAL,
			deadline);
		if (trywait_sem(s) == WAIT_OBJECT_0)
			return;
	}
}

/*
* We emulate X by running for a variable percentage of cpu from 0-100%
* in 1ms chunks.
*/
void emulate_x(struct thread *th)
{
	unsigned long long deadline;
	HANDLE s = th->sem.stop;
	struct timeval myts;

	th->decasecond_deadlines = 100;
	deadline = get_usecs(&myts);

	while (1) {
		int i, j;
		for (i = 0; i <= 100; i++) {
			j = 100 - i;
			deadline = periodic_schedule(th, i * 1000, j * 1000,
				deadline);
			deadline += i * 1000;
			if (trywait_sem(s) == WAIT_OBJECT_0)
				return;
		}
	}
}

/*
* We emulate gaming by using 100% cpu and seeing how many frames (jobs
* completed) we can do in that time. Deadlines are meaningless with
* unlocked frame rates. We do not use periodic schedule because for
* this load because this never wants to sleep.
*/
#define GAME_INTERVAL	(100000)
#define GAME_RUN	(GAME_INTERVAL)
void emulate_game(struct thread *th)
{
	unsigned long long deadline, current_time, latency;
	HANDLE s = th->sem.stop;
	struct timeval myts;
	struct data_table *tb;

	tb = th->dt;
	th->decasecond_deadlines = 1000000 / GAME_INTERVAL * 10;

	while (1) {
		deadline = get_usecs(&myts) + GAME_INTERVAL;
		burn_usecs(GAME_RUN);
		current_time = get_usecs(&myts);
		/* use usecs instead of simple count for game burn statistics */
		tb->achieved_burns += GAME_RUN;
		if (current_time > deadline) {
			latency = current_time - deadline;
			tb->missed_burns += latency;
		}
		else
			latency = 0;
		if (latency > tb->max_latency)
			tb->max_latency = latency;
		tb->total_latency += latency;
		tb->sum_latency_squared += latency * latency;
		tb->nr_samples++;
		if (trywait_sem(s) == WAIT_OBJECT_0)
			return;
	}
}

DWORD WINAPI burn_thread(LPVOID t)
{
	struct thread *th;
	HANDLE s;
	long i = (long)t;

	th = &threadlist[i];
	s = th->sem.stopchild;

	while (1) {
		burn_loops(ud.loops_per_ms);
		if (trywait_sem(s) == WAIT_OBJECT_0) {
			post_sem(s);
			break;
		}
	}

	ExitThread(0);
}

/* Have ud.cpu_load threads burn cpu continuously */
void emulate_burn(struct thread *th)
{
	HANDLE s = th->sem.stop;
	unsigned long i;
	long t;
	DWORD * burnthreads = malloc(ud.cpu_load * sizeof(DWORD));

	t = th->threadno;
	for (i = 0; i < ud.cpu_load; i++)
		create_pthread(burn_thread, (void*)(long)t, &burnthreads[i]);
	wait_sem(s);
	post_sem(th->sem.stopchild);
	for (i = 0; i < ud.cpu_load; i++)
		join_pthread(burnthreads[i], NULL);

	free(burnthreads);
}

DWORD WINAPI timekeeping_thread(LPVOID lpParam)
{
	struct thread *th;
	struct tk_thread *tk;
	struct sems *s;
	struct timeval myts;
	long i = (long)lpParam;

	th = &threadlist[i];
	tk = &th->tkthread;
	s = &th->tkthread.sem;
	/*
	* If this timekeeping thread is that of a benchmarked thread we run
	* even higher priority than the benched thread is if running real
	* time. Otherwise, the load timekeeping thread, which does not need
	* accurate accounting remains SCHED_NORMAL;
	*/
	if (th->dt != &th->benchmarks[NOT_BENCHING])
		set_nice(29);
	/* These values must be changed at the appropriate places or race */
	tk->sleep_interval = tk->slept_interval = 0;

	post_sem(s->ready);

	while (1) {
		unsigned long start_time, now;

		if (trywait_sem(s->stop) == WAIT_OBJECT_0)
			goto out;
		wait_sem(s->start);
		tk->slept_interval = 0;
		start_time = get_usecs(&myts);
		if (trywait_sem(s->stop) == WAIT_OBJECT_0)
			goto out;
		if (tk->sleep_interval) {
			unsigned long diff = 0;
			microsleep(tk->sleep_interval);
			now = get_usecs(&myts);
			/* now should always be > start_time but... */
			if (now > start_time) {
				diff = now - start_time;
				if (diff > tk->sleep_interval)
					tk->slept_interval = diff -
					tk->sleep_interval;
			}
		}
		tk->sleep_interval = 0;
		post_sem(s->complete);
	}
out:
	ExitThread(0);
}

/*
* All the sleep functions such as nanosleep can only guarantee that they
* sleep for _at least_ the time requested. We work around this by having
* a high priority real time thread that accounts for the extra time slept
* in nanosleep. This allows wakeup latency of the tested thread to be
* accurate and reflect true scheduling delays.
*/
DWORD WINAPI emulation_thread(LPVOID lpParam)
{
	struct thread *th;
	struct tk_thread *tk;
	struct sems *s, *tks;
	long i = (long)lpParam;

	th = &threadlist[i];
	tk = &th->tkthread;
	s = &th->sem;
	tks = &tk->sem;
	init_all_sems(tks);

	/* Start the timekeeping thread */
	create_pthread(timekeeping_thread, (void*)(long)i, &th->tk_pthread);
	/* Wait for timekeeping thread to be ready */
	wait_sem(tks->ready);


	/* Tell main we're ready to start*/
	post_sem(s->ready);

	/* Wait for signal from main to start thread */
	wait_sem(s->start);

	/* Start the actual function being benched/or running as load */
	th->name(th);

	/* Stop the timekeeping thread */
	post_sem(tks->stop);
	post_sem(tks->start);
	join_pthread(th->tk_pthread, NULL);

	/* Tell main we've finished */
	post_sem(s->complete);
	ExitThread(0);
}

void calibrate_loop()
{
	unsigned long long start_time, loops_per_msec, run_time = 0;
	unsigned long loops;
	struct timeval myts;

	loops_per_msec = 100000;

redo:
	/* Calibrate to within 1% accuracy */
	while (run_time > 1010000 || run_time < 990000) {
		loops = loops_per_msec;
		start_time = get_nsecs(&myts);
		burn_loops(loops);
		run_time = get_nsecs(&myts) - start_time;
		loops_per_msec = ((1000000 * loops_per_msec / run_time) ? (1000000 * loops_per_msec / run_time) :
			loops_per_msec);
	}

	/* Rechecking after a pause increases reproducibility */
	Sleep(1 * 1000);
	loops = loops_per_msec;
	start_time = get_nsecs(&myts);
	burn_loops(loops);
	run_time = get_nsecs(&myts) - start_time;

	/* Tolerate 5% difference on checking */
	if (run_time > 1050000 || run_time < 950000)
		goto redo;

	ud.loops_per_ms = loops_per_msec;
}

/* Calculate statistics and output them */
void show_latencies(struct thread *th)
{
	struct data_table *tbj;
	struct tk_thread *tk;
	double average_latency, deadlines_met, samples_met, sd, max_latency;
	long double variance = 0;

	tbj = th->dt;
	tk = &th->tkthread;

	if (tbj->nr_samples > 1) {
		average_latency = tbj->total_latency / tbj->nr_samples;
		variance = (tbj->sum_latency_squared - (average_latency *
			average_latency) / tbj->nr_samples) / (tbj->nr_samples - 1);
		sd = sqrtl(variance);
	}
	else {
		average_latency = tbj->total_latency;
		sd = 0.0;
	}

	/*
	* Landing on the boundary of a deadline can make loaded runs appear
	* to do more work than unloaded due to tiny duration differences.
	*/
	if (tbj->achieved_burns > 0)
		samples_met = (double)tbj->achieved_burns /
		(double)(tbj->achieved_burns + tbj->missed_burns) * 100;
	else
		samples_met = 0.0;
	max_latency = tbj->max_latency;
	/* When benchmarking rt we represent the data in us */
	if (!ud.do_rt) {
		average_latency /= 1000;
		sd /= 1000;
		max_latency /= 1000;
	}
	if (tbj->deadlines_met == 0)
		deadlines_met = 0;
	else
		deadlines_met = (double)tbj->deadlines_met /
		(double)(tbj->missed_deadlines + tbj->deadlines_met) * 100;

	/*
	* Messy nonsense to format the output nicely. Values less than 1ms
	* are meaningless for interactivity and values less than 1us for real
	* time tests are below noise, so round off to integers.
	*/
	printf("%6.1f +/- ", average_latency);
	printf("%-8.1f", sd);
	printf("%6.1f\t", max_latency);
	printf("\t%4.3g", samples_met);
	if (!th->nodeadlines)
		printf("\t%11.3g", deadlines_met);
	printf("\n");
	sync_flush();
}

void start_thread(struct thread *th)
{
	post_sem(th->sem.start);
}

void stop_thread(struct thread *th)
{
	post_sem(th->sem.stop);
	wait_sem(th->sem.complete);

	/* Kill the thread */
	join_pthread(th->pthread, NULL);
}

HANDLE init_sem(HANDLE sem)
{
	sem = CreateSemaphore(NULL, 0, 1, NULL);
	if (sem == NULL)
		terminal_error("sem_init");
	return sem;
}

void init_all_sems(struct sems *s)
{
	/* Initialise the semaphores */
	s->ready = init_sem(s->ready);
	s->start = init_sem(s->start);
	s->stop = init_sem(s->stop);
	s->complete = init_sem(s->complete);
	s->stopchild = init_sem(s->stopchild);

}

void initialise_thread(int i)
{

	struct thread *th = &threadlist[i];

	init_all_sems(&th->sem);
	/* Create the threads. Yes, the (long) cast is fugly but it's safe*/
	create_pthread(emulation_thread, (void*)(long)i, &th->pthread);

	wait_sem(th->sem.ready);
	/*
	* We set this pointer generically to NOT_BENCHING and set it to the
	* benchmarked array entry only on benched threads.
	*/
	th->dt = &th->benchmarks[NOT_BENCHING];
	initialise_thread_data(th->dt);

}

DWORD WINAPI run_loadchild(LPVOID lpParam)
{

	int j = *((int*)lpParam);

	struct thread *thj;
	thj = &threadlist[j];

	set_nice(30);
	initialise_thread(j);

	/* Tell main we're ready */
	ReleaseSemaphore(l2m, 1, NULL);

	/* Main tells us we're ready */
	WaitForSingleObject(m2l, INFINITE);
	start_thread(thj);

	/* Tell main we received the start and are running */
	ReleaseSemaphore(l2m, 1, NULL);

	/* Main tells us to stop */
	WaitForSingleObject(m2l, INFINITE);
	stop_thread(thj);

	/* Tell main we've finished */
	ReleaseSemaphore(l2m, 1, NULL);

	ExitThread(0);
}

struct Data
{
	int i;
	int j;
};

DWORD WINAPI run_benchchild(LPVOID lpParam){

	struct Data * task = (struct Data*)lpParam;
	int i, j;
	i = task->i;
	j = task->j;

	struct thread *thi;

	thi = &threadlist[i];

	set_nice(ud.bench_nice);
	
	initialise_thread(i);
	/* Point the data table to the appropriate load being tested */
	thi->dt = &thi->benchmarks[j];
	initialise_thread_data(thi->dt);
	if (ud.do_rt)
		set_realtime();//set_thread_fifo(thi->pthread, 95);

	/* Tell main we're ready */
	ReleaseSemaphore(b2m, 1, NULL);

	/* Main tells us we're ready */
	WaitForSingleObject(m2b, INFINITE);
	start_thread(thi);

	/* Tell main we have started */
	ReleaseSemaphore(b2m, 1, NULL);

	/* Main tells us to stop */
	WaitForSingleObject(m2b, INFINITE);
	stop_thread(thi);

	show_latencies(thi);

	/* Tell main we've finished */
	ReleaseSemaphore(b2m, 1, NULL);

	ExitThread(0);
}

void bench(int i, int j)
{
	//printf("\nBench \n");
	HANDLE bench_pid, load_pid;
	struct Data * task = malloc(sizeof(struct Data));
	task->i = i;
	task->j = j;

	load_pid = CreateThread(NULL, 0, run_loadchild, &j, 0, NULL);
	if (load_pid == NULL)
		terminal_error("Load Thread");

	/* Wait for load process to be ready */
	WaitForSingleObject(l2m, INFINITE);

	bench_pid = CreateThread(NULL, 0, run_benchchild, task, 0, NULL);
	if (bench_pid == NULL)
		terminal_error("Bench Thread");

	/* Wait for bench process to be ready */
	WaitForSingleObject(b2m, INFINITE);

	/*
	* We want to be higher priority than everything to signal them to
	* stop and we lock our memory if we can as well
	*/
	set_realtime();
	//set_mlock();

	/* Wakeup the load process */
	ReleaseSemaphore(m2l, 1, NULL);
	/* Load tells it has received the first message and is running */
	WaitForSingleObject(l2m, INFINITE);

	/* After a small delay, wake up the benched process */
	Sleep(1000);
	ReleaseSemaphore(m2b, 1, NULL);

	/* Bench tells it has received the first message and is running */
	WaitForSingleObject(b2m, INFINITE);
	microsleep(ud.duration * 1000000);

	/* Tell the benched process to stop its threads and output results */
	ReleaseSemaphore(m2b, 1, NULL);

	/* Tell the load process to stop its threads */
	ReleaseSemaphore(m2l, 1, NULL);

	/* Return to SCHED_NORMAL */
	set_normal();
	//set_munlock();

	/* Wait for load and bench processes to terminate */
	WaitForSingleObject(l2m, INFINITE);
	WaitForSingleObject(b2m, INFINITE);

	CloseHandle(load_pid);
	CloseHandle(bench_pid);

}

void init_pipes(void)
{
	m2l = CreateSemaphore(NULL, 0, 1, NULL);
	l2m = CreateSemaphore(NULL, 0, 1, NULL);
	m2b = CreateSemaphore(NULL, 0, 1, NULL);
	b2m = CreateSemaphore(NULL, 0, 1, NULL);
}

_inline int bit_is_on(const unsigned int mask, int index)
{
	return (mask & (1 << index)) != 0;
}

int main(int argc, char **argv){

	HANDLE process = GetCurrentProcess();
	DWORD_PTR processAffinityMask = 1;
	SetProcessAffinityMask(process, processAffinityMask);

	unsigned long custom_cpu = 0;
	int q, i, j, affinity, benchmark = 0;
	unsigned int selected_loads = 0;
	unsigned int excluded_loads = 0;
	unsigned int selected_benches = 0;
	unsigned int excluded_benches = 0;
	FILE *fp;
	/*
	* This file stores the loops_per_ms to be reused in a filename that
	* can't be confused
	*/
	char *fname = "interbench_loops_per_ms.txt";
	char *comment = NULL;

	/* default is all loads */
	if (selected_loads == 0)
		selected_loads = (unsigned int)-1;
	selected_loads &= ~excluded_loads;
	/* default is all benches */
	if (selected_benches == 0)
		selected_benches = (unsigned int)-1;
	selected_benches &= ~excluded_benches;

	/* Make benchmark a multiple of 10 seconds for proper range of X loads */
	if (ud.duration % 10)
		ud.duration += 10 - ud.duration % 10;

	if (benchmark)
		ud.loops_per_ms = 0;

	/*
	* Try to get loops_per_ms from command line first, file second, and
	* benchmark if not available.
	*/
	if (!ud.loops_per_ms) {
		if (benchmark)
			goto bench;
		if ((fp = fopen(fname, "r"))) {
			fscanf(fp, "%lu", &ud.loops_per_ms);
			if (fclose(fp) == -1)
				terminal_error("fclose");
			if (ud.loops_per_ms) {
				fprintf(stderr,
					"%lu loops_per_ms read from file interbench.loops_per_ms\n",
					ud.loops_per_ms);
				goto loops_known;
			}
		}
		else
		if (errno != ENOENT)
			terminal_error("fopen");
	bench:
		fprintf(stderr, "loops_per_ms unknown; benchmarking...\n");

		/*
		* To get as accurate a loop as possible we time it running
		* SCHED_FIFO if we can
		*/
		set_realtime();
		calibrate_loop();
		set_normal();
	}
	else
		fprintf(stderr, "loops_per_ms specified from command line\n");

	if (!(fp = fopen(fname, "w"))) {
		if (errno != EACCES)	/* No write access is not terminal */
			terminal_error("fopen");
		fprintf(stderr, "Unable to write to file interbench_loops_per_ms.txt\n");
		goto loops_known;
	}
	fprintf(fp, "%lu", ud.loops_per_ms);
	fprintf(stderr, "%lu loops_per_ms saved to file interbench_loops_per_ms.txt\n",
		ud.loops_per_ms);
	if (fclose(fp) == -1)
		terminal_error("fclose");

loops_known:
	init_pipes();

	printf("\n");
	printf("Using %lu loops per ms, running every load for %d seconds\n",
		ud.loops_per_ms, ud.duration);
	if (comment)
		printf("Comment: %s\n", comment);
	printf("\n");

	for (i = 0; i < THREADS; i++)
		threadlist[i].threadno = i;

	for (i = 0; i < THREADS; i++) {
		struct thread *thi = &threadlist[i];
		int *benchme;

		if (ud.do_rt)
			benchme = &threadlist[i].rtbench;
		else
			benchme = &threadlist[i].bench;

		if (!*benchme || !bit_is_on(selected_benches, i))
			continue;

		printf("--- Benchmarking simulated cpu of %s ", threadlist[i].label);
		if (ud.do_rt)
			printf("real time ");
		else if (ud.bench_nice)
			printf("nice %d ", ud.bench_nice);
		printf("in the presence of simulated ");
		if (ud.load_nice)
			printf("nice %d ", ud.load_nice);
		printf("---\n");

		printf("Load");
		if (ud.do_rt)
			printf("\tLatency +/- SD (us)");
		else
			printf("\tLatency +/- SD (ms)");
		printf("  Max Latency ");
		printf("  %% Desired CPU");
		if (!thi->nodeadlines)
			printf("  %% Deadlines Met");
		printf("\n");

		for (j = 0; j < THREADS; j++) {
			struct thread *thj = &threadlist[j];

			if (j == i || !bit_is_on(selected_loads, j) ||
				(!threadlist[j].load && !ud.do_rt) ||
				(!threadlist[j].rtload && ud.do_rt))
				continue;
			printf("%s\t", thj->label);
			sync_flush();
			bench(i, j);
		}
		printf("\n");
	}
	printf("\n");

	system("pause");
	return 0;
}
