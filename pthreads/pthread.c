/*
  Copyright (C) 2014 Szilard Biro

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#ifdef __MORPHOS__
#include <sys/time.h>
#endif
#include <dos/dostags.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#ifdef __AROS__
#include <aros/symbolsets.h>
#else
#include <constructor.h>
#define StackSwapArgs PPCStackSwapArgs
#define NewStackSwap NewPPCStackSwap
#endif

#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>

#include "pthread.h"
#include "debug.h"

#define FALLBACKSIGNAL SIGBREAKB_CTRL_E
#define NAMELEN 32
#define PTHREAD_INVALID_ID ((pthread_t)-1)
#define PTHREAD_FIRST_THREAD_ID (1)

typedef struct
{
	struct MinNode node;
	struct Task *task;
	ULONG sigmask;
} CondWaiter;

typedef struct
{
	void *value;
	void (*destructor)(void *);
	BOOL used;
} TLSKey;

typedef struct
{
	struct MinNode node;
	void (*routine)(void *);
	void *arg;
} CleanupHandler;

typedef struct
{
	void *(*start)(void *);
	void *arg;
	struct MsgPort *msgport;
	struct Message msg;
	struct Task *task;
	void *ret;
	jmp_buf jmp;
	pthread_attr_t attr;
	TLSKey tls[PTHREAD_KEYS_MAX];
	struct MinList cleanup;
} ThreadInfo;

static ThreadInfo threads[PTHREAD_THREADS_MAX];
static struct SignalSemaphore thread_sem;

//
// Helper functions
//

static int SemaphoreIsInvalid(struct SignalSemaphore *sem)
{
	DB2(bug("%s(%p)\n", __FUNCTION__, sem));

	return (!sem || sem->ss_Link.ln_Type != NT_SIGNALSEM || sem->ss_WaitQueue.mlh_Tail != NULL);
}

static ThreadInfo *GetThreadInfo(pthread_t thread)
{
	ThreadInfo *inf = NULL;

	DB2(bug("%s(%u)\n", __FUNCTION__, thread));

	// TODO: more robust error handling?
	if (thread < PTHREAD_THREADS_MAX)
		inf = &threads[thread];

	return inf;
}

static pthread_t GetThreadId(struct Task *task)
{
	pthread_t i;

	DB2(bug("%s(%p)\n", __FUNCTION__, task));

	ObtainSemaphore(&thread_sem);

	// First thread id will be 1 so that it is different than default value of pthread_t
	for (i = PTHREAD_FIRST_THREAD_ID; i < PTHREAD_THREADS_MAX; i++)
	{
		if (threads[i].task == task)
			break;
	}

	ReleaseSemaphore(&thread_sem);

	if (i >= PTHREAD_THREADS_MAX)
		i = PTHREAD_INVALID_ID;

	return i;
}

#if defined __mc68000__
/* No CAS instruction on m68k */
static int __m68k_sync_val_compare_and_swap(int *v, int o, int n)
{
	int ret;

	Disable();
	if ((*v) == (o))
		(*v) = (n);
	ret = (*v);
	Enable();

	return ret;
}
#undef __sync_val_compare_and_swap
#define __sync_val_compare_and_swap(v, o, n) __m68k_sync_val_compare_and_swap(v, o, n)

static int __m68k_sync_lock_test_and_set(int *v, int n)
{
	Disable();
	(*v) = (n);
	Enable();

	return n;
}
#undef __sync_lock_test_and_set
#define __sync_lock_test_and_set(v, n) __m68k_sync_lock_test_and_set(v, n)
#undef __sync_lock_release
#define __sync_lock_release(v) __m68k_sync_lock_test_and_set(v, 0)

static int __m68k_sync_add_and_fetch(int *v, int n)
{
	int ret;

	Disable();
	(*v) += (n);
	ret = (*v);
	Enable();

	return ret;
}
#undef __sync_add_and_fetch
#define __sync_add_and_fetch(v, n) __m68k_sync_add_and_fetch(v, n)
#undef __sync_sub_and_fetch
#define __sync_sub_and_fetch(v, n) __m68k_sync_add_and_fetch(v, -(n))
#endif

