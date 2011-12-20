/*
 * DBus encapsulation for bridge interfaces
 *
 * Copyright (C) 2011 Olaf Kirch <okir@suse.de>
 */

#include <sys/poll.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>

#include <wicked/netinfo.h>
#include <wicked/logging.h>
#include <wicked/bridge.h>
#include <wicked/system.h>
#include "dbus-common.h"
#include "model.h"
#include "debug.h"

static dbus_bool_t	__wicked_dbus_bridge_port_to_dict(const ni_bridge_port_t *port,
				ni_dbus_variant_t *dict,
				const ni_dbus_object_t *object,
				int config_only);
static dbus_bool_t	__wicked_dbus_bridge_port_from_dict(ni_bridge_port_t *port,
				const ni_dbus_variant_t *dict,
				DBusError *error,
				int config_only);

#define NULL_bridge		((ni_bridge_t *) 0)

/*
 * Create a new Bridge interface
 */
ni_dbus_object_t *
ni_objectmodel_new_bridge(ni_dbus_server_t *server, const ni_dbus_object_t *config, DBusError *error)
{
	ni_interface_t *cfg_ifp = ni_dbus_object_get_handle(config);
	ni_interface_t *new_ifp;
	const ni_bridge_t *bridge = ni_interface_get_bridge(cfg_ifp);
	ni_netconfig_t *nc = ni_global_state_handle(0);
	int rv;

	cfg_ifp->link.type = NI_IFTYPE_BRIDGE;
	if (cfg_ifp->name == NULL) {
		static char namebuf[64];
		unsigned int num;

		for (num = 0; num < 65536; ++num) {
			snprintf(namebuf, sizeof(namebuf), "br%u", num);
			if (!ni_interface_by_name(nc, namebuf)) {
				ni_string_dup(&cfg_ifp->name, namebuf);
				break;
			}
		}

		if (cfg_ifp->name == NULL) {
			dbus_set_error(error, DBUS_ERROR_FAILED,
					"Unable to create bridge - too many interfaces");
			return NULL;
		}
	}

	if ((rv = ni_system_bridge_create(nc, cfg_ifp->name, bridge, &new_ifp)) < 0) {
		dbus_set_error(error, DBUS_ERROR_FAILED,
				"Unable to create bridge interface: %s",
				ni_strerror(rv));
		return NULL;
	}

	if (new_ifp->link.type != NI_IFTYPE_BRIDGE) {
		dbus_set_error(error, DBUS_ERROR_FAILED,
				"Unable to create bridge interface: new interface is of type %s",
				ni_linktype_type_to_name(new_ifp->link.type));
		return NULL;
	}

	return ni_objectmodel_register_interface(server, new_ifp);
}

/*
 * Bridge.delete method
 */
static dbus_bool_t
__ni_dbus_bridge_delete(ni_dbus_object_t *object, const ni_dbus_method_t *method,
			unsigned int argc, const ni_dbus_variant_t *argv,
			ni_dbus_message_t *reply, DBusError *error)
{
	ni_netconfig_t *nc = ni_global_state_handle(0);
	ni_interface_t *ifp = object->handle;

	NI_TRACE_ENTER_ARGS("ifp=%s", ifp->name);
	if (ni_system_bridge_delete(nc, ifp) < 0) {
		dbus_set_error(error, DBUS_ERROR_FAILED,
				"Error deleting bridge interface", ifp->name);
		return FALSE;
	}

	/* FIXME: destroy the object */
	ni_dbus_object_free(object);

	return TRUE;
}

/*
 * Bridge.addPort method
 */
static dbus_bool_t
__ni_dbus_bridge_add_port(ni_dbus_object_t *object, const ni_dbus_method_t *method,
			unsigned int argc, const ni_dbus_variant_t *argv,
			ni_dbus_message_t *reply, DBusError *error)
{
	ni_netconfig_t *nc = ni_global_state_handle(0);
	ni_interface_t *ifp = object->handle, *portif;
	ni_bridge_port_t *port_cfg;
	const char *port_name;

	NI_TRACE_ENTER_ARGS("ifp=%s", ifp->name);
	if (argc != 2 || !ni_dbus_variant_get_string(&argv[0], &port_name))
		goto bad_args;

	{
		ni_dbus_object_t *parent = object->parent, *port_object;

		if (!parent)
			return FALSE;
		for (port_object = parent->children; port_object; port_object = port_object->next) {
			if (!strcmp(port_object->path, port_name))
				break;
		}
		if (!port_object) {
			dbus_set_error(error, DBUS_ERROR_FAILED,
				"Unable to add port; interface not known");
			return FALSE;
		}

		portif = port_object->handle;
	}

	port_cfg = ni_bridge_port_new(portif->name);
	port_cfg->device = ni_interface_get(portif);
	if (!__wicked_dbus_bridge_port_from_dict(port_cfg, &argv[1], error, 1)) {
		dbus_set_error(error, DBUS_ERROR_FAILED,
				"Error parsing bridge port properties");
		ni_bridge_port_free(port_cfg);
		return FALSE;
	}

	if (ni_system_bridge_add_port(nc, ifp, port_cfg) < 0) {
		dbus_set_error(error, DBUS_ERROR_FAILED,
				"Error adding bridge port %s", portif->name);
		ni_bridge_port_free(port_cfg);
		return FALSE;
	}

	ni_bridge_port_free(port_cfg);
	return TRUE;

bad_args:
	dbus_set_error(error, DBUS_ERROR_FAILED,
			"Bad arguments to Bridge.addPort");
	return FALSE;
}

