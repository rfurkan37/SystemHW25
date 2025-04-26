#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>

// --- Settings for the Simulation ---
#define NUM_ENGINEERS 5              // How many engineers we have
#define NUM_SATELLITES 25            // How many satellites will connect
#define CONNECTION_TIMEOUT 5         // Max time (seconds) a satellite waits for an engineer
#define MAX_PRIORITY 5               // Highest priority number (1 is best, 5 is lowest)
#define MIN_WORK_TIME 1              // Shortest time an engineer takes (seconds)
#define MAX_WORK_TIME 1              // Longest time an engineer takes (seconds)
#define SATELLITE_ARRIVAL_DELAY_MS 0 // Small delay between satellite starts (milliseconds)

// Structure to hold satellite request details
typedef struct SatelliteRequest
{
    int id;                          // Which satellite is this?
    int priority;                    // Its priority level (lower number = more important)
    time_t requestTime;              // When did it ask for help? (Just for info)
    struct timespec timeoutDeadline; // Absolute time when the satellite gives up
    bool handled;                    // Did an engineer pick this up or did it time out?
    struct SatelliteRequest *next;   // Pointer for making a linked list (our queue)
} SatelliteRequest;

// Simple struct to pass satellite ID and priority to its thread
typedef struct
{
    int id;
    int priority;
} SatelliteThreadData;

// --- Shared Stuff ---
SatelliteRequest *requestQueue = NULL; // The list where waiting satellites go (starts empty)
pthread_mutex_t engineerMutex;         // Lock to protect the requestQueue and availableEngineers count
sem_t newRequest;                      // Semaphore: Satellites signal when they add a request
sem_t requestHandled;                  // Semaphore: Engineers signal after taking a request

// --- Global Variables ---
volatile int availableEngineers = NUM_ENGINEERS; // How many engineers are free right now
volatile int activeSatellites = 0;               // How many satellite threads are still running/waiting
volatile bool allSatellitesLaunched = false;     // Flag to help engineers know when to stop
pthread_mutex_t activeSatellitesMutex;           // Lock for the activeSatellites counter

// --- Function Declarations ---
void *satellite(void *arg);
void *engineer(void *arg);
void addRequestToQueue(SatelliteRequest *newReq);          // Puts a new request into the list
SatelliteRequest *findAndRemoveHighestPriority();          // Gets the most important request from the list
void timespecAddSeconds(struct timespec *ts, int seconds); // Helper for timeouts

// --- Helper Functions ---

// Adds seconds to a timespec struct (for calculating timeout deadline)
void timespecAddSeconds(struct timespec *ts, int seconds)
{
    ts->tv_sec += seconds;
}

// Adds a new satellite request to the front of the queue list
void addRequestToQueue(SatelliteRequest *newReq)
{
    newReq->next = requestQueue; // Point the new request to the current start of the list
    requestQueue = newReq;       // Make the new request the new start of the list
}

