#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <lua.h>
#include <lauxlib.h>

#if (LUA_VERSION_NUM < 502 && !defined(luaL_newlib))
#  define luaL_newlib(L,l) (lua_newtable(L), luaL_register(L,NULL,l))
#endif

/**
 * #define ENABLE_SOCK_DEBUG
 */

#ifdef ENABLE_SOCK_DEBUG
# define LCS_DLOG(fmt, ...) fprintf(stderr, "<lcs>" fmt "\n", ##__VA_ARGS__)
#else
# define LCS_DLOG(...)
#endif


#define CLIENT "lcsock{client}"

#ifdef WIN32
#  include <windows.h>
#  include <winsock2.h>

static void lcs_startup()
{
	WORD wVersionRequested;
	WSADATA wsaData;
	int err;

	wVersionRequested = MAKEWORD(2, 2);

	err = WSAStartup(wVersionRequested, &wsaData);
	if (err != 0) {
		fprintf(stderr, "WSAStartup failed with error: %d\n", err);
		exit(1);
	}
}

#define EINTR WSAEINTR
#define EWOULDBLOCK WSAEWOULDBLOCK

#else

#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#define closesocket close

static void lcs_startup()
{
}

#endif

#define CHECK_CLIENT(L, idx)\
	(*(sock_client_t **)luaL_checkudata(L, idx, CLIENT))

static int lcs_fdcanread(int fd)
{
	int r = 0;
	fd_set rfds;
	struct timeval tv = { 0, 0 };

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	r = select(fd + 1, &rfds, NULL, NULL, &tv);
	/**
	 * LCS_DLOG("%s %d", __FUNCTION__, r);
	 */
	return r == 1;
}

typedef struct sock_client_s {
	int fd;
	int connected;
} sock_client_t;

static sock_client_t * lcsock_client_create()
{
	sock_client_t *p = malloc(sizeof(*p));
	if (p == NULL)
		goto nomem;
	p->fd = -1;
	p->connected = 0;
	return p;
nomem:
	return NULL;
}

static int lua__lcs_sleep(lua_State *L)
{
	int ms = luaL_optinteger(L, 1, 0);

#if (defined(WIN32) || defined(_WIN32))
	Sleep(ms);
#else
	usleep((useconds_t)ms * 1000);
#endif
	return 0;
}

static int lua__lcs_new(lua_State *L)
{
	int fd;
	sock_client_t **p;
	sock_client_t *client;
	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd <= 0) {
		lua_pushboolean(L, 0);
		lua_pushstring(L, "create socket failed");
		return 2;
	}
	p = lua_newuserdata(L, sizeof(void *));
	client = lcsock_client_create();
	if (client == NULL) {
		closesocket(fd);
		lua_pushboolean(L, 0);
		lua_pushstring(L, "create client failed");
		return 2;
	}
	client->fd = fd;
	*p = client;
	luaL_getmetatable(L, CLIENT);
	lua_setmetatable(L, -2);
	return 1;
}

static int lua__lcs_connect(lua_State *L)
{
	int ret;
	struct sockaddr_in addr;
	sock_client_t * client = CHECK_CLIENT(L, 1);
	const char *addrstr = luaL_checkstring(L, 2);
	int port = luaL_checkinteger(L, 3);

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(addrstr);
	addr.sin_port = htons(port);
	// memset(addr.sin_zero, 0x00, 8);

	ret = connect(client->fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret != 0) {
		lua_pushboolean(L, 0);
		lua_pushfstring(L, "connect %s:%d failed", addrstr, port);
		return 2;
	}
	client->connected = 1;
	lua_pushboolean(L, 1);
	return 1;
}

static int lua__lcs_isconnected(lua_State *L)
{
	sock_client_t * client = CHECK_CLIENT(L, 1);
	LCS_DLOG("%s %d", __FUNCTION__, client->connected);
	lua_pushboolean(L, client->connected);
	return 1;
}

static int lua__lcs_disconnect(lua_State *L)
{
	sock_client_t * client = CHECK_CLIENT(L, 1);
	closesocket(client->fd);
	client->connected = 0;
	return 0;
}

static int lua__lcs_read(lua_State *L)
{
	char tmp[8192];
	char *buf = (char *)&tmp;
	ssize_t rsz = 0;
	sock_client_t * client = CHECK_CLIENT(L, 1);
	size_t sz = luaL_optlong(L, 2, sizeof(tmp));
	if (!client->connected) {
		return luaL_error(L, "not connected");
	}
	if (!lcs_fdcanread(client->fd)) {
		lua_pushboolean(L, 0);
		lua_pushstring(L, "no data");
		return 2;
	}
	if (sz > sizeof(tmp)) {
		buf = malloc(sz);
		if (buf == NULL) {
			return luaL_error(L, "nomem while read");
		}
	}

	rsz = recv(client->fd, buf, sz, 0);
	if (rsz > 0) {
		lua_pushlstring(L, buf, rsz);
	} else if (rsz <= 0) {
		client->connected = 0;
	}
	if (buf != (char *)&tmp) {
		free(buf);
	}
	return rsz > 0 ? 1 : 0;
}

static int lua__lcs_write(lua_State *L)
{
	size_t sz;
	size_t p = 0;
	sock_client_t * client = CHECK_CLIENT(L, 1);
	const char *buf = luaL_checklstring(L, 2, &sz);
	if (!client->connected) {
		lua_pushboolean(L, 0);
		lua_pushstring(L, "not connected");
		return 2;
	}
	for (;;) {
		int wt = send(client->fd, buf + p, sz - p, 0);
		if (wt < 0) {
			switch (errno) {
			case EWOULDBLOCK:
			case EINTR:
				continue;
			default:
				closesocket(client->fd);
				client->fd = -1;
				client->connected = 0;
				lua_pushboolean(L, 0);
				return 1;
			}
		}
		if (wt == sz - p)
			break;
		p += wt;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int lua__lcs_gc(lua_State *L)
{
	sock_client_t * client = CHECK_CLIENT(L, 1);
	if (client != NULL)
		free(client);
	LCS_DLOG("client gc");
	return 0;
}


static int opencls__client(lua_State *L)
{
	luaL_Reg lmethods[] = {
		{"read", lua__lcs_read},
		{"write", lua__lcs_write},
		{"connect", lua__lcs_connect},
		{"isconnected", lua__lcs_isconnected},
		{"disconnect", lua__lcs_disconnect},
		{NULL, NULL},
	};
	luaL_newmetatable(L, CLIENT);
	lua_newtable(L);
	luaL_register(L, NULL, lmethods);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction (L, lua__lcs_gc);
	lua_setfield (L, -2, "__gc");
	return 1;
}

int luaopen_lcsock(lua_State* L)
{
	luaL_Reg lfuncs[] = {
		{"new", lua__lcs_new},
		{"sleep", lua__lcs_sleep},
		{NULL, NULL},
	};
	lcs_startup();
	opencls__client(L);
	luaL_newlib(L, lfuncs);
	return 1;
}
