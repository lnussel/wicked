/*
 * Handing bridge interfaces.
 *
 * Copyright (C) 2009-2010 Olaf Kirch <okir@suse.de>
 */
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <net/if_arp.h>
#include <arpa/inet.h>

#include <wicked/netinfo.h>
#include <wicked/bridge.h>
#include "netinfo_priv.h"

#define NI_BRIDGE_VALUE_NOT_SET		~0U
#define NI_BRIDGE_PORT_ARRAY_CHUNK	16

static int			__ni_bridge_str_to_uint(const char *, unsigned int *);
static int			__ni_bridge_uint_to_str(unsigned int, char **);
static int			__ni_bridge_str_to_time(const char *, unsigned int *);
static int			__ni_bridge_time_to_str(unsigned long, char **);

static ni_bridge_port_t *	__ni_bridge_port_clone(const ni_bridge_port_t *);

static void			ni_bridge_port_array_init(ni_bridge_port_array_t *);
static int			ni_bridge_port_array_copy(ni_bridge_port_array_t *,
					const ni_bridge_port_array_t *);
static void			ni_bridge_port_array_destroy(ni_bridge_port_array_t *);
static void			__ni_bridge_port_array_realloc(ni_bridge_port_array_t *,
					unsigned int);
static int			__ni_bridge_port_array_append(ni_bridge_port_array_t *,
					ni_bridge_port_t *);



/*
 * Bridge option value conversion utilities
 * Returns -1 on error, 0 if the value is not set, and 1 otherwise.
 */
static int
__ni_bridge_str_to_uint(const char *str, unsigned int *val)
{
	if (!str || !*str) {
		*val = NI_BRIDGE_VALUE_NOT_SET;
		return 0;
	} else {
		char *end = NULL;
		unsigned int i = strtoul(str, &end, 0);

		if (*end == '\0') {
			*val = i;
			return 1;
		}
	}
	return -1;
}

static int
__ni_bridge_uint_to_str(unsigned int val, char **str)
{
	if (val == NI_BRIDGE_VALUE_NOT_SET) {
		ni_string_free(str);
		return 0;
	} else {
		char   buf[32];

		snprintf(buf, sizeof(buf), "%u", val);
		ni_string_dup(str, buf);
		return *str ? 1 : -1;
	}
}

static int
__ni_bridge_str_to_time(const char *str, unsigned int *val)
{
	if (!str || !*str) {
		*val = NI_BRIDGE_VALUE_NOT_SET;
		return 0;
	} else {
		char *end = NULL;
		double d = strtod(str, &end);

		if (*end == '\0') {
			*val = (unsigned int)(d * 100);
			return 1;
		}
	}
	return -1;
}

static int
__ni_bridge_time_to_str(unsigned long val, char **str)
{
	if (val == NI_BRIDGE_VALUE_NOT_SET) {
		ni_string_free(str);
		return 0;
	} else {
		char   buf[32];
		double d = (double)val;

		snprintf(buf, sizeof(buf), "%.2lf", (d / 100));
		ni_string_dup(str, buf);
		return *str ? 1 : -1;
	}
}

ni_bridge_port_t *
ni_bridge_port_new(const char *name)
{
	ni_bridge_port_t *newport;

	newport = calloc(1, sizeof(ni_bridge_port_t));
	if (!newport)
		ni_fatal("%s: out of memory", __FUNCTION__);

	ni_string_dup(&newport->name, name);
	newport->priority = NI_BRIDGE_VALUE_NOT_SET;
	newport->path_cost = NI_BRIDGE_VALUE_NOT_SET;
	return newport;
}

static ni_bridge_port_t *
__ni_bridge_port_clone(const ni_bridge_port_t *port)
{
	ni_bridge_port_t *newport;

	newport = ni_bridge_port_new(port->name);
	newport->priority = port->priority;
	newport->path_cost = port->path_cost;
	return newport;
}

void
ni_bridge_port_free(ni_bridge_port_t *port)
{
	ni_string_free(&port->name);
	if (port->device)
		ni_interface_put(port->device);
	ni_bridge_port_status_destroy(&port->status);
	free(port);
}

static void
ni_bridge_port_array_init(ni_bridge_port_array_t *array)
{
	memset(array, 0, sizeof(*array));
}

