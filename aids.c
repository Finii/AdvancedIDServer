// This is a local event ID server:
// Advanced ID Server
//
// See end of file for more info
//
// 16 Oct 2015 Fini Jastrow

// cc % -l pthread

#ifdef WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <errno.h>
#  include <stdlib.h>
#  include <stdio.h>
#  include <string.h>
#  include <time.h>
#  include <sys/types.h>
/* not declared in windows :'( */
#  ifndef MSG_DONTWAIT
#    define MSG_DONTWAIT 0 /* -> 0x40 */
#  endif
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0 /* -> 0x4000 */
#  endif

#  ifndef __MINGW__
#define sleep(x) Sleep(x * 1000)
#define close(x) closesocket(x)

/* FILETIME of Jan 1 1970 00:00:00. */
static const unsigned __int64 epoch = ((unsigned __int64)116444736000000000ULL);

/*
* timezone information is stored outside the kernel so tzp isn't used anymore.
*
* Note: this function is not for Win32 high precision timing purpose. See
* elapsed_time().
*/
int
gettimeofday(struct timeval * tp, struct timezone * tzp)
{
	FILETIME    file_time;
	SYSTEMTIME  system_time;
	ULARGE_INTEGER ularge;

	GetSystemTime(&system_time);
	SystemTimeToFileTime(&system_time, &file_time);
	ularge.LowPart = file_time.dwLowDateTime;
	ularge.HighPart = file_time.dwHighDateTime;

	tp->tv_sec = (long)((ularge.QuadPart - epoch) / 10000000L);
	tp->tv_usec = (long)(system_time.wMilliseconds * 1000);

	return 0;
}
#  else
#    include <pthread.h>
#    include <unistd.h>
#    include <sys/time.h>
#  endif
#else
#  include <errno.h>
#  include <netdb.h>
#  include <pthread.h>
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#  include <unistd.h>
#  include <arpa/inet.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <sys/socket.h>
#endif

#define ID_SERVER       "131.169.232.205"
#define ID_SERVER_PORT  "58050"
#define OUR_PORT        58051

#define ID_MESSAGE_SIZE 10000
#define ID_EVENT_WINDOW_SIZE    40

/* SO_REUSEPORT is not supported by older systems
 *
 * see http://stackoverflow.com/questions/3261965/so-reuseport-on-linux
 * > On Linux, SO_REUSEADDR provide most of what SO_REUSEPORT
 * > provides on BSD. */
#ifndef SO_REUSEPORT
#define SO_REUSEPORT SO_REUSEADDR
#endif


// Turns on mostly receive and decode debug messages
//#define DEBUG(x) x
#define DEBUG(x)

// elements of circular buffer with received EventID mesg
typedef struct {
	struct timeval t;
	unsigned long int id;
	long long int offset;
} ID_event_t;

// this moments ID and state in one place
typedef struct {
	unsigned long int id_iq;
	unsigned long int id_sub;
	char state;
	int jitter;
	int jitter0;
} ID_t;

// God bless global variables
static ID_event_t events[ID_EVENT_WINDOW_SIZE]; // circular buffer
static int events_idx	= 0;	// next element to be overwritten
static int first_entry	= 1;	// events[] not yet initialized
static int connected	= 0;	// are we connected to the EventID server
static int recon_retry	= 0;	// limit reconnect retries