// Find the request with the highest priority (lowest number), remove it from the list, and return it
SatelliteRequest *findAndRemoveHighestPriority()
{
    if (requestQueue == NULL)
    { // Check if the queue is empty
        return NULL;
    }

    // Assume the first node is the highest priority initially
    SatelliteRequest *highestPriorityReq = requestQueue;
    SatelliteRequest *prevForHighest = NULL; // Will store the node *before* the highest priority one

    // Start iterating from the second node
    SatelliteRequest *currentPrev = requestQueue;      // Node before currentReq
    SatelliteRequest *currentReq = requestQueue->next; // Node being checked

    while (currentReq != NULL)
    {
        // Check if the current node has higher priority (lower number)
        if (currentReq->priority < highestPriorityReq->priority)
        {
            // Found a new highest priority node
            highestPriorityReq = currentReq;
            // Record the node that comes *before* this new highest priority node
            prevForHighest = currentPrev;
        }
        // Move to the next node
        currentPrev = currentReq;
        currentReq = currentReq->next;
    }

    // Now, remove the highest priority node from the linked list
    if (highestPriorityReq == requestQueue)
    {
        // The highest priority node was the first one in the list
        requestQueue = highestPriorityReq->next; // Update the head of the list
    }
    else
    {
        // The highest priority node was somewhere in the middle or end.
        // 'prevForHighest' should point to the node right before it.
        if (prevForHighest != NULL)
        {
            // Make the previous node skip over the highest priority node
            prevForHighest->next = highestPriorityReq->next;
        }
        else
        {
            // This case should logically be impossible if highestPriorityReq wasn't the head.
            // Indicates a potential issue if it ever occurs.
            fprintf(stderr, "Critical Logic Error: prevForHighest is NULL but node is not head during removal.\n");
            // Avoid modifying the list in an unknown state
            return NULL; // Indicate failure
        }
    }

    highestPriorityReq->next = NULL; // Detach the removed node from the list completely
    return highestPriorityReq;       // Return the node we found and removed
}

// --- Thread Functions ---

// Function run by each satellite thread
void *satellite(void *arg)
{
    // Get satellite info passed from main
    SatelliteThreadData *data = (SatelliteThreadData *)arg;
    int id = data->id;
    int priority = data->priority;
    free(arg); // Don't need the data struct anymore

    // Increment count of active satellites (using mutex for safety)
    pthread_mutex_lock(&activeSatellitesMutex);
    activeSatellites++;
    pthread_mutex_unlock(&activeSatellitesMutex);

    // Create the request structure for this satellite
    SatelliteRequest *request = (SatelliteRequest *)malloc(sizeof(SatelliteRequest));
    if (!request) // Check if malloc failed
    {
        perror("Failed to allocate memory for satellite request");
        // Need to decrement active count if we fail here
        pthread_mutex_lock(&activeSatellitesMutex);
        activeSatellites--;
        pthread_mutex_unlock(&activeSatellitesMutex);
        return NULL;
    }

    // Fill in request details
    request->id = id;
    request->priority = priority;
    request->requestTime = time(NULL); // Record current time
    request->handled = false;          // Not handled yet
    request->next = NULL;              // Not in the list yet

    // Calculate when this satellite's connection window closes
    clock_gettime(CLOCK_REALTIME, &request->timeoutDeadline);          // Get current time
    timespecAddSeconds(&request->timeoutDeadline, CONNECTION_TIMEOUT); // Add timeout duration

    // Add the request to the shared queue (needs protection with mutex)
    pthread_mutex_lock(&engineerMutex);
    printf("[SATELLITE] Satellite %d requesting (priority %d)\n", id, priority);
    addRequestToQueue(request);
    pthread_mutex_unlock(&engineerMutex);

    // Signal using the semaphore that there's a new request waiting
    sem_post(&newRequest);

    // Now wait: either an engineer signals, or we time out
    int waitResult;
    bool timedOut = false;

    // Keep trying sem_timedwait until it succeeds, times out, or gives a real error
    while (true)
    {
        // Wait on the 'requestHandled' semaphore until the deadline
        waitResult = sem_timedwait(&requestHandled, &request->timeoutDeadline);

        if (waitResult == 0)
        {
            // Got signaled by an engineer! Assume our request is being handled (or will be soon).
            // The global semaphore makes it slightly ambiguous, but this is the simple way.
            break; // Exit the waiting loop
        }
        else if (errno == ETIMEDOUT)
        {
            // We waited too long, the deadline passed.
            timedOut = true;
            break; // Exit the waiting loop
        }
        else if (errno == EINTR)
        {
            // Interrupted by OS signal, just try waiting again
            continue;
        }
        else
        {
            // Some other semaphore error occurred
            perror("sem_timedwait failed for satellite");
            timedOut = true; // Treat as a failure for this satellite
            break;           // Exit the waiting loop
        }
    }

    // Check if we timed out
    if (timedOut)
    {
        printf("[TIMEOUT] Satellite %d timed out after %d seconds.\n", id, CONNECTION_TIMEOUT);
        // If we timed out, try to find our request in the queue and mark it 'handled'.
        // This tells the engineer to ignore it if they find it later.
        pthread_mutex_lock(&engineerMutex);
        SatelliteRequest *curr = requestQueue;
        while (curr != NULL)
        {
            // Check if this node is the exact one we created (compare memory address)
            if (curr == request)
            {
                curr->handled = true; // Mark it so engineer knows it's stale
                break;                // Found it, stop searching
            }
            curr = curr->next;
        }
        // If curr is NULL, the engineer must have grabbed it right between our timeout and locking the mutex. That's fine.
        pthread_mutex_unlock(&engineerMutex);
    }
    // If not timed out, we assume an engineer handled it (or is about to). The engineer prints the completion message.

    // Satellite thread is finishing (either handled or timed out)
    // Decrement the active satellite count (use mutex)
    pthread_mutex_lock(&activeSatellitesMutex);
    activeSatellites--;
    pthread_mutex_unlock(&activeSatellitesMutex);

    // VERY IMPORTANT: The satellite does *not* free the 'request' memory here.
    // The engineer is responsible for freeing it when they handle or discard it.
    return NULL;
}

