/**************************************************************************
 *
 * Copyright (C) 2015 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
#include <stdio.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>

#include "util.h"
#include "util/u_memory.h"
#include "vtest.h"
#include "vtest_protocol.h"



struct vtest_program
{
   const char *socket_name;
   int socket;
   const char *read_file;
   int out_fd;
   int in_fd;

   bool do_fork;
   bool loop;
};

struct vtest_program prog = {
   .socket_name = VTEST_DEFAULT_SOCKET_NAME,
   .socket = -1,

   .read_file = NULL,

   .in_fd = -1,
   .out_fd = -1,
   .do_fork = true,
   .loop = true,
};

static void vtest_main_parse_args(int argc, char **argv);
static void vtest_main_set_signal(void);
static void vtest_main_open_read_file(void);
static void vtest_main_open_socket(void);
static void vtest_main_run_renderer(int in_fd, int out_fd);
static void vtest_main_wait_for_socket_accept(void);
static void vtest_main_tidy_fds(void);
static void vtest_main_close_socket(void);


int main(int argc, char **argv)
{
#ifdef __AFL_LOOP
while (__AFL_LOOP(1000)) {
#endif

   vtest_main_parse_args(argc, argv);

   if (prog.read_file != NULL) {
      vtest_main_open_read_file();
      goto start;
   }

   if (prog.do_fork) {
      vtest_main_set_signal();
   }

   vtest_main_open_socket();
restart:
   vtest_main_wait_for_socket_accept();

start:
   if (prog.do_fork) {
      /* fork a renderer process */
      if (fork() == 0) {
         vtest_main_run_renderer(prog.in_fd, prog.out_fd);
         exit(0);
      }
   } else {
      vtest_main_run_renderer(prog.in_fd, prog.out_fd);
   }

   vtest_main_tidy_fds();

   if (prog.loop) {
      goto restart;
   }

   vtest_main_close_socket();

#ifdef __AFL_LOOP
}
#endif
}

static void vtest_main_parse_args(int argc, char **argv)
{
   if (argc > 1) {
      if (!strcmp(argv[1], "--no-loop-or-fork")) {
         prog.do_fork = false;
         prog.loop = false;
      } else if (!strcmp(argv[1], "--no-fork")) {
         prog.do_fork = false;
      } else {
         prog.read_file = argv[1];
         prog.loop = false;
         prog.do_fork = false;
      }
   }
}

static void vtest_main_set_signal(void)
{
   struct sigaction sa;
   int ret;

   sa.sa_handler = SIG_IGN;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = 0;

   ret = sigaction(SIGCHLD, &sa, 0);
   if (ret == -1) {
      perror(NULL);
      exit(1);
   }
}

static void vtest_main_open_read_file(void)
{
   int ret;

   ret = open(prog.read_file, O_RDONLY);
   if (ret == -1) {
      perror(NULL);
      exit(1);
   }
   prog.in_fd = ret;

   ret = open("/dev/null", O_WRONLY);
   if (ret == -1) {
      perror(NULL);
      exit(1);
   }
   prog.out_fd = ret;
}

static void vtest_main_open_socket(void)
{
   struct sockaddr_un un;

   prog.socket = socket(PF_UNIX, SOCK_STREAM, 0);
   if (prog.socket < 0) {
      goto err;
   }

   memset(&un, 0, sizeof(un));
   un.sun_family = AF_UNIX;

   snprintf(un.sun_path, sizeof(un.sun_path), "%s", prog.socket_name);

   unlink(un.sun_path);

   if (bind(prog.socket, (struct sockaddr *)&un, sizeof(un)) < 0) {
      goto err;
   }

   if (listen(prog.socket, 1) < 0){
      goto err;
   }

   return;

err:
   perror("Failed to setup socket.");
   exit(1);
}

static void vtest_main_wait_for_socket_accept(void)
{
   fd_set read_fds;
   int new_fd;
   int ret;
   FD_ZERO(&read_fds);
   FD_SET(prog.socket, &read_fds);

   ret = select(prog.socket + 1, &read_fds, NULL, NULL, NULL);
   if (ret < 0) {
      perror("Failed to select on socket!");
      exit(1);
   }

   if (!FD_ISSET(prog.socket, &read_fds)) {
      perror("Odd state in fd_set.");
      exit(1);
   }

   new_fd = accept(prog.socket, NULL, NULL);
   if (new_fd < 0) {
      perror("Failed to accept socket.");
      exit(1);
   }

   prog.in_fd = new_fd;
   prog.out_fd = new_fd;
}

typedef int (*vtest_cmd_fptr_t)(uint32_t);

static const vtest_cmd_fptr_t vtest_commands[] = {
   NULL /* CMD ids starts at 1 */,
   vtest_send_caps,
   vtest_create_resource,
   vtest_resource_unref,
   vtest_transfer_get,
   vtest_transfer_put,
   vtest_submit_cmd,
   vtest_resource_busy_wait,
   NULL, /* vtest_create_renderer is a specific case */
   vtest_send_caps2,
   vtest_ping_protocol_version,
   vtest_protocol_version,
   vtest_create_resource2,
   vtest_transfer_get2,
   vtest_transfer_put2,
};

static void vtest_main_run_renderer(int in_fd, int out_fd)
{
   int err, ret;
   uint32_t header[VTEST_HDR_SIZE];
   int initialized = 0;

   do {
      ret = vtest_wait_for_fd_read(in_fd);
      if (ret < 0) {
         err = 1;
         break;
      }

      ret = vtest_block_read(in_fd, &header, sizeof(header));
      if (ret < 0 || (size_t)ret < sizeof(header)) {
         err = 2;
         break;
      }

      if (!initialized) {
         /* The first command MUST be VCMD_CREATE_RENDERER */
         if (header[1] != VCMD_CREATE_RENDERER) {
            err = 3;
            break;
         }

         ret = vtest_create_renderer(in_fd, out_fd, header[0]);
         initialized = 1;
         printf("%s: vtest initialized.\n", __func__);
         vtest_poll();
         continue;
      }

      vtest_poll();
      if (header[1] <= 0 || header[1] >= ARRAY_SIZE(vtest_commands)) {
         err = 4;
         break;
      }

      if (vtest_commands[header[1]] == NULL) {
         err = 5;
         break;
      }

      ret = vtest_commands[header[1]](header[0]);
      if (ret < 0) {
         err = 6;
         break;
      }
   } while (1);

   fprintf(stderr, "socket failed (%d) - closing renderer\n", err);

   vtest_destroy_renderer();
}

static void vtest_main_tidy_fds(void)
{
   // out_fd will be closed by the in_fd clause if they are the same.
   if (prog.out_fd == prog.in_fd) {
      prog.out_fd = -1;
   }

   if (prog.in_fd != -1) {
      close(prog.in_fd);
      prog.in_fd = -1;
   }

   if (prog.out_fd != -1) {
      close(prog.out_fd);
      prog.out_fd = -1;
   }
}

static void vtest_main_close_socket(void)
{
   if (prog.socket != -1) {
      close(prog.socket);
      prog.socket = -1;
   }
}
