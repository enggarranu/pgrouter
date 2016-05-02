/*
  Copyright (c) 2016 James Hunt

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
 */

#include "pgrouter.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

MSG* pgr_m_new()
{
	MSG *m = calloc(1, sizeof(MSG));
	if (!m) {
		pgr_abort(ABORT_MEMFAIL);
	}
	m->free = sizeof(m->buf);
	return m;
}

void pgr_m_init(MSG *m)
{
	memset(m, 0, sizeof(MSG));
	m->free = sizeof(m->buf);
}

void pgr_m_skip(MSG *m, size_t n)
{
	assert(pgr_m_unread(m) >= n);
	m->offset += n;
}

void pgr_m_discard(MSG *m, int fd)
{
	int len;
	ssize_t n;

	if (pgr_m_u8_at(m,0) == 0) {
		len = pgr_m_u32_at(m,0);
	} else {
		len = 1 + pgr_m_u32_at(m,1);
	}

	n = pgr_m_unread(m);
	while (len > n) {
		/* there's more data on the wire.                 */
		/* drop what we've got, and then go read the rest */
		len -= n;

		m->offset = 0;
		m->free = MSG_BUFSIZ;
		n = read(fd, m->buf, m->free);
		if (n <= 0) {
			return;
		}
	}

	pgr_m_skip(m, len);
	pgr_m_flush(m);
}

void pgr_m_flush(MSG *m)
{
	if (m->offset > 0 && pgr_m_unread(m) > 0) {
		memmove(m->buf, m->buf + pgr_m_offset(m), pgr_m_unread(m));
	}
	m->free += m->offset;
	m->offset = 0;
}

int pgr_m_write(MSG *m, const void *buf, size_t len)
{
	if (len > m->free) {
		memcpy(m->buf + pgr_m_used(m), buf, m->free);
		len -= m->free;
		m->free = 0;
		return len;
	}

	memcpy(m->buf + pgr_m_used(m), buf, len);
	m->free -= len;
	return 0;


}

int pgr_m_sendn(MSG *m, int fd, size_t len)
{
	int rc;
	assert(len != 0);

	pgr_debugf("sending %d bytes to fd %d", len, fd);
	rc = pgr_sendn(fd, m->buf + m->offset, len);
	if (rc != 0) {
		return rc;
	}

	pgr_m_skip(m, len);
	return 0;
}

int pgr_m_resend(MSG *m, int fd)
{
	pgr_m_rewind(m);
	return pgr_m_send(m, fd);
}

int pgr_m_next(MSG *m, int fd)
{
	size_t n;

	while (pgr_m_unread(m) < 5) {
		n = read(fd, pgr_m_buffer(m), pgr_m_free(m));
		if (n <= 0) {
			return -1;
		}
		m->free -= n;
	}

	return 0;
}

int pgr_m_relay(MSG *m, int from, int to)
{
	size_t len = pgr_m_u32_at(m, 1) + 1;
	size_t n   = pgr_m_unread(m);

	/* is there more to this message? */
	while (len > pgr_m_unread(m)) {
		len -= pgr_m_unread(m);
		pgr_m_send(m, to);
		pgr_m_flush(m);
		pgr_m_next(m, from);
	}
	pgr_m_sendn(m, to, len);
	pgr_m_flush(m);

	return 0;
}

int pgr_m_ignore(MSG *m, int fd, const char *until)
{
	size_t n, len;
	char type;

	for (;;) {
		type = pgr_m_u8_at(m, 0);
		len  = pgr_m_u32_at(m, 1) + 1;
		n    = pgr_m_unread(m);

		while (len > n) {
			/* there's more data on the wire.                 */
			/* send what we've got, and then go read the rest */
			len -= n;

			m->offset = 0;
			m->free = MSG_BUFSIZ;
			n = read(fd, m->buf, m->free);
			if (n <= 0) {
				return -1;
			}
		}

		pgr_m_skip(m, len);
		pgr_m_flush(m);

		if (strchr(until, type) != NULL) {
			return 0;
		}

		pgr_m_next(m, fd);
	}
}

