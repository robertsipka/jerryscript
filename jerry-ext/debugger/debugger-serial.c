/* Copyright JS Foundation and other contributors, http://js.foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// TODO: ezzel kezdeni valamit
#define _BSD_SOURCE
#include "jerryscript-debugger-transport.h"
#include "jerryscript-ext/debugger.h"
#include "jext-common.h"

#ifdef JERRY_DEBUGGER

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

// TODO: ezzel kezdeni valamit
#pragma GCC diagnostic ignored "-Wsign-conversion"

/**
 * Implementation of transport over serial connection.
 */
typedef struct
{
  jerry_debugger_transport_header_t header; /**< transport header */
  int fd; /**< file descriptor */
} jerryx_debugger_transport_serial_t;

/**
 * Correctly close a file descriptor.
 */
static inline void
jerryx_debugger_serial_close_fd (int fd) /**< file descriptor to close */
{
  if (close (fd) != 0)
  {
    JERRYX_ERROR_MSG ("Error while closing the file descriptor: %d\n", errno);
  }
} /* jerryx_debugger_serial_close_fd */

/**
 * Configure the file descriptor used by the serial communcation.
 *
 * @return true if everything is ok
 *         false if there was an error
 */
static inline bool
jerryx_debugger_serial_configure_attributes (int fd, int speed, int parity)
{
  struct termios tty;
  memset (&tty, 0, sizeof (tty));

  /* Get the parameters associated with the file descriptor */
  if (tcgetattr (fd, &tty) != 0)
  {
      JERRYX_ERROR_MSG ("Error from tcgetattr: %d\n", errno);
      return false;
  }

  /* Set the input and output baud rates */
  cfsetispeed (&tty, speed);
  cfsetospeed (&tty, speed);

  /* Set the control modes */
  tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; // set character size mask to 8-bit chars
  tty.c_cflag |= (CLOCAL | CREAD);// ignore modem control lines and enable the receiver
  tty.c_cflag &= ~(PARENB | PARODD); // shut off the parity
  tty.c_cflag |= parity; // set the input parity
  tty.c_cflag &= ~CSTOPB; // set the stop bits
  tty.c_cflag &= ~CRTSCTS; // set the RTS/CTS

  /* Set the input modes */
  tty.c_iflag &= ~IGNBRK; // disable break processing
  tty.c_iflag &= ~(IXON | IXOFF | IXANY); // disable xon/xoff ctrl

  /* Set the output modes: no remapping, no delays */
  tty.c_oflag = 0;

  /* Set the local modes: no signaling chars, no echo, no canoncial processing */
  tty.c_lflag = 0;

  /* Read returns either when at least one byte of data is available, or when the timer expires. */
  tty.c_cc[VMIN]  = 0; // read doesn't block
  tty.c_cc[VTIME] = 5; // 0.5 seconds read timeout

  /* Set the parameters associated with the file descriptor */
  if (tcsetattr (fd, TCSANOW, &tty) != 0)
  {
      JERRYX_ERROR_MSG ("Error from tcsetattr: %d\n", errno);
      return false;
  }

  return true;
} /* jerryx_debugger_serial_configure_attributes */

/**
 * Close a serial connection.
 */
static void
jerryx_debugger_serial_close (jerry_debugger_transport_header_t *header_p) /**< serial implementation */
{
  JERRYX_ASSERT (!jerry_debugger_transport_is_connected ());

  jerryx_debugger_transport_serial_t *serial_p = (jerryx_debugger_transport_serial_t *) header_p;

  JERRYX_DEBUG_MSG ("Serial connection closed.\n");

  jerryx_debugger_serial_close_fd (serial_p->fd);

  jerry_heap_free ((void *) header_p, sizeof (jerryx_debugger_transport_serial_t));
} /* jerryx_debugger_serial_close */

/**
 * Send data over a serial connection.
 *
 * @return true - if the data has been sent successfully
 *         false - otherwise
 */
