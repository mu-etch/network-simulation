#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "diag/trace.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "semphr.h"

// Basic network settings - packet sizes and timing
#define L1 500                  // Minimum packet size in bytes
#define L2 1500                 // Maximum packet size in bytes
#define T1_MS 100               // Minimum time between packets (milliseconds)
#define T2_MS 200               // Maximum time between packets (milliseconds)
#define K 40                    // ACK packet size in bytes
#define C_BPS 100000            // Network speed in bits per second
#define P_ACK 0.01              // Probability of dropping ACK packets
#define D_MS 5                  // Network delay in milliseconds
#define HEADER_SIZE 10          // Size of packet header in bytes
#define MAX_RETRIES 4           // Maximum times to retry sending a packet
#define TARGET_PACKETS 2000     // Stop simulation after this many packets per receiver

static double P_DROP = 0.02;    // Probability of dropping data packets
static int32_t TOUT_MS = 150;   // Timeout before retrying (milliseconds)
static int32_t N_WINDOW = 2;    // Window size (1 = Send-and-Wait, >1 = Go-Back-N)

// RTOS configuration
#define QUEUE_DEPTH 20
#define STACK_SIZE 512
#define SWITCH_STACK_SIZE 1024

// For doing math without floating point numbers
#define FIXED_POINT_SCALE 1000

// Types of packets we can send
typedef enum { PKT_DATA = 0, PKT_ACK = 1 } PacketType;

// Information that goes at the start of every packet
typedef struct {
    int8_t src, dest;           // Who sent it and who should receive it
    int32_t seq;                // Sequence number to keep packets in order
    int16_t len;                // Total packet length
    int8_t type, reserved;      // What type of packet this is
} PacketHeader;

// Dynamic packet structure - only allocate needed size
typedef struct {
    PacketHeader hdr;           // Standard header
    int8_t data[];              // Variable length data
} DataPacket;

// An acknowledgment packet to confirm we got a data packet
typedef struct {
    PacketHeader hdr;           // Standard header
    int8_t padding[K - HEADER_SIZE];
} AckPacket;

// Information about a packet we're waiting to get confirmed
typedef struct {
    DataPacket *packet;         // Copy of the packet we sent
    TimerHandle_t timer;        // Timer to know when to give up waiting
    int32_t retries;            // How many times we've tried sending this
    bool inUse;
} TxBufferEntry;

// Queues for passing packets
static QueueHandle_t switchDataQ, switchAckQ, recv3Q, recv4Q, sender1AckQ, sender2AckQ;
static SemaphoreHandle_t statsMutex;

// Keep track of what's happening in the simulation
static int32_t totalReceived[2] = {0}, totalSent[2] = {0};
static int32_t totalDropped[2][2] = {{0}}, receivedFrom[2][2] = {{0}};
static int32_t droppedPackets[2] = {0}, totalTransmissions = 0;
static int32_t totalBytesReceived = 0;
static int32_t simulationStartTime = 0;
static bool simulationDone = false;
static int32_t senderTransmissions[2] = {0};  // Track transmissions per sender
static bool receiverReachedTarget[2] = {false, false};

// Protocol state for each sender and receiver
static int32_t senderSeq[2][2] = {{0}}, expectedSeq[2][2] = {{0}};
static TxBufferEntry txBuffer[2][2][16];  // [sender][destination][seq%16]
static int32_t sendBase[2][2] = {{0}}, nextSeqNum[2][2] = {{0}};  // Window management per destination

// Helper function to allocate a packet with specific size
static DataPacket* allocatePacket(int16_t totalLen) {
    size_t allocSize = sizeof(PacketHeader) + (totalLen - HEADER_SIZE);
    return (DataPacket*)pvPortMalloc(allocSize);
}

// Helper function to divide numbers without floating point
static int32_t fixedDivide(int32_t numerator, int32_t denominator) {
    return (numerator * FIXED_POINT_SCALE) / denominator;
}

// Get a random number between min and max
static int32_t uniformRandom(int32_t min, int32_t max) {
    return min + (rand() % (max - min + 1));
}