//
// Thread specific data functions
//

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
	pthread_t thread;
	ThreadInfo *inf;
	TLSKey *tls;
	int i;

	D(bug("%s(%p, %p)\n", __FUNCTION__, key, destructor));

	if (key == NULL)
		return EINVAL;

	thread = pthread_self();
	inf = GetThreadInfo(thread);

	for (i = 0; i < PTHREAD_KEYS_MAX; i++)
	{
		if (inf->tls[i].used == FALSE)
			break;
	}

	if (i >= PTHREAD_KEYS_MAX)
		return EAGAIN;

	tls = &inf->tls[i];
	tls->used = TRUE;
	tls->destructor = destructor;

	*key = i;

	return 0;
}

int pthread_key_delete(pthread_key_t key)
{
	pthread_t thread;
	ThreadInfo *inf;
	TLSKey *tls;

	D(bug("%s(%u)\n", __FUNCTION__, key));

	if (key >= PTHREAD_KEYS_MAX)
		return EINVAL;

	thread = pthread_self();
	inf = GetThreadInfo(thread);
	tls = &inf->tls[key];

	if (tls->used == FALSE)
		return EINVAL;

	if (tls->destructor)
		tls->destructor(tls->value);

	tls->used = FALSE;
	tls->destructor = NULL;

	return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
	pthread_t thread;
	ThreadInfo *inf;
	TLSKey *tls;

	D(bug("%s(%u)\n", __FUNCTION__, key));

	if (key >= PTHREAD_KEYS_MAX)
		return EINVAL;

	thread = pthread_self();
	inf = GetThreadInfo(thread);
	tls = &inf->tls[key];

	if (tls->used == FALSE)
		return EINVAL;

	tls->value = (void *)value;

	return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
	pthread_t thread;
	ThreadInfo *inf;
	TLSKey *tls;
	void *value = NULL;

	D(bug("%s(%u)\n", __FUNCTION__, key));

	if (key >= PTHREAD_KEYS_MAX)
		return NULL;

	thread = pthread_self();
	inf = GetThreadInfo(thread);
	tls = &inf->tls[key];

	if (tls->used == TRUE)
		value = tls->value;

	return value;
}

//
// Mutex attribute functions
//

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
	D(bug("%s(%p)\n", __FUNCTION__, attr));

	if (attr == NULL)
		return EINVAL;

	memset(attr, 0, sizeof(pthread_mutexattr_t));

	return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
	D(bug("%s(%p)\n", __FUNCTION__, attr));

	if (attr == NULL)
		return EINVAL;

	memset(attr, 0, sizeof(pthread_mutexattr_t));

	return 0;
}

int pthread_mutexattr_gettype(pthread_mutexattr_t *attr, int *kind)
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, attr, kind));

	if (attr == NULL)
		return EINVAL;

	if (kind)
		*kind = attr->kind;

	return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int kind)
{
	D(bug("%s(%p)\n", __FUNCTION__, attr));

	if (attr == NULL)
		return EINVAL;

	attr->kind = kind;

	return 0;
}

//
// Mutex functions
//

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, mutex, attr));

	if (mutex == NULL)
		return EINVAL;

	InitSemaphore(&mutex->semaphore);

	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	D(bug("%s(%p)\n", __FUNCTION__, mutex));

	if (mutex == NULL || SemaphoreIsInvalid(&mutex->semaphore))
		return EINVAL;

	// TODO: handle mutexes being used with condition variables
	if (AttemptSemaphore(&mutex->semaphore) == FALSE)
		return EBUSY;

	ReleaseSemaphore(&mutex->semaphore);
	memset(mutex, 0, sizeof(pthread_mutex_t));

	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	D(bug("%s(%p)\n", __FUNCTION__, mutex));

	if (mutex == NULL)
		return EINVAL;

	// initialize static mutexes
	if (SemaphoreIsInvalid(&mutex->semaphore))
		pthread_mutex_init(mutex, NULL);

	ObtainSemaphore(&mutex->semaphore);

	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	ULONG ret;

	D(bug("%s(%p)\n", __FUNCTION__, mutex));

	if (mutex == NULL)
		return EINVAL;

	// initialize static mutexes
	if (SemaphoreIsInvalid(&mutex->semaphore))
		pthread_mutex_init(mutex, NULL);

	ret = AttemptSemaphore(&mutex->semaphore);

	return (ret == TRUE) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	D(bug("%s(%p)\n", __FUNCTION__, mutex));

	if (mutex == NULL)
		return EINVAL;

	// initialize static mutexes
	if (SemaphoreIsInvalid(&mutex->semaphore))
		pthread_mutex_init(mutex, NULL);

	ReleaseSemaphore(&mutex->semaphore);

	return 0;
}

//
// Condition variable attribute functions
//

