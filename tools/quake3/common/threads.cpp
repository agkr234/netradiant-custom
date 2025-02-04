/*
   Copyright (C) 1999-2006 Id Software, Inc. and contributors.
   For a list of contributors, see the accompanying CONTRIBUTORS file.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "cmdlib.h"
#include "inout.h"
#include "qthreads.h"
#include "timer.h"

#define MAX_THREADS 64
int numthreads = -1;
bool threaded;

int dispatch;
int workcount;
int oldf;
bool pacifier;

/*
   =============
   GetThreadWork

   =============
 */
int GetThreadWork(){
	ThreadLock();

	if ( dispatch == workcount ) {
		ThreadUnlock();
		return -1;
	}

	const int f = 40 * dispatch / workcount;
	if ( f < oldf ) {
		Sys_Warning( "progress went backwards (should never happen)\n" );
		oldf = f;
	}
	while ( f > oldf )
	{
		++oldf;
		if ( pacifier ) {
			if ( oldf % 4 == 0 ) {
				Sys_Printf( "%i", f / 4 );
			}
			else{
				Sys_Printf( "." );
			}
			fflush( stdout );   /* ydnar */
		}
	}

	const int r = dispatch++;
	ThreadUnlock();

	return r;
}


void ( *workfunction )( int );

void ThreadWorkerFunction( int threadnum ){
	while ( 1 )
	{
		const int work = GetThreadWork();
		if ( work == -1 ) {
			break;
		}
//Sys_Printf ("thread %i, work %i\n", threadnum, work);
		workfunction( work );
	}
}

void RunThreadsOn( void ( *func )( int ) );

void RunThreadsOnIndividual( int workcnt, bool showpacifier, void ( *func )( int ) ){
	if ( numthreads == -1 ) {
		ThreadSetDefault();
	}
	Timer timer;

	dispatch = 0;
	workcount = workcnt;
	oldf = -1;
	pacifier = showpacifier;

	workfunction = func;
	RunThreadsOn( ThreadWorkerFunction );

	if ( pacifier ) {
		Sys_Printf( " (%i)\n", int( timer.elapsed_sec() ) );
	}
}


#if 1

#include <thread>
#include <mutex>

std::mutex crit;
static bool enter;

void ThreadSetDefault(){
	if ( numthreads == -1 ) { // not set manually
		numthreads = std::max( std::thread::hardware_concurrency(), 1u );
	}

	Sys_Printf( "%i threads\n", numthreads );
}


void ThreadLock(){
	if ( threaded ) {
		crit.lock();
		if ( enter ) {
			Error( "Recursive ThreadLock\n" );
		}
		enter = true;
	}
}

void ThreadUnlock(){
	if ( threaded ) {
		if ( !enter ) {
			Error( "ThreadUnlock without lock\n" );
		}
		enter = false;
		crit.unlock();
	}
}

/*
   =============
   RunThreadsOn
   =============
 */
void RunThreadsOn( void ( *func )( int ) ){
	if ( numthreads == 1 ) { // use same thread
		func( 0 );
	}
	else
	{
		threaded = true;

		std::thread threads[MAX_THREADS];

		for ( int i = 0; i < numthreads; ++i )
			threads[i] = std::thread( func, i );

		for ( int i = 0; i < numthreads; ++i )
			threads[i].join();

		threaded = false;
	}
}

#else

#ifndef WIN32
// The below define is necessary to use
// pthreads extensions like pthread_mutexattr_settype
#define _GNU_SOURCE
#include <pthread.h>
#endif

/*
   ===================================================================

   WIN32

   ===================================================================
 */
#ifdef WIN32

#define USED

#include <windows.h>

CRITICAL_SECTION crit;
static int enter;

void ThreadSetDefault(){
	SYSTEM_INFO info;

	if ( numthreads == -1 ) { // not set manually
		GetSystemInfo( &info );
		numthreads = info.dwNumberOfProcessors;
		if ( numthreads < 1 || numthreads > 32 ) {
			numthreads = 1;
		}
	}

	Sys_Printf( "%i threads\n", numthreads );
}


void ThreadLock(){
	if ( !threaded ) {
		return;
	}
	EnterCriticalSection( &crit );
	if ( enter ) {
		Error( "Recursive ThreadLock\n" );
	}
	enter = 1;
}

void ThreadUnlock(){
	if ( !threaded ) {
		return;
	}
	if ( !enter ) {
		Error( "ThreadUnlock without lock\n" );
	}
	enter = 0;
	LeaveCriticalSection( &crit );
}

/*
   =============
   RunThreadsOn
   =============
 */
