#ifndef CAN_GEN_H
#define CAN_GEN_H

#include "main.h"

/* CAN test traffic from FDCAN1 (PD0 = RX, PD1 = TX, AF9), for the signal generator
 * role. Classic CAN 2.0A/B frames are sent one every CAN_GEN_PERIOD_MS, cycling
 * through a standard data frame, a DroneCAN-style extended NodeStatus frame (the
 * kind an ARK Flow sensor sends), an empty frame and a remote frame.
 *
 * With CAN_LOOPBACK = 1 (board_config.h) the controller runs in external loopback
 * mode: it ignores the missing ACK, and the frames appear on the TX pin as plain
 * 3.3 V logic, so the scope can probe PD1 directly with no transceiver and no
 * second node. With CAN_LOOPBACK = 0 it is a normal node: connect a transceiver
 * (3.3 V part, or level shift a 5 V one) and it ACKs frames from other nodes too. */
#define CAN_GEN_PERIOD_MS  50

/* Returns 1 if the controller started. */
uint8_t CanGen_Init(void);
/* Call every main-loop pass; sends the next frame when it is due. */
void    CanGen_Task(uint32_t now_ms);
/* Frames queued for transmission so far. */
uint32_t CanGen_Sent(void);
/* Short text such as RX12 REQ3 RSP3 (frames read, node ID requests, replies queued). */
void     CanGen_Stats(char *out, uint32_t size);

#endif
