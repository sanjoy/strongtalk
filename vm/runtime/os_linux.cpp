/* Copyright 1994 - 1996 LongView Technologies L.L.C. $Revision: 1.50 $ */
/* Copyright (c) 2006, Sun Microsystems, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the 
following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following 
	  disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of Sun Microsystems nor the names of its contributors may be used to endorse or promote products derived 
	  from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT 
NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL 
THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE 
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE


*/

#ifdef __LINUX__
# include "incls/_precompiled.incl"
# include "incls/_os.cpp.incl"
# include <unistd.h>
# include <semaphore.h>
# include <pthread.h>
# include <sys/times.h>
# include <time.h>
# include <stdio.h>
# include <dlfcn.h>

static int    main_thread_id;
class Lock {
    private:
        pthread_mutex_t* mutex;

    public:
        Lock(pthread_mutex_t* mutex) : mutex(mutex) {
            pthread_mutex_lock(mutex);
        }
        ~Lock() {
            pthread_mutex_unlock(mutex);
        }
};

static int _argc;
static char** _argv;

int os::argc() {
  return _argc;
}

char** os::argv() {
  return _argv;
}

extern int vm_main(int argc, char* argv[]);

int main(int argc, char* argv[]) {
    _argc = argc;
    _argv = argv;
    return vm_main(argc, argv);
}

class Event: public CHeapObj {
    private:
        bool _signalled;
        pthread_mutex_t mutex;
        pthread_cond_t notifier;
    public:
        void inline signal() {
            Lock mark(&mutex);
            _signalled = true;
            pthread_cond_signal(&notifier);
        }
        void inline reset() {
            Lock mark(&mutex);
            _signalled = false;
            pthread_cond_signal(&notifier);
        }
        bool inline waitFor() {
            Lock mark(&mutex);
            while (!_signalled)
              pthread_cond_wait(&notifier, &mutex);
        }
        Event(bool state) {
            _signalled = state;
            pthread_mutex_init(&mutex, NULL);
            pthread_cond_init(&notifier, NULL);
        }
        ~Event() {
            pthread_mutex_destroy(&mutex);
            pthread_cond_destroy(&notifier);
        }
};

class Thread : CHeapObj {
    private:
        pthread_t _threadId;
        clockid_t _clockId;
        Thread(pthread_t threadId) : _threadId(threadId) {
          pthread_getcpuclockid(_threadId, &_clockId); 
        };
        double get_cpu_time() {
          struct timespec cpu;
          clock_gettime(_clockId, &cpu);
          return ((double)cpu.tv_sec) + ((double)cpu.tv_nsec)/1000000000.0; 
        }
        friend class os;
};

static Thread* main_thread;

extern void intercept_for_single_step();

// No references in VM
int os::getenv(char* name,char* buffer,int len) {
 return 0;
}

// 1 reference (lprintf.cpp)
bool os::move_file(char* from, char* to) {
	return false;
}

// 1 reference (inliningdb.cpp)
bool os::check_directory(char* dir_name) {
  return false;
}

// 1 reference (memory/util.cpp)
void os::breakpoint() {
}

// 1 reference process.cpp
Thread* os::starting_thread(int* id_addr) {
  return main_thread;
}

typedef struct {
  int (*main)(void* parameter);
  void* parameter;
} thread_args_t;
  
void* mainWrapper(void* args) {
  thread_args_t* wrapperArgs = (thread_args_t*) args;
  int* result = (int*) malloc(sizeof(int));
  *result = wrapperArgs->main(wrapperArgs->parameter);
  free(args);
  return (void *) result; 
}

// 1 reference process.cpp
Thread* os::create_thread(int threadStart(void* parameter), void* parameter, int* id_addr) {
  pthread_t threadId;
  thread_args_t* threadArgs = (thread_args_t*) malloc(sizeof(thread_args_t));
  threadArgs->main = threadStart;
  threadArgs->parameter = parameter;
  int status = pthread_create(&threadId, NULL, &mainWrapper, threadArgs);
  if (status != 0) {
    fatal("Unable to create thread");
  }
  return new Thread(threadId);
}

// 1 reference process.cpp
void os::terminate_thread(Thread* thread) {
}

// 1 reference process.cpp
void os::delete_event(Event* event) {
    delete event;
}

// 1 reference process.cpp
Event* os::create_event(bool initial_state) {
  return new Event(initial_state);
}

tms processTimes;

// 2 references - prims/system_prims.cpp, timer.cpp
int os::updateTimes() {
  return times(&processTimes) != (clock_t) -1;
}

// 2 references - prims/system_prims.cpp, timer.cpp
double os::userTime() {
  return ((double) processTimes.tms_utime)/ CLOCKS_PER_SEC;
}

