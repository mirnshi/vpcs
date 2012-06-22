/*
 * Copyright (c) 2007-2011, Paul Meng (mirnshi@gmail.com)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met:
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE 
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF 
 * THE POSSIBILITY OF SUCH DAMAGE.
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "packets.h"
#include "vpcs.h"
#include "utils.h"
#include "dns.h"

extern int ctrl_c;

static int fmtstring(const char *name, char *buf);
static int dnsrequest(const char *name, char *data, int *namelen);
static int dnsparse(struct packet *m, const char *data, int dlen, u_int *ip);

int hostresolv(pcs *pc, const char *name, u_int *ip)
{
	sesscb cb;
	struct packet *m;
	char data[512];
	int dlen;
	u_int gip;
	struct in_addr in;
	struct timeval tv;
	int ok;
	int namelen;
	
	if (pc->ip4.dns[0] == 0 && pc->ip4.dns[1] == 0) {
		printf("No DNS server was found\n");
		return 0;
	}
	dlen = dnsrequest(name, data, &namelen);
	if (dlen == 0) 
		return 0;
		
  	memset(&cb, 0, sizeof(sesscb));
  	cb.data = data;
  	cb.dsize = dlen;
  	cb.proto = IPPROTO_UDP;
  	cb.mtu = pc->ip4.mtu;
  	cb.ipid =  time(0) & 0xffff;
  	cb.ttl = TTL;
  	cb.sip = pc->ip4.ip;
  	cb.dip = pc->ip4.dns[0];
  	if (cb.dip == 0)
  		cb.dip = pc->ip4.dns[1];
  	cb.sport = (random() % (65000 - 1024)) + 1024;
	cb.dport = 53;
	memcpy(cb.smac, pc->ip4.mac, 6);
	
	if (sameNet(cb.dip, pc->ip4.ip, pc->ip4.cidr))
		gip = cb.dip;
	else {
		if (pc->ip4.gw == 0) {
			printf("No gateway found\n");
			return 0;
		} else
		
		gip = pc->ip4.gw;
	}

  	if (!arpResolve(pc, gip, cb.dmac)) {
		in.s_addr = gip;
		printf("host (%s) not reachable\n", inet_ntoa(in));
		return 0;
	}
	
	m = packet(&cb);
	if (m == NULL) {
		printf("out of memory\n");
		return 0;
	}
	gettimeofday(&(tv), (void*)0);
	enq(&pc->oq, m);

	while (!timeout(tv, 1000) && !ctrl_c) {
		delay_ms(1);
		ok = 0;		
		while ((m = deq(&pc->iq)) != NULL && !ok) {
			ok = dnsparse(m, data + sizeof(dnshdr), namelen, ip);
			free(m);
		}
		if (ok)
			return 1;
	}
	return 0;
}

static int fmtstring(const char *name, char *buf)
{
	char *p, *s, *r;
	
	if (name == NULL || name[0] == '.' || strstr(name, "..") || 
	    !strchr(name, '.') || strlen(name) > MAX_DNS_NAME)
		return 0;
	
	memset(buf, 0, MAX_DNS_NAME);
	strcpy(buf + 1, name);
	s = buf;
	r = buf + 1;
	while (1) {
		p = strchr(r, '.');
		if (p) {
			*p = '\0';
			*s = strlen(r);
			s = p;
			r = p + 1;
		} else {
			if (s == buf)
				return 0;
			*s = strlen(r);
			break;		
		}
	}
	/* prefix and '\0' at end of the string */
        return strlen(name) + 2;
}

static int dnsrequest(const char *name, char *data, int *namelen)
{
	u_char buf[256];	
	dnshdr dh;
	int dlen = sizeof(dnshdr);
	int i;
	
	memset(&dh, 0, sizeof(dnshdr));
	dh.id = DNS_MAGIC;
	dh.flags = 0x0001; /* QR|OC|AA|TC|RD -  RA|Z|RCODE  */
	dh.query = htons(0x0001); /* one query */
	  	
  	memcpy(data, (void *)&dh, sizeof(dnshdr));
  	
  	/* query name */
  	memset(buf, 0, sizeof(buf));
  	i = fmtstring(name, (char *)buf);
  	if (i == 0)
  		return 0;
  	*namelen = i;
  	memcpy(data + dlen, buf, i);
  	dlen += i;
  	
  	/* A record */
  	data[dlen++] = 0x00;
  	data[dlen++] = 0x01;
  	/* IN class */
  	data[dlen++] = 0x00;
  	data[dlen++] = 0x01;
  	
	return dlen;
}

/* very simple DNS answer parser 
 * only search A record if exist, get IP address 
 * return 1 if host name was resolved.
 */
static int dnsparse(struct packet *m, const char *data, int dlen, u_int *cip)
{
	ethdr *eh;
	iphdr *ip;
	udpiphdr *ui;
	u_char *p;

	dnshdr *dh;
	u_short *sp;
	int rlen;
	int iplen;

	eh = (ethdr *)(m->data);
	ip = (iphdr *)(eh + 1);
	ui = (udpiphdr *)ip;
	iplen = ntohs(ip->len);

	dh = (dnshdr *)(ui + 1);
	if (dh->id != 0x424c)
		return 0;

	/* invalid name or answer */
	if ((dh->flags & 0x8081) != 0x8081)
		return 0;
		
	if (dh->query == 0 || dh->answer == 0)
		return 0;	
		
	p = (u_char *)(dh + 1);

	/* not my query */
	if (memcmp(p, data, dlen))
		return 0;

	/* skip type and class */
	p += dlen + 4;
	
	/* skip offset pointer, 
	 * normal is 0xc00c, 11 00000000001100, 11-pointer, 0c-offset from dnshdr 
	*/
	p += 2;
	while (p - (u_char *)ip < iplen) {
		sp = (u_short *)p;
		/* A record */
		if (*sp == 0x0100 && *(sp + 1) == 0x0100) {
			p += 2 + 2 + 4;
			sp = (u_short *)p;
			if (*sp == 0x0400) {
				*cip = ((u_int *)(p + 2))[0];
				return 1;
			}
		} else {
			/* skip type2, class2, ttl4, rlen2 */
			p += 2 + 2 + 4;
			sp = (u_short *)p;
			rlen = ntohs(*sp);
			p += rlen + 2;
			/* skip pointer */
			p += 2;
		}
	}
	return 0;
}
/* end of file */