int pgr_m_iserror(MSG *m, const char *code)
{
	if (pgr_m_u8_at(m, 0) != 'E') {
		return 0;
	}

	char *e;

	e = pgr_m_str_at(m, 5);
	while (*e) {
		if (*e == 'C') {
			e++;
			return strcmp(e, code) == 0;
		}
		while (*e++)
			;
	}

	return 0;
}

void pgr_m_errorf(MSG *m, char *sev, char *code, char *msgf, ...)
{
	int len;
	va_list ap;
	char *msg;

	va_start(ap, msgf);
	vasprintf(&msg, msgf, ap);
	va_end(ap);

	len = 1+4                /* type + len            */
	    + 1+strlen(sev)+1    /* 'S' + severity + '\0' */
	    + 1+strlen(code)+1   /* 'C' + code     + '\0' */
	    + 1+strlen(msg)+1    /* 'M' + msg      + '\0' */
	    + 1;                 /* trailing '\0'         */
	len = htonl(len);
	pgr_m_write(m, "E",  1); pgr_m_write(m, &len, 4);
	pgr_m_write(m, "S",  1); pgr_m_write(m, sev,  strlen(sev)  + 1);
	pgr_m_write(m, "C",  1); pgr_m_write(m, code, strlen(code) + 1);
	pgr_m_write(m, "M",  1); pgr_m_write(m, msg,  strlen(msg)  + 1);
	pgr_m_write(m, "\0", 1);
}

static int tmpfd()
{
	FILE *f = tmpfile();
	if (!f) {
		return -1;
	}
	return fileno(f);
}

#define available(m) ((m)->fill - (m)->start)
#define u16(v) ((uint16_t)((*(v)&0xff)<<8)|*((v)+1)&0xff)
#define u32(v) ((uint16_t)((*(v)&0xff)<<24)|((*((v)+1)&0xff)<<16)|((*((v)+2)&0xff)<<8)|(*((v)+3)&0xff))

static unsigned int size(MBUF *m)
{
	if (available(m) < 5) {
		return 0;
	}

	if (m->buf[m->start + 0] == 0) {
		return u32(m->buf + m->start);
	}
	return u32(m->buf + m->start + 1) + 1;
}

static ssize_t writen(int fd, const void *buf, size_t len)
{
	ssize_t n;
	while (len > 0) {
		n = write(fd, buf, len);
		if (n <= 0) {
			return n;
		}
		len -= n;
	}
	return n;
}

/* Generate a new MBUF structure of the given size,
   allocated on the heap. The `len` argument must be
   at least 16 (octets). */
MBUF* pgr_mbuf_new(size_t len)
{
	MBUF *m = malloc(sizeof(MBUF) + len);
	if (!m) {
		pgr_abort(ABORT_MEMFAIL);
	}
	memset(m, 0, sizeof(MBUF) + len);
	/* keep track of our size */
	m->len = len;
	/* invalidate all the fds */
	m->infd = m->outfd = m->cache = -1;
	return m;
}

/* Set the input and output file descriptors to the
   passed values.  To leave existing fd untouched,
   specify the constant `MBUF_SAME_FD`.  To unset a
   descriptor, specify `MBUF_NO_FD`. */
void pgr_mbuf_setfd(MBUF *m, int in, int out)
{
	assert(m != NULL);
	assert(in == MBUF_SAME_FD || in == MBUF_NO_FD || in >= 0);
	assert(out == MBUF_SAME_FD || out == MBUF_NO_FD || out >= 0);

	if (in != MBUF_SAME_FD) {
		m->infd = in;
	}
	if (out != MBUF_SAME_FD) {
		m->outfd = out;
	}
}

/* Concatenate caller-supplied buffer contents onto
   the end of our buffer.  Doesn't support messages
   that are too big to fit in the buffer */
int pgr_mbuf_cat(MBUF *m, const void *buf, size_t len)
{
	if (len > m->len - m->fill) {
		return 1;
	}

	memcpy(m->buf + m->fill, buf, len);
	m->fill += len;
	return 0;
}

/* Fill as much of the buffer with octets read from
   the input file descriptor.  If the buffer already
   contains enough data to see the first 5 octets of
   a message, this call does nothing and returns
   immediately. */
int pgr_mbuf_recv(MBUF *m)
{
	while (available(m) < 5) {
		ssize_t n = read(m->infd, m->buf + m->fill, m->len - m->fill);
		if (n <= 0) {
			return (int)n;
		}
		m->fill += n;
	}
	return available(m);
}

