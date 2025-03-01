/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2017 Red Hat, Inc.
 */

#include "libnm-core-impl/nm-default-libnm-core.h"

#include "nm-setting-ovs-interface.h"

#include "nm-connection-private.h"
#include "nm-setting-connection.h"
#include "nm-setting-private.h"

/**
 * SECTION:nm-setting-ovs-interface
 * @short_description: Describes connection properties for Open vSwitch interfaces.
 *
 * The #NMSettingOvsInterface object is a #NMSetting subclass that describes properties
 * necessary for Open vSwitch interfaces.
 **/

/*****************************************************************************/

NM_GOBJECT_PROPERTIES_DEFINE_BASE(PROP_TYPE, PROP_OFPORT_REQUEST, );

/**
 * NMSettingOvsInterface:
 *
 * Open vSwitch Interface Settings
 */
struct _NMSettingOvsInterface {
    NMSetting parent;

    char   *type;
    guint32 ofport_request;
};

struct _NMSettingOvsInterfaceClass {
    NMSettingClass parent;
};

G_DEFINE_TYPE(NMSettingOvsInterface, nm_setting_ovs_interface, NM_TYPE_SETTING)

/*****************************************************************************/

/**
 * nm_setting_ovs_interface_get_interface_type:
 * @self: the #NMSettingOvsInterface
 *
 * Returns: the #NMSettingOvsInterface:type property of the setting
 *
 * Since: 1.10
 **/
const char *
nm_setting_ovs_interface_get_interface_type(NMSettingOvsInterface *self)
{
    g_return_val_if_fail(NM_IS_SETTING_OVS_INTERFACE(self), NULL);

    return self->type;
}

/**
 * nm_setting_ovs_interface_get_ofport_request:
 * @self: the #NMSettingOvsInterface
 *
 * Returns: id of the preassigned ovs port
 *
 * Since: 1.42
 **/
guint32
nm_setting_ovs_interface_get_ofport_request(NMSettingOvsInterface *self)
{
    g_return_val_if_fail(NM_IS_SETTING_OVS_INTERFACE(self), 0);

    return self->ofport_request;
}

/*****************************************************************************/