int pthread_condattr_init(pthread_condattr_t *attr)
{
	D(bug("%s(%p)\n", __FUNCTION__, attr));

	if (attr == NULL)
		return EINVAL;

	memset(attr, 0, sizeof(pthread_condattr_t));

	return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr)
{
	D(bug("%s(%p)\n", __FUNCTION__, attr));

	if (attr == NULL)
		return EINVAL;

	memset(attr, 0, sizeof(pthread_condattr_t));

	return 0;
}

//
// Condition variable functions
//

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, cond, attr));

	if (cond == NULL)
		return EINVAL;

	InitSemaphore(&cond->semaphore);
	NewList((struct List *)&cond->waiters);

	return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
	D(bug("%s(%p)\n", __FUNCTION__, cond));

	if (cond == NULL || SemaphoreIsInvalid(&cond->semaphore))
		return EINVAL;

	if (AttemptSemaphore(&cond->semaphore) == FALSE)
		return EBUSY;

	if (!IsListEmpty((struct List *)&cond->waiters))
	{
		ReleaseSemaphore(&cond->semaphore);
		return EBUSY;
	}

	ReleaseSemaphore(&cond->semaphore);
	memset(cond, 0, sizeof(pthread_cond_t));

	return 0;
}

static int _pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime, BOOL relative)
{
	CondWaiter waiter;
	BYTE signal;
	ULONG sigs = 0;
	ULONG timermask = 0;
	struct MsgPort *timermp = NULL;
	struct timerequest *timerio = NULL;
	struct Device *TimerBase = NULL;

	DB2(bug("%s(%p, %p, %p)\n", __FUNCTION__, cond, mutex, abstime));

	if (cond == NULL || mutex == NULL)
		return EINVAL;

	// initialize static conditions
	if (SemaphoreIsInvalid(&cond->semaphore))
		pthread_cond_init(cond, NULL);

	if (abstime)
	{
		if (!(timermp = CreateMsgPort()))
			return EINVAL;

		if (!(timerio = (struct timerequest *)CreateIORequest(timermp, sizeof(struct timerequest))))
		{
			DeleteMsgPort(timermp);
			return EINVAL;
		}

		if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, &timerio->tr_node, 0) != 0)
		{
			DeleteMsgPort(timermp);
			DeleteIORequest((struct IORequest *)timerio);
			return EINVAL;
		}

		TimerBase = timerio->tr_node.io_Device;


		timerio->tr_node.io_Command = TR_ADDREQUEST;
		timerio->tr_time.tv_secs = abstime->tv_sec;
		timerio->tr_time.tv_micro = abstime->tv_nsec / 1000;
		if (!relative)
		{
			struct timeval starttime;

			// GetSysTime can't be used due to the timezone offset in abstime
			gettimeofday(&starttime, NULL);
			SubTime(&timerio->tr_time, &starttime);
		}
		timermask = 1 << timermp->mp_SigBit;
		sigs |= timermask;
		SendIO((struct IORequest *)timerio);
	}

	waiter.task = FindTask(NULL);
	signal = AllocSignal(-1);
	if (signal == -1)
	{
		signal = FALLBACKSIGNAL;
		SetSignal(1 << FALLBACKSIGNAL, 0);
	}
	waiter.sigmask = 1 << signal;
	sigs |= waiter.sigmask;
	ObtainSemaphore(&cond->semaphore);
	AddTail((struct List *)&cond->waiters, (struct Node *)&waiter);
	ReleaseSemaphore(&cond->semaphore);

	pthread_mutex_unlock(mutex);
	sigs = Wait(sigs);
	pthread_mutex_lock(mutex);

	ObtainSemaphore(&cond->semaphore);
	Remove((struct Node *)&waiter);
	ReleaseSemaphore(&cond->semaphore);

	if (signal != FALLBACKSIGNAL)
		FreeSignal(signal);

	if (TimerBase)
	{
		if (!CheckIO((struct IORequest *)timerio))
		{
			AbortIO((struct IORequest *)timerio);
			WaitIO((struct IORequest *)timerio);
		}
		CloseDevice((struct IORequest *)timerio);
		DeleteIORequest((struct IORequest *)timerio);
		DeleteMsgPort(timermp);

		if (sigs & timermask)
			return ETIMEDOUT;
	}

	return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
	D(bug("%s(%p, %p, %p)\n", __FUNCTION__, cond, mutex, abstime));

	return _pthread_cond_timedwait(cond, mutex, abstime, FALSE);
}