static int
ni_bridge_port_array_copy(ni_bridge_port_array_t *dst, const ni_bridge_port_array_t *src)
{
	unsigned int i;

	ni_bridge_port_array_destroy(dst);
	for (i = 0; i < src->count; ++i) {
		if (__ni_bridge_port_array_append(dst, __ni_bridge_port_clone(src->data[i])) < 0)
			return -1;
	}
	return 0;
}

static void
ni_bridge_port_array_destroy(ni_bridge_port_array_t *array)
{
	while (array->count > 0)
		ni_bridge_port_free(array->data[--array->count]);
	free(array->data);
	ni_bridge_port_array_init(array);
}

static void
__ni_bridge_port_array_realloc(ni_bridge_port_array_t *array, unsigned int newsize)
{
	ni_bridge_port_t **newdata;
	unsigned int i;

	newsize = (newsize + NI_BRIDGE_PORT_ARRAY_CHUNK);
	newdata = realloc(array->data, newsize * sizeof(ni_bridge_port_t));
	if (!newdata)
		ni_fatal("%s: out of memory", __FUNCTION__);

	array->data = newdata;
	for (i = array->count; i < newsize; ++i)
		array->data[i] = NULL;
}

static int
__ni_bridge_port_array_append(ni_bridge_port_array_t *array, ni_bridge_port_t *port)
{
	if ((array->count % NI_BRIDGE_PORT_ARRAY_CHUNK) == 0)
		__ni_bridge_port_array_realloc(array, array->count);

	array->data[array->count++] = port;
	return 0;
}

static int
__ni_bridge_port_array_index(ni_bridge_port_array_t *array, const char *port)
{
	unsigned int i;
	for (i = 0; i < array->count; ++i) {
		if (!strcmp(port, array->data[i]->name))
			return i;
	}
	return -1;
}

static int
ni_bridge_port_array_remove_index(ni_bridge_port_array_t *array, unsigned int pos)
{
	unsigned int i;

	if (pos >= array->count)
		return -1;

	ni_bridge_port_free(array->data[pos]);
	/* make it less cumbersome... */
	array->data[pos] = NULL;
	for (i = pos + 1; i < array->count; ++i) {
		array->data[i - 1] = array->data[i];
		array->data[i] = NULL;
	}
	array->count--;
	return 0;
}

/*
 * Add a port to the bridge configuration
 */
int
ni_bridge_add_port_name(ni_bridge_t *bridge, const char *ifname)
{
	if (!ifname || !*ifname)
		return -1;

	if (__ni_bridge_port_array_index(&bridge->ports, ifname) >= 0)
		return -1;

	__ni_bridge_port_array_append(&bridge->ports, ni_bridge_port_new(ifname));
	return 0;
}

int
ni_bridge_add_port(ni_bridge_t *bridge, const ni_bridge_port_t *port)
{
	unsigned int i;

	if (!port)
		return -1;

	for (i = 0; i < bridge->ports.count; ++i) {
		if (bridge->ports.data[i]->device == port->device)
			return -1;
	}

	__ni_bridge_port_array_append(&bridge->ports, __ni_bridge_port_clone(port));
	return 0;
}


int
ni_bridge_del_port(ni_bridge_t *bridge, const char *ifname)
{
	unsigned int i;

	for (i = 0; i < bridge->ports.count; ++i) {
		if (!strcmp(bridge->ports.data[i]->name, ifname)) {
			ni_bridge_port_array_remove_index(&bridge->ports, i);
			return 0;
		}
	}
	return -1;
}

int
ni_bridge_del_port_ifindex(ni_bridge_t *bridge, int ifindex)
{
	unsigned int i;

	for (i = 0; i < bridge->ports.count; ++i) {
		ni_interface_t *port_dev = bridge->ports.data[i]->device;

		if (port_dev && port_dev->link.ifindex == ifindex) {
			ni_bridge_port_array_remove_index(&bridge->ports, i);
			return 0;
		}
	}
	return -1;
}

void
ni_bridge_get_port_names(const ni_bridge_t *bridge, ni_string_array_t *ports)
{
	unsigned int i;

	for (i = 0; i < bridge->ports.count; ++i)
		ni_string_array_append(ports, bridge->ports.data[i]->name);
}

/*
 * Get bridge options
 */
int
ni_bridge_get_stp(ni_bridge_t *bridge, char **value)
{
	if (bridge->stp == NI_BRIDGE_NO_STP)
		ni_string_dup(value, "off");
	else
		ni_string_dup(value, "on");
	return *value ? 1 : 0;
}