/* Send the first message in the buffer to the output
   file descriptor, buffering all data sent, so that
   it can be resent later.  For very large messages,
   i.e. INSERT statements with large blobs), this may
   require reading from the input file descriptor. */
int pgr_mbuf_send(MBUF *m)
{
	int wr_ok, cache_ok;
	unsigned int len;
	ssize_t n, off;

	if (available(m) < 5) {
		return 1;
	}

	wr_ok = 1;
	len = size(m);
	while (len > available(m)) {
		if (m->cache < 0) {
			/* time to warm up the cache */
			m->cache = tmpfd();
			if (m->cache < 0) { /* still! */
				return 1;
			}

			if (m->start > 0) {
				n = writen(m->cache, m->buf, m->start);
				if (n <= 0) {
					return 1;
				}
				memmove(m->buf, m->buf + m->start, m->fill - m->start);
				m->fill -= m->start;
				m->start = 0;
			}
		}
		n = writen(m->outfd, m->buf + m->start, available(m));
		wr_ok = (n > 0);

		n = writen(m->cache, m->buf + m->start, available(m));
		if (n <= 0) {
			return 1;
		}

		len -= available(m);
		m->fill = m->start;

		n = read(m->infd, m->buf + m->fill, m->len - m->fill);
		if (n <= 0) {
			return n;
		}
		m->fill += n;
	}

	if (len > 0) {
		n = writen(m->outfd, m->buf + m->start, len);
		wr_ok = (n > 0);

		if (m->cache >= 0) {
			n = writen(m->cache, m->buf + m->start, len);
			if (n <= 0) {
				return 1;
			}

			memmove(m->buf + m->start, m->buf + m->start + len, m->fill - m->start - len);
			m->fill -= len;

		} else {
			m->start += len;
		}
	}

	return wr_ok ? 0 : 1;
}

/* Resend all buffered message for which we've
   buffered data (i.e. via pgr_mbuf_send) */
int pgr_mbuf_resend(MBUF *m)
{
	size_t until;
	ssize_t n;

	if (m->cache >= 0) {
		char *block = malloc(2048);
		off_t offset = lseek(m->cache, 0, SEEK_CUR);
		lseek(m->cache, 0, SEEK_SET);
		while (offset > 0) {
			n = read(m->cache, block, 2048);
			if (n <= 0) {
				return 1;
			}
			offset -= n;
			n = writen(m->outfd, block, n);
			if (n <= 0) {
				return 1;
			}
		}
	}

	until = m->start;
	m->start = 0; /* FIXME: naïve */

	while (until > m->start) {
		n = write(m->outfd, m->buf + m->start, until - m->start);
		if (n <= 0) {
			return 1;
		}
		m->start += n;
	}
	return 0;
}

/* Relay the first message in the buffer to the output
   file descriptor, and reposition the buffer at the
   beginning of the next message.  This may lead to an
   empty buffer. */
int pgr_mbuf_relay(MBUF *m)
{
	int len, wr_ok;
	ssize_t n, off;

	if (available(m) < 5) {
		return 1;
	}

	wr_ok = 1;
	len = size(m);
	while (len > available(m)) {
		off = m->start;
		while (wr_ok && off < m->fill) {
			n = write(m->outfd, m->buf + off, m->fill - off);
			wr_ok = (n > 0);
			off += n;
		}

		len -= available(m);
		m->fill = m->start;

		n = read(m->infd, m->buf + m->fill, m->len - m->fill);
		if (n <= 0) {
			return 1;
		}
		m->fill += n;
	}

	if (len > 0) {
		off = 0;
		while (wr_ok && off < len) {
			n = write(m->outfd, m->buf + m->start + off, (len - off));
			wr_ok = (n > 0);
			off += n;
		}

		memmove(m->buf + m->start, m->buf + m->start + len, m->fill - m->start - len);
		m->fill -= len;
	}
	return wr_ok ? 0 : 1;
}

/* Discard all buffered data for the current message,
   reading (and discarding) from the input descriptor
   if necessary. */
