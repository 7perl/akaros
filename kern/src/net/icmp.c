// INFERNO
#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>
#include <ip.h>

#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>
#include <ip.h>

typedef struct Icmp {
	uint8_t vihl;				/* Version and header length */
	uint8_t tos;				/* Type of service */
	uint8_t length[2];			/* packet length */
	uint8_t id[2];				/* Identification */
	uint8_t frag[2];			/* Fragment information */
	uint8_t ttl;				/* Time to live */
	uint8_t proto;				/* Protocol */
	uint8_t ipcksum[2];			/* Header checksum */
	uint8_t src[4];				/* Ip source */
	uint8_t dst[4];				/* Ip destination */
	uint8_t type;
	uint8_t code;
	uint8_t cksum[2];
	uint8_t icmpid[2];
	uint8_t seq[2];
	uint8_t data[1];
} Icmp;

enum {							/* Packet Types */
	EchoReply = 0,
	Unreachable = 3,
	SrcQuench = 4,
	Redirect = 5,
	EchoRequest = 8,
	TimeExceed = 11,
	InParmProblem = 12,
	Timestamp = 13,
	TimestampReply = 14,
	InfoRequest = 15,
	InfoReply = 16,
	AddrMaskRequest = 17,
	AddrMaskReply = 18,

	Maxtype = 18,
};

enum {
	MinAdvise = 24,				/* minimum needed for us to advise another protocol */
};

char *icmpnames[Maxtype + 1] = {
	[EchoReply] "EchoReply",
	[Unreachable] "Unreachable",
	[SrcQuench] "SrcQuench",
	[Redirect] "Redirect",
	[EchoRequest] "EchoRequest",
	[TimeExceed] "TimeExceed",
	[InParmProblem] "InParmProblem",
	[Timestamp] "Timestamp",
	[TimestampReply] "TimestampReply",
	[InfoRequest] "InfoRequest",
	[InfoReply] "InfoReply",
	[AddrMaskRequest] "AddrMaskRequest",
	[AddrMaskReply] "AddrMaskReply  ",
};

enum {
	IP_ICMPPROTO = 1,
	ICMP_IPSIZE = 20,
	ICMP_HDRSIZE = 8,
};

enum {
	InMsgs,
	InErrors,
	OutMsgs,
	CsumErrs,
	LenErrs,
	HlenErrs,

	Nstats,
};

static char *statnames[Nstats] = {
	[InMsgs] "InMsgs",
	[InErrors] "InErrors",
	[OutMsgs] "OutMsgs",
	[CsumErrs] "CsumErrs",
	[LenErrs] "LenErrs",
	[HlenErrs] "HlenErrs",
};

typedef struct Icmppriv Icmppriv;
struct Icmppriv {
	uint32_t stats[Nstats];

	/* message counts */
	uint32_t in[Maxtype + 1];
	uint32_t out[Maxtype + 1];
};

static void icmpkick(void *x, struct block *);

static void icmpcreate(struct conv *c)
{
	c->rq = qopen(64 * 1024, Qmsg, 0, c);
	c->wq = qbypass(icmpkick, c);
}

extern char *icmpconnect(struct conv *c, char **argv, int argc)
{
	char *e;

	e = Fsstdconnect(c, argv, argc);
	if (e != NULL)
		return e;
	Fsconnected(c, e);

	return NULL;
}

extern int icmpstate(struct conv *c, char *state, int n)
{
	return snprintf(state, n, "%s qin %d qout %d\n",
					"Datagram",
					c->rq ? qlen(c->rq) : 0, c->wq ? qlen(c->wq) : 0);
}

extern char *icmpannounce(struct conv *c, char **argv, int argc)
{
	char *e;

	e = Fsstdannounce(c, argv, argc);
	if (e != NULL)
		return e;
	Fsconnected(c, NULL);

	return NULL;
}

extern void icmpclose(struct conv *c)
{
	qclose(c->rq);
	qclose(c->wq);
	ipmove(c->laddr, IPnoaddr);
	ipmove(c->raddr, IPnoaddr);
	c->lport = 0;
}

static void icmpkick(void *x, struct block *bp)
{
	struct conv *c = x;
	Icmp *p;
	Icmppriv *ipriv;

	if (bp == NULL)
		return;

	bp = pullupblock(bp, ICMP_IPSIZE + ICMP_HDRSIZE);
	if (bp == 0)
		return;
	p = (Icmp *) (bp->rp);
	p->vihl = IP_VER4;
	ipriv = c->p->priv;
	if (p->type <= Maxtype)
		ipriv->out[p->type]++;

	v6tov4(p->dst, c->raddr);
	v6tov4(p->src, c->laddr);
	p->proto = IP_ICMPPROTO;
	hnputs(p->icmpid, c->lport);
	memset(p->cksum, 0, sizeof(p->cksum));
	hnputs(p->cksum, ptclcsum(bp, ICMP_IPSIZE, blocklen(bp) - ICMP_IPSIZE));
	ipriv->stats[OutMsgs]++;
	ipoput4(c->p->f, bp, 0, c->ttl, c->tos, NULL);
}

