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


 // Add a task to the threadpool. This function does not need to acquire a lock. The 
 // reason for this is because this is the only function that ever touches the tail
 // of the queue. If the queue head is pointing to the same location as the queue tail,
 // this is taken to indicate an empty queue. When the tail is incremented after adding
 // a task, it logically appears to have added the task atomically.
 // We do not have to worry about calls to add_task overlapping each other because only
 // the main thread that listens for connections ever calls add_task (whether it is adding
 // an actual new task or the dummy task to tell the worker threads to shut down). 
 // Since only one thread ever calls this function, all calls to it are sequential.
int threadpool_add_task(threadpool_t *pool, void (*function)(void *), void *argument)
{
    int err = 0;

    /* Add task to queue */
    // Queue is stored in a circular buffer. task_queue_size_limit + 1 is the size of the
    // array. Make sure that after the add, head != tail because that would look like
    // an empty queue. If it would, the queue is full and an element cannot be added.
    // We do not have to lock before checking queueHead because it will only ever be
    // moved to allow MORE space in the queue; therefore, we are not worried that some
    // time between making sure that there is space in the queue and completing the
    // addition of the task to the queue the spot that we are writing too becomes invalid.
    // As is mentioned above, calls to threadpool_add_task can never overlap, so no issues
    // with queueTail moving either.
    if((pool->queueTail + 1) % (pool->task_queue_size_limit + 1) != pool->queueHead)
    {
      pool->queue[pool->queueTail].function = function;
      pool->queue[pool->queueTail].argument = argument;

      // Must increment tail AFTER adding function to the queue in so that the task
      // is not grabbed before add_task has finished writing it
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
      return -1;
    }
        
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

    // Add dummy task to the threadpool to signal the
    // threads to exit.
    while(threadpool_add_task(pool, NULL, NULL));
    

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

// This function tries to grab a task from the task queue.
// The queue's head lock must be locked when this function is
// called. If a task is found to execute, the lock is released,
// the task is executed, and the function returns 1. If the queue
// is empty, the lock remains locked and the function returns 0.
static int try_grab_task(threadpool_t* pool)
{
  if(pool->queueHead != pool->queueTail)
  {
    //queue is not empty
    threadpool_task_t task = pool->queue[pool->queueHead];

    if(task.function == NULL)
    {
      // This is the dummy task telling the worker threads to exit so we can
      // shut down. Release the lock and exit.
      pthread_mutex_unlock(&(pool->lock));
      pthread_exit(NULL);
    }

    // Found a task to execute. Remove task from the queue by bumping up the
    // queue's head, release the lock, and execute the function. We want the
    // lock released before execution because the task does not have to do with
    // the thread pool.
    pool->queueHead = (pool->queueHead + 1) % (pool->task_queue_size_limit + 1);
    pthread_mutex_unlock(&(pool->lock));
    (task.function)(task.argument);
    return 1;
  }

  // Queue was empty. Did not find a task to execute.
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

        // Before going to sleep, we want to check whether the queue is empty.
        // This is very important because if all worker threads are busy doing
        // work when the condition is triggered, none of them will see the
        // notification. If no other tasks ever get added to the queue, the
        // condition will never be triggered again and a task will sit untouched
        // even after the threads finish their work because they would just go
        // to sleep waiting for a notification that never comes. To prevent this,
        // they check the queue to make sure that it is empty before going to sleep.
        // We do not have to worry about tasks being added to the queue between trying
        // to grab the task and sleeping because if the queue is found to be empty,
        // the executing thread never releases the lock until it gets to the wait() call.
        // add_task requires the lock to add a task to the queue, so it will not be able
        // to do so until the thread is waiting for a notification.
        if(try_grab_task(pool))
          continue;

        /* Wait on condition variable, check for spurious wakeups.
           When returning from pthread_cond_wait(), do some task. */
        pthread_cond_wait(&(pool->notify), &(pool->lock));

        // try_grab_task checks for spurious wakeups.
        // If the queue is found to be empty, try_grab_task does not release
        // the lock, so we have to do so here.
        if(!try_grab_task(pool))
          pthread_mutex_unlock(&(pool->lock));
    }

    pthread_exit(NULL);
    return(NULL);
}