// Calculate how long it takes to transmit a packet based on its size
static int32_t calcTransmissionDelay(int16_t packetLen) {
    return (packetLen * 8 * 1000) / C_BPS;
}

// Print all the final results when simulation ends
static void printFinalStats(void) {
    xSemaphoreTake(statsMutex, portMAX_DELAY);

    // Figure out how long the simulation ran
    int32_t simTimeMs = xTaskGetTickCount() * portTICK_PERIOD_MS - simulationStartTime;
    int32_t simTimeSec = simTimeMs / 1000;
    int32_t simTimeFrac = (simTimeMs % 1000) * FIXED_POINT_SCALE / 1000;

    // Calculate throughput
    int32_t throughput = 0;
    if (simTimeSec > 0) {
        int64_t temp = ((int64_t)totalBytesReceived * FIXED_POINT_SCALE) / simTimeSec;
        throughput = (int32_t)temp;
    }
    trace_printf("\n=== SIMULATION COMPLETE ===\n");
    int32_t p_drop_scaled = (int32_t)(P_DROP * 1000);
    trace_printf("Parameters: P_drop=%d.%03d, Timeout=%dms, Window=%d\n",
                 p_drop_scaled/1000, p_drop_scaled%1000, TOUT_MS, N_WINDOW);

    trace_printf("\nSIMULATION TIME: %d.%03d seconds\n",
                simTimeSec,
                (simTimeFrac * 1000) / FIXED_POINT_SCALE);
    trace_printf("THROUGHPUT (N=%d): %d.%03d bytes/sec\n",
                     N_WINDOW,
                     throughput / FIXED_POINT_SCALE,
                     (throughput % FIXED_POINT_SCALE) * 1000 / FIXED_POINT_SCALE);
    trace_printf("\nRECEIVED PACKETS:\n");
    trace_printf("  Receiver 3: %d total\n", totalReceived[0]);
    trace_printf("    From Sender 1: %d\n", receivedFrom[0][0]);
    trace_printf("    From Sender 2: %d\n", receivedFrom[0][1]);
    trace_printf("  Receiver 4: %d total\n", totalReceived[1]);
    trace_printf("    From Sender 1: %d\n", receivedFrom[1][0]);
    trace_printf("    From Sender 2: %d\n", receivedFrom[1][1]);

    trace_printf("\nDROPPED BY NETWORK:\n");
    for (int s = 0; s < 2; s++) {
        for (int r = 0; r < 2; r++) {
            trace_printf("  Sender %d -> Receiver %d: %d\n", s+1, r+3, totalDropped[s][r]);
        }
    }

    trace_printf("\nTHROUGHPUT ANALYSIS:\n");
    trace_printf("Total bytes received: %d\n", totalBytesReceived);
    trace_printf("Total simulation time: %d.%03d sec\n",
                simTimeSec,
                (simTimeFrac * 1000) / FIXED_POINT_SCALE);
    trace_printf("Throughput: %d.%03d bytes/sec\n",
                throughput / FIXED_POINT_SCALE,
                (throughput % FIXED_POINT_SCALE) * 1000 / FIXED_POINT_SCALE);

    trace_printf("\nDROPPED DUE TO MAX RETRIES:\n");
    trace_printf("  Sender 1: %d packets\n", droppedPackets[0]);
    trace_printf("  Sender 2: %d packets\n", droppedPackets[1]);

    trace_printf("\nTRANSMISSION DETAILS:\n");
    int32_t totalRx = totalReceived[0] + totalReceived[1];
    trace_printf("Total packets successfully received: %d\n", totalRx);
    trace_printf("Total transmissions (including retries): %d\n", totalTransmissions);

    // Per-sender transmission analysis
    for (int s = 0; s < 2; s++) {
        int32_t senderRx = receivedFrom[0][s] + receivedFrom[1][s];
        int32_t senderRetries = senderTransmissions[s] - totalSent[s];
        trace_printf("\nSender %d Analysis:\n", s + 1);
        trace_printf("  Original packets generated: %d\n", totalSent[s]);
        trace_printf("  Total transmissions (orig + retries): %d\n", senderTransmissions[s]);
        trace_printf("  Retransmissions: %d\n", senderRetries);
        trace_printf("  Successfully received: %d\n", senderRx);
        trace_printf("  Dropped due to max retries: %d\n", droppedPackets[s]);
        if (totalSent[s] > 0) {
            int32_t retryRate = fixedDivide(senderRetries, totalSent[s]);
            trace_printf("  Retransmission rate: %d.%03d retries per original packet\n",
                        retryRate / FIXED_POINT_SCALE,
                        (retryRate % FIXED_POINT_SCALE) * 1000 / FIXED_POINT_SCALE);
        }

        if (senderRx > 0) {
            int32_t avgTxPerSuccess = fixedDivide(senderTransmissions[s], senderRx);
            trace_printf("  Average transmissions per successfully received packet: %d.%03d\n",
                        avgTxPerSuccess / FIXED_POINT_SCALE,
                        (avgTxPerSuccess % FIXED_POINT_SCALE) * 1000 / FIXED_POINT_SCALE);
        }
    }

    if (totalRx > 0) {
        int32_t avgTx = fixedDivide(totalTransmissions, totalRx);
        trace_printf("\nOVERALL RETRANSMISSION PERFORMANCE:\n");
        trace_printf("Average transmissions per successfully received packet: %d.%03d\n",
                    avgTx / FIXED_POINT_SCALE,
                    (avgTx % FIXED_POINT_SCALE) * 1000 / FIXED_POINT_SCALE);

        int32_t totalRetries = totalTransmissions - (totalSent[0] + totalSent[1]);
        int32_t overallRetryRate = fixedDivide(totalRetries, totalSent[0] + totalSent[1]);
        trace_printf("Overall retransmission rate: %d.%03d retries per original packet\n",
                    overallRetryRate / FIXED_POINT_SCALE,
                    (overallRetryRate % FIXED_POINT_SCALE) * 1000 / FIXED_POINT_SCALE);
    }

    xSemaphoreGive(statsMutex);
}