extern void icmpttlexceeded(struct Fs *f, uint8_t * ia, struct block *bp)
{
	struct block *nbp;
	Icmp *p, *np;

	p = (Icmp *) bp->rp;

	netlog(f, Logicmp, "sending icmpttlexceeded -> %V\n", p->src);
	nbp = allocb(ICMP_IPSIZE + ICMP_HDRSIZE + ICMP_IPSIZE + 8);
	nbp->wp += ICMP_IPSIZE + ICMP_HDRSIZE + ICMP_IPSIZE + 8;
	np = (Icmp *) nbp->rp;
	np->vihl = IP_VER4;
	memmove(np->dst, p->src, sizeof(np->dst));
	v6tov4(np->src, ia);
	memmove(np->data, bp->rp, ICMP_IPSIZE + 8);
	np->type = TimeExceed;
	np->code = 0;
	np->proto = IP_ICMPPROTO;
	hnputs(np->icmpid, 0);
	hnputs(np->seq, 0);
	memset(np->cksum, 0, sizeof(np->cksum));
	hnputs(np->cksum, ptclcsum(nbp, ICMP_IPSIZE, blocklen(nbp) - ICMP_IPSIZE));
	ipoput4(f, nbp, 0, MAXTTL, DFLTTOS, NULL);

}

static void icmpunreachable(struct Fs *f, struct block *bp, int code, int seq)
{
	struct block *nbp;
	Icmp *p, *np;
	int i;
	uint8_t addr[IPaddrlen];

	p = (Icmp *) bp->rp;

	/* only do this for unicast sources and destinations */
	v4tov6(addr, p->dst);
	i = ipforme(f, addr);
	if ((i & Runi) == 0)
		return;
	v4tov6(addr, p->src);
	i = ipforme(f, addr);
	if (i != 0 && (i & Runi) == 0)
		return;

	netlog(f, Logicmp, "sending icmpnoconv -> %V\n", p->src);
	nbp = allocb(ICMP_IPSIZE + ICMP_HDRSIZE + ICMP_IPSIZE + 8);
	nbp->wp += ICMP_IPSIZE + ICMP_HDRSIZE + ICMP_IPSIZE + 8;
	np = (Icmp *) nbp->rp;
	np->vihl = IP_VER4;
	memmove(np->dst, p->src, sizeof(np->dst));
	memmove(np->src, p->dst, sizeof(np->src));
	memmove(np->data, bp->rp, ICMP_IPSIZE + 8);
	np->type = Unreachable;
	np->code = code;
	np->proto = IP_ICMPPROTO;
	hnputs(np->icmpid, 0);
	hnputs(np->seq, seq);
	memset(np->cksum, 0, sizeof(np->cksum));
	hnputs(np->cksum, ptclcsum(nbp, ICMP_IPSIZE, blocklen(nbp) - ICMP_IPSIZE));
	ipoput4(f, nbp, 0, MAXTTL, DFLTTOS, NULL);
}

extern void icmpnoconv(struct Fs *f, struct block *bp)
{
	icmpunreachable(f, bp, 3, 0);
}

extern void icmpcantfrag(struct Fs *f, struct block *bp, int mtu)
{
	icmpunreachable(f, bp, 4, mtu);
}

static void goticmpkt(struct Proto *icmp, struct block *bp)
{
	struct conv **c, *s;
	Icmp *p;
	uint8_t dst[IPaddrlen];
	uint16_t recid;

	p = (Icmp *) bp->rp;
	v4tov6(dst, p->src);
	recid = nhgets(p->icmpid);

	for (c = icmp->conv; *c; c++) {
		s = *c;
		if (s->lport == recid)
			if (ipcmp(s->raddr, dst) == 0) {
				bp = concatblock(bp);
				if (bp != NULL)
					qpass(s->rq, bp);
				return;
			}
	}
	freeblist(bp);
}

static struct block *mkechoreply(struct block *bp)
{
	Icmp *q;
	uint8_t ip[4];

	/* we're repurposing bp to send it back out.  we need to remove any inbound
	 * checksum flags (which were saying the HW did the checksum) */
	bp->flag &= ~BCKSUM_FLAGS;
	q = (Icmp *) bp->rp;
	q->vihl = IP_VER4;
	memmove(ip, q->src, sizeof(q->dst));
	memmove(q->src, q->dst, sizeof(q->src));
	memmove(q->dst, ip, sizeof(q->dst));
	q->type = EchoReply;
	memset(q->cksum, 0, sizeof(q->cksum));
	hnputs(q->cksum, ptclcsum(bp, ICMP_IPSIZE, blocklen(bp) - ICMP_IPSIZE));

	return bp;
}

static char *unreachcode[] = {
	[0] "net unreachable",
	[1] "host unreachable",
	[2] "protocol unreachable",
	[3] "port unreachable",
	[4] "fragmentation needed and DF set",
	[5] "source route failed",
};