static bool
jerryx_debugger_serial_send (jerry_debugger_transport_header_t *header_p, /**< serial implementation */
                             uint8_t *message_p, /**< message to be sent */
                             size_t message_length) /**< message length in bytes */
{
  JERRYX_ASSERT (jerry_debugger_transport_is_connected ());

  jerryx_debugger_transport_serial_t *serial_p = (jerryx_debugger_transport_serial_t *) header_p;

  do
  {
    ssize_t sent_bytes = write (serial_p->fd, message_p, message_length);

    if (sent_bytes < 0)
    {
      if (errno == EWOULDBLOCK)
      {
        continue;
      }

      JERRYX_ERROR_MSG ("Error: write to file descriptor: %d\n", errno);
      jerry_debugger_transport_close ();
      return false;
    }

    message_p += sent_bytes;
    message_length -= (size_t) sent_bytes;
  }
  while (message_length > 0);

  return true;
} /* jerryx_debugger_serial_send */

/**
 * Receive data from a serial connection.
 */
static bool
jerryx_debugger_serial_receive (jerry_debugger_transport_header_t *header_p, /**< serial implementation */
                                jerry_debugger_transport_receive_context_t *receive_context_p) /**< receive context */
{
  jerryx_debugger_transport_serial_t *serial_p = (jerryx_debugger_transport_serial_t *) header_p;

  uint8_t *buffer_p = receive_context_p->buffer_p + receive_context_p->received_length;
  size_t buffer_size = JERRY_DEBUGGER_TRANSPORT_MAX_BUFFER_SIZE - receive_context_p->received_length;

  ssize_t length = read (serial_p->fd, buffer_p, buffer_size);

  if (length <= 0)
  {
    if (errno != EWOULDBLOCK || length == 0)
    {
      JERRYX_ERROR_MSG ("Error: read from file descriptor: %d\n", errno);
      jerry_debugger_transport_close ();
      return false;
    }
    length = 0;
  }

  receive_context_p->received_length += (size_t) length;

  if (receive_context_p->received_length > 0)
  {
    receive_context_p->message_p = receive_context_p->buffer_p;
    receive_context_p->message_length = receive_context_p->received_length;
  }

  return true;
} /* jerryx_debugger_serial_receive */

/**
 * Create a serial connection.
 *
 * @return true if successful,
 *         false otherwise
 */
bool
jerryx_debugger_serial_create (const char *pathname) /**< specify the file */
{

  int fd = open (pathname, O_RDWR | O_NOCTTY | O_SYNC);

  if (fd < 0)
  {
    JERRYX_ERROR_MSG ("Error %d opening %s: %s", errno, pathname, strerror (errno));
    return false;
  }

  if (!jerryx_debugger_serial_configure_attributes (fd, B115200, 0))
  {
    jerryx_debugger_serial_close_fd (fd);
    return false;
  }

  JERRYX_DEBUG_MSG ("Waiting for client connection\n");

  size_t size = sizeof (jerryx_debugger_transport_serial_t);

  jerry_debugger_transport_header_t *header_p;
  header_p = (jerry_debugger_transport_header_t *) jerry_heap_alloc (size);

  if (!header_p)
  {
    JERRYX_DEBUG_MSG ("error?\n");
    jerryx_debugger_serial_close_fd (fd);
    return false;
  }

  header_p->close = jerryx_debugger_serial_close;
  header_p->send = jerryx_debugger_serial_send;
  header_p->receive = jerryx_debugger_serial_receive;

  ((jerryx_debugger_transport_serial_t *) header_p)->fd = fd;

  jerry_debugger_transport_add (header_p,
                                0,
                                JERRY_DEBUGGER_TRANSPORT_MAX_BUFFER_SIZE,
                                0,
                                JERRY_DEBUGGER_TRANSPORT_MAX_BUFFER_SIZE);

  return true;
} /* jerryx_debugger_serial_create */


#endif /* JERRY_DEBUGGER */