int pgr_mbuf_discard(MBUF *m)
{
	int len;
	ssize_t n;

	if (available(m) < 5) {
		return 1;
	}

	len = size(m);
	while (len > available(m)) {
		len -= available(m);
		m->fill = m->start;

		n = read(m->infd, m->buf + m->fill, m->len - m->fill);
		if (n <= 0) {
			return (int)n;
		}
		m->fill += n;
	}

	if (len > 0) {
		memmove(m->buf + m->start, m->buf + m->start + len, m->fill - m->start - len);
		m->fill -= len;
	}

	return 0;
}

char pgr_mbuf_msgtype(MBUF *m)
{
	if (available(m) == 0) {
		return -1;
	}
	if (m->buf[m->start] == 0) {
		/* untyped message */
		unsigned int len = pgr_mbuf_msglength(m);
		if (len == 4 && available(m) >= 8
		 && u16(m->buf + m->start + 4) == 1234
		 && u16(m->buf + m->start + 6) == 5679) {
			return MSG_SSLREQ;

		} else if (len == 12) {
			return MSG_CANCEL;

		} else if (len >= 5) {
			return MSG_STARTUP;
		}

		return -1;
	}
	return m->buf[m->start];
}

unsigned int pgr_mbuf_msglength(MBUF *m)
{
	/* FIXME: doesn't handle untyped messages! */
	if (available(m) < 5) {
		return 0;
	}

	if (m->buf[m->start] == 0) {
		/* untyped message */
		return u32(m->buf + m->start) - 4;
	}
	return u32(m->buf + m->start + 1) - 4;
}

void* pgr_mbuf_data(MBUF *m)
{
	if (available(m) < 5) {
		return NULL;
	}
	return m->buf + m->start + 5;
}

#ifdef PTEST
#include <strings.h>

#define so(s,x) do {\
	errno = 0; \
	if (x) { \
		fprintf(stderr, "%s ... OK\n", s); \
	} else { \
		fprintf(stderr, "%s:%d: FAIL: %s [!(%s)]\n", __FILE__, __LINE__, s, #x); \
		if (errno != 0) { \
			fprintf(stderr, "%s (errno %d)\n", strerror(errno), errno); \
		} \
		exit(1); \
	} \
} while (0)

#define is(x,n)    so(#x " should equal " #n, (x) == (n))
#define isnt(x,n)  so(#x " should not equal " #n, (x) == (n))
#define ok(x)      so(#x " should succeed", (x) == 0)
#define notok(x)   so(#x " should fail", (x) != 0)
#define null(x)    so(#x " should be NULL", (x) == NULL)
#define notnull(x) so(#x " should not be NULL", (x) != NULL)

#define writeok(f,s,n) so("writing " #n " bytes to fd `" #f "`", \
		write((f), (s), (n)) == (n))

#define diagfile(f) do {\
	off_t __o, __i; char *__s; \
	__o = lseek((f), 0, SEEK_CUR); \
	fprintf(stderr, "DIAG> fd %d is %lib long\n", (f), __o); \
	notnull(__s = malloc(__o)); \
	lseek((f), 0, SEEK_SET); \
	is(read((f), __s, __o), __o); \
	for (__i = 0; __i < __o; __i++) { \
		fprintf(stderr, "%s%02x ", __i % 16 == 0 ? "\n" : "", __s[__i] & 0xff); \
	}\
	fprintf(stderr, "\n"); \
	free(__s); \
	lseek((f), __o, SEEK_SET); \
} while (0)

#define fileok(f,x,n) do {\
	size_t __n; off_t __o; char *__s; \
	__n = (n); \
	notnull(__s = malloc(__n + 1)); \
	__o = lseek((f), 0, SEEK_CUR); \
	lseek((f), 0, SEEK_SET); \
	is(read((f), __s, __n + 1), __n); \
	ok(memcmp(__s, (x), __n)); \
	free(__s); \
	lseek((f), __o, SEEK_SET); \
} while (0)