int pthread_cond_timedwait_relative_np(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *reltime)
{
	D(bug("%s(%p, %p, %p)\n", __FUNCTION__, cond, mutex, reltime));

	return _pthread_cond_timedwait(cond, mutex, reltime, TRUE);
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	D(bug("%s(%p)\n", __FUNCTION__, cond));

	return _pthread_cond_timedwait(cond, mutex, NULL, FALSE);
}

static int _pthread_cond_broadcast(pthread_cond_t *cond, BOOL onlyfirst)
{
	CondWaiter *waiter;

	DB2(bug("%s(%p, %d)\n", __FUNCTION__, cond, onlyfirst));

	if (cond == NULL)
		return EINVAL;

	// initialize static conditions
	if (SemaphoreIsInvalid(&cond->semaphore))
		pthread_cond_init(cond, NULL);

	ObtainSemaphore(&cond->semaphore);
	ForeachNode(&cond->waiters, waiter)
	{
		Signal(waiter->task, waiter->sigmask);
		if (onlyfirst) break;
	}
	ReleaseSemaphore(&cond->semaphore);

	return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
	D(bug("%s(%p)\n", __FUNCTION__, cond));

	return _pthread_cond_broadcast(cond, TRUE);
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
	D(bug("%s(%p)\n", __FUNCTION__, cond));

	return _pthread_cond_broadcast(cond, FALSE);
}

//
// Barrier functions
//

int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count)
{
	D(bug("%s(%p, %p, %u)\n", __FUNCTION__, barrier, attr, count));

	if (barrier == NULL || count == 0)
		return EINVAL;

	barrier->curr_height = barrier->init_height = count;
	pthread_cond_init(&barrier->breeched, NULL);
	pthread_mutex_init(&barrier->lock, NULL);

	return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
	D(bug("%s(%p)\n", __FUNCTION__, barrier));

	if (barrier == NULL)
		return EINVAL;

	if (pthread_mutex_trylock(&barrier->lock) != 0)
		return EBUSY;

	pthread_mutex_unlock(&barrier->lock);
	pthread_cond_destroy(&barrier->breeched);
	pthread_mutex_destroy(&barrier->lock);
	barrier->curr_height = barrier->init_height = 0;

	return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier)
{
	int result = 0;

	D(bug("%s(%p)\n", __FUNCTION__, barrier));

	if (barrier == NULL)
		return EINVAL;

	if (__sync_sub_and_fetch(&barrier->curr_height, 1) == 0)
	{
		result = pthread_cond_broadcast(&barrier->breeched);
	}
	else
	{
		pthread_mutex_lock(&barrier->lock);
		result = pthread_cond_wait(&barrier->breeched, &barrier->lock);
		pthread_mutex_unlock(&barrier->lock);
	}

	if (__sync_add_and_fetch(&barrier->curr_height, 1) == barrier->init_height)
	{
		if (result == 0)
			result = PTHREAD_BARRIER_SERIAL_THREAD;
	}

	return result;
}

//
// Read-write lock functions
//