// Stop the simulation and print results
static void endSimulation(void) {
    if (!simulationDone) {
        simulationDone = true;
        printFinalStats();
        vTaskEndScheduler();
    }
}

// Called when we timeout waiting for an ACK
static void timeoutCallback(TimerHandle_t timer) {
    TxBufferEntry *entry = (TxBufferEntry *)pvTimerGetTimerID(timer);
    if (!entry || !entry->inUse || simulationDone) return;

    int senderId = entry->packet->hdr.src - 1;
    int destId = entry->packet->hdr.dest - 3;
    int32_t timedOutSeq = entry->packet->hdr.seq;

    // For Go-Back-N protocol
    if (N_WINDOW > 1) {
        entry->retries++;
        if (entry->retries >= MAX_RETRIES) {
            xSemaphoreTake(statsMutex, portMAX_DELAY);
            droppedPackets[senderId]++;
            xSemaphoreGive(statsMutex);
            trace_printf("Sender %d: Giving up on seq %d after %d retries\n",
                         senderId + 1, timedOutSeq, MAX_RETRIES);
            vPortFree(entry->packet);
            xTimerDelete(entry->timer, 0);
            entry->inUse = false;
            if (timedOutSeq == sendBase[senderId][destId]) {
                sendBase[senderId][destId] = timedOutSeq + 1;
                while (sendBase[senderId][destId] < nextSeqNum[senderId][destId] &&
                       !txBuffer[senderId][destId][sendBase[senderId][destId] % 16].inUse) {
                    sendBase[senderId][destId]++;
                }
            }
            expectedSeq[destId][senderId] = timedOutSeq + 1;
            return;
        }

        // Retransmit exactly N packets starting from timedOutSeq
        int32_t limit = timedOutSeq + N_WINDOW - 1;

        trace_printf("Sender %d: Go-Back-N timeout on seq %d, retransmitting window [%d..%d]\n",
                     senderId + 1, timedOutSeq, timedOutSeq, limit);

        for (int32_t seq = timedOutSeq; seq <= limit; seq++) {
            TxBufferEntry *e = &txBuffer[senderId][destId][seq % 16];
            if (e->inUse && e->packet->hdr.seq == seq) {
                e->retries++; // Increment retry count for each retransmitted packet
                DataPacket *newPkt = allocatePacket(e->packet->hdr.len);
                if (newPkt) {
                    memcpy(newPkt, e->packet,
                           sizeof(PacketHeader) + (e->packet->hdr.len - HEADER_SIZE));
                    if (xQueueSend(switchDataQ, &newPkt, pdMS_TO_TICKS(10)) == pdPASS) {
                        xSemaphoreTake(statsMutex, portMAX_DELAY);
                        totalTransmissions++;
                        senderTransmissions[senderId]++;
                        xSemaphoreGive(statsMutex);
                        trace_printf("Sender %d: Retransmitted seq %d (attempt %d)\n",
                                     senderId + 1, seq, e->retries + 1);
                    } else {
                        vPortFree(newPkt);
                    }
                }
            }
        }
        // Start the timer again for next timeout
        xTimerReset(entry->timer, 0);
        return;
    }

    // For Send-and-Wait or non-base packets in Go-Back-N
    entry->retries++;
    if (entry->retries >= MAX_RETRIES) {
        xSemaphoreTake(statsMutex, portMAX_DELAY);
        droppedPackets[senderId]++;
        xSemaphoreGive(statsMutex);
        trace_printf("Sender %d: Giving up on seq %d after %d retries\n",
                     senderId + 1, entry->packet->hdr.seq, MAX_RETRIES);

        vPortFree(entry->packet);
        xTimerDelete(entry->timer, 0);
        entry->inUse = false;

        if (N_WINDOW > 1) {
            // Update sender window if this was the base packet
            if (entry->packet->hdr.seq == sendBase[senderId][destId]) {
                sendBase[senderId][destId]++;
                while (sendBase[senderId][destId] < nextSeqNum[senderId][destId] &&
                       !txBuffer[senderId][destId][sendBase[senderId][destId] % 16].inUse) {
                    sendBase[senderId][destId]++;
                }
            }
        } else {
            //  update sendBase to allow next packet
            sendBase[senderId][destId] = entry->packet->hdr.seq + 1;
        }
        return;
    }

    // Try sending again
    DataPacket *newPkt = allocatePacket(entry->packet->hdr.len);
    if (newPkt) {
        memcpy(newPkt, entry->packet,
               sizeof(PacketHeader) + (entry->packet->hdr.len - HEADER_SIZE));
        if (xQueueSend(switchDataQ, &newPkt, pdMS_TO_TICKS(10)) == pdPASS) {
            xSemaphoreTake(statsMutex, portMAX_DELAY);
            totalTransmissions++;
            senderTransmissions[senderId]++;
            xSemaphoreGive(statsMutex);
            trace_printf("Sender %d: Retransmitting seq %d (attempt %d)\n",
                         senderId + 1, entry->packet->hdr.seq, entry->retries + 1);
            xTimerReset(timer, 0);
        } else {
            vPortFree(newPkt);
        }
    }
}

