#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>

// --- Configuration ---
#define NUM_ENGINEERS 3
#define NUM_SATELLITES 50            
#define CONNECTION_TIMEOUT 1         // Max wait time for satellite in seconds
#define MAX_PRIORITY 5               
#define MIN_WORK_TIME 1              // Min time engineer takes to handle request
#define MAX_WORK_TIME 3              // Max time engineer takes to handle request
#define SATELLITE_ARRIVAL_DELAY_MS 0 // Milliseconds between satellite arrivals (simulated)

typedef struct SatelliteRequest
{
    int id;
    int priority;
    sem_t request_handled_sem;        // Semaphore specific to this satellite
    time_t request_time;              // Time the request was made (for info)
    struct timespec timeout_deadline; // Absolute time for timeout
    bool handled;                     // Flag to indicate if handled or timed out
    struct SatelliteRequest *next;    // For linked list implementation
} SatelliteRequest;

// Structure for passing data to satellite threads
typedef struct
{
    int id;
    int priority; // Can be pre-assigned or generated inside thread
} SatelliteThreadData;

// --- Shared Resources ---
SatelliteRequest *requestQueue = NULL; // Head of the linked list (priority queue)
pthread_mutex_t engineerMutex;
sem_t newRequest; // Semaphore to signal engineers about new requests

// --- Global State ---
volatile bool all_satellites_launched = false;
volatile int active_satellites = 0;      // Counter for satellites still running/waiting
pthread_mutex_t active_satellites_mutex; // Mutex for the active_satellites counter

// --- Function Prototypes ---
void *satellite_thread_func(void *arg);
void *engineer_thread_func(void *arg);
void add_request_to_queue(SatelliteRequest *new_req);
SatelliteRequest *find_and_remove_highest_priority_request();
void timespec_add_seconds(struct timespec *ts, int seconds);

// --- Helper Functions ---