int pthread_rwlock_init(pthread_rwlock_t *lock, const pthread_rwlockattr_t *attr)
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, lock, attr));

	if (lock == NULL)
		return EINVAL;

	pthread_mutex_init(&lock->exclusive, NULL);
	pthread_mutex_init(&lock->shared, NULL);
	pthread_cond_init(&lock->shared_completed, NULL);
	lock->exclusive_count = lock->shared_count = lock->completed_count = 0;

	return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *lock)
{
	D(bug("%s(%p)\n", __FUNCTION__, lock));

	if (lock == NULL)
		return EINVAL;

	if (pthread_mutex_trylock(&lock->exclusive) != 0)
		return EBUSY;

	pthread_mutex_unlock(&lock->exclusive);
	pthread_cond_destroy(&lock->shared_completed);
	pthread_mutex_destroy(&lock->shared);
	pthread_mutex_destroy(&lock->exclusive);
	lock->exclusive_count = lock->shared_count = lock->completed_count = 0;

	return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *lock)
{
	D(bug("%s(%p)\n", __FUNCTION__, lock));

	if (lock == NULL)
		return EINVAL;

	if (SemaphoreIsInvalid(&lock->exclusive.semaphore))
		pthread_rwlock_init(lock, NULL);

	if (pthread_mutex_trylock(&lock->exclusive) != 0)
		return EBUSY;

	if (lock->shared_count++ == /*INT_MAX*/ (int)0x7FFFFFFF)
	{
		pthread_mutex_lock(&lock->shared);

		lock->shared_count -= lock->completed_count;
		lock->completed_count = 0;

		pthread_mutex_unlock(&lock->shared);
	}

	return pthread_mutex_unlock(&lock->exclusive);
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *lock)
{
	D(bug("%s(%p)\n", __FUNCTION__, lock));

	if (lock == NULL)
		return EINVAL;

	if (SemaphoreIsInvalid(&lock->exclusive.semaphore))
		pthread_rwlock_init(lock, NULL);

	if (pthread_mutex_trylock(&lock->exclusive) != 0)
		return EBUSY;

	if (pthread_mutex_trylock(&lock->shared) != 0)
	{
		pthread_mutex_unlock(&lock->exclusive);
		return EBUSY;
	}

	if (lock->exclusive_count != 0)
	{
		pthread_mutex_unlock(&lock->shared);
		pthread_mutex_unlock(&lock->exclusive);
		return EBUSY;
	}

	if (lock->completed_count > 0)
	{
		lock->shared_count -= lock->completed_count;
		lock->completed_count = 0;
	}

	if (lock->shared_count > 0)
	{
		pthread_mutex_unlock(&lock->shared);
		pthread_mutex_unlock(&lock->exclusive);
	}
	else
	{
		lock->exclusive_count = 1;
	}

	return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *lock)
{
	D(bug("%s(%p)\n", __FUNCTION__, lock));

	if (lock == NULL)
		return EINVAL;

	if (SemaphoreIsInvalid(&lock->exclusive.semaphore))
		pthread_rwlock_init(lock, NULL);

	pthread_mutex_lock(&lock->exclusive);

	if (lock->shared_count++ == /*INT_MAX*/ (int)0x7FFFFFFF)
	{
		pthread_mutex_lock(&lock->shared);

		lock->shared_count -= lock->completed_count;
		lock->completed_count = 0;

		pthread_mutex_unlock(&lock->shared);
	}

	return pthread_mutex_unlock(&lock->exclusive);
}

int pthread_rwlock_wrlock(pthread_rwlock_t *lock)
{
	int result = 0;

	D(bug("%s(%p)\n", __FUNCTION__, lock));

	if (lock == NULL)
		return EINVAL;

	if (SemaphoreIsInvalid(&lock->exclusive.semaphore))
		pthread_rwlock_init(lock, NULL);

	pthread_mutex_lock(&lock->exclusive);
	pthread_mutex_lock(&lock->shared);

	if (lock->exclusive_count == 0)
	{
		if (lock->completed_count > 0)
		{
			lock->shared_count -= lock->completed_count;
			lock->completed_count = 0;
		}

		if (lock->shared_count > 0)
		{
			lock->completed_count = -lock->shared_count;

			do
			{
				result = pthread_cond_wait(&lock->shared_completed, &lock->shared);
			}
			while (result == 0 && lock->completed_count < 0);

			if (result == 0)
				lock->shared_count = 0;
		}
	}

	if (result == 0)
		lock->exclusive_count++;

	return result;
}

int pthread_rwlock_unlock(pthread_rwlock_t *lock)
{
	int result = 0;

	D(bug("%s(%p)\n", __FUNCTION__, lock));

	if (lock == NULL)
		return EINVAL;

	if (SemaphoreIsInvalid(&lock->exclusive.semaphore))
		return 0; // race condition?

	if (lock->exclusive_count == 0)
	{
		pthread_mutex_lock(&lock->shared);

		if (lock->completed_count++ == 0)
			result = pthread_cond_signal(&lock->shared_completed);

		pthread_mutex_unlock(&lock->shared);
	}
	else
	{
		lock->exclusive_count--;

		pthread_mutex_unlock(&lock->shared);
		pthread_mutex_unlock(&lock->exclusive);
	}

	return result;
}

//
// Spinlock functions
//

int pthread_spin_init(pthread_spinlock_t *lock, int pshared)
{
	D(bug("%s(%p, %d)\n", __FUNCTION__, lock, pshared));

	if (lock == NULL)
		return EINVAL;

	*lock = 0;

	return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock)
{
	D(bug("%s(%p)\n", __FUNCTION__, lock));

	return 0;
}

int pthread_spin_lock(pthread_spinlock_t *lock)
{
	D(bug("%s(%p)\n", __FUNCTION__, lock));

	if (lock == NULL)
		return EINVAL;

	while (__sync_lock_test_and_set((int *)lock, 1))
		sched_yield(); // TODO: don't yield the CPU every iteration

	return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *lock)
{
	D(bug("%s(%p)\n", __FUNCTION__, lock));

	if (lock == NULL)
		return EINVAL;

	if (__sync_lock_test_and_set((int *)lock, 1))
		return EBUSY;

	return 0;
}