int
ni_bridge_get_forward_delay(ni_bridge_t *bridge, char **value)
{
	return __ni_bridge_time_to_str(bridge->forward_delay, value);
}

int
ni_bridge_get_ageing_time(ni_bridge_t *bridge, char **value)
{
	return __ni_bridge_time_to_str(bridge->ageing_time, value);
}

int
ni_bridge_get_hello_time(ni_bridge_t *bridge, char **value)
{
	return __ni_bridge_time_to_str(bridge->hello_time, value);
}

int
ni_bridge_get_max_age(ni_bridge_t *bridge, char **value)
{
	return __ni_bridge_time_to_str(bridge->max_age, value);
}

int
ni_bridge_get_priority(ni_bridge_t *bridge, char **value)
{
	return __ni_bridge_uint_to_str(bridge->priority, value);
}

int
ni_bridge_get(ni_bridge_t *bridge, unsigned int opt, char **value)
{
	switch (opt) {
	case NI_BRIDGE_STP_ENABLED:
		return ni_bridge_get_stp(bridge, value);
	case NI_BRIDGE_FORWARD_DELAY:
		return ni_bridge_get_forward_delay(bridge, value);
	case NI_BRIDGE_AGEING_TIME:
		return ni_bridge_get_ageing_time(bridge, value);
	case NI_BRIDGE_HELLO_TIME:
		return ni_bridge_get_hello_time(bridge, value);
	case NI_BRIDGE_MAX_AGE:
		return ni_bridge_get_max_age(bridge, value);
	case NI_BRIDGE_PRIORITY:
		return ni_bridge_get_priority(bridge, value);
	}
	return -1;
}

/*
 * Set bridge options
 */
int
ni_bridge_set_stp(ni_bridge_t *bridge, const char *value)
{
	/* brctl accepts "on" / "off" as well as "yes" / "no"
	 * note: it is a bool {0,!0} while write, just sysfs
	 * shows details {0=off,1=stp,2=rstp} in stp_state.
	 */
	if (!value || !*value) {
		bridge->stp = NI_BRIDGE_NO_STP;
		return 0;
	} else
	if (!strcmp(value, "off") || !strcmp(value, "no")) {
		bridge->stp = NI_BRIDGE_NO_STP;
		return 0;
	} else
	if (!strcmp(value, "on") || !strcmp(value, "yes")) {
		bridge->stp = NI_BRIDGE_STP;
		return 0;
	}
	return -1;
}

int
ni_bridge_set_forward_delay(ni_bridge_t *bridge, const char *value)
{
	return __ni_bridge_str_to_time(value, &bridge->forward_delay);
}

int
ni_bridge_set_ageing_time(ni_bridge_t *bridge, const char *value)
{
	return __ni_bridge_str_to_time(value, &bridge->ageing_time);
}

int
ni_bridge_set_hello_time(ni_bridge_t *bridge, const char *value)
{
	return __ni_bridge_str_to_time(value, &bridge->hello_time);
}

int
ni_bridge_set_max_age(ni_bridge_t *bridge, const char *value)
{
	return __ni_bridge_str_to_time(value, &bridge->max_age);
}

int
ni_bridge_set_priority(ni_bridge_t *bridge, const char *value)
{
	return __ni_bridge_str_to_uint(value, &bridge->priority);
}

int
ni_bridge_set(ni_bridge_t *bridge, unsigned int opt, const char *value)
{
	switch (opt) {
	case NI_BRIDGE_STP_ENABLED:
		return ni_bridge_set_stp(bridge, value);
	case NI_BRIDGE_FORWARD_DELAY:
		return ni_bridge_set_forward_delay(bridge, value);
	case NI_BRIDGE_AGEING_TIME:
		return ni_bridge_set_ageing_time(bridge, value);
	case NI_BRIDGE_HELLO_TIME:
		return ni_bridge_set_hello_time(bridge, value);
	case NI_BRIDGE_MAX_AGE:
		return ni_bridge_set_max_age(bridge, value);
	case NI_BRIDGE_PRIORITY:
		return ni_bridge_set_priority(bridge, value);
	}
	return -1;
}

/*
 * Get bridge port options
 */