// 2 references - prims/system_prims.cpp, timer.cpp
double os::systemTime() {
  return ((double) processTimes.tms_stime)/ CLOCKS_PER_SEC;
}

// 1 reference - process.cpp
double os::user_time_for(Thread* thread) {
  //Hack warning - assume half time is spent in kernel, half in user code
  return thread->get_cpu_time()/2;
}

// 1 reference - process.cpp
double os::system_time_for(Thread* thread) {
  //Hack warning - assume half time is spent in kernel, half in user code
  return thread->get_cpu_time()/2;
}

static int      has_performance_count = 0;
static long_int initial_performance_count(0,0);
static long_int performance_frequency(0,0);

// 2 references - memory/error.cpp, evaluator.cpp
void os::fatalExit(int num) {
    exit(num);
}

class DLLLoadError {
};
  
class DLL : CHeapObj {
  private:
    char* _name;
    void* _handle;

    DLL(char* name) {
      _handle = dlopen(name, RTLD_LAZY);
      checkHandle(_handle, "could not find library: %s");
      _name = (char*)malloc(strlen(name) + 1);
      strcpy(_name, name);
    }
    void checkHandle(void* handle, const char* format) {
      if (handle == NULL) {
        char* message = (char*) malloc(200);
        sprintf(message, format, dlerror());
        assert(handle != NULL, message);
        free(message);
      }
    }
    ~DLL() {
      if (_handle) dlclose(_handle);
      if (_name) free(_name);
    }
    bool isValid() {
      return (_handle != NULL) && (_name != NULL);
    }
    dll_func lookup(char* funcname) {
      dll_func function = dll_func(dlsym(_handle, funcname));
      checkHandle((void*) function, "could not find function: %s"); 
      return function; 
    }
  friend class os;
};

// 1 reference - prims/dll.cpp
dll_func os::dll_lookup(char* name, DLL* library) {
  return library->lookup(name);
}

// 1 reference - prims/dll.cpp
DLL* os::dll_load(char* name) {
  DLL* library = new DLL(name);
  if (library->isValid()) return library;
  delete library;
  return NULL;
}

// 1 reference - prims/dll.cpp
bool os::dll_unload(DLL* library) {
  delete library;
  return true;
}

int       nCmdShow      = 0;

// 1 reference - prims/system_prims.cpp
void* os::get_hInstance()    { return (void*) NULL;     }
// 1 reference - prims/system_prims.cpp
void* os::get_prevInstance() { return (void*) NULL; }
// 1 reference - prims/system_prims.cpp
int   os::get_nCmdShow()     { return 0;            }

extern int bootstrapping;

// 1 reference - prims/debug_prims.cpp
void os::timerStart() {}

// 1 reference - prims/debug_prims.cpp
void os::timerStop() {}

// 1 reference - prims/debug_prims.cpp
void os::timerPrintBuffer() {}

// Virtual Memory
class Allocations {
  private:
    int allocationSize;
    char** allocations;
    char** reallocBoundary;
    char** next;
    
    void checkCapacity() {
      assert(next <= reallocBoundary, "next is outside expected range");
      if (next == reallocBoundary) {
        reallocate();
      }
    }
    void reallocate() {
        char** oldAllocations = allocations;
        int oldSize = allocationSize;
        allocationSize *= 2;
        allocations = (char**)malloc(sizeof(char*) * allocationSize);
        memcpy(allocations, oldAllocations, oldSize * sizeof(char*));
        next = allocations + oldSize;
        reallocBoundary = allocations + allocationSize;
        free(oldAllocations);
    }
  public:
    Allocations() {
      initialize();
    }
    ~Allocations() {
      release();
    }
    void initialize() {
      allocationSize = 10;
      allocations = (char**)malloc(sizeof(char*) * allocationSize);
      reallocBoundary = allocations + allocationSize;
      next = allocations;
    }
    void release() {
      assert(allocations == next, "allocations should be empty");
      free(allocations);
    }
    void remove(char* allocation) {
      for (char** current = allocations; current < next; current++)
        if (*current == allocation) {
          for (char** to_move = current; to_move < next; to_move++)
            to_move[0] = to_move[1];
          next--;
          return;
        }
    }
    void add(char* allocation) {
      *next = allocation;
      next++;
      checkCapacity();
    }
    bool contains(char* allocation) {
      for (char** current = allocations; current < (allocations + allocationSize); current++)
        if (*current == allocation) return true;
      return false;
    }
};

Allocations allocations;

// 1 reference - virtualspace.cpp
char* os::reserve_memory(int size) {
  ThreadCritical tc;
  char* allocation = (char*) valloc(size);
  allocations.add(allocation);
  return allocation;
}
  