// Helper to add seconds to a timespec
void timespec_add_seconds(struct timespec *ts, int seconds)
{
    ts->tv_sec += seconds;
    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

// Add request to the queue (maintains no specific order here, engineer finds highest)
void add_request_to_queue(SatelliteRequest *new_req)
{
    // Simple prepend to the list
    new_req->next = requestQueue;
    requestQueue = new_req;
}

// Find the request with the highest priority, remove it, and return it
SatelliteRequest *find_and_remove_highest_priority_request()
{
    if (requestQueue == NULL)
    {
        return NULL;
    }

    SatelliteRequest *highest_priority_req = requestQueue;
    SatelliteRequest *prev_req = NULL;
    SatelliteRequest *current_req = requestQueue;
    SatelliteRequest *prev_for_highest = NULL;

    // Iterate to find the highest priority request
    while (current_req != NULL)
    {
        // Higher number = higher priority
        if (current_req->priority > highest_priority_req->priority)
        {
            highest_priority_req = current_req;
            prev_for_highest = prev_req;
        }
        prev_req = current_req;
        current_req = current_req->next;
    }

    // Remove the highest priority request from the list
    if (highest_priority_req == requestQueue)
    {
        // Highest priority is the head
        requestQueue = highest_priority_req->next;
    }
    else if (prev_for_highest != NULL)
    {
        // Highest priority is in the middle or end
        prev_for_highest->next = highest_priority_req->next;
    }
    // Should not happen if list wasn't empty, but defensive check
    else
    {
        fprintf(stderr, "Error: Logic error in finding highest priority request.\n");
        return NULL;
    }

    highest_priority_req->next = NULL; // Detach the node fully
    return highest_priority_req;
}

// --- Thread Functions ---

void *satellite_thread_func(void *arg)
{
    SatelliteThreadData *data = (SatelliteThreadData *)arg;
    int id = data->id;
    int priority = data->priority; // Use assigned priority
    free(arg);                     // Free the malloc'd data structure

    pthread_mutex_lock(&active_satellites_mutex);
    active_satellites++;
    pthread_mutex_unlock(&active_satellites_mutex);

    // Prepare the request
    SatelliteRequest *request = (SatelliteRequest *)malloc(sizeof(SatelliteRequest));
    if (!request)
    {
        perror("Failed to allocate memory for satellite request");
        pthread_mutex_lock(&active_satellites_mutex);
        active_satellites--;
        pthread_mutex_unlock(&active_satellites_mutex);
        return NULL;
    }

    request->id = id;
    request->priority = priority;
    request->request_time = time(NULL);
    request->handled = false;
    request->next = NULL;

    // Initialize the satellite's specific semaphore
    if (sem_init(&request->request_handled_sem, 0, 0) != 0)
    {
        perror("Satellite semaphore initialization failed");
        free(request);
        pthread_mutex_lock(&active_satellites_mutex);
        active_satellites--;
        pthread_mutex_unlock(&active_satellites_mutex);
        return NULL;
    }

    // Calculate timeout deadline
    clock_gettime(CLOCK_REALTIME, &request->timeout_deadline);
    timespec_add_seconds(&request->timeout_deadline, CONNECTION_TIMEOUT);

    // Lock mutex to add request to queue
    pthread_mutex_lock(&engineerMutex);
    printf("[SATELLITE] Satellite %d requesting (priority %d)\n", id, priority);
    add_request_to_queue(request);
    pthread_mutex_unlock(&engineerMutex);

    // Signal that a new request is available
    sem_post(&newRequest);

    // Wait for an engineer OR timeout
    int wait_result;
    do
    {
        wait_result = sem_timedwait(&request->request_handled_sem,
                                    &request->timeout_deadline);
    } while (wait_result == -1 && errno == EINTR); /* retry if interrupted */

    if (wait_result == 0)
    {
        // Engineer handled the request - the engineer function sets request->handled = true
        // No specific message needed here according to the prompt example
        // The engineer will print "Handling" and "Finished"
    }
    else if (errno == ETIMEDOUT)
    {
        // Timeout occurred
        pthread_mutex_lock(&engineerMutex);
        // Crucial: Check if it was *just* handled before timeout registered.
        // If !request->handled, it truly timed out. Try to remove it.
        if (!request->handled)
        {
            printf("[TIMEOUT] Satellite %d timeout %d second.\n", id, CONNECTION_TIMEOUT);
            request->handled = true; // Mark as 'done' (via timeout) to prevent engineer pickup
        }
        pthread_mutex_unlock(&engineerMutex);
    }
    else
    {
        // Another error occurred during sem_timedwait
        perror("sem_timedwait failed");
        // Mark as handled anyway to prevent dangling pointers if engineer finds it later
        pthread_mutex_lock(&engineerMutex);
        request->handled = true; // Prevent engineer pickup
        pthread_mutex_unlock(&engineerMutex);
    }

    // Cleanup satellite-specific semaphore
    sem_destroy(&request->request_handled_sem);

    pthread_mutex_lock(&active_satellites_mutex);
    active_satellites--;
    pthread_mutex_unlock(&active_satellites_mutex);

    return NULL;
}

void *engineer_thread_func(void *arg)
{
    int id = *(int *)arg;
    free(arg); // Free the malloc'd ID

    while (true)
    {
        // Wait for a new request to arrive
        // Check periodically if we should shut down, even if no new requests
        int sem_status;
        do
        {
            sem_status = sem_wait(&newRequest);
        } while (sem_status == -1 && errno == EINTR); // Retry if interrupted

        if (sem_status == -1)
        {
            perror("Engineer sem_timedwait failed");
            // Decide how to handle this - maybe exit?
            break; // Exit loop on semaphore error
        }

        pthread_mutex_lock(&engineerMutex);

        // Shutdown condition: No more satellites are running/waiting AND queue is empty
        pthread_mutex_lock(&active_satellites_mutex);
        bool should_shutdown = all_satellites_launched && active_satellites == 0 && requestQueue == NULL;
        pthread_mutex_unlock(&active_satellites_mutex);

        if (should_shutdown)
        {
            pthread_mutex_unlock(&engineerMutex);
            break; // Exit engineer loop
        }

        SatelliteRequest *req_to_handle = NULL;
        if (!should_shutdown && requestQueue != NULL)
        {
            req_to_handle = find_and_remove_highest_priority_request();

            if (req_to_handle != NULL)
            {
                // Check if the request timed out *just* before we got the lock
                if (req_to_handle->handled)
                {
                    // This request already timed out, satellite is gone. Clean up.
                    pthread_mutex_unlock(&engineerMutex); // Unlock before free
                    free(req_to_handle);
                    req_to_handle = NULL; // Don't process it
                    continue;             // Go back to waiting
                }
                else
                {
                    req_to_handle->handled = true; // Mark it as being handled
                }
            }
            else
            {
                // Spurious wakeup or queue became empty between sem_wait and lock?
                // Or highest priority one was marked handled?
                // Just loop again.
            }
        }

        pthread_mutex_unlock(&engineerMutex);

        if (req_to_handle != NULL)
        {
            // Handle the request
            printf("[ENGINEER %d] Handling Satellite %d (Priority %d)\n", id, req_to_handle->id, req_to_handle->priority);

            // Signal the satellite that its request is being handled
            sem_post(&req_to_handle->request_handled_sem);

            // Simulate work
            int work_time = (rand() % (MAX_WORK_TIME - MIN_WORK_TIME + 1)) + MIN_WORK_TIME;
            sleep(work_time); // Simulate processing time

            printf("[ENGINEER %d] Finished Satellite %d\n", id, req_to_handle->id);

            // Free the request memory (engineer is responsible)
            free(req_to_handle); // Free after processing

            // Make engineer available again
            pthread_mutex_lock(&engineerMutex);
            pthread_mutex_unlock(&engineerMutex);
        }
        else if (should_shutdown)
        {
            // Break was missed above due to mutex unlock timing, check again
            break;
        }
        // If req_to_handle is NULL and not shutting down, loop back to wait
    }

    printf("[ENGINEER %d] Exiting...\n", id);
    return NULL;
}

// --- Main Function ---
int main()
{
    pthread_t engineer_threads[NUM_ENGINEERS];
    pthread_t satellite_threads[NUM_SATELLITES];

    // Seed random number generator
    srand(time(NULL));

    // Initialize mutex and semaphores
    if (pthread_mutex_init(&engineerMutex, NULL) != 0)
    {
        perror("Mutex init failed");
        return 1;
    }
    if (pthread_mutex_init(&active_satellites_mutex, NULL) != 0)
    {
        perror("Active satellites Mutex init failed");
        pthread_mutex_destroy(&engineerMutex);
        return 1;
    }
    if (sem_init(&newRequest, 0, 0) != 0)
    {
        perror("Semaphore init failed");
        pthread_mutex_destroy(&engineerMutex);
        pthread_mutex_destroy(&active_satellites_mutex);
        return 1;
    }

    printf("Starting ground station simulation with %d engineers and %d satellites.\n", NUM_ENGINEERS, NUM_SATELLITES);
    printf("Satellite timeout window: %d seconds.\n", CONNECTION_TIMEOUT);

    // Create engineer threads
    for (int i = 0; i < NUM_ENGINEERS; i++)
    {
        int *id_ptr = malloc(sizeof(int)); // Allocate memory for the ID
        if (!id_ptr)
        {
            perror("Failed to allocate memory for engineer ID");
            // Basic cleanup before exiting needed here in a real scenario
            return 1;
        }
        *id_ptr = i;
        if (pthread_create(&engineer_threads[i], NULL, engineer_thread_func, id_ptr) != 0)
        {
            perror("Failed to create engineer thread");
            free(id_ptr);
            // Basic cleanup before exiting needed here in a real scenario
            return 1;
        }
    }

    // Create satellite threads with random priorities
    for (int i = 0; i < NUM_SATELLITES; i++)
    {
        SatelliteThreadData *data = (SatelliteThreadData *)malloc(sizeof(SatelliteThreadData));
        if (!data)
        {
            perror("Failed to allocate memory for satellite data");
            // Basic cleanup before exiting needed here in a real scenario
            return 1;
        }
        data->id = i;
        data->priority = (rand() % MAX_PRIORITY) + 1; // Priority 1 to MAX_PRIORITY

        if (pthread_create(&satellite_threads[i], NULL, satellite_thread_func, data) != 0)
        {
            perror("Failed to create satellite thread");
            free(data);
            // Basic cleanup before exiting needed here in a real scenario
            return 1;
        }
        // Simulate staggered arrival
        usleep(SATELLITE_ARRIVAL_DELAY_MS * 1000);
    }

    // Mark that all satellites have been launched (created)
    all_satellites_launched = true;

    // Wait for all satellite threads to complete
    for (int i = 0; i < NUM_SATELLITES; i++)
    {
        pthread_join(satellite_threads[i], NULL);
    }

    printf("All satellite threads have finished.\n");

    /* wake each engineer once so it can exit cleanly */
    for (int i = 0; i < NUM_ENGINEERS; i++)
    {
        sem_post(&newRequest);
    }

    // Wait for all engineer threads to complete
    for (int i = 0; i < NUM_ENGINEERS; i++)
    {
        pthread_join(engineer_threads[i], NULL);
    }

    printf("All engineer threads have exited.\n");

    // Cleanup
    pthread_mutex_destroy(&engineerMutex);
    pthread_mutex_destroy(&active_satellites_mutex);
    sem_destroy(&newRequest);

    // Verify queue is empty (should be)
    if (requestQueue != NULL)
    {
        fprintf(stderr, "Warning: Request queue not empty at exit.\n");
        // Optional: Clean up remaining requests (memory leak otherwise)
        SatelliteRequest *current = requestQueue;
        while (current != NULL)
        {
            SatelliteRequest *next = current->next;
            // Semaphores inside requests were destroyed by satellites or should have been.
            free(current);
            current = next;
        }
    }

    printf("Simulation finished.\n");

    return 0;
}