void RunThreadsOn( void ( *func )( int ) ){
	HANDLE threadhandle[MAX_THREADS];
	int i;

	threaded = true;

	//
	// run threads in parallel
	//
	InitializeCriticalSection( &crit );

	if ( numthreads == 1 ) { // use same thread
		func( 0 );
	}
	else
	{
		for ( i = 0; i < numthreads; ++i )
		{
			threadhandle[i] = CreateThread(
			                      NULL,                         // LPSECURITY_ATTRIBUTES  lpThreadAttributes,
			                      //0,                          // SIZE_T                 dwStackSize,

			                      /* ydnar: cranking stack size to eliminate radiosity crash with 1MB stack on win32 */
			                      ( 4096 * 1024 ),

			                      (LPTHREAD_START_ROUTINE)func, // LPTHREAD_START_ROUTINE lpStartAddress,
			                      (LPVOID)i,                    // LPVOID                 lpParameter,
			                      0,                            // DWORD                  dwCreationFlags,
			                      NULL );                       // LPDWORD                lpThreadId
		}

		for ( i = 0; i < numthreads; ++i )
			WaitForSingleObject( threadhandle[i], INFINITE );
	}
	DeleteCriticalSection( &crit );

	threaded = false;
}


#endif

/*
   ===================================================================

   OSF1

   ===================================================================
 */

#ifdef __osf__
#define USED

void ThreadSetDefault(){
	if ( numthreads == -1 ) { // not set manually
		numthreads = 4;
	}
}


#include <pthread.h>

pthread_mutex_t *my_mutex;

void ThreadLock(){
	if ( my_mutex ) {
		pthread_mutex_lock( my_mutex );
	}
}

void ThreadUnlock(){
	if ( my_mutex ) {
		pthread_mutex_unlock( my_mutex );
	}
}


/*
   =============
   RunThreadsOn
   =============
 */
void RunThreadsOn( void ( *func )( int ) ){
	int i;
	pthread_t work_threads[MAX_THREADS];
	pthread_addr_t status;
	pthread_attr_t attrib;
	pthread_mutexattr_t mattrib;

	threaded = true;

	if ( pacifier ) {
		setbuf( stdout, NULL );
	}

	if ( !my_mutex ) {
		my_mutex = safe_malloc( sizeof( *my_mutex ) );
		if ( pthread_mutexattr_create( &mattrib ) == -1 ) {
			Error( "pthread_mutex_attr_create failed" );
		}
		if ( pthread_mutexattr_setkind_np( &mattrib, MUTEX_FAST_NP ) == -1 ) {
			Error( "pthread_mutexattr_setkind_np failed" );
		}
		if ( pthread_mutex_init( my_mutex, mattrib ) == -1 ) {
			Error( "pthread_mutex_init failed" );
		}
	}

	if ( pthread_attr_create( &attrib ) == -1 ) {
		Error( "pthread_attr_create failed" );
	}
	if ( pthread_attr_setstacksize( &attrib, 0x100000 ) == -1 ) {
		Error( "pthread_attr_setstacksize failed" );
	}

	for ( i = 0; i < numthreads; ++i )
	{
		if ( pthread_create( &work_threads[i], attrib
		                     , (pthread_startroutine_t)func, (pthread_addr_t)i ) == -1 ) {
			Error( "pthread_create failed" );
		}
	}

	for ( i = 0; i < numthreads; ++i )
	{
		if ( pthread_join( work_threads[i], &status ) == -1 ) {
			Error( "pthread_join failed" );
		}
	}

	threaded = false;
}


#endif

/*
   ===================================================================

   IRIX

   ===================================================================
 */

#ifdef _MIPS_ISA
#define USED

#include <task.h>
#include <abi_mutex.h>
#include <sys/types.h>
#include <sys/prctl.h>


abilock_t lck;

void ThreadSetDefault(){
	if ( numthreads == -1 ) {
		numthreads = prctl( PR_MAXPPROCS );
	}
	Sys_Printf( "%i threads\n", numthreads );
	usconfig( CONF_INITUSERS, numthreads );
}


void ThreadLock(){
	spin_lock( &lck );
}

void ThreadUnlock(){
	release_lock( &lck );
}


/*
   =============
   RunThreadsOn
   =============
 */
void RunThreadsOn( void ( *func )( int ) ){
	int i;
	int pid[MAX_THREADS];

	threaded = true;

	if ( pacifier ) {
		setbuf( stdout, NULL );
	}

	init_lock( &lck );

	for ( i = 0; i < numthreads - 1; ++i )
	{
		pid[i] = sprocsp( ( void ( * )( void *, size_t ) )func, PR_SALL, (void *)i
		                  , NULL, 0x200000 ); // 2 meg stacks
		if ( pid[i] == -1 ) {
			perror( "sproc" );
			Error( "sproc failed" );
		}
	}

	func( i );

	for ( i = 0; i < numthreads - 1; ++i )
		wait( NULL );

	threaded = false;
}


#endif


/*
   =======================================================================

   Linux pthreads

   =======================================================================
 */

#if defined( __linux__ ) || defined( __APPLE__ )
#define USED

#include <unistd.h>

