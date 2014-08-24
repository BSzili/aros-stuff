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

//#include <exec/lists.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <setjmp.h>
#include <string.h>
#include <stdio.h>

#include "pthread.h"
#include "debug.h"

#define FALLBACKSIGNAL SIGBREAKB_CTRL_E
#define NAMELEN 64

struct cond_waiter
{
	struct MinNode node;
	struct Task *task;
	ULONG sigmask;
};

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
	struct Process *process;
	//pthread_t id;
	void *ret;
	jmp_buf jmp;
	const pthread_attr_t *attr;
	//char name[256];
	//size_t oldlen;
	TLSKey tls[PTHREAD_KEYS_MAX];
	struct MinList cleanup;
} ThreadInfo;

// TODO: make this a list
static ThreadInfo threads[PTHREAD_THREADS_MAX];
static pthread_t nextid = 0;

//
// Helper functions
//

static int SemaphoreIsInvalid(struct SignalSemaphore *sem)
{
	return (!sem || sem->ss_Link.ln_Type != NT_SIGNALSEM || sem->ss_WaitQueue.mlh_Tail != NULL);
}

static ThreadInfo *GetThreadInfo(pthread_t thread)
{
	ThreadInfo *inf = NULL;

	if (thread < nextid)
		inf = &threads[thread];

	return inf;
}

static pthread_t GetThreadId(struct Task *task)
{
	pthread_t i;

	for (i = 0; i < /*nextid*/PTHREAD_THREADS_MAX; i++)
	{
		if ((struct Task *)threads[i].process == task)
			break;
	}

	// TODO: error handling?

	return i;
}

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

	if (mutex == NULL)
		return EINVAL;

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

	// TODO: non-recursive mutexes?
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

	// TODO: non-recursive mutexes?
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
// Condition variable functions
//

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, cond, attr));

	if (cond == NULL)
		return EINVAL;

	InitSemaphore(&cond->semaphore);
	NewList((struct List *)&cond->waiters);
	cond->waiting = 0;

	return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
	D(bug("%s(%p)\n", __FUNCTION__, cond));

	if (cond == NULL)
		return EINVAL;

	if (cond->waiting > 0)
		return EBUSY;

	memset(cond, 0, sizeof(pthread_cond_t));

	return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	struct cond_waiter waiter;
	BYTE signal;

	D(bug("%s(%p, %p)\n", __FUNCTION__, cond, mutex));

	if (cond == NULL || mutex == NULL)
		return EINVAL;

	// initialize static conditions
	if (SemaphoreIsInvalid(&cond->semaphore))
		pthread_cond_init(cond, NULL);

	waiter.task = FindTask(NULL);
	signal = AllocSignal(-1);
	if (signal == -1)
	{
		signal = FALLBACKSIGNAL;
		SetSignal(1 << FALLBACKSIGNAL, 0);
	}
	waiter.sigmask = 1 << signal;
	ObtainSemaphore(&cond->semaphore);
	AddTail((struct List *)&cond->waiters, (struct Node *)&waiter);
	cond->waiting++;
	ReleaseSemaphore(&cond->semaphore);

	pthread_mutex_unlock(mutex);
	Wait(waiter.sigmask);
	pthread_mutex_lock(mutex);

	ObtainSemaphore(&cond->semaphore);
	Remove((struct Node *)&waiter);
	cond->waiting--;
	ReleaseSemaphore(&cond->semaphore);

	if (signal != FALLBACKSIGNAL)
		FreeSignal(signal);

	return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
	struct cond_waiter *waiter;

	D(bug("%s(%p)\n", __FUNCTION__, cond));

	if (cond == NULL)
		return EINVAL;

	// initialize static conditions
	if (SemaphoreIsInvalid(&cond->semaphore))
		pthread_cond_init(cond, NULL);

	ObtainSemaphore(&cond->semaphore);
	if (cond->waiting > 0)
	{
		waiter = (struct cond_waiter *)cond->waiters.mlh_Head;
		if (waiter->node.mln_Succ)
			Signal(waiter->task, waiter->sigmask);
	}
	ReleaseSemaphore(&cond->semaphore);

	return 0;
}

//
// Thread attribute functions
//

