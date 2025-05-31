#!/bin/bash

# --- Configuration ---
SERVER_IP="127.0.0.1"  # Change if your server is on a different IP
SERVER_PORT="5000"     # Change if your server is on a different port
NUM_CLIENTS=30         # Number of clients to launch
CLIENT_EXECUTABLE="./chatclient"
LOG_PREFIX="client_log_"
COMMAND_DELAY=1 # Seconds to wait between sending commands for a client
INTER_CLIENT_DELAY=0.1 # Seconds to wait between launching each client

# --- Basic Command Sequences for Clients ---
# You can customize these or make them more complex
declare -a commands_sequence_1=(
    "/join general"
    "/broadcast Hello from general room!"
    "/whisper user$((RANDOM % NUM_CLIENTS + 1)) Quick private message!" # Whisper a random user
    "/join projectX"
    "/broadcast Switched to projectX."
    # "/sendfile sample.txt user$((RANDOM % NUM_CLIENTS + 1))" # Uncomment to test file sends
)

declare -a commands_sequence_2=(
    "/join projectX"
    "/broadcast Greetings from projectX members!"
    "/whisper user$((RANDOM % NUM_CLIENTS + 1)) Another whisper."
    "/join general"
    "/broadcast Back in general!"
)

# Create a dummy sample.txt if you uncomment the sendfile command
# echo "This is a sample file for testing." > sample.txt

# --- Function to run a single client ---
run_client() {
    local client_id=$1
    local username="user${client_id}"
    local log_file="${LOG_PREFIX}${client_id}.txt"
    local commands_to_run=("${@:2}") # Get command sequence passed as arguments

    echo "Starting client ${client_id} (Username: ${username}) logging to ${log_file}"

    # Use expect or a named pipe for more controlled interaction.
    # For simplicity here, we'll pipe commands. This is less robust for interactive prompts.
    # This assumes your client reads commands line by line after login.

    (
        echo "${username}" # Send username for login prompt
        sleep 1 # Wait for login to complete

        for cmd in "${commands_to_run[@]}"; do
            # Replace dynamic parts if needed, e.g., for specific recipient in sendfile
            # For now, whisper targets are randomized above.
            echo "${cmd}"
            sleep "${COMMAND_DELAY}"
        done
        echo "/exit"
        sleep 1 # Give time for exit command to be processed
    ) | "${CLIENT_EXECUTABLE}" "${SERVER_IP}" "${SERVER_PORT}" > "${log_file}" 2>&1 &
    # The '&' runs the client in the background
}

# --- Main Script Logic ---
echo "Launching ${NUM_CLIENTS} clients..."
echo "Server Target: ${SERVER_IP}:${SERVER_PORT}"
echo "Make sure your chatserver is running!"

# Create log directory if it doesn't exist
mkdir -p client_logs
LOG_PREFIX="client_logs/${LOG_PREFIX}"


for i in $(seq 1 "${NUM_CLIENTS}"); do
    # Alternate between command sequences for variety
    if (( i % 2 == 0 )); then
        run_client "$i" "${commands_sequence_1[@]}"
    else
        run_client "$i" "${commands_sequence_2[@]}"
    fi
    sleep "${INTER_CLIENT_DELAY}" # Stagger client launches slightly
done

echo "All ${NUM_CLIENTS} clients launched. Check client_logs/ directory for individual logs."
echo "Waiting for clients to finish (this is a rough estimate)..."

# Wait for a while for clients to do their thing.
# A more robust way would be to monitor PIDs, but this is simpler for now.
# Adjust the total wait time based on your COMMAND_DELAY and number of commands.
TOTAL_WAIT_TIME=$(echo "$NUM_CLIENTS * $COMMAND_DELAY * 5 + 10" | bc) # Rough estimate
echo "Script will exit in approximately ${TOTAL_WAIT_TIME} seconds."
sleep 15

echo "Script finished."
echo "Remember to check server.log and the client_logs/ directory."