void ThreadSetDefault(){
	if ( numthreads == -1 ) { // not set manually
#ifdef _SC_NPROCESSORS_ONLN
		long cpus = sysconf( _SC_NPROCESSORS_ONLN );
		if ( cpus > 0 ) {
			numthreads = cpus;
		}
		else
#endif
		/* can't detect, so default to four threads */
		numthreads = 4;
	}

	if ( numthreads > 1 ) {
		Sys_Printf( "threads: %d\n", numthreads );
	}
}

#include <pthread.h>

struct pt_mutex_t
{
	pthread_t       *owner;
	pthread_mutex_t a_mutex;
	pthread_cond_t cond;
	unsigned int lock;
};

pt_mutex_t global_lock;

void ThreadLock(){
	pt_mutex_t *pt_mutex = &global_lock;

	if ( !threaded ) {
		return;
	}

	pthread_mutex_lock( &pt_mutex->a_mutex );
	if ( pthread_equal( pthread_self(), (pthread_t)&pt_mutex->owner ) ) {
		pt_mutex->lock++;
	}
	else
	{
		if ( ( !pt_mutex->owner ) && ( pt_mutex->lock == 0 ) ) {
			pt_mutex->owner = (pthread_t *)pthread_self();
			pt_mutex->lock  = 1;
		}
		else
		{
			while ( 1 )
			{
				pthread_cond_wait( &pt_mutex->cond, &pt_mutex->a_mutex );
				if ( ( !pt_mutex->owner ) && ( pt_mutex->lock == 0 ) ) {
					pt_mutex->owner = (pthread_t *)pthread_self();
					pt_mutex->lock  = 1;
					break;
				}
			}
		}
	}
	pthread_mutex_unlock( &pt_mutex->a_mutex );
}

void ThreadUnlock(){
	pt_mutex_t *pt_mutex = &global_lock;

	if ( !threaded ) {
		return;
	}

	pthread_mutex_lock( &pt_mutex->a_mutex );
	pt_mutex->lock--;

	if ( pt_mutex->lock == 0 ) {
		pt_mutex->owner = NULL;
		pthread_cond_signal( &pt_mutex->cond );
	}

	pthread_mutex_unlock( &pt_mutex->a_mutex );
}

void recursive_mutex_init( pthread_mutexattr_t attribs ){
	pt_mutex_t *pt_mutex = &global_lock;

	pt_mutex->owner = NULL;
	if ( pthread_mutex_init( &pt_mutex->a_mutex, &attribs ) != 0 ) {
		Error( "pthread_mutex_init failed\n" );
	}
	if ( pthread_cond_init( &pt_mutex->cond, NULL ) != 0 ) {
		Error( "pthread_cond_init failed\n" );
	}

	pt_mutex->lock = 0;
}

/*
   =============
   RunThreadsOn
   =============
 */
void RunThreadsOn( void ( *func )( int ) ){
	pthread_mutexattr_t mattrib;
	pthread_attr_t attr;
	pthread_t work_threads[MAX_THREADS];
	size_t stacksize;

	int i = 0;

	pthread_attr_init( &attr );
	if ( pthread_attr_setstacksize( &attr, 8388608 ) != 0 ) {
		stacksize = 0;
		pthread_attr_getstacksize( &attr, &stacksize );
		Sys_Printf( "Could not set a per-thread stack size of 8 MB, using only %.2f MB\n", stacksize / 1048576.0 );
	}

	if ( numthreads == 1 ) {
		func( 0 );
	}
	else
	{
		threaded  = true;

		if ( pacifier ) {
			setbuf( stdout, NULL );
		}

		if ( pthread_mutexattr_init( &mattrib ) != 0 ) {
			Error( "pthread_mutexattr_init failed" );
		}
		if ( pthread_mutexattr_settype( &mattrib, PTHREAD_MUTEX_ERRORCHECK ) != 0 ) {
			Error( "pthread_mutexattr_settype failed" );
		}
		recursive_mutex_init( mattrib );

		for ( i = 0; i < numthreads; ++i )
		{
			/* Default pthread attributes: joinable & non-realtime scheduling */
			if ( pthread_create( &work_threads[i], &attr, (void *(*)(void *)) func, (void*)(size_t)i ) != 0 ) {
				Error( "pthread_create failed" );
			}
		}
		for ( i = 0; i < numthreads; ++i )
		{
			if ( pthread_join( work_threads[i], NULL ) != 0 ) {
				Error( "pthread_join failed" );
			}
		}
		pthread_mutexattr_destroy( &mattrib );
		threaded = false;
	}
}
#endif // ifdef __linux__


/*
   =======================================================================

   SINGLE THREAD

   =======================================================================
 */

#ifndef USED

void ThreadSetDefault(){
	numthreads = 1;
}

void ThreadLock(){
}

void ThreadUnlock(){
}

/*
   =============
   RunThreadsOn
   =============
 */
void RunThreadsOn( int workcnt, bool showpacifier, void ( *func )( int ) ){
	func( 0 );
}

#endif

#endif