int pthread_spin_unlock(pthread_spinlock_t *lock)
{
	D(bug("%s(%p)\n", __FUNCTION__, lock));

	if (lock == NULL)
		return EINVAL;

	__sync_lock_release((int *)lock);

	return 0;
}

//
// Thread attribute functions
//

int pthread_attr_init(pthread_attr_t *attr)
{
	struct Task *task;

	D(bug("%s(%p)\n", __FUNCTION__, attr));

	if (attr == NULL)
		return EINVAL;

	memset(attr, 0, sizeof(pthread_attr_t));
	// inherit the priority and stack size of the parent thread
	task = FindTask(NULL);
	attr->param.sched_priority = task->tc_Node.ln_Pri;
	attr->stacksize = (UBYTE *)task->tc_SPUpper - (UBYTE *)task->tc_SPLower;

	return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
	D(bug("%s(%p)\n", __FUNCTION__, attr));

	if (attr == NULL)
		return EINVAL;

	memset(attr, 0, sizeof(pthread_attr_t));

	return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate)
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, attr, detachstate));

	if (attr == NULL)
		return EINVAL;

	if (detachstate != NULL)
		*detachstate = attr->detachstate;

	return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
	D(bug("%s(%p, %d)\n", __FUNCTION__, attr, detachstate));

	if (attr == NULL || detachstate != PTHREAD_CREATE_JOINABLE)
		return EINVAL;

	return 0;
}

int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr, size_t *stacksize)
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, attr, stackaddr));

	if (attr == NULL)
		return EINVAL;

	if (stackaddr != NULL)
		*stackaddr = attr->stackaddr;

	if (stacksize != NULL)
		*stacksize = attr->stacksize;

	return 0;
}

int pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr, size_t stacksize)
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, attr, stackaddr));

	if (attr == NULL || (stackaddr != NULL && stacksize == 0))
		return EINVAL;

	attr->stackaddr = stackaddr;
	attr->stacksize = stacksize;

	return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize)
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, attr, stacksize));

	return pthread_attr_getstack(attr, NULL, stacksize);
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
	D(bug("%s(%p, %u)\n", __FUNCTION__, attr, stacksize));

	return pthread_attr_setstack(attr, NULL, stacksize);
}

int pthread_attr_getschedparam(const pthread_attr_t *attr, struct sched_param *param)
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, attr, param));

	if (attr == NULL || param == NULL)
		return EINVAL;

	*param = attr->param;

	return 0;
}

int pthread_attr_setschedparam(pthread_attr_t *attr, const struct sched_param *param)
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, attr, param));

	if (attr == NULL || param == NULL)
		return EINVAL;

	attr->param = *param;

	return 0;
}

//
// Thread functions
//