// Function run by each engineer thread
void *engineer(void *arg)
{
    // Get engineer ID passed from main
    int id = *(int *)arg;
    free(arg); // Free the memory used to pass the ID

    // Engineers keep working until explicitly told to stop
    while (true)
    {
        // Wait for the 'newRequest' semaphore. This blocks until a satellite signals.
        int semStatus;
        do
        {
            semStatus = sem_wait(&newRequest); // Wait indefinitely
        } while (semStatus == -1 && errno == EINTR); // Retry if interrupted by OS

        if (semStatus == -1)
        {
            // If sem_wait fails for real, something's wrong
            perror("Engineer sem_wait failed");
            break; // Exit the loop/thread
        }

        // Woken up! Let's check for work or if it's time to shut down.
        SatelliteRequest *reqToHandle = NULL;
        bool shouldShutdown = false;

        // Lock the mutex to safely access shared queue and state
        pthread_mutex_lock(&engineerMutex);

        // --- Check if we should shut down ---
        // Need to lock the other mutex briefly to check activeSatellites count
        pthread_mutex_lock(&activeSatellitesMutex);
        // Condition: All satellites created, no satellites running, queue is empty
        shouldShutdown = allSatellitesLaunched && activeSatellites == 0 && requestQueue == NULL;
        pthread_mutex_unlock(&activeSatellitesMutex);

        if (shouldShutdown)
        {
            pthread_mutex_unlock(&engineerMutex); // Unlock before breaking
            break;                                // Time to exit the engineer's main loop
        }

        // --- If not shutting down, try to get a request ---
        // Find and remove the highest priority request from the queue
        reqToHandle = findAndRemoveHighestPriority();

        if (reqToHandle != NULL)
        {
            // We got a request! Now check if it timed out before we got it.
            if (reqToHandle->handled)
            {
                // The 'handled' flag was already true, meaning the satellite timed out and marked it.
                // We should just discard it.
                pthread_mutex_unlock(&engineerMutex); // Unlock mutex *before* freeing memory
                free(reqToHandle);                    // Free the timed-out request struct
                reqToHandle = NULL;
                // Don't signal requestHandled semaphore, as we didn't really handle it.
                // Don't change availableEngineers count either.
                continue; // Go back to wait for another newRequest signal
            }
            else
            {
                // This is a valid request we need to process!
                availableEngineers--;                 // Mark this engineer as busy
                reqToHandle->handled = true;          // Mark request as being handled now
                pthread_mutex_unlock(&engineerMutex); // *** Unlock mutex BEFORE signaling and doing work ***

                // --- Process the satellite request ---
                printf("[ENGINEER %d] Handling Satellite %d (Priority %d)\n", id, reqToHandle->id, reqToHandle->priority);

                // Signal the 'requestHandled' semaphore - lets waiting satellites know *someone* took a task
                sem_post(&requestHandled);

                // Simulate doing the update work
                int workTime = (rand() % (MAX_WORK_TIME - MIN_WORK_TIME + 1)) + MIN_WORK_TIME;
                sleep(workTime); // Pause for the simulated work duration

                printf("[ENGINEER %d] Finished Satellite %d\n", id, reqToHandle->id);

                // Free the memory used by the request struct now that we're done
                free(reqToHandle);
                reqToHandle = NULL;

                // Mark this engineer as available again (needs mutex protection)
                pthread_mutex_lock(&engineerMutex);
                availableEngineers++;
                pthread_mutex_unlock(&engineerMutex);
            }
        }
        else
        {
            // If reqToHandle is NULL, the queue was empty when we checked.
            // This might happen if another engineer grabbed the request between sem_wait
            // waking us and us locking the mutex (a "spurious" wakeup for us).
            pthread_mutex_unlock(&engineerMutex); // Unlock mutex

            // Briefly re-check shutdown condition here too, just in case.
            pthread_mutex_lock(&activeSatellitesMutex);
            shouldShutdown = allSatellitesLaunched && activeSatellites == 0;
            pthread_mutex_unlock(&activeSatellitesMutex);
            if (shouldShutdown)
            {
                pthread_mutex_lock(&engineerMutex);
                if (requestQueue == NULL)
                { // Double check queue is still empty
                    pthread_mutex_unlock(&engineerMutex);
                    break; // Exit loop
                }
                pthread_mutex_unlock(&engineerMutex);
            }
            // Otherwise, just loop back and wait on sem_wait(&newRequest) again.
        }
    } // End of main engineer loop (while(true))

    printf("[ENGINEER %d] Exiting...\n", id);
    return NULL;
}

