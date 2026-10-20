#pragma once
/* One bounded mailbox. Only the entry task submits work; only that task
 * pumps LVGL through the host bridge. The I/O task never touches widgets. */
static void io_ui_pulse(void);
static uint32_t g_navigation_generation;
static bool g_discard_io;
static bool g_catalog_loading;
static bool g_catalog_stale=true;
static bool g_io_cancel;
static int g_io_state; /* 0 idle, 1 submitted, 2 done, 3 parked for deletion */
static bool g_io_stop;
static void *g_io_task;
static int (*g_io_function)(void *);
static void *g_io_argument;
static int g_io_result;
static unsigned g_io_jobs, g_io_cancelled;

static void io_worker(void *unused)
{
  (void)unused;
  for (;;) {
    if (__atomic_load_n(&g_io_stop, __ATOMIC_ACQUIRE)) break;
    if (__atomic_load_n(&g_io_state, __ATOMIC_ACQUIRE) == 1) {
      g_io_result = g_io_function(g_io_argument);
      __atomic_store_n(&g_io_state, 2, __ATOMIC_RELEASE);
    }
    g_svc->delay_ms(2);
  }
  __atomic_store_n(&g_io_state, 3, __ATOMIC_RELEASE);
  /* Delete from the owner after this task has finished all I/O, before the
   * PAX code/data can be unmapped. Never publish 'done' then self-delete. */
  for (;;) g_svc->delay_ms(100);
}

static bool io_start(void)
{
  if (!g_svc->task_create || !g_svc->task_delete) return false;
  g_io_state=0;g_io_stop=false;g_io_cancel=false;g_io_task=NULL;
  return g_svc->task_create(io_worker,"launcher_io",16384,NULL,3,&g_io_task,0)==0 && g_io_task;
}

static int io_run_mode(int (*function)(void *), void *argument, bool committed)
{
  if (!g_io_task || g_discard_io || __atomic_load_n(&g_io_state,__ATOMIC_ACQUIRE)!=0) return -2;
  const uint32_t generation=__atomic_load_n(&g_navigation_generation,__ATOMIC_ACQUIRE);
  const int action=g_open_index;
  __atomic_store_n(&g_io_cancel,false,__ATOMIC_RELEASE);
  g_io_function=function;g_io_argument=argument;
  __atomic_store_n(&g_io_state,1,__ATOMIC_RELEASE);
  while (__atomic_load_n(&g_io_state,__ATOMIC_ACQUIRE)!=2) {
    io_ui_pulse();
    if (__atomic_load_n(&g_navigation_generation,__ATOMIC_ACQUIRE)!=generation || g_open_index!=action) {
      g_discard_io=true;
      if (!committed) __atomic_store_n(&g_io_cancel,true,__ATOMIC_RELEASE);
    }
    g_svc->delay_ms(1);
  }
  if (__atomic_load_n(&g_navigation_generation,__ATOMIC_ACQUIRE)!=generation || g_open_index!=action)
    g_discard_io=true;
  int result=g_io_result;
  g_io_jobs++;
  if(g_discard_io && !committed)g_io_cancelled++;
  __atomic_store_n(&g_io_state,0,__ATOMIC_RELEASE);
  return g_discard_io && !committed ? -2 : result;
}

/* Mutations finish exactly once even after navigation. Their result remains
 * meaningful; navigation is dispatched by the owner after the worker joins. */
static int io_run(int (*function)(void *), void *argument)
{ return io_run_mode(function, argument, false); }
static int io_run_committed(int (*function)(void *), void *argument)
{ return io_run_mode(function, argument, true); }

static void io_finish(void)
{
  if (!g_io_task) return;
  __atomic_store_n(&g_io_stop,true,__ATOMIC_RELEASE);
  while (__atomic_load_n(&g_io_state,__ATOMIC_ACQUIRE)!=3) g_svc->delay_ms(1);
  g_svc->task_delete(g_io_task);g_io_task=NULL;
}