static void StarterFunc(void)
{
	ThreadInfo *inf;

	DB2(bug("%s()\n", __FUNCTION__));

	inf = (ThreadInfo *)FindTask(NULL)->tc_UserData;
	// trim the name
	//inf->task->tc_Node.ln_Name[inf->oldlen];

	// we have to set the priority here to avoid race conditions
	SetTaskPri(inf->task, inf->attr.param.sched_priority);

	if (!setjmp(inf->jmp))
	{
		if (inf->attr.stackaddr != NULL && inf->attr.stacksize > 0)
		{
			struct StackSwapArgs swapargs;
			struct StackSwapStruct stack; 

			swapargs.Args[0] = (IPTR)inf->arg;
			stack.stk_Lower = inf->attr.stackaddr;
			stack.stk_Upper = (APTR)((IPTR)stack.stk_Lower + inf->attr.stacksize);
			stack.stk_Pointer = stack.stk_Upper; 

			inf->ret = (void *)NewStackSwap(&stack, inf->start, &swapargs);
		}
		else
		{
			inf->ret = inf->start(inf->arg);
		}
	}

	Forbid();
	ReplyMsg(&inf->msg);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
	ThreadInfo *inf;
	char name[NAMELEN];
	size_t oldlen;
	pthread_t threadnew;

	D(bug("%s(%p, %p, %p, %p)\n", __FUNCTION__, thread, attr, start, arg));

	if (start == NULL)
		return EINVAL;

	ObtainSemaphore(&thread_sem);

	threadnew = GetThreadId(NULL);
	if (threadnew == PTHREAD_INVALID_ID)
	{
		ReleaseSemaphore(&thread_sem);
		return EAGAIN;
	}

	inf = GetThreadInfo(threadnew);
	memset(inf, 0, sizeof(ThreadInfo));
	inf->start = start;
	if (attr)
		inf->attr = *attr;
	else
		pthread_attr_init(&inf->attr);
	inf->arg = arg;
	NewList((struct List *)&inf->cleanup);

	// let's trick CreateNewProc into allocating a larger buffer for the name
	snprintf(name, sizeof(name), "pthread thread #%d", threadnew);
	oldlen = strlen(name);
	memset(name + oldlen, ' ', sizeof(name) - oldlen - 1);
	name[sizeof(name) - 1] = '\0';

	inf->msgport = CreateMsgPort();
	if (!inf->msgport)
	{
		ReleaseSemaphore(&thread_sem);
		return EAGAIN;
	}

	inf->msg.mn_Node.ln_Type = NT_MESSAGE;
	inf->msg.mn_ReplyPort = inf->msgport;
	inf->msg.mn_Length = sizeof(inf->msg);

	inf->task = (struct Task *)CreateNewProcTags(NP_Entry, StarterFunc,
#ifdef __MORPHOS__
		NP_CodeType, CODETYPE_PPC,
		(inf->attr.stackaddr == NULL && inf->attr.stacksize > 0) ? NP_PPCStackSize : TAG_IGNORE, inf->attr.stacksize,
#else
		(inf->attr.stackaddr == NULL && inf->attr.stacksize > 0) ? NP_StackSize : TAG_IGNORE, inf->attr.stacksize,
#endif
		NP_UserData, inf,
		NP_Name, name,
		TAG_DONE);

	ReleaseSemaphore(&thread_sem);

	if (!inf->task)
	{
		DeleteMsgPort(inf->msgport);
		inf->msgport = NULL;
		return EAGAIN;
	}

	if (thread != NULL)
		*thread = threadnew;

	return 0;
}

int pthread_detach(pthread_t thread)
{
	D(bug("%s(%u) not implemented\n", __FUNCTION__, thread));

	return ESRCH;
}

int pthread_join(pthread_t thread, void **value_ptr)
{
	ThreadInfo *inf;

	D(bug("%s(%u, %p)\n", __FUNCTION__, thread, value_ptr));

	inf = GetThreadInfo(thread);

	if (inf == NULL || inf->msgport == NULL)
		return ESRCH;

	WaitPort(inf->msgport);
	DeleteMsgPort(inf->msgport);

	if (value_ptr)
		*value_ptr = inf->ret;

	ObtainSemaphore(&thread_sem);
	memset(inf, 0, sizeof(ThreadInfo));
	ReleaseSemaphore(&thread_sem);

	return 0;
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
	D(bug("%s(%u, %u)\n", __FUNCTION__, t1, t2));

	return (t1 == t2);
}

pthread_t pthread_self(void)
{
	struct Task *task;
	pthread_t thread;

	D(bug("%s()\n", __FUNCTION__));

	task = FindTask(NULL);
	thread = GetThreadId(task);

	// add non-pthread processes to our list, so we can handle the main thread
	if (thread == PTHREAD_INVALID_ID)
	{
		ThreadInfo *inf;

		ObtainSemaphore(&thread_sem);
		thread = GetThreadId(NULL);
		if (thread == PTHREAD_INVALID_ID)
		{
			// TODO: pthread_self is supposed to always succeed, but we can fail
			// here if we run out of thread slots
			//ReleaseSemaphore(&thread_sem);
			//return EAGAIN;
		}
		inf = GetThreadInfo(thread);
		memset(inf, 0, sizeof(ThreadInfo));
		NewList((struct List *)&inf->cleanup);
		inf->task = task;
		ReleaseSemaphore(&thread_sem);
	}

	return thread;
}

int pthread_cancel(pthread_t thread)
{
	D(bug("%s(%u) not implemented\n", __FUNCTION__, thread));

	// TODO: should I do a pthread_join here?
	return ESRCH;
}