// calculates the event ID and some state information
// valid for the moment this function is called
static void calc_id(ID_t* return_id) {
	long long int offset, offset0;
	long long int id;
	long long int id_iq;
	long long int jitter = 0;
	long long int jitter0;
	struct timeval old;
	struct timeval tv;
	char state;
	double td;
	int i;

	gettimeofday(&tv, NULL); // == NOW

	// find smallest offset value
	// find oldest entry
	// add the offset jitter
	old = events[ID_EVENT_WINDOW_SIZE-1].t;
	offset = events[ID_EVENT_WINDOW_SIZE-1].offset;
	jitter0 = offset0 = offset;

	for (i = ID_EVENT_WINDOW_SIZE-1; i--;) {
		if (events[i].offset < offset)	offset = events[i].offset;
		if (events[i].t.tv_sec < old.tv_sec ||
			(events[i].t.tv_sec == old.tv_sec && events[i].t.tv_usec < old.tv_usec))	{
			old = events[i].t;
		}
		jitter += abs(events[i].offset - offset0);
		jitter0 -= events[i].offset / (ID_EVENT_WINDOW_SIZE-1);
	}
	jitter /= ID_EVENT_WINDOW_SIZE;

	// calculate EventID for NOW based on offset found in loop above
	// (T-O) / 0.1 = ID (based on T in seconds)

	id = (unsigned long long int) tv.tv_usec
		+ 1000000 * (unsigned long long int) tv.tv_sec
		- offset;
	id_iq = id / 100000;

	return_id->id_iq = id_iq;
	return_id->id_sub = id - id_iq * 100000;
	return_id->jitter = jitter;
	return_id->jitter0 = jitter0;

	// calculate how old the oldest entry is
	// used to detect state 'stale'
	if (tv.tv_usec < old.tv_usec) {
		// needed for unsigned subtraction
		tv.tv_usec += 1000000;
		old.tv_sec += 1;
	}
	tv.tv_sec -= old.tv_sec;
	tv.tv_usec -= old.tv_usec;
	// tv holds now the time difference to the oldest entry
	td = tv.tv_sec;
	td += tv.tv_usec / 1000000.0;

	if (td > ID_EVENT_WINDOW_SIZE * 0.15) state = 'S';
	else state = 'O';
	if (!connected) state = 'D';
	return_id->state = state;
}

// how our output (payload) looks like
static inline int make_id_str(char* const s, size_t len, ID_t id) {
	return snprintf(s, len, "%lu.%05lu %c %d %d\r\n",
		id.id_iq, id.id_sub, id.state,
		id.jitter, id.jitter0);
}

// connects to the EventID Server
static int ID_connect(void) {
	int sock, c, err, opt = 1;
	struct addrinfo *a;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		fprintf(stderr, "ID_connect() socket failed: %s\r\n", strerror(errno));
		return -1;
	}

#if WIN32
	DWORD to = 2 * 1000;
	err = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
	if (err < 0) {
		fprintf(stderr, "ID_connect() setsockopt failed: %s\r\n", strerror(errno));
		close(sock);
		return -1;
	}
#else
	struct timeval tv;
	// 2s receive timeout
	tv.tv_sec = 2;
	tv.tv_usec = 0;
	err = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const void *)&tv, sizeof(tv));
	if (err < 0) {
		fprintf(stderr, "ID_connect() setsockopt failed: %s\r\n", strerror(errno));
		close(sock);
		return -1;
	}
#endif
	/* allow port reuse (optional, linux only) */
	err = setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (char*)&opt, sizeof(opt));
	if (err < 0) {
		fprintf(stderr, "ID_connect() setsockopt failed: %s\r\n", strerror(errno));
	}

	err = getaddrinfo(ID_SERVER, ID_SERVER_PORT, NULL, &a);
	if (err) {
		fprintf(stderr, "ID_connect() getaddrinfo failed: %s\r\n", strerror(err));
		close(sock);
		return -1;
	}

	c = connect(sock, a->ai_addr, a->ai_addrlen);
	freeaddrinfo(a);
	if (c < 0) {
		fprintf(stderr, "ID_connect() connect failed: %s\r\n", strerror(errno));
		close(sock);
		return -1;
	}

	fprintf(stdout, "Connection to %s successful\r\n", a->ai_canonname ? a->ai_canonname : ID_SERVER);
	return sock;
}