// 1 reference - virtualspace.cpp
bool os::commit_memory(char* addr, int size) {
  return true;
}

// 1 reference - virtualspace.cpp
bool os::uncommit_memory(char* addr, int size) {
  return true;
}

// 1 reference - virtualspace.cpp
bool os::release_memory(char* addr, int size) {
  ThreadCritical tc;
  if (allocations.contains(addr)) {
      allocations.remove(addr);
      free(addr);
  }
  return true;
}

// No references
bool os::guard_memory(char* addr, int size) {
  return false;
}

// 1 reference - process.cpp
void os::transfer(Thread* from_thread, Event* from_event, Thread* to_thread, Event* to_event) {
  from_event->reset();
  to_event->signal();
  from_event->waitFor();  
}

// 1 reference - process.cpp
void os::transfer_and_continue(Thread* from_thread, Event* from_event, Thread* to_thread, Event* to_event) {
  from_event->reset();
  to_event->signal();
}

// 1 reference - process.cpp
void os::suspend_thread(Thread* thread) {
}

// 1 reference - process.cpp
void os::resume_thread(Thread* thread) {
}

// No references
void os::sleep(int ms) {
}

// 1 reference - process.cpp
void os::fetch_top_frame(Thread* thread, int** sp, int** fp, char** pc) {
}
  
// 1 reference - callBack.cpp
int os::current_thread_id() {
  return 0;
}

// 1 reference - process.cpp
void os::wait_for_event(Event* event) {
    event->waitFor();
}

// 1 reference - process.cpp
void os::reset_event(Event* event) {
    event->reset();
}

// 1 reference - process.cpp
void os::signal_event(Event* event) {
    event->signal();
}

// 1 reference - process.cpp
bool os::wait_for_event_or_timer(Event* event, int timeout_in_ms) {
  return false;
}

extern "C" bool WizardMode;

void process_settings_file(char* file_name, bool quiet);

static int number_of_ctrl_c = 0;

// 2 references - memory/universe, runtime/virtualspace
int os::_vm_page_size = getpagesize();

// 1 reference - timer.cpp
long_int os::elapsed_counter() {
  struct timespec current_time;
  clock_gettime(CLOCK_REALTIME, &current_time);
  int64_t current64 = ((int64_t)current_time.tv_sec) * 1000000000 + current_time.tv_nsec;
  uint high = current64 >> 32;
  uint low  = current64 & 0xffffffff;
  long_int current(low, high);
  return current;
}

// 1 reference - timer.cpp
long_int os::elapsed_frequency() {
  return long_int(1000000000, 0);
}

static struct timespec initial_time;

// 1 reference - prims/system_prims.cpp
double os::elapsedTime() {
  struct timespec current_time;
  clock_gettime(CLOCK_REALTIME, &current_time);
  long int secs = current_time.tv_sec - initial_time.tv_sec;
  long int nsecs = current_time.tv_nsec - initial_time.tv_nsec;
  if (nsecs < 0) {
    secs--;
    nsecs += 1000000000;
  } 
  return secs + (nsecs / 1000000000.0);
}

// No references
double os::currentTime() {
  return 0;
}

static void initialize_performance_counter() {
  clock_gettime(CLOCK_REALTIME, &initial_time);
}

// No references
void os::initialize_system_info() {
    main_thread = new Thread(pthread_self());
    initialize_performance_counter();
}

// 1 reference - memory/error.cpp
int os::message_box(char* title, char* message) {
  return 0;
}

char* os::platform_class_name() { return "UnixPlatform"; }

extern "C" bool EnableTasks;

pthread_mutex_t ThreadSection; 

void ThreadCritical::intialize() { pthread_mutex_init(&ThreadSection, NULL); }
void ThreadCritical::release()   { pthread_mutex_destroy(&ThreadSection);     }

ThreadCritical::ThreadCritical() {
  pthread_mutex_lock(&ThreadSection);
}

ThreadCritical::~ThreadCritical() {
  pthread_mutex_unlock(&ThreadSection);
}

void real_time_tick(int delay_time);

void* watcherMain(void* ignored) {
  const struct timespec delay = { 0, 10 * 1000 * 1000 };
  const int delay_interval = 10; // Delay 10 ms
  while(1) {
    int status = nanosleep(&delay, NULL);
    if (!status) return 0;
    real_time_tick(delay_interval);
  }
  return 0;
}

void os_init() {
  ThreadCritical::intialize();
  os::initialize_system_info();
  
  if (EnableTasks) {
    pthread_t watcherThread;
    int status = pthread_create(&watcherThread, NULL, &watcherMain, NULL);
    if (status != 0) {
      fatal("Unable to create thread");
    }
  }
}

void os_exit() {
  ThreadCritical::release();
}
#endif /* __GNUC__ */