void pthread_exit(void *value_ptr)
{
	pthread_t thread;
	ThreadInfo *inf;
	CleanupHandler *handler;

	D(bug("%s(%p)\n", __FUNCTION__, value_ptr));

	thread = pthread_self();
	inf = GetThreadInfo(thread);
	inf->ret = value_ptr;

	while ((handler = (CleanupHandler *)RemTail((struct List *)&inf->cleanup)))
		if (handler->routine)
			handler->routine(handler->arg);

	longjmp(inf->jmp, 1);
}

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, once_control, init_routine));

	if (once_control == NULL || init_routine == NULL)
		return EINVAL;

	if (__sync_val_compare_and_swap(&once_control->started, FALSE, TRUE))
	{
		if (!once_control->done)
		{
			(*init_routine)();
			once_control->done = TRUE;
		}
	}

	return 0;
}

//
// Scheduling functions
//

int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param)
{
	ThreadInfo *inf;

	D(bug("%s(%u, %d, %p)\n", __FUNCTION__, thread, policy, param));

	if (param == NULL)
		return EINVAL;

	inf = GetThreadInfo(thread);

	if (inf == NULL)
		return ESRCH;

	SetTaskPri(inf->task, param->sched_priority);

	return 0;
}

//
// Non-portable functions
//
int pthread_setname_np(pthread_t thread, const char *name)
{
	ThreadInfo *inf;
	char *currentname;
	size_t namelen;

	D(bug("%s(%u, %s)\n", __FUNCTION__, thread, name));

	if (name == NULL)
		return ERANGE;

	inf = GetThreadInfo(thread);

	if (inf == NULL)
		return ERANGE;

	currentname = inf->task->tc_Node.ln_Name;

	if (inf->msgport == NULL)
		namelen = strlen(currentname) + 1;
	else
		namelen = NAMELEN;

	if (strlen(name) + 1 > namelen)
		return ERANGE;

	strncpy(currentname, name, namelen);

	return 0;
}

int pthread_getname_np(pthread_t thread, char *name, size_t len)
{
	ThreadInfo *inf;
	char *currentname;

	D(bug("%s(%u, %p, %u)\n", __FUNCTION__, thread, name, len));

	if (name == NULL || len == 0)
		return ERANGE;

	inf = GetThreadInfo(thread);

	if (inf == NULL)
		return ERANGE;

	currentname = inf->task->tc_Node.ln_Name;

	if (strlen(currentname) + 1 > len)
		return ERANGE;

	// TODO: partially copy the name?
	strncpy(name, currentname, len);

	return 0;
}

//
// Cancellation cleanup
//

// theads can't be cancelled, but they can still call pthread_exit, which
// will execute these clean-up handlers
void pthread_cleanup_push(void (*routine)(void *), void *arg)
{
	pthread_t thread;
	ThreadInfo *inf;
	CleanupHandler *handler;

	D(bug("%s(%p, %p)\n", __FUNCTION__, routine, arg));

	if (routine == NULL)
		return;

	handler = calloc(1, sizeof(CleanupHandler));

	if (handler == NULL)
		return;

	thread = pthread_self();
	inf = GetThreadInfo(thread);

	AddTail((struct List *)&inf->cleanup, (struct Node *)handler);
}

void pthread_cleanup_pop(int execute)
{
	pthread_t thread;
	ThreadInfo *inf;
	CleanupHandler *handler;

	D(bug("%s(%d)\n", __FUNCTION__, execute));

	thread = pthread_self();
	inf = GetThreadInfo(thread);
	handler = (CleanupHandler *)RemTail((struct List *)&inf->cleanup);

	if (handler && handler->routine && execute)
		handler->routine(handler->arg);

	free(handler);
}

//
// Signalling
//

int pthread_kill(pthread_t thread, int sig)
{
	D(bug("%s(%u, %d) not implemented\n", __FUNCTION__, thread, sig));

	return EINVAL;
}

//
// Constructors, destructors
//

static int _Init_Func(void)
{
	DB2(bug("%s()\n", __FUNCTION__));

	memset(&threads, 0, sizeof(threads));
	InitSemaphore(&thread_sem);

	return TRUE;
}

static void _Exit_Func(void)
{
#if 0
	pthread_t i;
#endif

	DB2(bug("%s()\n", __FUNCTION__));

	// wait for the threads?
#if 0
	for (i = 0; i < PTHREAD_THREADS_MAX; i++)
		pthread_join(i, NULL);
#endif
}

#ifdef __AROS__
ADD2INIT(_Init_Func, 0);
ADD2EXIT(_Exit_Func, 0);
#else
static CONSTRUCTOR_P(_Init_Func, 100)
{
	return !_Init_Func();
}

static DESTRUCTOR_P(_Exit_Func, 100)
{
	_Exit_Func();
}
#endif