// collects continuously all data from the EventID server
// and stores it in our ringbuffer
//
// runs in a sub-thread
static void* ID_collect(void* nyx) {
	char buff[ID_MESSAGE_SIZE];
	unsigned long int id;
	long long int offset;
	struct timeval tv;
	int sock, x;
	ID_t myid;

	unsigned long int id_iq, id_sub;

	for(;;) { // loop over several reconnects
		sock = ID_connect();
		if (sock < 0) {
			// Maybe try to reconnect etc pp
			recon_retry++;
			if (recon_retry > 20) {
				fprintf(stderr, "ID_collect() giving up to reconnect\r\n");
#ifdef WIN32
				WSACleanup();
#endif
				exit(0);
			}
			fprintf(stderr, "Connect failed, %d/20 retries left...\r\n", recon_retry);
			fprintf(stderr, "Next retry in %dsec\r\n", 2*recon_retry);
			sleep(2*recon_retry);
			continue;
		}

		for(;;) { // loop over several msgs
			x = recv(sock, buff, ID_MESSAGE_SIZE, 0);
			gettimeofday(&tv, NULL);
			if (x <= 0) {
				// timeout ... check if still connected
				// and/or maybe reconnect
				fprintf(stderr, "ID_collect() recv failed, %d bytes received, closing socket: %s\r\n",
					x, strerror(errno));
				close(sock); // lazy...
				connected = 0;
				break;
			} else if (x == ID_MESSAGE_SIZE) {
				// we dont want these big messages, just drop it
				// and wait for the next one
				printf("ID_collect() skipping invalid message\r\n");
				continue;
			}
			// decode message and put into events array
			// "151016 090505.543 45C7A4D\r\n"

			// find first blank backwards from end
			for (;--x>0;) if (buff[x] == ' ') break;

			if (x <= 0) {
				// error reading packet, just ignore it
				printf("ID_collect() skipping unparsable message\r\n");
				continue;
			}

			sscanf(buff+x, "%lx", &id);

			// (T-O) / 0.1 = ID (based on T in seconds)
			// O = T - 0.1 * ID
			offset = (unsigned long long int) tv.tv_usec
				+ 1000000 * (unsigned long long int) tv.tv_sec
				- 100000 * (unsigned long long int) id;

			DEBUG(printf("ID %2d found %lx -=> Offset %lld\r\n", events_idx, id, offset));

			// fill all buffer entries with the first ID we receive
			// this alleviates us of several nasty sanity checks ;)
			if (first_entry) {
				first_entry = 0;
				for (events_idx = ID_EVENT_WINDOW_SIZE; --events_idx;) {
					events[events_idx].t.tv_sec = tv.tv_sec;
					events[events_idx].t.tv_usec = tv.tv_usec;
					events[events_idx].id = id;
					events[events_idx].offset = offset;
				}
				printf("Connected to EventID server and getting data\r\n");

			}
			events[events_idx].t.tv_sec = tv.tv_sec;
			events[events_idx].t.tv_usec = tv.tv_usec;
			events[events_idx].id = id;
			events[events_idx].offset = offset;

			events_idx = (events_idx + 1) % ID_EVENT_WINDOW_SIZE;
			connected = 1;
			recon_retry = 0;

			DEBUG(calc_id(&myid));
			DEBUG(make_id_str(buff, sizeof(buff), myid));
			DEBUG(printf("%s", buff));

		} // end of forever (mesgs)
	} // end of forever (reconnects)
}