// Create a new packet
static void generatePacket(int8_t senderId) {
    if (simulationDone) return;

    int sIdx = senderId - 1;
    int8_t dest = (rand() & 1) ? 3 : 4;  // Random destination (receiver 3 or 4)
    int rIdx = dest - 3;  // Convert dest (3,4) to index (0,1)

    // Check if window is full per destination (applies to both Send-and-Wait and Go-Back-N)
    if (nextSeqNum[sIdx][rIdx] >= sendBase[sIdx][rIdx] + N_WINDOW) return;

    // Generate random packet length
    int16_t packetLen = uniformRandom(L1, L2);

    // Allocate memory for the new packet
    DataPacket *pkt = allocatePacket(packetLen);
    if (!pkt) return;

    // Fill in packet information
    pkt->hdr.src = senderId;
    pkt->hdr.dest = dest;
    pkt->hdr.len = packetLen;
    pkt->hdr.type = PKT_DATA;
    pkt->hdr.seq = senderSeq[sIdx][rIdx]++;

    // Fill with random data
    for (int i = 0; i < pkt->hdr.len - HEADER_SIZE; i++) {
        pkt->data[i] = (int8_t)(rand() & 0xFF);
    }

    // Store packet in transmission buffer for retransmission if needed
    int bufIdx = pkt->hdr.seq % 16;
    TxBufferEntry *entry = &txBuffer[sIdx][rIdx][bufIdx];

    if (entry->inUse) {
        // Buffer slot is already in use, drop this packet
        vPortFree(pkt);
        return;
    }

    // Make a copy for the buffer
    entry->packet = allocatePacket(pkt->hdr.len);
    if (!entry->packet) {
        vPortFree(pkt);
        return;
    }
    memcpy(entry->packet, pkt,
           sizeof(PacketHeader) + (pkt->hdr.len - HEADER_SIZE));
    entry->retries = 0;
    entry->inUse = true;

    // Set up timeout timer
    entry->timer = xTimerCreate(
        "TxTimer", pdMS_TO_TICKS(TOUT_MS), pdFALSE, entry, timeoutCallback
    );
    if (!entry->timer) {
        vPortFree(entry->packet);
        vPortFree(pkt);
        entry->inUse = false;
        return;
    }

    // Send the packet to the switch with timeout to prevent blocking
    if (xQueueSend(switchDataQ, &pkt, pdMS_TO_TICKS(10)) == pdPASS) {
        xTimerStart(entry->timer, 0);

        // Update statistics
        xSemaphoreTake(statsMutex, portMAX_DELAY);
        totalSent[senderId - 1]++;
        totalTransmissions++;
        senderTransmissions[senderId - 1]++;
        xSemaphoreGive(statsMutex);

        // Update window for both Send-and-Wait and Go-Back-N
        nextSeqNum[sIdx][rIdx]++;

        trace_printf("Sender %d: Generated packet seq=%d, dest=%d, len=%d\n",
                     senderId, pkt->hdr.seq, pkt->hdr.dest, pkt->hdr.len);
    } else {
        // Queue full, clean up
        vPortFree(entry->packet);
        vPortFree(pkt);
        xTimerDelete(entry->timer, 0);
        entry->inUse = false;
    }
}