int
ni_bridge_port_get_priority(ni_bridge_t *bridge, const char *port, char **value)
{
	int i = __ni_bridge_port_array_index(&bridge->ports, port);

	if (i < 0)
		return -1;
	return __ni_bridge_uint_to_str(bridge->ports.data[i]->priority, value);
}

int
ni_bridge_port_get_path_cost(ni_bridge_t *bridge, const char *port, char **value)
{
	int i = __ni_bridge_port_array_index(&bridge->ports, port);

	if (i < 0)
		return -1;
	return __ni_bridge_uint_to_str(bridge->ports.data[i]->path_cost, value);
}

int
ni_bridge_port_get(ni_bridge_t *bridge, const char *port, unsigned int opt, char **value)
{
	switch (opt) {
	case NI_BRIDGE_PORT_PRIORITY:
		return ni_bridge_port_get_priority(bridge, port, value);
	case NI_BRIDGE_PORT_PATH_COST:
		return ni_bridge_port_get_path_cost(bridge, port, value);
	}
	return -1;
}

/*
 * Set bridge port options
 */
int
ni_bridge_port_set_priority(ni_bridge_t *bridge, const char *port, const char *value)
{
	int i = __ni_bridge_port_array_index(&bridge->ports, port);

	if (i < 0)
		return -1;
	return __ni_bridge_str_to_uint(value, &bridge->ports.data[i]->priority);
}

int
ni_bridge_port_set_path_cost(ni_bridge_t *bridge, const char *port, const char *value)
{
	int i = __ni_bridge_port_array_index(&bridge->ports, port);

	if (i < 0)
		return -1;
	return __ni_bridge_str_to_uint(value, &bridge->ports.data[i]->path_cost);
}

int
ni_bridge_port_set(ni_bridge_t *bridge, const char *port, unsigned int opt, const char *value)
{
	switch (opt) {
	case NI_BRIDGE_PORT_PRIORITY:
		return ni_bridge_port_set_priority(bridge, port, value);
	case NI_BRIDGE_PORT_PATH_COST:
		return ni_bridge_port_set_path_cost(bridge, port, value);
	}
	return -1;
}

/*
 * Create a copy of a bridge's configuration
 */
ni_bridge_t *
ni_bridge_clone(const ni_bridge_t *src)
{
	ni_bridge_t *dst;

	dst = calloc(1, sizeof(ni_bridge_t));
	if (!dst)
		return NULL;

#define C(x)	dst->x = src->x
	C(priority);
	C(stp);
	C(forward_delay);
	C(ageing_time);
	C(hello_time);
	C(max_age);
#undef C

	if (ni_bridge_port_array_copy(&dst->ports, &src->ports) < 0)
		goto failed;

	return dst;

failed:
	ni_error("Error cloning bridge configuration");
	ni_bridge_free(dst);
	return NULL;
}

/*
 * Bridge constructor and new operator
 */
static void
__ni_bridge_init(ni_bridge_t *bridge)
{
	/* apply "not set" defaults */
	bridge->forward_delay = NI_BRIDGE_VALUE_NOT_SET;
	bridge->ageing_time = NI_BRIDGE_VALUE_NOT_SET;
	bridge->hello_time = NI_BRIDGE_VALUE_NOT_SET;
	bridge->max_age = NI_BRIDGE_VALUE_NOT_SET;
	bridge->priority = NI_BRIDGE_VALUE_NOT_SET;
}

ni_bridge_t *
ni_bridge_new(void)
{
	ni_bridge_t *bridge;

	bridge = calloc(1, sizeof(*bridge));
	__ni_bridge_init(bridge);
	return bridge;
}

/*
 * Bridge destructor and delete operator
 */
static void
__ni_bridge_destroy(ni_bridge_t *bridge)
{
	ni_bridge_port_array_destroy(&bridge->ports);
	ni_bridge_status_destroy(&bridge->status);
}

void
ni_bridge_free(ni_bridge_t *bridge)
{
	__ni_bridge_destroy(bridge);
	free(bridge);
}

void
ni_bridge_status_destroy(ni_bridge_status_t *bs)
{
	ni_string_free(&bs->root_id);
	ni_string_free(&bs->bridge_id);
	ni_string_free(&bs->group_addr);
	memset(bs, 0, sizeof(*bs));
}

void
ni_bridge_port_status_destroy(ni_bridge_port_status_t *ps)
{
	ni_string_free(&ps->designated_root);
	ni_string_free(&ps->designated_bridge);
}