// --- Main Program ---
int main()
{
    pthread_t engineerThreads[NUM_ENGINEERS];   // Array to hold engineer thread IDs
    pthread_t satelliteThreads[NUM_SATELLITES]; // Array to hold satellite thread IDs

    srand(time(NULL)); // Initialize random numbers (for priorities and work times)

    // --- Set up Mutexes and Semaphores ---
    if (pthread_mutex_init(&engineerMutex, NULL) != 0)
    {
        perror("Mutex engineerMutex init failed");
        return 1;
    }
    if (pthread_mutex_init(&activeSatellitesMutex, NULL) != 0)
    {
        perror("Mutex activeSatellitesMutex init failed");
        pthread_mutex_destroy(&engineerMutex);
        return 1;
    }
    // Initialize semaphores: 0 means they block initially
    if (sem_init(&newRequest, 0, 0) != 0)
    { // 0 = shared between threads in this process
        perror("Semaphore newRequest init failed");
        pthread_mutex_destroy(&engineerMutex);
        pthread_mutex_destroy(&activeSatellitesMutex);
        return 1;
    }
    if (sem_init(&requestHandled, 0, 0) != 0)
    {
        perror("Semaphore requestHandled init failed");
        sem_destroy(&newRequest);
        pthread_mutex_destroy(&engineerMutex);
        pthread_mutex_destroy(&activeSatellitesMutex);
        return 1;
    }

    printf("Starting ground station simulation with %d engineers and %d satellites.\n", NUM_ENGINEERS, NUM_SATELLITES);
    printf("Satellite timeout window: %d seconds. Work time: %d-%d sec. Lower priority number = higher priority.\n",
           CONNECTION_TIMEOUT, MIN_WORK_TIME, MAX_WORK_TIME);

    // --- Start the Engineer Threads ---
    for (int i = 0; i < NUM_ENGINEERS; i++)
    {
        // Need to pass the engineer's ID to the thread function
        int *id_ptr = malloc(sizeof(int)); // Allocate memory for the ID
        if (!id_ptr)
        {
            perror("Failed to allocate memory for engineer ID"); /* TODO: Add cleanup for already created threads/semaphores */
            return 1;
        }
        *id_ptr = i; // Store the ID
        if (pthread_create(&engineerThreads[i], NULL, engineer, id_ptr) != 0)
        {
            perror("Failed to create engineer thread");
            free(id_ptr); /* TODO: Add cleanup */
            return 1;
        }
    }

    // --- Start the Satellite Threads ---
    for (int i = 0; i < NUM_SATELLITES; i++)
    {
        // Create data struct to pass ID and priority
        SatelliteThreadData *data = (SatelliteThreadData *)malloc(sizeof(SatelliteThreadData));
        if (!data)
        {
            perror("Failed to allocate memory for satellite data"); /* TODO: Add cleanup */
            return 1;
        }
        data->id = i;                                 // Assign satellite ID
        data->priority = (rand() % MAX_PRIORITY) + 1; // Assign random priority (1 to MAX_PRIORITY)

        if (pthread_create(&satelliteThreads[i], NULL, satellite, data) != 0)
        {
            perror("Failed to create satellite thread");
            free(data); /* TODO: Add cleanup */
            return 1;
        }
        // Add a small delay between starting satellites to make the output less simultaneous
        if (SATELLITE_ARRIVAL_DELAY_MS > 0)
        {
            usleep(SATELLITE_ARRIVAL_DELAY_MS * 1000); // usleep takes microseconds
        }
    }

    // --- All threads launched ---
    // Set the flag so engineers know no more satellites are coming
    pthread_mutex_lock(&activeSatellitesMutex);
    allSatellitesLaunched = true;
    pthread_mutex_unlock(&activeSatellitesMutex);

    printf("All satellite threads created and requesting...\n");

    // --- Wait for all Satellite Threads to finish ---
    // Main thread waits here until each satellite thread completes (either handled or timed out)
    for (int i = 0; i < NUM_SATELLITES; i++)
    {
        pthread_join(satelliteThreads[i], NULL); // Wait for thread i to finish
    }
    printf("All satellite threads have finished (handled or timed out).\n");

    // --- Tell Engineers to Check for Shutdown ---
    // Since satellites might finish without new requests coming in,
    // engineers might be stuck waiting on sem_wait(&newRequest).
    // We post the semaphore enough times to wake them all up so they can check the shutdown condition.
    printf("Signaling engineers for final shutdown check...\n");
    for (int i = 0; i < NUM_ENGINEERS; i++)
    {
        sem_post(&newRequest);
    }

    // --- Wait for all Engineer Threads to finish ---
    // Main thread waits here until each engineer thread exits its loop
    for (int i = 0; i < NUM_ENGINEERS; i++)
    {
        pthread_join(engineerThreads[i], NULL); // Wait for engineer i to finish
    }
    printf("All engineer threads have exited.\n");

    // --- Clean up Resources ---
    pthread_mutex_destroy(&engineerMutex);
    pthread_mutex_destroy(&activeSatellitesMutex);
    sem_destroy(&newRequest);
    sem_destroy(&requestHandled);

    // Final check: Was the request queue properly emptied?
    // If not, it might indicate a logic error or memory leak.
    if (requestQueue != NULL)
    {
        fprintf(stderr, "Warning: Request queue not empty at exit. Cleaning up...\n");
        SatelliteRequest *cur = requestQueue;
        while (cur != NULL)
        {
            SatelliteRequest *next = cur->next;
            fprintf(stderr, " - Removing leftover request for satellite %d (priority %d, handled=%d)\n", cur->id, cur->priority, cur->handled);
            free(cur); // Free any remaining nodes
            cur = next;
        }
    }

    printf("Simulation finished.\n");
    return 0; // Success!
}