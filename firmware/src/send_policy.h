/*
 * How the drive stream gives up on a send the modem will not take.  Pure
 * decisions, so they are checked on the host: tests/test_send_policy.py.
 *
 * send() on the modem blocks while it is not delivering: its buffer is full of
 * the datagrams it is holding, or it is changing cell.  The loop that builds
 * the records is the one waiting, so nothing is recorded for as long as it
 * waits, and neither the RAM backlog nor the flash can make that up: four
 * holes of 10 to 15 s in four days' driving, each a send that timed out at 10 s
 * and then a reconnect and a second send.
 *
 * While the drive stream runs the timeout is short, and a send that times out
 * is given up: its records wait in the backlog, as those of a shut flow-control
 * window do, and the next send tries again.  The third in a row replaces the
 * socket, which is what the long timeout did on its first.
 */
#ifndef SEND_POLICY_H
#define SEND_POLICY_H

#include <errno.h>
#include <stdbool.h>

/* The send timeout to set on the socket, in ms: the stream's own while its
 * short timeout is on and set (> 0), else the normal one. */
static inline int send_policy_timeout_ms(bool short_send, int short_ms, int normal_ms)
{
	return (short_send && short_ms > 0) ? short_ms : normal_ms;
}

/* A send failed with errno `err`.  True to give the datagram up and hold its
 * records; false to replace the socket and send once more.  Only a timeout
 * (EAGAIN) while the short timeout is on is given up, and not for the
 * held_max-th time in a row: a modem that takes nothing for that long has more
 * wrong with it than a busy moment.  *held counts the sends given up in a row;
 * the caller clears it when a send goes out. */
static inline bool send_policy_hold(int err, bool short_send, int short_ms,
				    int *held, int held_max)
{
	if (err != EAGAIN || !short_send || short_ms <= 0 || *held + 1 >= held_max) {
		*held = 0;
		return false;
	}
	(*held)++;
	return true;
}

#endif /* SEND_POLICY_H */