// Timer callback to generate packets at random intervals
static void packetGenCallback(TimerHandle_t timer) {
    int8_t senderId = (int8_t)(intptr_t)pvTimerGetTimerID(timer);
    generatePacket(senderId);

    // Reset timer with new random interval
    TickType_t newPeriod = pdMS_TO_TICKS(uniformRandom(T1_MS, T2_MS));
    xTimerChangePeriod(timer, newPeriod, 0);
}

//Representing a sender node
static void senderTask(void *param) {
    int8_t senderId = (int8_t)(intptr_t)param;
    QueueHandle_t ackQ = (senderId == 1) ? sender1AckQ : sender2AckQ;

    // Set up timer
    TimerHandle_t genTimer = xTimerCreate(
        "GenTimer",
        pdMS_TO_TICKS(uniformRandom(T1_MS, T2_MS)),
        pdTRUE,
        (void *)(intptr_t)senderId,
        packetGenCallback
    );
    xTimerStart(genTimer, 0);

    while (!simulationDone) {
        AckPacket *ack;
        // Check for incoming ACKs
        if (xQueueReceive(ackQ, &ack, pdMS_TO_TICKS(50)) == pdPASS) {
            int sIdx = senderId - 1;
            int rIdx = ack->hdr.src - 3;  // ACK source is the receiver
            int32_t ackSeq = ack->hdr.seq;

            if (N_WINDOW > 1) {
                // GO-Back-N
                bool foundAckedPacket = false;
                for (int32_t seq = sendBase[sIdx][rIdx]; seq <= ackSeq; seq++) {
                    int bufIdx = seq % 16;
                    TxBufferEntry *entry = &txBuffer[sIdx][rIdx][bufIdx];
                    if (entry->inUse && entry->packet->hdr.seq == seq) {
                        xTimerDelete(entry->timer, 0);
                        vPortFree(entry->packet);
                        entry->inUse = false;
                        foundAckedPacket = true;
                        trace_printf("Sender %d: ACKed seq %d\n", senderId, seq);
                    }
                }
                if (foundAckedPacket) {
                    sendBase[sIdx][rIdx] = ackSeq + 1;
                    trace_printf("Sender %d: Window advanced to base=%d\n",
                                 senderId, sendBase[sIdx][rIdx]);
                }
            } else {
                // Send-and-Wait
                int bufIdx = ackSeq % 16;
                TxBufferEntry *entry = &txBuffer[sIdx][rIdx][bufIdx];
                if (entry->inUse && entry->packet->hdr.seq == ackSeq) {
                    trace_printf("Sender %d: ACK received for seq %d\n",
                                 senderId, ackSeq);
                    xTimerDelete(entry->timer, 0);
                    vPortFree(entry->packet);
                    entry->inUse = false;
                    // Update sendBase for Send-and-Wait
                    sendBase[sIdx][rIdx] = ackSeq + 1;
                }
            }
            vPortFree(ack);
        }
    }
    vTaskDelete(NULL);
}

