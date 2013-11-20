#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

#include "thread_pool.h"

/**
 *  @struct threadpool_task
 *  @brief the work struct
 *
 *  Feel free to make any modifications you want to the function prototypes and structs
 *
 *  @var function Pointer to the function that will perform the task.
 *  @var argument Argument to be passed to the function.
 */

typedef struct {
    void (*function)(void *);
    void *argument;
} threadpool_task_t;


struct threadpool_t {
  pthread_mutex_t lock;
  pthread_cond_t notify;
  pthread_t *threads;
  threadpool_task_t *queue;
  int thread_count;
  int task_queue_size_limit;
  int queueHead;
  int queueTail;
};

/**
 * @function void *threadpool_work(void *threadpool)
 * @brief the worker thread
 * @param threadpool the pool which own the thread
 */
static void *thread_do_work(void *threadpool);
static int try_grab_task(threadpool_t* threadpool);


/*
 * Create a threadpool, initialize variables, etc
 *
 */
threadpool_t *threadpool_create(int thread_count, int queue_size)
{
  int i;

  // Initialize all of the elements of the threadpool_t struct
  threadpool_t* pool = (threadpool_t*)malloc(sizeof(threadpool_t));
  pthread_mutex_init(&(pool->lock), NULL);
  pthread_cond_init(&(pool->notify), NULL);

  // Array of threads
  pool->threads = (pthread_t*)malloc(sizeof(pthread_t) * thread_count);
  for(i = 0; i < thread_count; ++i)
    pthread_create(&(pool->threads[i]), NULL, thread_do_work, (void*)pool);

  // Queue is an array of size queue_size + 1 in order to properly queue as an array
  pool->queue = (threadpool_task_t*)malloc(sizeof(threadpool_task_t) * (queue_size + 1));
  pool->thread_count = thread_count;
  pool->task_queue_size_limit = queue_size;
  pool->queueHead = 0;
  pool->queueTail = 0;

  return pool;
}


/*
 * Add a task to the threadpool
 *
 */
int threadpool_add_task(threadpool_t *pool, void (*function)(void *), void *argument)
{
    int err = 0;
    /* Get the lock */
    err = pthread_mutex_lock(&(pool->lock));
    if(err)
    {
      printf("Error: lock failed\n");
      return err;
    }

    /* Add task to queue */
    // Queue is stored in a circular buffer. task_queue_size_limit + 1 is the size of the
    // array. Make sure that after the add, head != tail because that would look like
    // an empty queue. If it would, the queue is full and an element cannot be added
    if((pool->queueTail + 1) % (pool->task_queue_size_limit + 1) != pool->queueHead)
    {
      pool->queue[pool->queueTail].function = function;
      pool->queue[pool->queueTail].argument = argument;
      pool->queueTail = (pool->queueTail + 1) % (pool->task_queue_size_limit + 1);

      // Notify sleeping threads that a new task has been added to the queue.
      err = pthread_cond_broadcast(&(pool->notify));
      if(err)
        printf("Error: broadcast failed\n");
    }
    else
    {
      // The queue is full.  We cannot add a task. We fail here and return -1. The caller
      // may try to call add_task repeatedly until a task is added successfully. We are not
      // worried about starvation in that case. Explanation for why is in http_server.c
      err = pthread_mutex_unlock(&(pool->lock));
      if(err)
        printf("Error: failed to unlock\n");

      return -1;
    }
        
    /* pthread_cond_broadcast and unlock */
    err = pthread_mutex_unlock(&(pool->lock));
    if(err)
      printf("Error: failed to unlock\n");

    
    return err;
}



/*
 * Destroy the threadpool, free all memory, destroy treads, etc
 *
 */
int threadpool_destroy(threadpool_t *pool)
{
    int err = 0;
    int i;

    
    /* Wake up all worker threads */
    while(threadpool_add_task(pool, NULL, NULL));
    
    err = pthread_cond_broadcast(&(pool->notify));

    

    /* Join all worker thread */
    for(i = 0; i < pool->thread_count; ++i)
    {
      if(pool->threads[i] != pthread_self())
        pthread_join(pool->threads[i], NULL);
    }

    free((void*)pool->queue);
    free((void*)pool->threads);
    free((void*)pool);

    /* Only if everything went well do we deallocate the pool */
    return err;
}

static int try_grab_task(threadpool_t* pool)
{
  if(pool->queueHead != pool->queueTail)
  {
    threadpool_task_t task = pool->queue[pool->queueHead];

    if(task.function == NULL)
    {
      pthread_mutex_unlock(&(pool->lock));
      pthread_exit(NULL);
    }
    pool->queueHead = (pool->queueHead + 1) % (pool->task_queue_size_limit + 1);
    pthread_mutex_unlock(&(pool->lock));
    (task.function)(task.argument);
    return 1;
  }
  return 0;
}

/*
 * Work loop for threads. Should be passed into the pthread_create() method.
 *
 */
static void *thread_do_work(void *threadpool)
{ 
    threadpool_t* pool = (threadpool_t*)threadpool;
    while(1) {
        /* Lock must be taken to wait on conditional variable */
        pthread_mutex_lock(&(pool->lock));

        if(try_grab_task(pool))
          continue;

        /* Wait on condition variable, check for spurious wakeups.
           When returning from pthread_cond_wait(), do some task. */
        pthread_cond_wait(&(pool->notify), &(pool->lock));

        if(!try_grab_task(pool))
          pthread_mutex_unlock(&(pool->lock));
    }

    pthread_exit(NULL);
    return(NULL);
}