static void icmpiput(struct Proto *icmp, struct Ipifc *unused, struct block *bp)
{
	int n, iplen;
	Icmp *p;
	struct block *r;
	struct Proto *pr;
	char *msg;
	char m2[128];
	Icmppriv *ipriv;

	ipriv = icmp->priv;

	ipriv->stats[InMsgs]++;

	p = (Icmp *) bp->rp;
	netlog(icmp->f, Logicmp, "icmpiput %d %d\n", p->type, p->code);
	n = blocklen(bp);
	if (n < ICMP_IPSIZE + ICMP_HDRSIZE) {
		ipriv->stats[InErrors]++;
		ipriv->stats[HlenErrs]++;
		netlog(icmp->f, Logicmp, "icmp hlen %d\n", n);
		goto raise;
	}
	iplen = nhgets(p->length);
	if (iplen > n || (iplen % 1)) {
		ipriv->stats[LenErrs]++;
		ipriv->stats[InErrors]++;
		netlog(icmp->f, Logicmp, "icmp length %d\n", iplen);
		goto raise;
	}
	if (ptclcsum(bp, ICMP_IPSIZE, iplen - ICMP_IPSIZE)) {
		ipriv->stats[InErrors]++;
		ipriv->stats[CsumErrs]++;
		netlog(icmp->f, Logicmp, "icmp checksum error\n");
		goto raise;
	}
	if (p->type <= Maxtype)
		ipriv->in[p->type]++;

	switch (p->type) {
		case EchoRequest:
			if (iplen < n)
				bp = trimblock(bp, 0, iplen);
			r = mkechoreply(bp);
			ipriv->out[EchoReply]++;
			ipoput4(icmp->f, r, 0, MAXTTL, DFLTTOS, NULL);
			break;
		case Unreachable:
			if (p->code > 5)
				msg = unreachcode[1];
			else
				msg = unreachcode[p->code];

			bp->rp += ICMP_IPSIZE + ICMP_HDRSIZE;
			if (blocklen(bp) < MinAdvise) {
				ipriv->stats[LenErrs]++;
				goto raise;
			}
			p = (Icmp *) bp->rp;
			pr = Fsrcvpcolx(icmp->f, p->proto);
			if (pr != NULL && pr->advise != NULL) {
				(*pr->advise) (pr, bp, msg);
				return;
			}

			bp->rp -= ICMP_IPSIZE + ICMP_HDRSIZE;
			goticmpkt(icmp, bp);
			break;
		case TimeExceed:
			if (p->code == 0) {
				snprintf(m2, sizeof(m2), "ttl exceeded at %V", p->src);

				bp->rp += ICMP_IPSIZE + ICMP_HDRSIZE;
				if (blocklen(bp) < MinAdvise) {
					ipriv->stats[LenErrs]++;
					goto raise;
				}
				p = (Icmp *) bp->rp;
				pr = Fsrcvpcolx(icmp->f, p->proto);
				if (pr != NULL && pr->advise != NULL) {
					(*pr->advise) (pr, bp, m2);
					return;
				}
				bp->rp -= ICMP_IPSIZE + ICMP_HDRSIZE;
			}

			goticmpkt(icmp, bp);
			break;
		default:
			goticmpkt(icmp, bp);
			break;
	}
	return;

raise:
	freeblist(bp);
}

void icmpadvise(struct Proto *icmp, struct block *bp, char *msg)
{
	struct conv **c, *s;
	Icmp *p;
	uint8_t dst[IPaddrlen];
	uint16_t recid;

	p = (Icmp *) bp->rp;
	v4tov6(dst, p->dst);
	recid = nhgets(p->icmpid);

	for (c = icmp->conv; *c; c++) {
		s = *c;
		if (s->lport == recid)
			if (ipcmp(s->raddr, dst) == 0) {
				qhangup(s->rq, msg);
				qhangup(s->wq, msg);
				break;
			}
	}
	freeblist(bp);
}

int icmpstats(struct Proto *icmp, char *buf, int len)
{
	Icmppriv *priv;
	char *p, *e;
	int i;

	priv = icmp->priv;
	p = buf;
	e = p + len;
	for (i = 0; i < Nstats; i++)
		p = seprintf(p, e, "%s: %u\n", statnames[i], priv->stats[i]);
	for (i = 0; i <= Maxtype; i++) {
		if (icmpnames[i])
			p = seprintf(p, e, "%s: %u %u\n", icmpnames[i], priv->in[i],
						 priv->out[i]);
		else
			p = seprintf(p, e, "%d: %u %u\n", i, priv->in[i], priv->out[i]);
	}
	return p - buf;
}

void icmpinit(struct Fs *fs)
{
	struct Proto *icmp;

	icmp = kzmalloc(sizeof(struct Proto), 0);
	icmp->priv = kzmalloc(sizeof(Icmppriv), 0);
	icmp->name = "icmp";
	icmp->connect = icmpconnect;
	icmp->announce = icmpannounce;
	icmp->state = icmpstate;
	icmp->create = icmpcreate;
	icmp->close = icmpclose;
	icmp->rcv = icmpiput;
	icmp->stats = icmpstats;
	icmp->ctl = NULL;
	icmp->advise = icmpadvise;
	icmp->gc = NULL;
	icmp->ipproto = IP_ICMPPROTO;
	icmp->nc = 128;
	icmp->ptclsize = 0;

	Fsproto(fs, icmp);
}