/*
 * Bridge.removePort method
 */
static dbus_bool_t
__ni_dbus_bridge_remove_port(ni_dbus_object_t *object, const ni_dbus_method_t *method,
			unsigned int argc, const ni_dbus_variant_t *argv,
			ni_dbus_message_t *reply, DBusError *error)
{
	ni_netconfig_t *nc = ni_global_state_handle(0);
	ni_interface_t *ifp = object->handle, *portif;
	const char *port_name;

	NI_TRACE_ENTER_ARGS("ifp=%s", ifp->name);
	if (argc != 1 || !ni_dbus_variant_get_string(&argv[0], &port_name))
		goto bad_args;

	{
		ni_dbus_object_t *parent = object->parent, *port_object;

		if (!parent)
			return FALSE;
		for (port_object = parent->children; port_object; port_object = port_object->next) {
			if (!strcmp(port_object->path, port_name))
				break;
		}
		if (!port_object) {
			dbus_set_error(error, DBUS_ERROR_FAILED,
				"Unable to remove port; interface not known");
			return FALSE;
		}

		portif = port_object->handle;
	}

	if (ni_system_bridge_remove_port(nc, ifp, portif->link.ifindex) < 0) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "Unable to remove port");
		return FALSE;
	}

	return TRUE;

bad_args:
	dbus_set_error(error, DBUS_ERROR_FAILED, "Bad arguments to Bridge.removePort");
	return FALSE;
}

/*
 * Helper function to obtain bridge config from dbus object
 */
static ni_bridge_t *
__wicked_dbus_bridge_handle(const ni_dbus_object_t *object, DBusError *error)
{
	ni_interface_t *ifp = ni_dbus_object_get_handle(object);

	return ni_interface_get_bridge(ifp);
}

void *
ni_objectmodel_get_bridge(const ni_dbus_object_t *object, DBusError *error)
{
	ni_interface_t *ifp = ni_dbus_object_get_handle(object);
	ni_bridge_t *br;

	if (!(br = ni_interface_get_bridge(ifp))) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "Error getting bridge handle for interface");
		return NULL;
	}
	return br;
}


static dbus_bool_t
__wicked_dbus_bridge_parse_stp(const ni_dbus_property_t *property,
				ni_dbus_variant_t *result,
				const char *value)
{
	if (!value)
		return FALSE;
	if (!strcmp(value, "yes") || !strcmp(value, "on"))
		return ni_dbus_variant_set_int(result, 1);
	if (!strcmp(value, "no") || !strcmp(value, "off"))
		return ni_dbus_variant_set_int(result, 0);
	return ni_dbus_variant_parse(result, value, property->signature);
}

/*
 * Property ports
 */
static dbus_bool_t
__wicked_dbus_bridge_get_ports(const ni_dbus_object_t *object, const ni_dbus_property_t *property,
				ni_dbus_variant_t *result, DBusError *error)
{
	ni_bridge_t *bridge;
	unsigned int i;

	if (!(bridge = __wicked_dbus_bridge_handle(object, error)))
		return FALSE;

	ni_dbus_dict_array_init(result);
	for (i = 0; i < bridge->ports.count; ++i) {
		const ni_bridge_port_t *port = bridge->ports.data[i];
		ni_dbus_variant_t *dict;

		/* Append a new element to the array */
		if (!(dict = ni_dbus_dict_array_add(result)))
			return FALSE;
		ni_dbus_variant_init_dict(dict);

		if (!__wicked_dbus_bridge_port_to_dict(port, dict, object, 0))
			return FALSE;
	}
	return TRUE;
}

static dbus_bool_t
__wicked_dbus_bridge_set_ports(ni_dbus_object_t *object, const ni_dbus_property_t *property,
				const ni_dbus_variant_t *argument, DBusError *error)
{
	/* TBD */
	return TRUE;
}

/*
 * Helper functions to represent ports as a dbus dict
 */