int
_nm_setting_ovs_interface_verify_interface_type(NMSettingOvsInterface *self,
                                                const char            *type,
                                                NMConnection          *connection,
                                                gboolean               normalize,
                                                gboolean              *out_modified,
                                                const char           **out_normalized_type,
                                                GError               **error)
{
    const char *type_from_setting = NULL;
    const char *type_setting      = NULL;
    const char *connection_type;
    gboolean    is_ovs_connection_type;

    if (normalize) {
        g_return_val_if_fail(NM_IS_SETTING_OVS_INTERFACE(self), FALSE);
        g_return_val_if_fail(NM_IS_CONNECTION(connection), FALSE);
        nm_assert(self == nm_connection_get_setting_ovs_interface(connection));
    } else {
        g_return_val_if_fail(!self || NM_IS_SETTING_OVS_INTERFACE(self), FALSE);
        g_return_val_if_fail(!connection || NM_IS_CONNECTION(connection), FALSE);
    }

    NM_SET_OUT(out_modified, FALSE);
    NM_SET_OUT(out_normalized_type, NULL);

    if (type && !NM_IN_STRSET(type, "internal", "system", "patch", "dpdk")) {
        g_set_error(error,
                    NM_CONNECTION_ERROR,
                    NM_CONNECTION_ERROR_INVALID_PROPERTY,
                    _("'%s' is not a valid interface type"),
                    type);
        g_prefix_error(error,
                       "%s.%s: ",
                       NM_SETTING_OVS_INTERFACE_SETTING_NAME,
                       NM_SETTING_OVS_INTERFACE_TYPE);
        return FALSE;
    }

    if (!connection) {
        NM_SET_OUT(out_normalized_type, type);
        return TRUE;
    }

    connection_type = nm_connection_get_connection_type(connection);
    if (!connection_type) {
        /* if we have an ovs-interface, then the connection type must be either
         * "ovs-interface" (for non "system" type) or anything else (for "system" type).
         *
         * The connection type usually can be normalized based on the presence of a
         * base setting. However, in this case, if the connection type is missing,
         * that is too complicate to guess what the user wanted.
         *
         * Require the use to be explicit and fail. */
        g_set_error(error,
                    NM_CONNECTION_ERROR,
                    NM_CONNECTION_ERROR_INVALID_PROPERTY,
                    _("A connection with a '%s' setting needs connection.type explicitly set"),
                    NM_SETTING_OVS_INTERFACE_SETTING_NAME);
        g_prefix_error(error,
                       "%s.%s: ",
                       NM_SETTING_CONNECTION_SETTING_NAME,
                       NM_SETTING_CONNECTION_TYPE);
        return FALSE;
    }

    if (nm_streq(connection_type, NM_SETTING_OVS_INTERFACE_SETTING_NAME)) {
        if (type && nm_streq(type, "system")) {
            g_set_error(error,
                        NM_CONNECTION_ERROR,
                        NM_CONNECTION_ERROR_INVALID_PROPERTY,
                        _("A connection of type '%s' cannot have ovs-interface.type \"system\""),
                        NM_SETTING_OVS_INTERFACE_SETTING_NAME);
            g_prefix_error(error,
                           "%s.%s: ",
                           NM_SETTING_OVS_INTERFACE_SETTING_NAME,
                           NM_SETTING_OVS_INTERFACE_TYPE);
            return FALSE;
        }
        is_ovs_connection_type = TRUE;
    } else {
        if (type && !nm_streq(type, "system")) {
            g_set_error(error,
                        NM_CONNECTION_ERROR,
                        NM_CONNECTION_ERROR_INVALID_PROPERTY,
                        _("A connection of type '%s' cannot have an ovs-interface.type \"%s\""),
                        connection_type,
                        type);
            g_prefix_error(error,
                           "%s.%s: ",
                           NM_SETTING_OVS_INTERFACE_SETTING_NAME,
                           NM_SETTING_OVS_INTERFACE_TYPE);
            return FALSE;
        }
        is_ovs_connection_type = FALSE;
    }

    if (nm_connection_get_setting_by_name(connection, NM_SETTING_OVS_PATCH_SETTING_NAME)) {
        type_from_setting = "patch";
        type_setting      = NM_SETTING_OVS_PATCH_SETTING_NAME;
    }

    if (nm_connection_get_setting_by_name(connection, NM_SETTING_OVS_DPDK_SETTING_NAME)) {
        if (type_from_setting) {
            g_set_error(error,
                        NM_CONNECTION_ERROR,
                        NM_CONNECTION_ERROR_INVALID_PROPERTY,
                        _("A connection can not have both '%s' and '%s' settings at the same time"),
                        NM_SETTING_OVS_DPDK_SETTING_NAME,
                        type_setting);
            return FALSE;
        }
        type_from_setting = "dpdk";
        type_setting      = NM_SETTING_OVS_DPDK_SETTING_NAME;
    }

    if (type_from_setting) {
        if (!is_ovs_connection_type) {
            g_set_error(error,
                        NM_CONNECTION_ERROR,
                        NM_CONNECTION_ERROR_INVALID_PROPERTY,
                        _("A connection with '%s' setting must be of connection.type "
                          "\"ovs-interface\" but is \"%s\""),
                        NM_SETTING_OVS_PATCH_SETTING_NAME,
                        connection_type);
            g_prefix_error(error,
                           "%s.%s: ",
                           NM_SETTING_OVS_INTERFACE_SETTING_NAME,
                           NM_SETTING_OVS_INTERFACE_TYPE);
            return FALSE;
        }

        if (type) {
            if (!nm_streq(type, type_from_setting)) {
                g_set_error(error,
                            NM_CONNECTION_ERROR,
                            NM_CONNECTION_ERROR_INVALID_PROPERTY,
                            _("A connection with '%s' setting needs to be of '%s' interface type, "
                              "not '%s'"),
                            type_setting,
                            type_from_setting,
                            type);
                g_prefix_error(error,
                               "%s.%s: ",
                               NM_SETTING_OVS_INTERFACE_SETTING_NAME,
                               NM_SETTING_OVS_INTERFACE_TYPE);
                return FALSE;
            }
            NM_SET_OUT(out_normalized_type, type);
            return TRUE;
        }
        type = type_from_setting;
        goto normalize;
    } else {
        if (nm_streq0(type, "patch")) {
            g_set_error(
                error,
                NM_CONNECTION_ERROR,
                NM_CONNECTION_ERROR_MISSING_SETTING,
                _("A connection with ovs-interface.type '%s' setting a 'ovs-patch' setting"),
                type);
            g_prefix_error(error,
                           "%s.%s: ",
                           NM_SETTING_OVS_INTERFACE_SETTING_NAME,
                           NM_SETTING_OVS_INTERFACE_TYPE);
            return FALSE;
        }
    }

    if (type) {
        NM_SET_OUT(out_normalized_type, type);
        return TRUE;
    }

    if (is_ovs_connection_type)
        type = "internal";
    else
        type = "system";

    NM_SET_OUT(out_normalized_type, type);

normalize:
    if (!normalize) {
        if (!self) {
            g_set_error(error,
                        NM_CONNECTION_ERROR,
                        NM_CONNECTION_ERROR_MISSING_SETTING,
                        _("Missing ovs interface setting"));
            g_prefix_error(error, "%s: ", NM_SETTING_OVS_INTERFACE_SETTING_NAME);
        } else {
            g_set_error(error,
                        NM_CONNECTION_ERROR,
                        NM_CONNECTION_ERROR_MISSING_PROPERTY,
                        _("Missing ovs interface type"));
            g_prefix_error(error,
                           "%s.%s: ",
                           NM_SETTING_OVS_INTERFACE_SETTING_NAME,
                           NM_SETTING_OVS_INTERFACE_TYPE);
        }
        return NM_SETTING_VERIFY_NORMALIZABLE_ERROR;
    }

    if (!self) {
        self = NM_SETTING_OVS_INTERFACE(nm_setting_ovs_interface_new());
        nm_connection_add_setting(connection, NM_SETTING(self));
    }
    g_object_set(self, NM_SETTING_OVS_INTERFACE_TYPE, type, NULL);
    NM_SET_OUT(out_modified, TRUE);

    return TRUE;
}