// Timer callback to forward a data packet after network delay
static void forwardCallback(TimerHandle_t timer) {
    DataPacket *pkt = (DataPacket *)pvTimerGetTimerID(timer);
    QueueHandle_t destQ = (pkt->hdr.dest == 3) ? recv3Q : recv4Q;
    if (xQueueSend(destQ, &pkt, pdMS_TO_TICKS(10)) != pdPASS) {
        vPortFree(pkt);
    }
    xTimerDelete(timer, 0);
}

// Timer callback to forward an ACK packet after network delay
static void forwardAckCallback(TimerHandle_t timer) {
    AckPacket *ack = (AckPacket *)pvTimerGetTimerID(timer);
    QueueHandle_t destQ = (ack->hdr.dest == 1) ? sender1AckQ : sender2AckQ;
    if (xQueueSend(destQ, &ack, pdMS_TO_TICKS(10)) != pdPASS) {
        vPortFree(ack);
    }
    xTimerDelete(timer, 0);
}

//simulating the network switch
static void switchTask(void *param) {
    (void)param;

    while (!simulationDone) {
        DataPacket *dataPkt = NULL;
        AckPacket *ackPkt = NULL;

        // Check for data packets to forward
        if (xQueueReceive(switchDataQ, &dataPkt, 0) == pdPASS) {
            double dropProb = (double)(rand()) / RAND_MAX;
            if (dropProb < P_DROP) {
                // Drop the packet
                int sIdx = dataPkt->hdr.src - 1;
                int rIdx = dataPkt->hdr.dest - 3;
                xSemaphoreTake(statsMutex, portMAX_DELAY);
                totalDropped[sIdx][rIdx]++;
                trace_printf("SWITCH: DATA packet DROPPED! Sender %d -> Receiver %d (seq=%d, len=%d)\n",
                             dataPkt->hdr.src, dataPkt->hdr.dest, dataPkt->hdr.seq, dataPkt->hdr.len);
                xSemaphoreGive(statsMutex);
                vPortFree(dataPkt);
            } else {
                // Forward the packet after network delay
                int32_t txDelay = calcTransmissionDelay(dataPkt->hdr.len);
                int32_t totalDelay = D_MS + txDelay;
                xSemaphoreTake(statsMutex, portMAX_DELAY);
                trace_printf("SWITCH: Forwarding DATA packet from Sender %d to Receiver %d (seq=%d, delay=%dms)\n",
                             dataPkt->hdr.src, dataPkt->hdr.dest, dataPkt->hdr.seq, totalDelay);
                xSemaphoreGive(statsMutex);
                TimerHandle_t fwdTimer = xTimerCreate(
                    "FwdTimer",
                    pdMS_TO_TICKS(totalDelay),
                    pdFALSE,
                    dataPkt,
                    forwardCallback
                );
                if (fwdTimer) {
                    xTimerStart(fwdTimer, 0);
                } else {
                    vPortFree(dataPkt);
                }
            }
        }

        // Check for ACK packets to forward
        if (xQueueReceive(switchAckQ, &ackPkt, 0) == pdPASS) {
            double dropProb = (double)(rand()) / RAND_MAX;
            if (dropProb < P_ACK) {
                // Drop the ACK
                xSemaphoreTake(statsMutex, portMAX_DELAY);
                trace_printf("SWITCH: ACK packet DROPPED! Receiver %d -> Sender %d (seq=%d)\n",
                             ackPkt->hdr.src, ackPkt->hdr.dest, ackPkt->hdr.seq);
                xSemaphoreGive(statsMutex);
                vPortFree(ackPkt);
            } else {
                // Forward the ACK after network delay
                int32_t txDelay = calcTransmissionDelay(K);
                int32_t totalDelay = D_MS + txDelay;
                xSemaphoreTake(statsMutex, portMAX_DELAY);
                trace_printf("SWITCH: Forwarding ACK from Receiver %d to Sender %d (seq=%d, delay=%dms)\n",
                             ackPkt->hdr.src, ackPkt->hdr.dest, ackPkt->hdr.seq, totalDelay);
                xSemaphoreGive(statsMutex);
                TimerHandle_t fwdTimer = xTimerCreate(
                    "FwdAckTimer",
                    pdMS_TO_TICKS(totalDelay),
                    pdFALSE,
                    ackPkt,
                    forwardAckCallback
                );
                if (fwdTimer) {
                    xTimerStart(fwdTimer, 0);
                } else {
                    vPortFree(ackPkt);
                }
            }
        }

        // Small delay to let other tasks run
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    vTaskDelete(NULL);
}

//Representing a receiver node
static void receiverTask(void *param) {
    int8_t receiverId = (int8_t)(intptr_t)param;
    QueueHandle_t myQ = (receiverId == 3) ? recv3Q : recv4Q;
    int rIdx = receiverId - 3;

    while (!simulationDone) {
        DataPacket *pkt;
        if (xQueueReceive(myQ, &pkt, pdMS_TO_TICKS(100)) == pdPASS) {
            int sIdx = pkt->hdr.src - 1;

            bool acceptPacket = !receiverReachedTarget[rIdx];
            // For Go-Back-N (only accept packets in order)
            if (acceptPacket && N_WINDOW > 1) {
                if (pkt->hdr.seq != expectedSeq[rIdx][sIdx]) {
                    acceptPacket = false;
                    trace_printf("Receiver %d: Out-of-order packet from Sender %d seq=%d, expected=%d - DROPPED\n",
                                 receiverId, pkt->hdr.src, pkt->hdr.seq, expectedSeq[rIdx][sIdx]);
                }
            }

            if (acceptPacket) {
                // Update statistics for accepted packets
                xSemaphoreTake(statsMutex, portMAX_DELAY);
                totalReceived[rIdx]++;
                receivedFrom[rIdx][sIdx]++;
                totalBytesReceived += pkt->hdr.len;

                // Check if this receiver just reached its target
                if (totalReceived[rIdx] >= TARGET_PACKETS) {
                    receiverReachedTarget[rIdx] = true;
                }
                xSemaphoreGive(statsMutex);

                // Update expected sequence number for Go-Back-N
                if (N_WINDOW > 1) {
                    expectedSeq[rIdx][sIdx]++;
                }

                trace_printf("Receiver %d: Received packet src=%d, seq=%d, len=%d (R3:%d, R4:%d)\n",
                             receiverId, pkt->hdr.src, pkt->hdr.seq, pkt->hdr.len,
                             totalReceived[0], totalReceived[1]);

                // Check if both receivers have reached their targets
                if (receiverReachedTarget[0] && receiverReachedTarget[1]) {
                    endSimulation();
                }
            }

            // Always send an ACK back
            AckPacket *ack = pvPortMalloc(sizeof(AckPacket));
            if (ack) {
                ack->hdr.src = receiverId;
                ack->hdr.dest = pkt->hdr.src;
                ack->hdr.seq = acceptPacket ? pkt->hdr.seq : (expectedSeq[rIdx][sIdx] - 1);
                ack->hdr.len = K;
                ack->hdr.type = PKT_ACK;

                if (xQueueSend(switchAckQ, &ack, pdMS_TO_TICKS(10)) == pdPASS) {
                    trace_printf("Receiver %d: Sent ACK for seq %d to sender %d\n",
                                 receiverId, ack->hdr.seq, pkt->hdr.src);
                } else {
                    vPortFree(ack);
                }
            }

            vPortFree(pkt);
        }
    }
    vTaskDelete(NULL);
}

//Memory allocation fails
void vApplicationMallocFailedHook(void) {
    trace_printf("FATAL: Malloc failed!\n");
    taskDISABLE_INTERRUPTS();
    for (;;);
}

//Using too much stack memory
void vApplicationStackOverflowHook(TaskHandle_t task, char *taskName) {
    (void)task;
    trace_printf("FATAL: Stack overflow in task %s!\n", taskName);
    taskDISABLE_INTERRUPTS();
    for (;;);
}

//Hook functions
void vApplicationTickHook(void) {}
void vApplicationIdleHook(void) {}

int main(void) {
    srand(12345);  // Set random seed for consistent results
    trace_printf("Starting Network Communication Simulation\n");
    int32_t p_drop_scaled = (int32_t)(P_DROP * 1000);
    trace_printf("Configuration: %s Protocol (N=%d), P_drop=%d.%03d, Timeout=%dms\n",
                 (N_WINDOW == 1) ? "S&W" : "Go-Back-N",
                 N_WINDOW,
                 p_drop_scaled / 1000,
                 p_drop_scaled % 1000,
                 TOUT_MS);

    simulationStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Create mutex to protect shared statistics
    statsMutex = xSemaphoreCreateMutex();
    if (!statsMutex) {
        trace_printf("Failed to create mutex!\n");
        return -1;
    }

    // Create all the queues
    switchDataQ = xQueueCreate(QUEUE_DEPTH, sizeof(DataPacket *));
    switchAckQ = xQueueCreate(QUEUE_DEPTH, sizeof(AckPacket *));
    recv3Q = xQueueCreate(QUEUE_DEPTH, sizeof(DataPacket *));
    recv4Q = xQueueCreate(QUEUE_DEPTH, sizeof(DataPacket *));
    sender1AckQ = xQueueCreate(QUEUE_DEPTH, sizeof(AckPacket *));
    sender2AckQ = xQueueCreate(QUEUE_DEPTH, sizeof(AckPacket *));

    if (!switchDataQ || !switchAckQ || !recv3Q || !recv4Q || !sender1AckQ || !sender2AckQ) {
        trace_printf("Failed to create queues!\n");
        return -1;
    }

    // Initialize transmission buffers as empty
    for (int s = 0; s < 2; s++) {
        for (int r = 0; r < 2; r++) {
            senderSeq[s][r] = 0;
            expectedSeq[r][s] = 0;
            sendBase[s][r] = 0;
            nextSeqNum[s][r] = 0;
            for (int i = 0; i < 16; i++) {
                txBuffer[s][r][i].inUse = false;
            }
        }
    }

    // Create all the tasks with different priorities
    xTaskCreate(senderTask, "Sender1", STACK_SIZE, (void *)1, 2, NULL);
    xTaskCreate(senderTask, "Sender2", STACK_SIZE, (void *)2, 2, NULL);
    xTaskCreate(switchTask, "Switch", SWITCH_STACK_SIZE, NULL, 3, NULL);
    xTaskCreate(receiverTask, "Receiver3", STACK_SIZE, (void *)3, 4, NULL);
    xTaskCreate(receiverTask, "Receiver4", STACK_SIZE, (void *)4, 4, NULL);

    // Start the RTOS scheduler
    vTaskStartScheduler();

    // This should never be reached
    trace_printf("ERROR: Scheduler returned!\n");
    return -1;
}
