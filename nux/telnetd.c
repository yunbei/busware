/*****************************************************************************
Copyright (C) 2011  busware

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*************************************************************************/

/* Scheduler includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "arch/cc.h"

#include <string.h>

#include "lwip/debug.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "utils/cmdline.h"
#include "utils/vstdlib.h"
#include "console.h"


#ifdef LWIP_DEBUG
#define LWIP_DEBUGAPPS LWIPDebug
#else
#define LWIP_DEBUGAPPS while(0)((int (*)(char *, ...))0)
#endif

extern const portCHAR * const prompt;
extern const portCHAR * const welcome;

const portCHAR * const UNKNOWN_COMMAND = "Unknown command\n";
const portCHAR * const TOO_MANY_ARGS ="Too many arguments for command processor!\n";

enum telnet_states {
  ES_NONE = 0,
  ES_ACCEPTED,
  ES_RECEIVED,
  ES_CLOSING
};

struct telnet_state {
  u8_t state;
  char line[TELNETD_CONF_LINELEN];
};

void print_tcp(struct console_state *hs,struct tcp_pcb *pcb) {
	int i;
//TODO: check free send buffer	
	for(i=0;i<=hs->line;i++) {
		tcp_write(pcb, hs->lines[i], strlen(hs->lines[i]), 1);
		vPortFree(hs->lines[i]);
	}
	hs->line=-1;
}


void telnet_error(void *arg, err_t err) {
  struct telnet_state *es;

  LWIP_UNUSED_ARG(err);

  es = (struct telnet_state *)arg;
  if (es != NULL) {
    vPortFree(es);
  }
}

void telnet_close(struct tcp_pcb *tpcb, struct telnet_state *es) {
	tcp_arg(tpcb, NULL);
	tcp_sent(tpcb, NULL);
	tcp_recv(tpcb, NULL);
	tcp_err(tpcb, NULL);
	tcp_poll(tpcb, NULL, 0);

	if (es != NULL) {
		vPortFree(es);
	}

	tcp_close(tpcb);
}


err_t telnet_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
	char *data;
	extern struct console_state *cmd_out;
	int len,i,cmd_status;
 	struct telnet_state *es;

		
  	LWIP_ASSERT("arg != NULL",arg != NULL);
  	es = (struct telnet_state *)arg;

	if (p == NULL)   {
		err = ERR_OK;
	} else if(err != ERR_OK)  {
	/* cleanup, for unkown reason */
		if (p != NULL) {
			pbuf_free(p);
		}
		err = err;
	} else if(es->state == ES_ACCEPTED) {
		tcp_recved(tpcb, p->tot_len);

		data = (char *)p->payload; // the first byte is a control byte for telnet sessions, we ignore it for now

		es->state = ES_RECEIVED;
		es->line[0] = 0;

		err = tcp_write(tpcb, prompt, ustrlen(prompt), 0);
		if(err == ERR_OK) {
			tcp_output(tpcb);
		}
		if (p != NULL) {
			pbuf_free(p);
		}
	} else  if (es->state == ES_RECEIVED) {
		tcp_recved(tpcb, p->tot_len);

		data = (char *)p->payload;
		len=ustrlen(es->line);

		for(i=0; i < p->len; i++) {
			es->line[len+i] = *data;
			data++;
		}
		es->line[len+p->len] = (char)0;
		
		pbuf_free(p);
		if(ustrstr(es->line,"\n") == NULL) { // not finished command line
			return ERR_OK;
		}
		len=ustrlen(es->line);
		es->line[len-2] = (char)0; // remove cr+lf

		cmd_out = (struct console_state *)pvPortMalloc(sizeof(struct console_state));
		cmd_out->line=-1;
        //
        // Pass the line from the user to the command processor.  It will be
        // parsed and valid commands executed.
        //
        cmd_status = CmdLineProcess(es->line);

		print_tcp(cmd_out,tpcb);
		vPortFree(cmd_out);

        if(cmd_status == CMDLINE_BAD_CMD)  {
			err = tcp_write(tpcb, UNKNOWN_COMMAND, ustrlen(UNKNOWN_COMMAND), 0);
		} else if(cmd_status == CMDLINE_TOO_MANY_ARGS) {
			err = tcp_write(tpcb, TOO_MANY_ARGS, ustrlen(TOO_MANY_ARGS), 0);
        }


		err = tcp_write(tpcb, prompt, ustrlen(prompt), 0);

		if(err == ERR_OK) {
			tcp_output(tpcb);
		}
		es->line[0] = (char)0;
		
		if (cmd_status == CMDLINE_QUIT) {
			telnet_close(tpcb,es);
		}
	}
	return err;
}

err_t telnet_accept(void *arg, struct tcp_pcb *tpcb, err_t err) {
    struct telnet_state *es;
	
	LWIP_UNUSED_ARG(arg);

/* commonly observed practive to call tcp_setprio(), why? */
	tcp_setprio(tpcb, TCP_PRIO_MIN);
	
	es = (struct telnet_state *)pvPortMalloc(sizeof(struct telnet_state));
  	if (es != NULL)   {
		es->state = ES_ACCEPTED;
		/* pass newly allocated es to our callbacks */
	    tcp_arg(tpcb, es);
		tcp_err(tpcb, telnet_error);
		tcp_recv(tpcb, telnet_recv);

		err = tcp_write(tpcb, welcome, ustrlen(welcome), 0);
		if(err == ERR_OK) {
			tcp_output(tpcb);
		}
	} else {
		err = ERR_MEM;
	}
	
	return err;  
}


void telnetd_init(void) {
	struct tcp_pcb *pcb;
	
	pcb = tcp_new();
	if (pcb != NULL)   {
		err_t err;

		err = tcp_bind(pcb, IP_ADDR_ANY, 23);
		if (err == ERR_OK)   {
			pcb = tcp_listen(pcb);
			tcp_accept(pcb, telnet_accept);
		}
	}
}