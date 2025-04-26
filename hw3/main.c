#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>

// --- Configuration ---
#define NUM_ENGINEERS 1 // The constraint
#define NUM_SATELLITES 5
#define CONNECTION_TIMEOUT 5 // Moderate timeout
#define MAX_PRIORITY 5
#define MIN_WORK_TIME 2     // Engineer takes noticeable time
#define MAX_WORK_TIME 3
#define SATELLITE_ARRIVAL_DELAY_MS 100 // Stagger arrivals slightly// Milliseconds between satellite arrivals

typedef struct SatelliteRequest
{
    int id;
    int priority; // Lower number = higher priority
    // Removed per-satellite semaphore: sem_t request_handled_sem;
    time_t request_time;              // Time the request was made (for info)
    struct timespec timeout_deadline; // Absolute time for timeout
    bool handled;                     // Flag to indicate if handled or timed out
    struct SatelliteRequest *next;    // For linked list implementation
} SatelliteRequest;

// Structure for passing data to satellite threads
typedef struct
{
    int id;
    int priority;
} SatelliteThreadData;

// --- Shared Resources ---
SatelliteRequest *requestQueue = NULL; // Head of the linked list (priority queue)
pthread_mutex_t engineerMutex;         // Protects requestQueue and availableEngineers
sem_t newRequest;                      // Signaled by satellites
sem_t requestHandled;                  // Signaled by engineers (GLOBAL)

// --- Global State ---
volatile int active_satellites = 0;              // Counter for satellites still running/waiting
volatile int availableEngineers = NUM_ENGINEERS; // Number of engineers available
volatile bool all_satellites_launched = false;
pthread_mutex_t active_satellites_mutex;         // Mutex for the active_satellites counter

// --- Function Prototypes ---
void *satellite_thread_func(void *arg);
void *engineer_thread_func(void *arg);
void add_request_to_queue(SatelliteRequest *new_req);
SatelliteRequest *find_and_remove_highest_priority_request(); // Finds lowest number
void timespec_add_seconds(struct timespec *ts, int seconds);
long timespec_diff_ms(struct timespec start, struct timespec end); // Helper for recalculating timeout (optional but good)

// --- Helper Functions ---