#define m_should_be(s,m,offset,unread,used,free) do {\
	so(s ", offset should be " #offset, pgr_m_offset(m) == offset); \
	so(s ", unread should be " #unread, pgr_m_unread(m) == unread); \
	so(s ", used should be "   #used,   pgr_m_used(m) == used); \
	so(s ", free should be "   #free,   pgr_m_free(m) == free); \
} while (0)

#define msg_should_be(s,m,type,len) do {\
	so(s ", message type should be " #type, pgr_m_u8_at(m,0) == (type)); \
	so(s ", length should be "       #len,  pgr_m_u32_at(m,1) == (len)); \
} while (0)

#define msg_is(s,m,t,l) do { \
	so(s ", message type should be "   #t, pgr_mbuf_msgtype(m) == (t)); \
	so(s ", message length should be " #l, pgr_mbuf_msglength(m) == (l)); \
} while (0)

MBUF *m;
int in, out;

static void init_test()
{
	FILE *inf, *outf;

	notnull(inf = tmpfile());
	in = fileno(inf);

	notnull(outf = tmpfile());
	out = fileno(outf);

	notnull(m = pgr_mbuf_new(512));
	pgr_mbuf_setfd(m, in, out);
}

static void reset_test()
{
	char *s;

	m->fill = 0;

	ftruncate(in, 0);
	lseek(in, 0, SEEK_SET);
	writeok(in, "\0\0\0\x08\x04\xd2\x16\x2f", 8); /* SSLRequest */
	writeok(in, "\0\0\0\x09\x00\x03\x00\x00\x00", 9); /* StartupMessage */
	writeok(in, "I\0\0\0\x04", 5);
	writeok(in, "Q\0\0\0\x25"
	            "Do you know the way to San Jose?\0", 38);
	writeok(in, "L\0\0\x80\x04", 5);
	notnull(s = malloc(0x8000));
	memset(s, '.', 0x8000);
	writeok(in, s, 0x8000);
	free(s);

	writeok(in, "S\0\0\0\x4", 5);
	lseek(in, 0, SEEK_SET);

	ftruncate(out, 0);
	lseek(out, 0, SEEK_SET);
	lseek(out, 0, SEEK_SET);
}

int main(int argc, char **argv)
{
	int i;
	char *s;

	init_test();

	so("initial fill offset should be 0", m->fill == 0);
	so("discarding with an empty buffer returns an error",
			pgr_mbuf_discard(m) != 0);
	so("interrogating message type from an empty buffer returns an error",
			pgr_mbuf_msgtype(m) == -1);
	so("interrogating message length from an empty buffer returns a zero-length",
			pgr_mbuf_msglength(m) == 0);
	so("interrogating message data from an empty buffer returns NULL",
			pgr_mbuf_data(m) == NULL);

	 /********************************************************/
	/** Discard                                            **/
	reset_test();
	so("recv ok", pgr_mbuf_recv(m) > 0);
	msg_is("message #1", m, MSG_SSLREQ, 4);

	ok(pgr_mbuf_discard(m));
	so("recv ok", pgr_mbuf_recv(m) > 0);
	msg_is("message #2", m, MSG_STARTUP, 5);

	ok(pgr_mbuf_discard(m));
	so("recv ok", pgr_mbuf_recv(m) > 0);
	msg_is("message #3", m, 'I', 0);

	ok(pgr_mbuf_discard(m));
	so("recv ok", pgr_mbuf_recv(m) > 0);
	msg_is("message #4", m, 'Q', 33);
	ok(memcmp(pgr_mbuf_data(m), "Do you know the way to San Jose?", 33));

	ok(pgr_mbuf_discard(m));
	so("recv ok", pgr_mbuf_recv(m) > 0);
	msg_is("message #5", m, 'L', 0x8000);

	ok(pgr_mbuf_discard(m));
	so("recv ok", pgr_mbuf_recv(m) > 0);
	msg_is("message #6", m, 'S', 0);

	ok(pgr_mbuf_discard(m));
	so("eof", pgr_mbuf_recv(m) == 0);

	 /********************************************************/
	/** Relay                                              **/
	reset_test();
	so("recv ok", pgr_mbuf_recv(m) > 0);
	ok(pgr_mbuf_relay(m));
	fileok(out, "\0\0\0\x08\x04\xd2\x16\x2f", 8);

	msg_is("after relaying SSLRequest message", m, MSG_STARTUP, 5);
	so("recv ok", pgr_mbuf_recv(m) > 0);
	ok(pgr_mbuf_relay(m));
	fileok(out, "\0\0\0\x08\x04\xd2\x16\x2f"
	            "\0\0\0\x09\x00\x03\x00\x00\x00", 8+9);

	msg_is("after relaying StartupMessage message", m, 'I', 0);
	so("recv ok", pgr_mbuf_recv(m) > 0);
	ok(pgr_mbuf_relay(m));
	fileok(out, "\0\0\0\x08\x04\xd2\x16\x2f"
	            "\0\0\0\x09\x00\x03\x00\x00\x00"
	            "I\0\0\0\4", 8+9+5);

	msg_is("after relaying 'I' message", m, 'Q', 33);
	so("recv ok", pgr_mbuf_recv(m) > 0);
	ok(pgr_mbuf_relay(m));
	fileok(out, "\0\0\0\x08\x04\xd2\x16\x2f"
	            "\0\0\0\x09\x00\x03\x00\x00\x00"
	            "I\0\0\0\4"
	            "Q\0\0\0\x25" "Do you know the way to San Jose?\0", 8+9+5+38);

	msg_is("after relaying 'Q' message", m, 'L', 0x8000);
	so("recv ok", pgr_mbuf_recv(m) > 0);
	ok(pgr_mbuf_relay(m));
	so("all of the L message was relayed",
			lseek(out, 0, SEEK_CUR) == 8+9+5+38+0x8000+5);

	msg_is("after relaying 'L' message", m, 'S', 0);

	 /********************************************************/
	/* Resend                                               */
	reset_test();
	so("recv ok", pgr_mbuf_recv(m) > 0);
	ok(pgr_mbuf_send(m));
	fileok(out, "\0\0\0\x08\x04\xd2\x16\x2f", 8);

	msg_is("after sending SSLRequest message", m, MSG_STARTUP, 5);
	so("recv ok", pgr_mbuf_recv(m) > 0);
	ok(pgr_mbuf_send(m));
	fileok(out, "\0\0\0\x08\x04\xd2\x16\x2f"
	            "\0\0\0\x09\x00\x03\x00\x00\x00", 8+9);

	msg_is("after sending StartupMessage message", m, 'I', 0);
	so("recv ok", pgr_mbuf_recv(m) > 0);
	ok(pgr_mbuf_send(m));
	fileok(out, "\0\0\0\x08\x04\xd2\x16\x2f"
	            "\0\0\0\x09\x00\x03\x00\x00\x00"
	            "I\0\0\0\4", 8+9+5);

	msg_is("after sending 'I' message", m, 'Q', 33);
	so("recv ok", pgr_mbuf_recv(m) > 0);
	ok(pgr_mbuf_send(m));
	fileok(out, "\0\0\0\x08\x04\xd2\x16\x2f"
	            "\0\0\0\x09\x00\x03\x00\x00\x00"
	            "I\0\0\0\4"
	            "Q\0\0\0\x25" "Do you know the way to San Jose?\0", 8+9+5+38);

	/* do the resend */
	ftruncate(out, 0);
	lseek(out, 0, SEEK_SET);
	ok(pgr_mbuf_resend(m));
	fileok(out, "\0\0\0\x08\x04\xd2\x16\x2f"
	            "\0\0\0\x09\x00\x03\x00\x00\x00"
	            "I\0\0\0\4"
	            "Q\0\0\0\x25" "Do you know the way to San Jose?\0", 8+9+5+38);

	msg_is("after sending 'Q' message", m, 'L', 0x8000);
	so("recv ok", pgr_mbuf_recv(m) > 0);
	ok(pgr_mbuf_send(m));
	so("all of the L message was sent",
			lseek(out, 0, SEEK_CUR) == 8+9+5+38+0x8000+5);

	ftruncate(out, 0);
	lseek(out, 0, SEEK_SET);
	ok(pgr_mbuf_resend(m));
	so("all of the L message was resent",
			lseek(out, 0, SEEK_CUR) == 8+9+5+38+0x8000+5);

	msg_is("after sending 'L' message", m, 'S', 0);

	 /********************************************************/
	/* Concatenate                                          */
	reset_test();
	pgr_mbuf_cat(m, "Q\0\0\0\x12" "SELECT THINGS\0", 19);
	ok(pgr_mbuf_relay(m));
	fileok(out, "Q\0\0\0\x12" "SELECT THINGS\0", 19);

	/********************************************************/

	printf("PASS\n");
	return 0;
}
#endif
