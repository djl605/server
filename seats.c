#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "seats.h"

// Changed seats from a linked list to an array to improve
// performance, so now we also need to keep track of the number
// of seats in the array.
seat_t* seats = NULL;
int num_seats;

char seat_state_to_char(seat_state_t);

// Self-explanatory. Traverse the array, print out the status of every seat.
// No locking needed because it is not harmful if a seat's state is changed
// either before or after printing out the status. Since state is the only
// variable of the seat_t structure which we print here that can change,
// we do not have to worry about being consistent between different printed
// variables reflecting different (inconsistent) states.
void list_seats(char* buf, int bufsize)
{
  int i, index = 0;
  for(i = 0; i < num_seats && index < bufsize + strlen("%d %c,"); ++i)
  {
    int length = snprintf(buf+index, bufsize-index,
            "%d %c,", seats[i].id, seat_state_to_char(seats[i].state));
    if (length > 0)
        index = index + length;
  }
  if (index > 0)
      snprintf(buf+index-1, bufsize-index-1, "\n");
  else
      snprintf(buf, bufsize, "No seats not found\n\n");
}


void view_seat(char* buf, int bufsize,  int seat_id, int customer_id, int customer_priority)
{
  int i = seat_id - 1;
  if(i >= num_seats)
  {
    snprintf(buf, bufsize, "Requested seat not found\n\n");
    return;
  }
  
  seat_state_t temp_state = seats[i].state;
  int temp_cust_id = seats[i].customer_id;

  // check to see whether we even need to lock. This is an optimization to try to avoid
  // locking whenever possible. If we see a state where we might want to update, we need
  // to lock so that no other thread sees the seat in an inconsistent state. After locking,
  // we also must make sure that no other thread has jumped ahead of us and claimed the seat
  // while we were trying to lock.
  if(temp_state == AVAILABLE || (temp_state == PENDING && temp_cust_id == customer_id))
  {
    pthread_mutex_lock(&(seats[i].lock));
    //check to make sure nothing has changed while we were trying to acquire the lock
    if(seats[i].state == AVAILABLE || (seats[i].state == PENDING && seats[i].customer_id == customer_id))
    {
        snprintf(buf, bufsize, "Confirm seat: %d %c ?\n\n",
                seats[i].id, seat_state_to_char(seats[i].state));
        seats[i].state = PENDING;
        seats[i].customer_id = customer_id;
    }
    else
    {
        snprintf(buf, bufsize, "Seat unavailable\n\n");
    }
    pthread_mutex_unlock(&(seats[i].lock));
  } 
  //Else not doing anything dangerous, no need to lock
  else
    snprintf(buf, bufsize, "Seat unavailable\n\n");
}


void confirm_seat(char* buf, int bufsize, int seat_id, int customer_id, int customer_priority)
{
  int i = seat_id - 1;
  if(i >= num_seats)
    snprintf(buf, bufsize, "Requested seat not found\n\n");

  seat_state_t temp_state = seats[i].state;
  int temp_cust_id = seats[i].customer_id;
  
  // check to see whether we even need to lock. Same explanation as above.
  if(temp_state == PENDING && temp_cust_id == customer_id)
  {
    pthread_mutex_lock(&(seats[i].lock));
    //check to make sure nothing has changed while we were trying to acquire the lock
    if(seats[i].state == PENDING && seats[i].customer_id == customer_id )
    {
        snprintf(buf, bufsize, "Seat confirmed: %d %c\n\n",
                seats[i].id, seat_state_to_char(seats[i].state));
        seats[i].state = OCCUPIED;
    }
    else if(seats[i].customer_id != customer_id )
    {
        snprintf(buf, bufsize, "Permission denied - seat held by another user\n\n");
    }
    else if(seats[i].state != PENDING)
    {
        snprintf(buf, bufsize, "No pending request\n\n");
    }

    pthread_mutex_unlock(&(seats[i].lock));
  }
  //Else not doing anything dangerous, no need to lock
  else if(temp_cust_id != customer_id)
    snprintf(buf, bufsize, "Permission denied - seat held by another user\n\n");
  else if(temp_state != PENDING)
    snprintf(buf, bufsize, "No pending request\n\n");
}

void cancel(char* buf, int bufsize, int seat_id, int customer_id, int customer_priority)
{
  int i = seat_id - 1;
  if(i >= num_seats)
    snprintf(buf, bufsize, "Seat not found\n\n");
  
  seat_state_t temp_state = seats[i].state;
  int temp_cust_id = seats[i].customer_id;

  //check whether we even have to lock
  if(temp_state == PENDING && temp_cust_id == customer_id)
  {
    pthread_mutex_lock(&(seats[i].lock));
    //check to make sure nothing has changed while we were trying to acquire the lock
    if(seats[i].state == PENDING && seats[i].customer_id == customer_id )
    {
        snprintf(buf, bufsize, "Seat request cancelled: %d %c\n\n",
                seats[i].id, seat_state_to_char(seats[i].state));
        seats[i].state = AVAILABLE;
    }
    else if(seats[i].customer_id != customer_id )
    {
        snprintf(buf, bufsize, "Permission denied - seat held by another user\n\n");
    }
    else if(seats[i].state != PENDING)
    {
        snprintf(buf, bufsize, "No pending request\n\n");
    }
    pthread_mutex_unlock(&(seats[i].lock));
  }
  //Else not doing anything dangerous, no need to lock
  else if(temp_cust_id != customer_id)
    snprintf(buf, bufsize, "Permission denied - seat held by another user\n\n");
  else if(temp_state != PENDING)
    snprintf(buf, bufsize, "No pending request\n\n");
}

void load_seats(int number_of_seats)
{
    int i;
    seats = (seat_t*)malloc(sizeof(seat_t) * number_of_seats);
    num_seats = number_of_seats;
    for(i = 0; i < number_of_seats; i++)
    {   
        seats[i].id = i+1;
        seats[i].customer_id = -1;
        seats[i].state = AVAILABLE;
        pthread_mutex_init(&(seats[i].lock), NULL);
    }
}

void unload_seats()
{
  free((void*)seats);
}

char seat_state_to_char(seat_state_t state)
{
    switch(state)
    {
        case AVAILABLE:
            return 'A';
        case PENDING:
            return 'P';
        case OCCUPIED:
            return 'O';
    }

    return '?';
}