void timespec_add_seconds(struct timespec *ts, int seconds)
{
    ts->tv_sec += seconds;
    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

// Calculate difference between two timespecs in milliseconds
long timespec_diff_ms(struct timespec start, struct timespec end)
{
    return (end.tv_sec - start.tv_sec) * 1000L + (end.tv_nsec - start.tv_nsec) / 1000000L;
}

void add_request_to_queue(SatelliteRequest *new_req)
{
    new_req->next = requestQueue;
    requestQueue = new_req;
}

// Find the request with the highest priority (lowest number), remove it, and return it
SatelliteRequest *find_and_remove_highest_priority_request()
{
    if (requestQueue == NULL)
        return NULL;

    SatelliteRequest *highest_priority_req = requestQueue;
    SatelliteRequest *prev_req = NULL;
    SatelliteRequest *current_req = requestQueue;
    SatelliteRequest *prev_for_highest = NULL;

    while (current_req != NULL)
    {
        // LOWER number = higher priority
        if (current_req->priority < highest_priority_req->priority)
        {
            highest_priority_req = current_req;
            prev_for_highest = prev_req;
        }
        prev_req = current_req;
        current_req = current_req->next;
    }

    if (highest_priority_req == requestQueue)
    {
        requestQueue = highest_priority_req->next;
    }
    else if (prev_for_highest != NULL)
    {
        prev_for_highest->next = highest_priority_req->next;
    }
    else
    {
        fprintf(stderr, "Error: Logic error in finding highest priority request.\n");
        return NULL;
    }

    highest_priority_req->next = NULL;
    return highest_priority_req;
}

// --- Thread Functions ---

void *satellite_thread_func(void *arg)
{
    SatelliteThreadData *data = (SatelliteThreadData *)arg;
    int id = data->id;
    int priority = data->priority;
    free(arg);

    pthread_mutex_lock(&active_satellites_mutex);
    active_satellites++;
    pthread_mutex_unlock(&active_satellites_mutex);

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
    // No per-satellite semaphore initialization needed

    clock_gettime(CLOCK_REALTIME, &request->timeout_deadline);
    timespec_add_seconds(&request->timeout_deadline, CONNECTION_TIMEOUT);

    pthread_mutex_lock(&engineerMutex);
    printf("[SATELLITE] Satellite %d requesting (priority %d)\n", id, priority);
    add_request_to_queue(request);
    pthread_mutex_unlock(&engineerMutex);

    sem_post(&newRequest);

    bool satellite_completed = false;
    bool successfully_handled = false;
    while (!successfully_handled)
    {
        int wait_result;
        // Wait on the GLOBAL requestHandled semaphore until the absolute deadline
        wait_result = sem_timedwait(&requestHandled, &request->timeout_deadline);

        if(wait_result == 0)
        {
            pthread_mutex_lock(&engineerMutex);

            successfully_handled = true;
            satellite_completed = true;

            pthread_mutex_unlock(&engineerMutex);
        }
        else if (errno == ETIMEDOUT)
        {
            // Timeout occurred
            pthread_mutex_lock(&engineerMutex);
            
            if
        }
        else if (errno == EINTR)
        {
            // Interrupted by signal, retry
            continue;
        }
        else
        {
            perror("sem_timedwait failed");
            break;
        }
    } // end while(!was_handled_or_timed_out)

    // Cleanup: The engineer is responsible for freeing the request struct memory
    // if it handles it. If it timed out, the engineer *might* still find it
    // later, see handled=true, and free it. The satellite itself should NOT free the request struct.

    pthread_mutex_lock(&active_satellites_mutex);
    active_satellites--;
    pthread_mutex_unlock(&active_satellites_mutex);

    return NULL;
}

void *engineer_thread_func(void *arg)
{
    int id = *(int *)arg;
    free(arg);

    while (true)
    {
        int sem_status;
        do
        {
            sem_status = sem_wait(&newRequest);
        } while (sem_status == -1 && errno == EINTR);

        if (sem_status == -1)
        {
            perror("Engineer sem_wait failed");
            break;
        }

        SatelliteRequest *req_to_handle = NULL;
        bool should_shutdown = false;

        pthread_mutex_lock(&engineerMutex);

        pthread_mutex_lock(&active_satellites_mutex);
        should_shutdown = all_satellites_launched && active_satellites == 0 && requestQueue == NULL;
        pthread_mutex_unlock(&active_satellites_mutex);

        if (should_shutdown)
        {
            pthread_mutex_unlock(&engineerMutex);
            break;
        }

        req_to_handle = find_and_remove_highest_priority_request();

        if (req_to_handle != NULL)
        {
            if (req_to_handle->handled)
            {
                // Request timed out before we could remove it. Satellite marked it. Free memory.
                pthread_mutex_unlock(&engineerMutex);
                free(req_to_handle);
                req_to_handle = NULL;
            }
            else
            {
                // We got a valid request. Mark handled, claim engineer.
                req_to_handle->handled = true;
                availableEngineers--;
                pthread_mutex_unlock(&engineerMutex); // Unlock before work/signal

                // --- Handle the request ---
                printf("[ENGINEER %d] Handling Satellite %d (Priority %d)\n", id, req_to_handle->id, req_to_handle->priority);

                // Signal the GLOBAL semaphore that *a* request was handled
                sem_post(&requestHandled); // Strictly adhering to PDF

                // Simulate work
                int work_time = (rand() % (MAX_WORK_TIME - MIN_WORK_TIME + 1)) + MIN_WORK_TIME;
                sleep(work_time);

                printf("[ENGINEER %d] Finished Satellite %d\n", id, req_to_handle->id);

                // Free the request memory
                free(req_to_handle);

                // Release engineer
                pthread_mutex_lock(&engineerMutex);
                availableEngineers++;
                pthread_mutex_unlock(&engineerMutex);
            }
        }
        else
        {
            // No request found or spurious wakeup for engineer? Or shutdown?
            pthread_mutex_unlock(&engineerMutex);
            // Optional: Add brief sleep here if spurious wakeups are very frequent
            // Check shutdown again more thoroughly if needed
            pthread_mutex_lock(&active_satellites_mutex);
            should_shutdown = all_satellites_launched && active_satellites == 0;
            pthread_mutex_unlock(&active_satellites_mutex);
            if (should_shutdown)
            {
                pthread_mutex_lock(&engineerMutex);
                if (requestQueue == NULL)
                {
                    pthread_mutex_unlock(&engineerMutex);
                    break;
                }
                pthread_mutex_unlock(&engineerMutex);
            }
        }
    } // End while(true)

    printf("[ENGINEER %d] Exiting...\n", id);
    return NULL;
}

// --- Main Function ---
int main()
{
    pthread_t engineer_threads[NUM_ENGINEERS];
    pthread_t satellite_threads[NUM_SATELLITES];

    srand(time(NULL));

    if (pthread_mutex_init(&engineerMutex, NULL) != 0)
    {
        perror("Mutex engineerMutex init failed");
        return 1;
    }
    if (pthread_mutex_init(&active_satellites_mutex, NULL) != 0)
    {
        perror("Mutex active_satellites_mutex init failed");
        pthread_mutex_destroy(&engineerMutex);
        return 1;
    }
    if (sem_init(&newRequest, 0, 0) != 0)
    {
        perror("Semaphore newRequest init failed");
        pthread_mutex_destroy(&engineerMutex);
        pthread_mutex_destroy(&active_satellites_mutex);
        return 1;
    }
    if (sem_init(&requestHandled, 0, 0) != 0)
    {
        perror("Semaphore requestHandled init failed");
        sem_destroy(&newRequest);
        pthread_mutex_destroy(&engineerMutex);
        pthread_mutex_destroy(&active_satellites_mutex);
        return 1;
    }

    printf("Starting ground station simulation with %d engineers and %d satellites.\n", NUM_ENGINEERS, NUM_SATELLITES);
    printf("Satellite timeout window: %d seconds. Lower priority number = higher priority.\n", CONNECTION_TIMEOUT);

    for (int i = 0; i < NUM_ENGINEERS; i++)
    {
        int *id_ptr = malloc(sizeof(int));
        if (!id_ptr)
        {
            perror("Failed to allocate memory for engineer ID"); /* Add cleanup */
            return 1;
        }
        *id_ptr = i;
        if (pthread_create(&engineer_threads[i], NULL, engineer_thread_func, id_ptr) != 0)
        {
            perror("Failed to create engineer thread");
            free(id_ptr); /* Add cleanup */
            return 1;
        }
    }

    for (int i = 0; i < NUM_SATELLITES; i++)
    {
        SatelliteThreadData *data = (SatelliteThreadData *)malloc(sizeof(SatelliteThreadData));
        if (!data)
        {
            perror("Failed to allocate memory for satellite data"); /* Add cleanup */
            return 1;
        }
        data->id = i;
        data->priority = (rand() % MAX_PRIORITY) + 1; // Priority 1 (highest) to MAX_PRIORITY (lowest)

        if (pthread_create(&satellite_threads[i], NULL, satellite_thread_func, data) != 0)
        {
            perror("Failed to create satellite thread");
            free(data); /* Add cleanup */
            return 1;
        }
        if (SATELLITE_ARRIVAL_DELAY_MS > 0)
        {
            usleep(SATELLITE_ARRIVAL_DELAY_MS * 1000);
        }
    }

    pthread_mutex_lock(&active_satellites_mutex);
    all_satellites_launched = true;
    pthread_mutex_unlock(&active_satellites_mutex);

    printf("All satellite threads created and requesting...\n");

    for (int i = 0; i < NUM_SATELLITES; i++)
    {
        pthread_join(satellite_threads[i], NULL);
    }
    printf("All satellite threads have finished.\n");

    // Wake up engineers for final shutdown check
    for (int i = 0; i < NUM_ENGINEERS; i++)
    {
        sem_post(&newRequest);
    }

    for (int i = 0; i < NUM_ENGINEERS; i++)
    {
        pthread_join(engineer_threads[i], NULL);
    }
    printf("All engineer threads have exited.\n");

    // Cleanup
    pthread_mutex_destroy(&engineerMutex);
    pthread_mutex_destroy(&active_satellites_mutex);
    sem_destroy(&newRequest);
    sem_destroy(&requestHandled); // Destroy the global semaphore

    if (requestQueue != NULL)
    {
        fprintf(stderr, "Warning: Request queue not empty at exit. Cleaning up...\n");
        SatelliteRequest *cur = requestQueue;
        while (cur != NULL)
        {
            SatelliteRequest *next = cur->next;
            fprintf(stderr, " - Removing leftover request for satellite %d (priority %d)\n", cur->id, cur->priority);
            free(cur);
            cur = next;
        }
    }

    printf("Simulation finished.\n");
    return 0;
}