static dbus_bool_t
__wicked_dbus_bridge_port_to_dict(const ni_bridge_port_t *port, ni_dbus_variant_t *dict,
				const ni_dbus_object_t *object,
				int config_only)
{
	ni_netconfig_t *nc = ni_global_state_handle(0);
	ni_interface_t *ifp;

	if (port->name) {
		ni_dbus_dict_add_string(dict, "name", port->name);
		if ((ifp = ni_interface_by_name(nc, port->name)) != NULL) {
			ni_dbus_dict_add_object_path(dict, "object",
					ni_objectmodel_interface_path(ifp));
		}
	}
	ni_dbus_dict_add_uint32(dict, "priority", port->priority);
	ni_dbus_dict_add_uint32(dict, "path_cost", port->path_cost);

	if (config_only)
		return TRUE;

	ni_dbus_dict_add_uint32(dict, "state", port->status.state);
	ni_dbus_dict_add_uint32(dict, "port_id", port->status.port_id);
	ni_dbus_dict_add_uint32(dict, "port_no", port->status.port_no);
	return TRUE;
}

static dbus_bool_t
__wicked_dbus_bridge_port_from_dict(ni_bridge_port_t *port, const ni_dbus_variant_t *dict,
				DBusError *error,
				int config_only)
{
	const char *string;
	uint32_t value;

	if (dict->array.len == 0)
		return TRUE;
	if (ni_dbus_dict_get_string(dict, "name", &string))
		ni_string_dup(&port->name, string);
	if (ni_dbus_dict_get_uint32(dict, "priority", &value))
		port->priority = value;
	if (ni_dbus_dict_get_uint32(dict, "path_cost", &value))
		port->path_cost = value;

	if (ni_dbus_dict_get_uint32(dict, "state", &value))
		port->status.state = value;
	if (ni_dbus_dict_get_uint32(dict, "port_id", &value))
		port->status.port_id = value;
	if (ni_dbus_dict_get_uint32(dict, "port_no", &value))
		port->status.port_no = value;

	return TRUE;
}

/*
 * Dummy properties for bridge port.
 * This is needed for encoding bridge port properties in addPort calls
 */
#define WICKED_BRIDGE_PORT_PROPERTY(type, __name) \
	NI_DBUS_DUMMY_PROPERTY(type, __name)

static ni_dbus_property_t	wicked_dbus_bridge_port_properties[] = {
	WICKED_BRIDGE_PORT_PROPERTY(UINT32, priority),
	WICKED_BRIDGE_PORT_PROPERTY(UINT32, path_cost),

	{ NULL }
};

#define BRIDGE_INT_PROPERTY(dbus_name, member_name, rw) \
	NI_DBUS_GENERIC_INT_PROPERTY(bridge, dbus_name, member_name, rw)
#define BRIDGE_UINT_PROPERTY(dbus_name, member_name, rw) \
	NI_DBUS_GENERIC_UINT_PROPERTY(bridge, dbus_name, member_name, rw)
#define WICKED_BRIDGE_PROPERTY_SIGNATURE(signature, __name, rw) \
	__NI_DBUS_PROPERTY(signature, __name, __wicked_dbus_bridge, rw)

static ni_dbus_property_t	wicked_dbus_bridge_properties[] = {
	BRIDGE_UINT_PROPERTY(priority, priority, RO),
	/* This one needs a special parse function: */
	__NI_DBUS_GENERIC_PROPERTY(bridge, DBUS_TYPE_UINT32_AS_STRING, stp, uint, stp, RO,
			.parse = __wicked_dbus_bridge_parse_stp),
	BRIDGE_UINT_PROPERTY(forward-delay, forward_delay, RO),
	BRIDGE_UINT_PROPERTY(aging-time, ageing_time, RO),
	BRIDGE_UINT_PROPERTY(hello-time, hello_time, RO),
	BRIDGE_UINT_PROPERTY(max-age, max_age, RO),

	/* ports is an array of dicts */
	WICKED_BRIDGE_PROPERTY_SIGNATURE(DBUS_TYPE_ARRAY_AS_STRING NI_DBUS_DICT_SIGNATURE,
			ports, RO),

	{ NULL }
};


static ni_dbus_method_t		wicked_dbus_bridge_methods[] = {
	{ "delete",		"",		__ni_dbus_bridge_delete },
	{ "addPort",		"sa{sv}",	__ni_dbus_bridge_add_port },
	{ "removePort",		"s",		__ni_dbus_bridge_remove_port },
	{ NULL }
};

ni_dbus_service_t	wicked_dbus_bridge_service = {
	.name = WICKED_DBUS_BRIDGE_INTERFACE,
	.methods = wicked_dbus_bridge_methods,
	.properties = wicked_dbus_bridge_properties,
};

ni_dbus_service_t	wicked_dbus_bridge_port_dummy_service = {
	.name = WICKED_DBUS_INTERFACE ".BridgePort",
	.properties = wicked_dbus_bridge_port_properties,
};