// Open a listening port
// (the main user interaction)
// Shout-out the calculated EventID on establishing the connection
// Shout-out again on any incoming packet
void deliver_id() {
	int sock, con;
	int yes = 1, i, c;
	struct addrinfo hints, *svr, *p;
	ID_t myid;
	struct sockaddr_in client_addr, serv;
	socklen_t sin_size;
	char s[INET_ADDRSTRLEN];
	char msg[100];

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		fprintf(stderr, "deliver_id() socket failed: %s\r\n", strerror(errno));
		return;
	}

	c = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(int));
	if (c < 0) {
		fprintf(stderr, "deliver_id() setsockopt failed: %s\r\n", strerror(errno));
		return;
	}

	serv.sin_family = AF_INET;
	serv.sin_addr.s_addr = INADDR_ANY;
	serv.sin_port = htons(OUR_PORT);
	c = bind(sock, (struct sockaddr*)&serv, sizeof(serv));
	if (c < 0) {
		close(sock);
		fprintf(stderr, "deliver_id() bind failed: %s\r\n", strerror(errno));
		return;
	}

	c = listen(sock, 1);
	if (c < 0) {
		fprintf(stderr, "deliver_id() listen failed: %s\r\n", strerror(errno));
		return;
	}

	printf("waiting for connections...\r\n");

	for (;;) { // main accept() loop
		sin_size = sizeof(client_addr);
		con = accept(sock, (struct sockaddr*)&client_addr, &sin_size);
		if (con < 0) {
			fprintf(stderr, "deliver_id() accept failed: %s\r\n", strerror(errno));
			break;
		}

#ifdef WIN32
		getnameinfo((const struct sockaddr *)&client_addr, sizeof(client_addr), s, INET_ADDRSTRLEN, NULL, 0, 0);
#else
		inet_ntop(AF_INET, (const void*)(&client_addr.sin_addr), s, INET_ADDRSTRLEN);
#endif
		printf("Connected to %s\r\n", s);

		do { // loop for repeated shouts within one connect
			calc_id(&myid);
			make_id_str(msg, sizeof(msg), myid);
			c = send(con, msg, strlen(msg), MSG_NOSIGNAL);
			if (c < 0) {
				close(con);
				if (errno != ECONNRESET && errno != EPIPE) {
					fprintf(stderr, "deliver_id() send failed: %s\r\n", strerror(errno));
					break;
				}
			}

			c = recv(con, msg, sizeof(msg), 0);
#ifdef WIN32
		} while (c > 0);
#else
			while (c>0) c = recv(con, msg, sizeof(msg), MSG_DONTWAIT); // flush input queue
		} while (errno == EAGAIN || errno == EWOULDBLOCK);
#endif
		printf("Connection dropped\r\n");
	}
	// only reached if failure to open listening socket (= fatal)
}

int main(int argc, char *argv[]) {
#if defined(WIN32) && !defined(__MINGW__)
	WSADATA wsaData;
	// initialize the windows socket api
	memset(&wsaData, 0, sizeof(wsaData));
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		printf("Init winsock failed\r\n");
		return 1;
	}

	CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&ID_collect, NULL, 0, NULL);
#else
	pthread_t t;
	// connect to the EventID Server to gather information
	pthread_create(&t, NULL, &ID_collect, NULL);
#endif
	// deliver EventIDs with less jitter to users
	deliver_id();

#ifdef WIN32
	WSACleanup();
#endif
	return 0;
}

// Following the readme
#if 0

What does AIDS do?
==================
It connects to the TCP based event ID server and continuously gets the Ethernet packets with the IDs. These packets are time stamped with microsecond resolution and stored. This runs all the time. Because we have a lot of data we can correlate the local time on that machine (that generated the local time stamps) and the IDs. If the Ethernet connection lags sometimes, chokes, hick ups and messes around with our packets, even drops some... Who cares. By closely monitoring all data we can define a function to calculate the event ID from the local time on that machine.

The users can access this server via a socket. This time we use localhost:58051. Connect your program to that socket and you get immediately the actual event ID for this very moment. The packet you get looks like this:

73657523.41652 O 354

More abstract you get a number, dot, a second number. A character. A third number.

The first number is the event ID (decimal). The number after the dot is the fractional part of the event ID. Like how far after the light hit the chamber did you query. Thus you can estimate your timing. So in fact you get the event ID as float.

The char gives basic information about the state of the AIDS. 
O means Okay, all is well
S means Stale, our database is quite old and if the state does not change to O soon something is wrong.
D means Disconnected, we get no new information. Hopefully it will change to O in a moment.

So O means no worries. S and D are warnings, if they stay for more than some seconds your timing might run off.

The last number is the statistical quality of our calculation. Interpret it like something equal to timing jitter with the unit of microseconds. You can use it to monitor your Ethernet link quality. 10000 means 10 ms, everything below will not affect anyone I assume.

You get a new calculated ID every time you send any data to the server. So normally your program looks like this and should not close and open the socket all the time:

Connect to AIDS.
Check the state flag and jitter number in the first packet. Ignore the ID.
loop
  do your measurement
  get a trigger
  send something to AIDS and get the event ID
  save your aquired data with that ID
until shift finished or FLASH beam lost

Have fun
  Fini

#endif