static int
verify(NMSetting *setting, NMConnection *connection, GError **error)
{
    NMSettingOvsInterface *self  = NM_SETTING_OVS_INTERFACE(setting);
    NMSettingConnection   *s_con = NULL;

    if (connection) {
        const char *slave_type;

        s_con = nm_connection_get_setting_connection(connection);
        if (!s_con) {
            g_set_error(error,
                        NM_CONNECTION_ERROR,
                        NM_CONNECTION_ERROR_MISSING_SETTING,
                        _("missing setting"));
            g_prefix_error(error, "%s: ", NM_SETTING_CONNECTION_SETTING_NAME);
            return FALSE;
        }

        if (!nm_setting_connection_get_master(s_con)) {
            g_set_error(error,
                        NM_CONNECTION_ERROR,
                        NM_CONNECTION_ERROR_INVALID_PROPERTY,
                        _("A connection with a '%s' setting must have a controller."),
                        NM_SETTING_OVS_INTERFACE_SETTING_NAME);
            g_prefix_error(error,
                           "%s.%s: ",
                           NM_SETTING_CONNECTION_SETTING_NAME,
                           NM_SETTING_CONNECTION_CONTROLLER);
            return FALSE;
        }

        slave_type = nm_setting_connection_get_port_type(s_con);
        if (slave_type && !nm_streq(slave_type, NM_SETTING_OVS_PORT_SETTING_NAME)) {
            g_set_error(error,
                        NM_CONNECTION_ERROR,
                        NM_CONNECTION_ERROR_INVALID_PROPERTY,
                        _("A connection with a '%s' setting must have the slave-type set to '%s'. "
                          "Instead it is '%s'"),
                        NM_SETTING_OVS_INTERFACE_SETTING_NAME,
                        NM_SETTING_OVS_PORT_SETTING_NAME,
                        slave_type);
            g_prefix_error(error,
                           "%s.%s: ",
                           NM_SETTING_CONNECTION_SETTING_NAME,
                           NM_SETTING_CONNECTION_PORT_TYPE);
            return FALSE;
        }
    }

    return _nm_setting_ovs_interface_verify_interface_type(self,
                                                           self->type,
                                                           connection,
                                                           FALSE,
                                                           NULL,
                                                           NULL,
                                                           error);
}

/*****************************************************************************/

static void
nm_setting_ovs_interface_init(NMSettingOvsInterface *self)
{}

/**
 * nm_setting_ovs_interface_new:
 *
 * Creates a new #NMSettingOvsInterface object with default values.
 *
 * Returns: (transfer full): the new empty #NMSettingOvsInterface object
 *
 * Since: 1.10
 **/
NMSetting *
nm_setting_ovs_interface_new(void)
{
    return g_object_new(NM_TYPE_SETTING_OVS_INTERFACE, NULL);
}

static void
nm_setting_ovs_interface_class_init(NMSettingOvsInterfaceClass *klass)
{
    GObjectClass   *object_class        = G_OBJECT_CLASS(klass);
    NMSettingClass *setting_class       = NM_SETTING_CLASS(klass);
    GArray         *properties_override = _nm_sett_info_property_override_create_array();

    object_class->get_property = _nm_setting_property_get_property_direct;
    object_class->set_property = _nm_setting_property_set_property_direct;

    setting_class->verify = verify;

    /**
     * NMSettingOvsInterface:type:
     *
     * The interface type. Either "internal", "system", "patch", "dpdk", or empty.
     *
     * Since: 1.10
     **/
    _nm_setting_property_define_direct_string(properties_override,
                                              obj_properties,
                                              NM_SETTING_OVS_INTERFACE_TYPE,
                                              PROP_TYPE,
                                              NM_SETTING_PARAM_INFERRABLE,
                                              NMSettingOvsInterface,
                                              type,
                                              .direct_string_allow_empty = TRUE);
    /**
     * NMSettingOvsInterface:ofport-request:
     *
     * Open vSwitch openflow port number.
     * Defaults to zero which means that port number will not be specified
     * and it will be chosen randomly by ovs. OpenFlow ports are the network interfaces
     * for passing packets between OpenFlow processing and the rest of the network.
     * OpenFlow switches connect logically to each other via their OpenFlow ports.
     *
     * Since: 1.42
     **/
    _nm_setting_property_define_direct_uint32(properties_override,
                                              obj_properties,
                                              NM_SETTING_OVS_INTERFACE_OFPORT_REQUEST,
                                              PROP_OFPORT_REQUEST,
                                              0,
                                              65279,
                                              0,
                                              NM_SETTING_PARAM_INFERRABLE,
                                              NMSettingOvsInterface,
                                              ofport_request);

    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST, obj_properties);

    _nm_setting_class_commit(setting_class,
                             NM_META_SETTING_TYPE_OVS_INTERFACE,
                             NULL,
                             properties_override,
                             0);
}