int pthread_attr_init(pthread_attr_t *attr)
{
	D(bug("%s(%p)\n", __FUNCTION__, attr));

	if (attr == NULL)
		return EINVAL;

	memset(attr, 0, sizeof(pthread_attr_t));
	// TODO: sensible defaults?

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

int pthread_attr_setschedparam(pthread_attr_t *attr, const struct sched_param *param)
{
	D(bug("%s(%p, %p)\n", __FUNCTION__, attr, param));

	if (attr == NULL)
		return EINVAL;

	attr->param = *param;

	return 0;
}

//
// Thread functions
//

/*
   struct PPCStackSwapArgs swapargs;
   swapargs.Args[0] = (ULONG)arg;
   return NewPPCStackSwap(stack, function, &swapargs);

   struct StackSwapArgs swapargs;
   swapargs.Args[0] = (IPTR)arg;
   return NewStackSwap(stack, function, &swapargs);
*/

static void StarterFunc(void)
{
	ThreadInfo *inf;

	D(bug("%s()\n", __FUNCTION__));

	inf = (ThreadInfo *)FindTask(NULL)->tc_UserData;
	// trim the name
	//inf->process->pr_Task.tc_Node.ln_Name[inf->oldlen];

	if (!setjmp(inf->jmp))
		inf->ret = inf->start(inf->arg);

	Forbid();
	ReplyMsg(&inf->msg);
}

// TODO: we assume that sub-threads can only be started from the main one, which is NOT thread-safe
// a semaphore could be added 
int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
	ThreadInfo *inf;
	char buf[NAMELEN];
	char name[NAMELEN];
	size_t oldlen;

	D(bug("%s(%p, %p, %p, %p)\n", __FUNCTION__, thread, attr, start, arg));

	if (thread == NULL || start == NULL)
		return EINVAL;

	inf = GetThreadInfo(nextid);
	memset(inf, 0, sizeof(ThreadInfo));
	inf->start = start;
	inf->attr = attr;
	inf->arg = arg;
	NewList((struct List *)&inf->cleanup);

	// let's trick CreateNewProc into allocating a larger buffer for the name
	snprintf(buf, sizeof(buf), "pthread thread #%d", nextid);
	oldlen = strlen(buf);
	memset(name, ' ', sizeof(name));
	memcpy(name, buf, oldlen);
	name[sizeof(name) - 1] = '\0';

	inf->msgport = CreateMsgPort();
	if (!inf->msgport)
		return EAGAIN;

	inf->msg.mn_Node.ln_Type = NT_MESSAGE;
	inf->msg.mn_ReplyPort = inf->msgport;
	inf->msg.mn_Length = sizeof(inf->msg);

	inf->process = CreateNewProcTags(NP_Entry, StarterFunc,
#ifdef __MORPHOS__
		NP_CodeType, CODETYPE_PPC,
		(attr && attr->stacksize > 0) ? NP_PPCStackSize : TAG_IGNORE, attr->stacksize,
#else
		(attr && attr->stacksize > 0) ? NP_StackSize : TAG_IGNORE, attr->stacksize,
#endif
		NP_UserData, inf,
		(attr) ? NP_Priority : TAG_IGNORE, attr->param.sched_priority,
		NP_Name, name,
		TAG_DONE);

	if (!inf->process)
	{
		DeleteMsgPort(inf->msgport);
		return EAGAIN;
	}

	*thread = nextid++;

	return 0;
}

int pthread_join(pthread_t thread, void **value_ptr)
{
	ThreadInfo *inf;

	D(bug("%s(%u, %p)\n", __FUNCTION__, thread, value_ptr));

	inf = GetThreadInfo(thread);

	//while (!GetMsg(inf->msgport))
		WaitPort(inf->msgport);

	DeleteMsgPort(inf->msgport);
	if (value_ptr)
		*value_ptr = inf->ret;

	memset(inf, 0, sizeof(ThreadInfo));

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
	// TODO: this is not thread-safe
	if (/*nextid == 1 ||*/ thread >= PTHREAD_THREADS_MAX)
	{
		ThreadInfo *inf;
		
		thread = nextid++;
		inf = GetThreadInfo(thread);
		memset(inf, 0, sizeof(ThreadInfo));
		NewList((struct List *)&inf->cleanup);
		inf->process = (struct Process *)task;
	}

	return thread;
}

void pthread_exit(void *value_ptr)
{
	ThreadInfo *inf;

	D(bug("%s(%p)\n", __FUNCTION__, value_ptr));

	inf = GetThreadInfo(pthread_self());
	inf->ret = value_ptr;

	longjmp(inf->jmp, 1);
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
	SetTaskPri((struct Task *)inf->process, param->sched_priority);

	return 0;
}

//
// NP
//
int pthread_setname_np(pthread_t thread, const char *name)
{
	ThreadInfo *inf;
	char *currentName;

	D(bug("%s(%u, %s)\n", __FUNCTION__, thread, name));

	inf = GetThreadInfo(thread);
	currentName = inf->process->pr_Task.tc_Node.ln_Name;

	if (strlen(name) + 1 > NAMELEN)
		return ERANGE;

	strncpy(currentName, name, NAMELEN);

	return 0;
}

int pthread_getname_np(pthread_t thread, char *name, size_t len)
{
	ThreadInfo *inf;
	char *currentName;

	D(bug("%s(%u, %p, %u)\n", __FUNCTION__, thread, name, len));

	inf = GetThreadInfo(thread);
	currentName = inf->process->pr_Task.tc_Node.ln_Name;

	if (strlen(currentName) + 1 > len)
		return ERANGE;
	// TODO: partially copy the name?
	strncpy(name, currentName, len);

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

	handler = AllocVec(sizeof(CleanupHandler), MEMF_PUBLIC | MEMF_CLEAR);

	if (routine == NULL || handler == NULL)
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
}

