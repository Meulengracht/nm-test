/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2011 Red Hat, Inc.
 * Copyright (C) 2013 Thomas Bechtold <thomasbechtold@jpberlin.de>
 */

#include "src/core/nm-default-daemon.h"

#include "nm-config-data.h"

#include "nm-config.h"
#include "devices/nm-device.h"
#include "libnm-core-intern/nm-core-internal.h"
#include "libnm-core-intern/nm-keyfile-internal.h"
#include "libnm-core-intern/nm-keyfile-utils.h"

/*****************************************************************************/

typedef struct {
    char    *group_name;
    gboolean stop_match;
    struct {
        /* have a separate boolean field @has, because a @spec with
         * value %NULL does not necessarily mean, that the property
         * "match-device" was unspecified. */
        gboolean has;
        GSList  *spec;
    } match_device;
    union {
        struct {
            GSList  *allowed_connections;
            gboolean allowed_connections_has;
        } device;
    };
    gboolean is_device;

    /* List of key/value pairs in the section, sorted by key */
    gsize                    lookup_len;
    const NMUtilsNamedValue *lookup_idx;
} MatchSectionInfo;

struct _NMGlobalDnsDomain {
    char  *name;
    char **servers;
    char **options;
};

struct _NMGlobalDnsConfig {
    char       **searches;
    char       **options;
    GHashTable  *domains;
    const char **domain_list;
    gboolean     internal;
};

/*****************************************************************************/

NM_GOBJECT_PROPERTIES_DEFINE_BASE(PROP_CONFIG_MAIN_FILE,
                                  PROP_CONFIG_DESCRIPTION,
                                  PROP_KEYFILE_USER,
                                  PROP_KEYFILE_INTERN,
                                  PROP_CONNECTIVITY_ENABLED,
                                  PROP_CONNECTIVITY_URI,
                                  PROP_CONNECTIVITY_INTERVAL,
                                  PROP_CONNECTIVITY_RESPONSE,
                                  PROP_NO_AUTO_DEFAULT, );

typedef struct {
    char *config_main_file;
    char *config_description;

    GKeyFile *keyfile;
    GKeyFile *keyfile_user;
    GKeyFile *keyfile_intern;

    /* A zero-terminated list of pre-processed information from the
     * [connection] sections. This is to speed up lookup. */
    MatchSectionInfo *connection_infos;

    /* A zero-terminated list of pre-processed information from the
     * [device] sections. This is to speed up lookup. */
    MatchSectionInfo *device_infos;

    struct {
        gboolean enabled;
        char    *uri;
        char    *response;
        guint    interval;
    } connectivity;

    int autoconnect_retries_default;

    struct {
        /* from /var/lib/NetworkManager/no-auto-default.state */
        char  **arr;
        GSList *specs;

        /* from main.no-auto-default setting in NetworkManager.conf. */
        GSList *specs_config;
    } no_auto_default;

    GSList *ignore_carrier;
    GSList *assume_ipv6ll_only;

    char *dns_mode;
    char *rc_manager;

    NMGlobalDnsConfig *global_dns;

    bool systemd_resolved : 1;

    char *iwd_config_path;
} NMConfigDataPrivate;

struct _NMConfigData {
    GObject             parent;
    NMConfigDataPrivate _priv;
};

struct _NMConfigDataClass {
    GObjectClass parent;
};

G_DEFINE_TYPE(NMConfigData, nm_config_data, G_TYPE_OBJECT)

#define NM_CONFIG_DATA_GET_PRIVATE(self) _NM_GET_PRIVATE(self, NMConfigData, NM_IS_CONFIG_DATA)

/*****************************************************************************/

static const char *
_match_section_info_get_str(const MatchSectionInfo *m, GKeyFile *keyfile, const char *property);

static const char *_config_data_get_device_config(const NMConfigData          *self,
                                                  const char                  *property,
                                                  const NMMatchSpecDeviceData *match_data,
                                                  NMDevice                    *device,
                                                  gboolean                    *has_match);

/*****************************************************************************/

const char *
nm_config_data_get_config_main_file(const NMConfigData *self)
{
    g_return_val_if_fail(self, NULL);

    return NM_CONFIG_DATA_GET_PRIVATE(self)->config_main_file;
}

const char *
nm_config_data_get_config_description(const NMConfigData *self)
{
    g_return_val_if_fail(self, NULL);

    return NM_CONFIG_DATA_GET_PRIVATE(self)->config_description;
}

gboolean
nm_config_data_has_group(const NMConfigData *self, const char *group)
{
    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), FALSE);
    g_return_val_if_fail(group && *group, FALSE);

    return g_key_file_has_group(NM_CONFIG_DATA_GET_PRIVATE(self)->keyfile, group);
}

char *
nm_config_data_get_value(const NMConfigData   *self,
                         const char           *group,
                         const char           *key,
                         NMConfigGetValueFlags flags)
{
    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), NULL);
    g_return_val_if_fail(group && *group, NULL);
    g_return_val_if_fail(key && *key, NULL);

    return nm_config_keyfile_get_value(NM_CONFIG_DATA_GET_PRIVATE(self)->keyfile,
                                       group,
                                       key,
                                       flags);
}

gboolean
nm_config_data_has_value(const NMConfigData   *self,
                         const char           *group,
                         const char           *key,
                         NMConfigGetValueFlags flags)
{
    gs_free char *value = NULL;

    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), FALSE);
    g_return_val_if_fail(group && *group, FALSE);
    g_return_val_if_fail(key && *key, FALSE);

    value =
        nm_config_keyfile_get_value(NM_CONFIG_DATA_GET_PRIVATE(self)->keyfile, group, key, flags);
    return !!value;
}

int
nm_config_data_get_value_boolean(const NMConfigData *self,
                                 const char         *group,
                                 const char         *key,
                                 int                 default_value)
{
    char *str;
    int   value = default_value;

    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), default_value);
    g_return_val_if_fail(group && *group, default_value);
    g_return_val_if_fail(key && *key, default_value);

    /* when parsing the boolean, base it on the raw value from g_key_file_get_value(). */
    str = nm_config_keyfile_get_value(NM_CONFIG_DATA_GET_PRIVATE(self)->keyfile,
                                      group,
                                      key,
                                      NM_CONFIG_GET_VALUE_RAW);
    if (str) {
        value = nm_config_parse_boolean(str, default_value);
        g_free(str);
    }
    return value;
}

gint64
nm_config_data_get_value_int64(const NMConfigData *self,
                               const char         *group,
                               const char         *key,
                               guint               base,
                               gint64              min,
                               gint64              max,
                               gint64              fallback)
{
    int    errsv;
    gint64 val;
    char  *str;

    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), fallback);
    g_return_val_if_fail(group && *group, fallback);
    g_return_val_if_fail(key && *key, fallback);

    str = nm_config_keyfile_get_value(NM_CONFIG_DATA_GET_PRIVATE(self)->keyfile,
                                      group,
                                      key,
                                      NM_CONFIG_GET_VALUE_NONE);
    val = _nm_utils_ascii_str_to_int64(str, base, min, max, fallback);
    if (str) {
        errsv = errno;
        g_free(str);
        errno = errsv;
    }
    return val;
}

char **
nm_config_data_get_plugins(const NMConfigData *self, gboolean allow_default)
{
    gs_free_error GError      *error = NULL;
    const NMConfigDataPrivate *priv;
    char                     **list;

    g_return_val_if_fail(self, NULL);

    priv = NM_CONFIG_DATA_GET_PRIVATE(self);

    list = g_key_file_get_string_list(priv->keyfile,
                                      NM_CONFIG_KEYFILE_GROUP_MAIN,
                                      "plugins",
                                      NULL,
                                      &error);
    if (nm_keyfile_error_is_not_found(error) && allow_default) {
        nm_auto_unref_keyfile GKeyFile *kf = nm_config_create_keyfile();

        /* let keyfile split the default string according to its own escaping rules. */
        g_key_file_set_value(kf,
                             NM_CONFIG_KEYFILE_GROUP_MAIN,
                             "plugins",
                             NM_CONFIG_DEFAULT_MAIN_PLUGINS);
        list = g_key_file_get_string_list(kf, NM_CONFIG_KEYFILE_GROUP_MAIN, "plugins", NULL, NULL);
    }
    return nm_strv_cleanup(list, TRUE, TRUE, TRUE);
}

gboolean
nm_config_data_get_connectivity_enabled(const NMConfigData *self)
{
    g_return_val_if_fail(self, FALSE);

    return NM_CONFIG_DATA_GET_PRIVATE(self)->connectivity.enabled;
}

const char *
nm_config_data_get_connectivity_uri(const NMConfigData *self)
{
    g_return_val_if_fail(self, NULL);

    return NM_CONFIG_DATA_GET_PRIVATE(self)->connectivity.uri;
}

guint
nm_config_data_get_connectivity_interval(const NMConfigData *self)
{
    g_return_val_if_fail(self, 0);

    return NM_CONFIG_DATA_GET_PRIVATE(self)->connectivity.interval;
}

const char *
nm_config_data_get_connectivity_response(const NMConfigData *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return NM_CONFIG_DATA_GET_PRIVATE(self)->connectivity.response;
}

int
nm_config_data_get_autoconnect_retries_default(const NMConfigData *self)
{
    g_return_val_if_fail(self, FALSE);

    return NM_CONFIG_DATA_GET_PRIVATE(self)->autoconnect_retries_default;
}

const char *const *
nm_config_data_get_no_auto_default(const NMConfigData *self)
{
    g_return_val_if_fail(self, FALSE);

    return (const char *const *) NM_CONFIG_DATA_GET_PRIVATE(self)->no_auto_default.arr;
}

gboolean
nm_config_data_get_no_auto_default_for_device(const NMConfigData *self, NMDevice *device)
{
    const NMConfigDataPrivate *priv;

    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), FALSE);
    g_return_val_if_fail(NM_IS_DEVICE(device), FALSE);

    priv = NM_CONFIG_DATA_GET_PRIVATE(self);
    return nm_device_spec_match_list(device, priv->no_auto_default.specs)
           || nm_device_spec_match_list(device, priv->no_auto_default.specs_config);
}

const char *
nm_config_data_get_dns_mode(const NMConfigData *self)
{
    g_return_val_if_fail(self, NULL);

    return NM_CONFIG_DATA_GET_PRIVATE(self)->dns_mode;
}

const char *
nm_config_data_get_rc_manager(const NMConfigData *self)
{
    g_return_val_if_fail(self, NULL);

    return NM_CONFIG_DATA_GET_PRIVATE(self)->rc_manager;
}

gboolean
nm_config_data_get_systemd_resolved(const NMConfigData *self)
{
    g_return_val_if_fail(self, FALSE);

    return NM_CONFIG_DATA_GET_PRIVATE(self)->systemd_resolved;
}

const char *
nm_config_data_get_iwd_config_path(const NMConfigData *self)
{
    return NM_CONFIG_DATA_GET_PRIVATE(self)->iwd_config_path;
}

gboolean
nm_config_data_get_ignore_carrier_for_port(const NMConfigData *self,
                                           const char         *master,
                                           const char         *slave_type)
{
    const char           *value;
    gboolean              has_match;
    int                   m;
    NMMatchSpecDeviceData match_data;

    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), FALSE);

    if (!master || !slave_type)
        goto out_default;

    if (!nm_utils_ifname_valid_kernel(master, NULL))
        goto out_default;

    match_data = (NMMatchSpecDeviceData){
        .interface_name = master,
        .device_type    = slave_type,
    };

    value = _config_data_get_device_config(self,
                                           NM_CONFIG_KEYFILE_KEY_DEVICE_IGNORE_CARRIER,
                                           &match_data,
                                           NULL,
                                           &has_match);
    if (has_match)
        m = nm_config_parse_boolean(value, -1);
    else {
        NMMatchSpecMatchType x;

        x = nm_match_spec_device(NM_CONFIG_DATA_GET_PRIVATE(self)->ignore_carrier, &match_data);
        m = nm_match_spec_match_type_to_bool(x, -1);
    }

    if (NM_IN_SET(m, TRUE, FALSE))
        return m;

out_default:
    /* if ignore-carrier is not explicitly or detected for the master, then we assume it's
     * enabled. This is in line with nm_config_data_get_ignore_carrier_by_device(), where
     * ignore-carrier is enabled based on nm_device_ignore_carrier_by_default().
     */
    return TRUE;
}

gboolean
nm_config_data_get_ignore_carrier_by_device(const NMConfigData *self, NMDevice *device)
{
    const char *value;
    gboolean    has_match;
    int         m;

    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), FALSE);
    g_return_val_if_fail(NM_IS_DEVICE(device), FALSE);

    value = nm_config_data_get_device_config_by_device(self,
                                                       NM_CONFIG_KEYFILE_KEY_DEVICE_IGNORE_CARRIER,
                                                       device,
                                                       &has_match);
    if (has_match)
        m = nm_config_parse_boolean(value, -1);
    else
        m = nm_device_spec_match_list_full(device,
                                           NM_CONFIG_DATA_GET_PRIVATE(self)->ignore_carrier,
                                           -1);

    if (NM_IN_SET(m, TRUE, FALSE))
        return m;

    /* if ignore-carrier is not explicitly configed, then it depends on the device (type). */
    return nm_device_ignore_carrier_by_default(device);
}

gboolean
nm_config_data_get_assume_ipv6ll_only(const NMConfigData *self, NMDevice *device)
{
    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), FALSE);
    g_return_val_if_fail(NM_IS_DEVICE(device), FALSE);

    return nm_device_spec_match_list(device, NM_CONFIG_DATA_GET_PRIVATE(self)->assume_ipv6ll_only);
}

GKeyFile *
nm_config_data_clone_keyfile_intern(const NMConfigData *self)
{
    const NMConfigDataPrivate *priv;
    GKeyFile                  *keyfile;

    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), FALSE);

    priv = NM_CONFIG_DATA_GET_PRIVATE(self);

    keyfile = nm_config_create_keyfile();
    if (priv->keyfile_intern)
        _nm_keyfile_copy(keyfile, priv->keyfile_intern);
    return keyfile;
}

GKeyFile *
_nm_config_data_get_keyfile(const NMConfigData *self)
{
    return NM_CONFIG_DATA_GET_PRIVATE(self)->keyfile;
}

GKeyFile *
_nm_config_data_get_keyfile_intern(const NMConfigData *self)
{
    return NM_CONFIG_DATA_GET_PRIVATE(self)->keyfile_intern;
}

GKeyFile *
_nm_config_data_get_keyfile_user(const NMConfigData *self)
{
    return NM_CONFIG_DATA_GET_PRIVATE(self)->keyfile_user;
}

/*****************************************************************************/

static NMAuthPolkitMode
nm_auth_polkit_mode_from_string(const char *str)
{
    int as_bool;

    if (!str)
        return NM_AUTH_POLKIT_MODE_UNKNOWN;

    if (nm_streq(str, "root-only"))
        return NM_AUTH_POLKIT_MODE_ROOT_ONLY;

    as_bool = _nm_utils_ascii_str_to_bool(str, -1);
    if (as_bool != -1) {
        return as_bool ? NM_AUTH_POLKIT_MODE_USE_POLKIT : NM_AUTH_POLKIT_MODE_ALLOW_ALL;
    }

    return NM_AUTH_POLKIT_MODE_UNKNOWN;
}

static NMAuthPolkitMode
_config_data_get_main_auth_polkit(const NMConfigData *self, gboolean *out_invalid_config)
{
    NMAuthPolkitMode auth_polkit_mode;
    gs_free char    *str = NULL;

    str              = nm_config_data_get_value(self,
                                   NM_CONFIG_KEYFILE_GROUP_MAIN,
                                   NM_CONFIG_KEYFILE_KEY_MAIN_AUTH_POLKIT,
                                   NM_CONFIG_GET_VALUE_STRIP | NM_CONFIG_GET_VALUE_NO_EMPTY);
    auth_polkit_mode = nm_auth_polkit_mode_from_string(str);
    if (auth_polkit_mode == NM_AUTH_POLKIT_MODE_UNKNOWN) {
        NM_SET_OUT(out_invalid_config, (str != NULL));
        auth_polkit_mode = nm_auth_polkit_mode_from_string(NM_CONFIG_DEFAULT_MAIN_AUTH_POLKIT);
        if (auth_polkit_mode == NM_AUTH_POLKIT_MODE_UNKNOWN) {
            nm_assert_not_reached();
            auth_polkit_mode = NM_AUTH_POLKIT_MODE_ROOT_ONLY;
        }
    } else
        NM_SET_OUT(out_invalid_config, FALSE);

    return auth_polkit_mode;
}

NMAuthPolkitMode
nm_config_data_get_main_auth_polkit(const NMConfigData *self)
{
    return _config_data_get_main_auth_polkit(self, NULL);
}

/*****************************************************************************/

/**
 * nm_config_data_get_groups:
 * @self: the #NMConfigData instance
 *
 * Returns: (transfer full): the list of groups in the configuration. The order
 * of the section is undefined, as the configuration gets merged from multiple
 * sources.
 */
char **
nm_config_data_get_groups(const NMConfigData *self)
{
    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), NULL);

    return g_key_file_get_groups(NM_CONFIG_DATA_GET_PRIVATE(self)->keyfile, NULL);
}

char **
nm_config_data_get_keys(const NMConfigData *self, const char *group)
{
    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), NULL);
    g_return_val_if_fail(group && *group, NULL);

    return g_key_file_get_keys(NM_CONFIG_DATA_GET_PRIVATE(self)->keyfile, group, NULL, NULL);
}

/**
 * nm_config_data_is_intern_atomic_group:
 * @self:
 * @group: name of the group to check.
 *
 * whether a configuration group @group exists and is entirely overwritten
 * by internal configuration, i.e. whether it is an atomic group that is
 * overwritten.
 *
 * It doesn't say, that there actually is a user setting that was overwritten. That
 * means there could be no corresponding section defined in user configuration
 * that required overwriting.
 *
 * Returns: %TRUE if @group exists and is an atomic group set via internal configuration.
 */
gboolean
nm_config_data_is_intern_atomic_group(const NMConfigData *self, const char *group)
{
    const NMConfigDataPrivate *priv;

    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), FALSE);
    g_return_val_if_fail(group && *group, FALSE);

    priv = NM_CONFIG_DATA_GET_PRIVATE(self);

    if (!priv->keyfile_intern
        || !g_key_file_has_key(priv->keyfile_intern,
                               group,
                               NM_CONFIG_KEYFILE_KEY_ATOMIC_SECTION_WAS,
                               NULL))
        return FALSE;

    /* we have a .was entry for the section. That means that the section would be overwritten
     * from user configuration. But it doesn't mean that the merged configuration contains this
     * groups, because the internal setting could hide the user section.
     * Only return TRUE, if we actually have such a group in the merged configuration.*/
    return g_key_file_has_group(priv->keyfile, group);
}

/*****************************************************************************/

static GKeyFile *
_merge_keyfiles(GKeyFile *keyfile_user, GKeyFile *keyfile_intern)
{
    gs_strfreev char **groups = NULL;
    guint              g, k;
    GKeyFile          *keyfile;
    gsize              ngroups;

    keyfile = nm_config_create_keyfile();
    if (keyfile_user)
        _nm_keyfile_copy(keyfile, keyfile_user);
    if (!keyfile_intern)
        return keyfile;

    groups = g_key_file_get_groups(keyfile_intern, &ngroups);
    if (!groups)
        return keyfile;

    /* we must reverse the order of the connection settings so that we
     * have lowest priority last. */
    _nm_config_sort_groups(groups, ngroups);
    for (g = 0; groups[g]; g++) {
        const char        *group = groups[g];
        gs_strfreev char **keys  = NULL;
        gboolean           is_intern, is_atomic = FALSE;

        keys = g_key_file_get_keys(keyfile_intern, group, NULL, NULL);
        if (!keys)
            continue;

        is_intern = NM_STR_HAS_PREFIX(group, NM_CONFIG_KEYFILE_GROUPPREFIX_INTERN);
        if (!is_intern
            && g_key_file_has_key(keyfile_intern,
                                  group,
                                  NM_CONFIG_KEYFILE_KEY_ATOMIC_SECTION_WAS,
                                  NULL)) {
            /* the entire section is atomically overwritten by @keyfile_intern. */
            g_key_file_remove_group(keyfile, group, NULL);
            is_atomic = TRUE;
        }

        for (k = 0; keys[k]; k++) {
            const char   *key   = keys[k];
            gs_free char *value = NULL;

            if (is_atomic && nm_streq(key, NM_CONFIG_KEYFILE_KEY_ATOMIC_SECTION_WAS))
                continue;

            if (!is_intern && !is_atomic
                && NM_STR_HAS_PREFIX_WITH_MORE(key, NM_CONFIG_KEYFILE_KEYPREFIX_WAS)) {
                const char *key_base = &key[NM_STRLEN(NM_CONFIG_KEYFILE_KEYPREFIX_WAS)];

                if (!g_key_file_has_key(keyfile_intern, group, key_base, NULL))
                    g_key_file_remove_key(keyfile, group, key_base, NULL);
                continue;
            }
            if (!is_intern && !is_atomic
                && NM_STR_HAS_PREFIX_WITH_MORE(key, NM_CONFIG_KEYFILE_KEYPREFIX_SET))
                continue;

            value = g_key_file_get_value(keyfile_intern, group, key, NULL);
            g_key_file_set_value(keyfile, group, key, value);
        }
    }
    return keyfile;
}

/*****************************************************************************/

static int
_nm_config_data_log_sort(const char **pa, const char **pb, gpointer dummy)
{
    gboolean    a_is_connection, b_is_connection;
    gboolean    a_is_device, b_is_device;
    gboolean    a_is_intern, b_is_intern;
    gboolean    a_is_main, b_is_main;
    const char *a = *pa;
    const char *b = *pb;

    nm_assert(a && b && !nm_streq(a, b));

    /* we sort intern groups to the end. */
    a_is_intern = NM_STR_HAS_PREFIX(a, NM_CONFIG_KEYFILE_GROUPPREFIX_INTERN);
    b_is_intern = NM_STR_HAS_PREFIX(b, NM_CONFIG_KEYFILE_GROUPPREFIX_INTERN);

    if (a_is_intern && b_is_intern)
        return 0;
    if (a_is_intern)
        return 1;
    if (b_is_intern)
        return -1;

    /* we sort connection groups before intern groups (to the end). */
    a_is_connection = NM_STR_HAS_PREFIX(a, NM_CONFIG_KEYFILE_GROUPPREFIX_CONNECTION);
    b_is_connection = NM_STR_HAS_PREFIX(b, NM_CONFIG_KEYFILE_GROUPPREFIX_CONNECTION);

    if (a_is_connection && b_is_connection) {
        /* if both are connection groups, we want the explicit [connection] group first. */
        a_is_connection = a[NM_STRLEN(NM_CONFIG_KEYFILE_GROUPPREFIX_CONNECTION)] == '\0';
        b_is_connection = b[NM_STRLEN(NM_CONFIG_KEYFILE_GROUPPREFIX_CONNECTION)] == '\0';

        if (a_is_connection != b_is_connection) {
            if (a_is_connection)
                return -1;
            return 1;
        }
        /* the sections are ordered lowest-priority first. Reverse their order. */
        return pa < pb ? 1 : -1;
    }
    if (a_is_connection && !b_is_connection)
        return 1;
    if (b_is_connection && !a_is_connection)
        return -1;

    /* we sort device groups before connection groups (to the end). */
    a_is_device = NM_STR_HAS_PREFIX(a, NM_CONFIG_KEYFILE_GROUPPREFIX_DEVICE);
    b_is_device = NM_STR_HAS_PREFIX(b, NM_CONFIG_KEYFILE_GROUPPREFIX_DEVICE);

    if (a_is_device && b_is_device) {
        /* if both are device groups, we want the explicit [device] group first. */
        a_is_device = a[NM_STRLEN(NM_CONFIG_KEYFILE_GROUPPREFIX_DEVICE)] == '\0';
        b_is_device = b[NM_STRLEN(NM_CONFIG_KEYFILE_GROUPPREFIX_DEVICE)] == '\0';

        if (a_is_device != b_is_device) {
            if (a_is_device)
                return -1;
            return 1;
        }
        /* the sections are ordered lowest-priority first. Reverse their order. */
        return pa < pb ? 1 : -1;
    }
    if (a_is_device && !b_is_device)
        return 1;
    if (b_is_device && !a_is_device)
        return -1;

    a_is_main = nm_streq0(a, "main");
    b_is_main = nm_streq0(b, "main");
    if (a_is_main != b_is_main)
        return a_is_main ? -1 : 1;

    return g_strcmp0(a, b);
}

static const struct {
    const char *group;
    const char *key;
    const char *value;
} default_values[] = {
    {NM_CONFIG_KEYFILE_GROUP_MAIN, "plugins", NM_CONFIG_DEFAULT_MAIN_PLUGINS},
    {NM_CONFIG_KEYFILE_GROUP_MAIN, "rc-manager", NM_CONFIG_DEFAULT_MAIN_RC_MANAGER},
    {NM_CONFIG_KEYFILE_GROUP_MAIN, "migrate-ifcfg-rh", NM_CONFIG_DEFAULT_MAIN_MIGRATE_IFCFG_RH},
    {NM_CONFIG_KEYFILE_GROUP_MAIN,
     NM_CONFIG_KEYFILE_KEY_MAIN_AUTH_POLKIT,
     NM_CONFIG_DEFAULT_MAIN_AUTH_POLKIT},
    {NM_CONFIG_KEYFILE_GROUP_MAIN, NM_CONFIG_KEYFILE_KEY_MAIN_DHCP, NM_CONFIG_DEFAULT_MAIN_DHCP},
    {NM_CONFIG_KEYFILE_GROUP_MAIN, NM_CONFIG_KEYFILE_KEY_MAIN_IWD_CONFIG_PATH, ""},
    {NM_CONFIG_KEYFILE_GROUP_LOGGING, "backend", NM_CONFIG_DEFAULT_LOGGING_BACKEND},
    {NM_CONFIG_KEYFILE_GROUP_LOGGING, "audit", NM_CONFIG_DEFAULT_LOGGING_AUDIT},
    {NM_CONFIG_KEYFILE_GROUPPREFIX_DEVICE,
     NM_CONFIG_KEYFILE_KEY_DEVICE_WIFI_BACKEND,
     NM_CONFIG_DEFAULT_WIFI_BACKEND},
};

void
nm_config_data_log(const NMConfigData  *self,
                   const char          *prefix,
                   const char          *key_prefix,
                   const char          *no_auto_default_file,
                   /* FILE* */ gpointer print_stream)
{
    const NMConfigDataPrivate   *priv;
    gs_strfreev char           **groups = NULL;
    gsize                        ngroups;
    guint                        g, k, i;
    FILE                        *stream        = print_stream;
    gs_unref_ptrarray GPtrArray *groups_full   = NULL;
    gboolean                     print_default = !!stream;

    g_return_if_fail(NM_IS_CONFIG_DATA(self));

    if (!stream && !nm_logging_enabled(LOGL_DEBUG, LOGD_CORE))
        return;

    if (!prefix)
        prefix = "";
    if (!key_prefix)
        key_prefix = "";

#define _LOG(stream, prefix, ...)                                \
    G_STMT_START                                                 \
    {                                                            \
        if (!stream)                                             \
            _nm_log(LOGL_DEBUG,                                  \
                    LOGD_CORE,                                   \
                    0,                                           \
                    NULL,                                        \
                    NULL,                                        \
                    "%s"_NM_UTILS_MACRO_FIRST(__VA_ARGS__) "%s", \
                    prefix _NM_UTILS_MACRO_REST(__VA_ARGS__),    \
                    "");                                         \
        else                                                     \
            fprintf(stream,                                      \
                    "%s"_NM_UTILS_MACRO_FIRST(__VA_ARGS__) "%s", \
                    prefix _NM_UTILS_MACRO_REST(__VA_ARGS__),    \
                    "\n");                                       \
    }                                                            \
    G_STMT_END

    priv = NM_CONFIG_DATA_GET_PRIVATE(self);

    groups = g_key_file_get_groups(priv->keyfile, &ngroups);
    if (!groups)
        ngroups = 0;

    groups_full = g_ptr_array_sized_new(ngroups + 5);

    if (ngroups) {
        /* g_key_file_get_groups() can return duplicates ( :( ), but the
         * keyfile that we constructed should not have any. Assert for that. */
        nm_assert(!nm_strv_has_duplicate((const char *const *) groups, ngroups, FALSE));

        g_ptr_array_set_size(groups_full, ngroups);
        memcpy(groups_full->pdata, groups, sizeof(groups[0]) * ngroups);
    }

    if (print_default) {
        for (g = 0; g < G_N_ELEMENTS(default_values); g++) {
            const char *group = default_values[g].group;
            guint       g2;

            if (g > 0) {
                if (nm_streq(group, default_values[g - 1].group)) {
                    /* Repeated values. We already added this one. Skip */
                    continue;
                }
                if (NM_MORE_ASSERT_ONCE(20)) {
                    /* We require that the default values are grouped by their "group".
                     * That is, all default values for a certain "group" are close to
                     * each other in the list. Assert for that. */
                    for (g2 = g + 1; g2 < groups_full->len; g2++) {
                        nm_assert(!nm_streq(default_values[g - 1].group, default_values[g2].group));
                    }
                }
            }

            for (g2 = 0; g2 < groups_full->len; g2++) {
                if (nm_streq(group, groups_full->pdata[g2]))
                    goto next;
            }

            g_ptr_array_add(groups_full, (gpointer) group);

next:
            (void) 0;
        }
    }

    g_ptr_array_sort_with_data(groups_full, (GCompareDataFunc) _nm_config_data_log_sort, NULL);

    if (!stream)
        _LOG(stream, prefix, "config-data[%p]: %u groups", self, groups_full->len);

    for (g = 0; g < groups_full->len; g++) {
        const char        *group = groups_full->pdata[g];
        gs_strfreev char **keys  = NULL;
        gboolean           is_atomic;

        is_atomic = nm_config_data_is_intern_atomic_group(self, group);

        _LOG(stream, prefix, "");
        _LOG(stream, prefix, "[%s]%s", group, is_atomic && !stream ? " # atomic section" : "");

        /* Print default values as comments */
        if (print_default) {
            for (i = 0; i < G_N_ELEMENTS(default_values); i++) {
                if (nm_streq(default_values[i].group, group)
                    && !g_key_file_has_key(priv->keyfile, group, default_values[i].key, NULL)) {
                    _LOG(stream,
                         prefix,
                         "%s# %s=%s",
                         key_prefix,
                         default_values[i].key,
                         default_values[i].value);
                }
            }
        }

        keys = g_key_file_get_keys(priv->keyfile, group, NULL, NULL);
        for (k = 0; keys && keys[k]; k++) {
            const char   *key   = keys[k];
            gs_free char *value = NULL;

            value = g_key_file_get_value(priv->keyfile, group, key, NULL);
            _LOG(stream, prefix, "%s%s=%s", key_prefix, key, value);
        }
    }

    _LOG(stream, prefix, "");
    _LOG(stream, prefix, "# no-auto-default file \"%s\"", no_auto_default_file);
    {
        gs_free char *msg = NULL;

        msg = nm_utils_g_slist_strlist_join(priv->no_auto_default.specs, ",");
        if (msg)
            _LOG(stream, prefix, "# no-auto-default specs \"%s\"", msg);
    }

    if (nm_config_kernel_command_line_nm_debug()) {
        _LOG(stream,
             prefix,
             "# /proc/cmdline contains \"" NM_CONFIG_KERNEL_CMDLINE_NM_DEBUG
             "\". Debug log enabled");
    }

#undef _LOG
}

/*****************************************************************************/

const char *const *
nm_global_dns_config_get_searches(const NMGlobalDnsConfig *dns_config)
{
    g_return_val_if_fail(dns_config, NULL);

    return (const char *const *) dns_config->searches;
}

const char *const *
nm_global_dns_config_get_options(const NMGlobalDnsConfig *dns_config)
{
    g_return_val_if_fail(dns_config, NULL);

    return (const char *const *) dns_config->options;
}

guint
nm_global_dns_config_get_num_domains(const NMGlobalDnsConfig *dns_config)
{
    g_return_val_if_fail(dns_config, 0);

    return dns_config->domains ? g_hash_table_size(dns_config->domains) : 0;
}

NMGlobalDnsDomain *
nm_global_dns_config_get_domain(const NMGlobalDnsConfig *dns_config, guint i)
{
    NMGlobalDnsDomain *domain;

    g_return_val_if_fail(dns_config, NULL);
    g_return_val_if_fail(dns_config->domains, NULL);
    g_return_val_if_fail(i < g_hash_table_size(dns_config->domains), NULL);

    nm_assert(NM_PTRARRAY_LEN(dns_config->domain_list) == g_hash_table_size(dns_config->domains));

    domain = g_hash_table_lookup(dns_config->domains, dns_config->domain_list[i]);

    nm_assert(domain);
    return domain;
}

NMGlobalDnsDomain *
nm_global_dns_config_lookup_domain(const NMGlobalDnsConfig *dns_config, const char *name)
{
    g_return_val_if_fail(dns_config, NULL);
    g_return_val_if_fail(name, NULL);

    return dns_config->domains ? g_hash_table_lookup(dns_config->domains, name) : NULL;
}

const char *
nm_global_dns_domain_get_name(const NMGlobalDnsDomain *domain)
{
    g_return_val_if_fail(domain, NULL);

    return (const char *) domain->name;
}

const char *const *
nm_global_dns_domain_get_servers(const NMGlobalDnsDomain *domain)
{
    g_return_val_if_fail(domain, NULL);

    return (const char *const *) domain->servers;
}

const char *const *
nm_global_dns_domain_get_options(const NMGlobalDnsDomain *domain)
{
    g_return_val_if_fail(domain, NULL);

    return (const char *const *) domain->options;
}

gboolean
nm_global_dns_config_is_internal(const NMGlobalDnsConfig *dns_config)
{
    return dns_config->internal;
}

gboolean
nm_global_dns_config_is_empty(const NMGlobalDnsConfig *dns_config)
{
    g_return_val_if_fail(dns_config, TRUE);

    return !dns_config->searches && !dns_config->options && !dns_config->domain_list;
}

int
nm_global_dns_config_cmp(const NMGlobalDnsConfig *a,
                         const NMGlobalDnsConfig *b,
                         gboolean                 check_internal)
{
    guint i;

    NM_CMP_SELF(a, b);

    NM_CMP_RETURN(
        nm_strv_cmp_n(a->searches ?: NM_STRV_EMPTY(), -1, b->searches ?: NM_STRV_EMPTY(), -1));

    NM_CMP_RETURN(
        nm_strv_cmp_n(a->options ?: NM_STRV_EMPTY(), -1, b->options ?: NM_STRV_EMPTY(), -1));

    NM_CMP_RETURN(nm_strv_cmp_n(a->domain_list ?: NM_STRV_EMPTY_CC(),
                                -1,
                                b->domain_list ?: NM_STRV_EMPTY_CC(),
                                -1));

    if (a->domain_list) {
        for (i = 0; a->domain_list[i]; i++) {
            const NMGlobalDnsDomain *domain_a;
            const NMGlobalDnsDomain *domain_b;

            nm_assert(nm_streq(a->domain_list[i], b->domain_list[i]));

            domain_a = g_hash_table_lookup(a->domains, a->domain_list[i]);
            nm_assert(domain_a);

            domain_b = g_hash_table_lookup(b->domains, b->domain_list[i]);
            nm_assert(domain_b);

            NM_CMP_FIELD_STR0(domain_a, domain_b, name);

            NM_CMP_RETURN(nm_strv_cmp_n(domain_a->servers ?: NM_STRV_EMPTY(),
                                        -1,
                                        domain_b->servers ?: NM_STRV_EMPTY(),
                                        -1));

            NM_CMP_RETURN(nm_strv_cmp_n(domain_a->options ?: NM_STRV_EMPTY(),
                                        -1,
                                        domain_b->options ?: NM_STRV_EMPTY(),
                                        -1));
        }
    }

    if (check_internal)
        NM_CMP_FIELD(a, b, internal);

    return 0;
}

void
nm_global_dns_config_update_checksum(const NMGlobalDnsConfig *dns_config, GChecksum *sum)
{
    NMGlobalDnsDomain *domain;
    guint              i, j;
    guint8             v8;

    g_return_if_fail(dns_config);
    g_return_if_fail(sum);

    v8 = NM_HASH_COMBINE_BOOLS(guint8,
                               !dns_config->searches,
                               !dns_config->options,
                               !dns_config->domain_list);
    g_checksum_update(sum, (guchar *) &v8, 1);

    if (dns_config->searches) {
        for (i = 0; dns_config->searches[i]; i++)
            g_checksum_update(sum,
                              (guchar *) dns_config->searches[i],
                              strlen(dns_config->searches[i]) + 1);
    }
    if (dns_config->options) {
        for (i = 0; dns_config->options[i]; i++)
            g_checksum_update(sum,
                              (guchar *) dns_config->options[i],
                              strlen(dns_config->options[i]) + 1);
    }

    if (dns_config->domain_list) {
        for (i = 0; dns_config->domain_list[i]; i++) {
            domain = g_hash_table_lookup(dns_config->domains, dns_config->domain_list[i]);
            nm_assert(domain);

            v8 = NM_HASH_COMBINE_BOOLS(guint8, !domain->servers, !domain->options);
            g_checksum_update(sum, (guchar *) &v8, 1);

            g_checksum_update(sum, (guchar *) domain->name, strlen(domain->name) + 1);

            if (domain->servers) {
                for (j = 0; domain->servers[j]; j++)
                    g_checksum_update(sum,
                                      (guchar *) domain->servers[j],
                                      strlen(domain->servers[j]) + 1);
            }
            if (domain->options) {
                for (j = 0; domain->options[j]; j++)
                    g_checksum_update(sum,
                                      (guchar *) domain->options[j],
                                      strlen(domain->options[j]) + 1);
            }
        }
    }
}

static void
global_dns_domain_free(NMGlobalDnsDomain *domain)
{
    if (domain) {
        g_free(domain->name);
        g_strfreev(domain->servers);
        g_strfreev(domain->options);
        g_free(domain);
    }
}

void
nm_global_dns_config_free(NMGlobalDnsConfig *dns_config)
{
    if (dns_config) {
        g_strfreev(dns_config->searches);
        g_strfreev(dns_config->options);
        g_free(dns_config->domain_list);
        if (dns_config->domains)
            g_hash_table_unref(dns_config->domains);
        g_free(dns_config);
    }
}

NMGlobalDnsConfig *
nm_config_data_get_global_dns_config(const NMConfigData *self)
{
    g_return_val_if_fail(NM_IS_CONFIG_DATA(self), NULL);

    return NM_CONFIG_DATA_GET_PRIVATE(self)->global_dns;
}

static void
global_dns_config_seal_domains(NMGlobalDnsConfig *dns_config)
{
    nm_assert(dns_config);
    nm_assert(dns_config->domains);
    nm_assert(!dns_config->domain_list);

    if (g_hash_table_size(dns_config->domains) == 0)
        nm_clear_pointer(&dns_config->domains, g_hash_table_unref);
    else
        dns_config->domain_list = nm_strdict_get_keys(dns_config->domains, TRUE, NULL);
}

static NMGlobalDnsConfig *
load_global_dns(GKeyFile *keyfile, gboolean internal)
{
    NMGlobalDnsConfig *dns_config;
    char              *group, *domain_prefix;
    gs_strfreev char **groups = NULL;
    int                g, i, j, domain_prefix_len;
    gboolean           default_found = FALSE;
    char             **strv;

    if (internal) {
        group         = NM_CONFIG_KEYFILE_GROUP_INTERN_GLOBAL_DNS;
        domain_prefix = NM_CONFIG_KEYFILE_GROUPPREFIX_INTERN_GLOBAL_DNS_DOMAIN;
    } else {
        group         = NM_CONFIG_KEYFILE_GROUP_GLOBAL_DNS;
        domain_prefix = NM_CONFIG_KEYFILE_GROUPPREFIX_GLOBAL_DNS_DOMAIN;
    }
    domain_prefix_len = strlen(domain_prefix);

    if (!nm_config_keyfile_has_global_dns_config(keyfile, internal))
        return NULL;

    dns_config          = g_malloc0(sizeof(NMGlobalDnsConfig));
    dns_config->domains = g_hash_table_new_full(nm_str_hash,
                                                g_str_equal,
                                                g_free,
                                                (GDestroyNotify) global_dns_domain_free);

    strv = g_key_file_get_string_list(keyfile,
                                      group,
                                      NM_CONFIG_KEYFILE_KEY_GLOBAL_DNS_SEARCHES,
                                      NULL,
                                      NULL);
    if (strv) {
        nm_strv_cleanup(strv, TRUE, TRUE, TRUE);
        if (!strv[0])
            g_free(strv);
        else
            dns_config->searches = strv;
    }

    strv = g_key_file_get_string_list(keyfile,
                                      group,
                                      NM_CONFIG_KEYFILE_KEY_GLOBAL_DNS_OPTIONS,
                                      NULL,
                                      NULL);
    if (strv) {
        nm_strv_cleanup(strv, TRUE, TRUE, TRUE);
        for (i = 0, j = 0; strv[i]; i++) {
            if (_nm_utils_dns_option_validate(strv[i], NULL, NULL, AF_UNSPEC, NULL))
                strv[j++] = strv[i];
            else
                g_free(strv[i]);
        }
        if (j == 0)
            g_free(strv);
        else {
            strv[j]             = NULL;
            dns_config->options = strv;
        }
    }

    groups = g_key_file_get_groups(keyfile, NULL);
    for (g = 0; groups[g]; g++) {
        char              *name;
        char             **servers = NULL, **options = NULL;
        NMGlobalDnsDomain *domain;

        if (!g_str_has_prefix(groups[g], domain_prefix) || !groups[g][domain_prefix_len])
            continue;

        strv = g_key_file_get_string_list(keyfile,
                                          groups[g],
                                          NM_CONFIG_KEYFILE_KEY_GLOBAL_DNS_DOMAIN_SERVERS,
                                          NULL,
                                          NULL);
        if (strv) {
            nm_strv_cleanup(strv, TRUE, TRUE, TRUE);
            for (i = 0, j = 0; strv[i]; i++) {
                if (nm_inet_is_valid(AF_INET, strv[i]) || nm_inet_is_valid(AF_INET6, strv[i]))
                    strv[j++] = strv[i];
                else
                    g_free(strv[i]);
            }
            if (j == 0)
                g_free(strv);
            else {
                strv[j] = NULL;
                servers = strv;
            }
        }

        if (!servers)
            continue;

        strv = g_key_file_get_string_list(keyfile,
                                          groups[g],
                                          NM_CONFIG_KEYFILE_KEY_GLOBAL_DNS_DOMAIN_OPTIONS,
                                          NULL,
                                          NULL);
        if (strv) {
            options = nm_strv_cleanup(strv, TRUE, TRUE, TRUE);
            if (!options[0])
                nm_clear_g_free(&options);
        }

        name            = strdup(&groups[g][domain_prefix_len]);
        domain          = g_malloc0(sizeof(NMGlobalDnsDomain));
        domain->name    = name;
        domain->servers = servers;
        domain->options = options;

        g_hash_table_insert(dns_config->domains, strdup(name), domain);

        if (name[0] == '*' && name[1] == '\0')
            default_found = TRUE;
    }

    if (!default_found && g_hash_table_size(dns_config->domains)) {
        nm_log_dbg(LOGD_CORE,
                   "%s global DNS configuration is missing default domain, ignore it",
                   internal ? "internal" : "user");
        nm_global_dns_config_free(dns_config);
        return NULL;
    }

    dns_config->internal = internal;
    global_dns_config_seal_domains(dns_config);
    return dns_config;
}

void
nm_global_dns_config_to_dbus(const NMGlobalDnsConfig *dns_config, GValue *value)
{
    GVariantBuilder conf_builder, domains_builder, domain_builder;
    guint           i;

    g_variant_builder_init(&conf_builder, G_VARIANT_TYPE("a{sv}"));
    if (!dns_config)
        goto out;

    if (dns_config->searches) {
        g_variant_builder_add(&conf_builder,
                              "{sv}",
                              "searches",
                              g_variant_new_strv((const char *const *) dns_config->searches, -1));
    }

    if (dns_config->options) {
        g_variant_builder_add(&conf_builder,
                              "{sv}",
                              "options",
                              g_variant_new_strv((const char *const *) dns_config->options, -1));
    }

    g_variant_builder_init(&domains_builder, G_VARIANT_TYPE("a{sv}"));
    if (dns_config->domain_list) {
        for (i = 0; dns_config->domain_list[i]; i++) {
            NMGlobalDnsDomain *domain;

            domain = g_hash_table_lookup(dns_config->domains, dns_config->domain_list[i]);

            g_variant_builder_init(&domain_builder, G_VARIANT_TYPE("a{sv}"));

            if (domain->servers) {
                g_variant_builder_add(
                    &domain_builder,
                    "{sv}",
                    "servers",
                    g_variant_new_strv((const char *const *) domain->servers, -1));
            }
            if (domain->options) {
                g_variant_builder_add(
                    &domain_builder,
                    "{sv}",
                    "options",
                    g_variant_new_strv((const char *const *) domain->options, -1));
            }

            g_variant_builder_add(&domains_builder,
                                  "{sv}",
                                  domain->name,
                                  g_variant_builder_end(&domain_builder));
        }
    }
    g_variant_builder_add(&conf_builder,
                          "{sv}",
                          "domains",
                          g_variant_builder_end(&domains_builder));

out:
    g_value_take_variant(value, g_variant_builder_end(&conf_builder));
}

static NMGlobalDnsDomain *
global_dns_domain_from_dbus(char *name, GVariant *variant)
{
    NMGlobalDnsDomain *domain;
    GVariantIter       iter;
    char             **strv, *key;
    GVariant          *val;
    int                i, j;

    if (!g_variant_is_of_type(variant, G_VARIANT_TYPE("a{sv}")))
        return NULL;

    domain       = g_malloc0(sizeof(NMGlobalDnsDomain));
    domain->name = g_strdup(name);

    g_variant_iter_init(&iter, variant);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &val)) {
        if (nm_streq0(key, "servers") && g_variant_is_of_type(val, G_VARIANT_TYPE("as"))) {
            strv = g_variant_dup_strv(val, NULL);
            nm_strv_cleanup(strv, TRUE, TRUE, TRUE);
            for (i = 0, j = 0; strv && strv[i]; i++) {
                if (nm_inet_is_valid(AF_INET, strv[i]) || nm_inet_is_valid(AF_INET6, strv[i]))
                    strv[j++] = strv[i];
                else
                    g_free(strv[i]);
            }
            if (j == 0)
                g_free(strv);
            else {
                strv[j] = NULL;
                g_strfreev(domain->servers);
                domain->servers = strv;
            }
        } else if (nm_streq0(key, "options") && g_variant_is_of_type(val, G_VARIANT_TYPE("as"))) {
            strv = g_variant_dup_strv(val, NULL);
            g_strfreev(domain->options);
            domain->options = nm_strv_cleanup(strv, TRUE, TRUE, TRUE);
            if (!domain->options[0])
                nm_clear_g_free(&domain->options);
        }

        g_variant_unref(val);
    }

    /* At least one server is required */
    if (!domain->servers) {
        global_dns_domain_free(domain);
        return NULL;
    }

    return domain;
}

NMGlobalDnsConfig *
nm_global_dns_config_from_dbus(const GValue *value, GError **error)
{
    NMGlobalDnsConfig *dns_config;
    GVariant          *variant, *val;
    GVariantIter       iter;
    char             **strv, *key;
    int                i, j;

    if (!G_VALUE_HOLDS_VARIANT(value)) {
        g_set_error(error, NM_MANAGER_ERROR, NM_MANAGER_ERROR_FAILED, "invalid value type");
        return NULL;
    }

    variant = g_value_get_variant(value);
    if (!g_variant_is_of_type(variant, G_VARIANT_TYPE("a{sv}"))) {
        g_set_error(error, NM_MANAGER_ERROR, NM_MANAGER_ERROR_FAILED, "invalid variant type");
        return NULL;
    }

    dns_config          = g_malloc0(sizeof(NMGlobalDnsConfig));
    dns_config->domains = g_hash_table_new_full(nm_str_hash,
                                                g_str_equal,
                                                g_free,
                                                (GDestroyNotify) global_dns_domain_free);

    g_variant_iter_init(&iter, variant);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &val)) {
        if (nm_streq0(key, "searches") && g_variant_is_of_type(val, G_VARIANT_TYPE("as"))) {
            strv                 = g_variant_dup_strv(val, NULL);
            dns_config->searches = nm_strv_cleanup(strv, TRUE, TRUE, TRUE);
        } else if (nm_streq0(key, "options") && g_variant_is_of_type(val, G_VARIANT_TYPE("as"))) {
            strv = g_variant_dup_strv(val, NULL);
            nm_strv_cleanup(strv, TRUE, TRUE, TRUE);

            for (i = 0, j = 0; strv && strv[i]; i++) {
                if (_nm_utils_dns_option_validate(strv[i], NULL, NULL, AF_UNSPEC, NULL))
                    strv[j++] = strv[i];
                else
                    g_free(strv[i]);
            }
            if (j == 0)
                g_free(strv);
            else {
                strv[j]             = NULL;
                dns_config->options = strv;
            }
        } else if (nm_streq0(key, "domains")
                   && g_variant_is_of_type(val, G_VARIANT_TYPE("a{sv}"))) {
            NMGlobalDnsDomain *domain;
            GVariantIter       domain_iter;
            GVariant          *v;
            char              *k;

            g_variant_iter_init(&domain_iter, val);
            while (g_variant_iter_next(&domain_iter, "{&sv}", &k, &v)) {
                if (k) {
                    domain = global_dns_domain_from_dbus(k, v);
                    if (domain)
                        g_hash_table_insert(dns_config->domains, strdup(k), domain);
                }
                g_variant_unref(v);
            }
        }
        g_variant_unref(val);
    }

    /* An empty value is valid and clears the internal configuration */
    if (!nm_global_dns_config_is_empty(dns_config)
        && !nm_global_dns_config_lookup_domain(dns_config, "*")) {
        g_set_error_literal(error,
                            NM_MANAGER_ERROR,
                            NM_MANAGER_ERROR_FAILED,
                            "Global DNS configuration is missing the default domain");
        nm_global_dns_config_free(dns_config);
        return NULL;
    }

    global_dns_config_seal_domains(dns_config);
    return dns_config;
}

static gboolean
global_dns_equal(NMGlobalDnsConfig *old, NMGlobalDnsConfig *new)
{
    NMGlobalDnsDomain *domain_old, *domain_new;
    gpointer           key, value_old, value_new;
    GHashTableIter     iter;

    if (old == new)
        return TRUE;

    if (!old || !new)
        return FALSE;

    if (!nm_strv_equal(old->options, new->options) || !nm_strv_equal(old->searches, new->searches))
        return FALSE;

    if ((!old->domains || !new->domains) && old->domains != new->domains)
        return FALSE;

    if (nm_g_hash_table_size(old->domains) != nm_g_hash_table_size(new->domains))
        return FALSE;

    if (old->domains) {
        g_hash_table_iter_init(&iter, old->domains);
        while (g_hash_table_iter_next(&iter, &key, &value_old)) {
            value_new = g_hash_table_lookup(new->domains, key);
            if (!value_new)
                return FALSE;

            domain_old = value_old;
            domain_new = value_new;

            if (!nm_strv_equal(domain_old->options, domain_new->options)
                || !nm_strv_equal(domain_old->servers, domain_new->servers))
                return FALSE;
        }
    }

    return TRUE;
}

/*****************************************************************************/

static const MatchSectionInfo *
_match_section_infos_lookup(const MatchSectionInfo      *match_section_infos,
                            GKeyFile                    *keyfile,
                            const char                  *property,
                            const NMMatchSpecDeviceData *match_data,
                            NMDevice                    *device,
                            const char                 **out_value)
{
    NMMatchSpecDeviceData match_data_local;

    /* Caller must either provide a "match_data" or a "device" (actually,
     * neither is also fine, albeit unusual). */
    nm_assert(!match_data || !device);
    nm_assert(!device || NM_IS_DEVICE(device));

    if (!match_section_infos)
        goto out;

    for (; match_section_infos->group_name; match_section_infos++) {
        const char *value;
        gboolean    match;

        /* FIXME: Here we use g_key_file_get_string(). This should be in sync with what keyfile-reader
         * does.
         *
         * Unfortunately that is currently not possible because keyfile-reader does the two steps
         * string_to_value(keyfile_to_string(keyfile)) in one. Optimally, keyfile library would
         * expose both functions, and we would return here keyfile_to_string(keyfile).
         * The caller then could convert the string to the proper value via string_to_value(value). */
        value = _match_section_info_get_str(match_section_infos, keyfile, property);
        if (!value && !match_section_infos->stop_match)
            continue;

        if (match_section_infos->match_device.has) {
            NMMatchSpecMatchType m;

            if (G_UNLIKELY(!match_data)) {
                /* In most cases, we don't actually have any matches. So we "optimize"
                 * here by allowing the user to specify a NMDEvice directly, and only
                 * initialize the match-data when needed. */
                match_data = nm_match_spec_device_data_init_from_device(&match_data_local, device);
            }

            m     = nm_match_spec_device(match_section_infos->match_device.spec, match_data);
            match = nm_match_spec_match_type_to_bool(m, FALSE);
        } else
            match = TRUE;

        if (match) {
            NM_SET_OUT(out_value, value);
            return match_section_infos;
        }
    }

out:
    NM_SET_OUT(out_value, NULL);
    return NULL;
}

static const char *
_config_data_get_device_config(const NMConfigData          *self,
                               const char                  *property,
                               const NMMatchSpecDeviceData *match_data,
                               NMDevice                    *device,
                               gboolean                    *has_match)
{
    const NMConfigDataPrivate *priv;
    const MatchSectionInfo    *connection_info;
    const char                *value;

    NM_SET_OUT(has_match, FALSE);

    g_return_val_if_fail(self, NULL);
    g_return_val_if_fail(property && *property, NULL);

    nm_assert(!match_data || !device);
    nm_assert(!device || NM_IS_DEVICE(device));

    priv = NM_CONFIG_DATA_GET_PRIVATE(self);

    connection_info = _match_section_infos_lookup(&priv->device_infos[0],
                                                  priv->keyfile,
                                                  property,
                                                  match_data,
                                                  device,
                                                  &value);
    NM_SET_OUT(has_match, !!connection_info);
    return value;
}

const char *
nm_config_data_get_device_config(const NMConfigData          *self,
                                 const char                  *property,
                                 const NMMatchSpecDeviceData *match_data,
                                 gboolean                    *has_match)
{
    return _config_data_get_device_config(self, property, match_data, NULL, has_match);
}

const char *
nm_config_data_get_device_config_by_device(const NMConfigData *self,
                                           const char         *property,
                                           NMDevice           *device,
                                           gboolean           *has_match)
{
    return _config_data_get_device_config(self, property, NULL, device, has_match);
}

const char *
nm_config_data_get_device_config_by_pllink(const NMConfigData   *self,
                                           const char           *property,
                                           const NMPlatformLink *pllink,
                                           const char           *match_device_type,
                                           gboolean             *has_match)
{
    const NMConfigDataPrivate *priv;
    const MatchSectionInfo    *connection_info;
    const char                *value;
    NMMatchSpecDeviceData      match_data;

    g_return_val_if_fail(self, NULL);
    g_return_val_if_fail(property && *property, NULL);

    priv = NM_CONFIG_DATA_GET_PRIVATE(self);

    nm_match_spec_device_data_init_from_platform(&match_data,
                                                 pllink,
                                                 match_device_type,
                                                 nm_dhcp_manager_get_config(nm_dhcp_manager_get()));

    connection_info = _match_section_infos_lookup(&priv->device_infos[0],
                                                  priv->keyfile,
                                                  property,
                                                  &match_data,
                                                  NULL,
                                                  &value);
    NM_SET_OUT(has_match, !!connection_info);
    return value;
}

gboolean
nm_config_data_get_device_config_boolean_by_device(const NMConfigData *self,
                                                   const char         *property,
                                                   NMDevice           *device,
                                                   int                 val_no_match,
                                                   int                 val_invalid)
{
    const char *value;
    gboolean    has_match;

    value = nm_config_data_get_device_config_by_device(self, property, device, &has_match);
    if (!has_match)
        return val_no_match;
    return nm_config_parse_boolean(value, val_invalid);
}

gint64
nm_config_data_get_device_config_int64_by_device(const NMConfigData *self,
                                                 const char         *property,
                                                 NMDevice           *device,
                                                 int                 base,
                                                 gint64              min,
                                                 gint64              max,
                                                 gint64              val_no_match,
                                                 gint64              val_invalid)
{
    const char *value;
    gboolean    has_match;

    value = nm_config_data_get_device_config_by_device(self, property, device, &has_match);
    if (!has_match) {
        errno = ENOENT;
        return val_no_match;
    }
    return _nm_utils_ascii_str_to_int64(value, base, min, max, val_invalid);
}

const GSList *
nm_config_data_get_device_allowed_connections_specs(const NMConfigData *self,
                                                    NMDevice           *device,
                                                    gboolean           *has_match)
{
    const NMConfigDataPrivate *priv;
    const MatchSectionInfo    *connection_info;
    const GSList              *ret = NULL;

    g_return_val_if_fail(self, NULL);

    priv = NM_CONFIG_DATA_GET_PRIVATE(self);

    connection_info = _match_section_infos_lookup(&priv->device_infos[0],
                                                  priv->keyfile,
                                                  NM_CONFIG_KEYFILE_KEY_DEVICE_ALLOWED_CONNECTIONS,
                                                  NULL,
                                                  device,
                                                  NULL);

    if (connection_info) {
        nm_assert(connection_info->device.allowed_connections_has);
        ret = connection_info->device.allowed_connections;
        NM_SET_OUT(has_match, TRUE);
    } else
        NM_SET_OUT(has_match, FALSE);

    return ret;
}

const char *
nm_config_data_get_connection_default(const NMConfigData *self,
                                      const char         *property,
                                      NMDevice           *device)
{
    const NMConfigDataPrivate *priv;
    const char                *value;

    g_return_val_if_fail(self, NULL);
    g_return_val_if_fail(property && *property, NULL);
    g_return_val_if_fail(strchr(property, '.'), NULL);

    priv = NM_CONFIG_DATA_GET_PRIVATE(self);

#if NM_MORE_ASSERTS > 10
    {
        const char **ptr;

        for (ptr = __start_connection_defaults; ptr < __stop_connection_defaults; ptr++) {
            if (nm_streq(property, *ptr))
                break;
        }

        nm_assert(ptr < __stop_connection_defaults);
    }
#endif

    _match_section_infos_lookup(&priv->connection_infos[0],
                                priv->keyfile,
                                property,
                                NULL,
                                device,
                                &value);
    return value;
}

gint64
nm_config_data_get_connection_default_int64(const NMConfigData *self,
                                            const char         *property,
                                            NMDevice           *device,
                                            gint64              min,
                                            gint64              max,
                                            gint64              fallback)
{
    const char *value;

    value = nm_config_data_get_connection_default(self, property, device);
    return _nm_utils_ascii_str_to_int64(value, 0, min, max, fallback);
}

static const char *
_match_section_info_get_str(const MatchSectionInfo *m, GKeyFile *keyfile, const char *property)
{
    gssize      idx;
    const char *value;

    idx   = nm_utils_named_value_list_find(m->lookup_idx, m->lookup_len, property, TRUE);
    value = idx >= 0 ? m->lookup_idx[idx].value_str : NULL;

#if NM_MORE_ASSERTS > 10
    {
        gs_free char *value2 = g_key_file_get_string(keyfile, m->group_name, property, NULL);

        nm_assert(nm_streq0(value2, value));
    }
#endif

    return value;
}

static void
_match_section_info_init(MatchSectionInfo *connection_info,
                         GKeyFile         *keyfile,
                         char             *group,
                         gboolean          is_device)
{
    char             **keys = NULL;
    gsize              n_keys;
    gsize              i;
    gsize              j;
    NMUtilsNamedValue *vals;

    /* pass ownership of @group on... */
    connection_info->group_name = group;

    connection_info->match_device.spec =
        nm_config_get_match_spec(keyfile,
                                 group,
                                 NM_CONFIG_KEYFILE_KEY_MATCH_DEVICE,
                                 &connection_info->match_device.has);
    connection_info->stop_match =
        nm_config_keyfile_get_boolean(keyfile, group, NM_CONFIG_KEYFILE_KEY_STOP_MATCH, FALSE);

    if (is_device) {
        connection_info->device.allowed_connections =
            nm_config_get_match_spec(keyfile,
                                     group,
                                     NM_CONFIG_KEYFILE_KEY_DEVICE_ALLOWED_CONNECTIONS,
                                     &connection_info->device.allowed_connections_has);
    }

    keys = g_key_file_get_keys(keyfile, group, &n_keys, NULL);
    nm_strv_sort(keys, n_keys);

    vals = g_new(NMUtilsNamedValue, n_keys);

    for (i = 0, j = 0; i < n_keys; i++) {
        gs_free char *key = g_steal_pointer(&keys[i]);
        char         *value;

        if (NM_IN_STRSET(key, NM_CONFIG_KEYFILE_KEY_STOP_MATCH, NM_CONFIG_KEYFILE_KEY_MATCH_DEVICE))
            continue;

        if (j > 0 && nm_streq(vals[j - 1].name, key))
            continue;

        value = g_key_file_get_string(keyfile, group, key, NULL);
        if (!value)
            continue;

        vals[j++] = (NMUtilsNamedValue){
            .name      = g_steal_pointer(&key),
            .value_str = value,
        };
    }

    g_free(keys);

    if (n_keys != j) {
        gs_free NMUtilsNamedValue *vals2 = vals;

        /* since this buffer will be kept around for a long time,
         * get rid of the excess allocation. */
        vals   = nm_memdup(vals2, sizeof(NMUtilsNamedValue) * j);
        n_keys = j;
    }

    if (n_keys == 0)
        nm_clear_g_free(&vals);

    connection_info->lookup_idx = vals;
    connection_info->lookup_len = n_keys;
}

static void
_match_section_infos_free(MatchSectionInfo *match_section_infos)
{
    MatchSectionInfo *m;
    gsize             i;

    if (!match_section_infos)
        return;

    for (m = match_section_infos; m->group_name; m++) {
        g_free(m->group_name);
        g_slist_free_full(m->match_device.spec, g_free);
        if (m->is_device) {
            g_slist_free_full(m->device.allowed_connections, g_free);
        }
        for (i = 0; i < m->lookup_len; i++) {
            g_free(m->lookup_idx[i].name_mutable);
            g_free(m->lookup_idx[i].value_str_mutable);
        }
        g_free((gpointer) m->lookup_idx);
    }
    g_free(match_section_infos);
}

static MatchSectionInfo *
_match_section_infos_construct(GKeyFile *keyfile, gboolean is_device)
{
    char            **groups;
    gsize             i, j, ngroups;
    char             *connection_tag      = NULL;
    MatchSectionInfo *match_section_infos = NULL;
    const char       *prefix;

    prefix =
        is_device ? NM_CONFIG_KEYFILE_GROUPPREFIX_DEVICE : NM_CONFIG_KEYFILE_GROUPPREFIX_CONNECTION;

    /* get the list of existing [connection.\+]/[device.\+] sections.
     *
     * We expect the sections in their right order, with lowest priority
     * first. Only exception is the (literal) [connection] section, which
     * we will always reorder to the end. */
    groups = g_key_file_get_groups(keyfile, &ngroups);
    if (!groups)
        return NULL;

    if (ngroups > 0) {
        gsize l = strlen(prefix);

        for (i = 0, j = 0; i < ngroups; i++) {
            if (g_str_has_prefix(groups[i], prefix)) {
                if (groups[i][l] == '\0')
                    connection_tag = groups[i];
                else
                    groups[j++] = groups[i];
            } else
                g_free(groups[i]);
        }
        ngroups = j;
    }

    if (ngroups == 0 && !connection_tag) {
        g_free(groups);
        return NULL;
    }

    match_section_infos = g_new0(MatchSectionInfo, ngroups + 1 + (connection_tag ? 1 : 0));
    match_section_infos->is_device = is_device;
    for (i = 0; i < ngroups; i++) {
        /* pass ownership of @group on... */
        _match_section_info_init(&match_section_infos[i],
                                 keyfile,
                                 groups[ngroups - i - 1],
                                 is_device);
    }
    if (connection_tag) {
        /* pass ownership of @connection_tag on... */
        _match_section_info_init(&match_section_infos[i], keyfile, connection_tag, is_device);
    }
    g_free(groups);

    return match_section_infos;
}

/*****************************************************************************/

NMConfigChangeFlags
nm_config_data_diff(NMConfigData *old_data, NMConfigData *new_data)
{
    NMConfigChangeFlags  changes = NM_CONFIG_CHANGE_NONE;
    NMConfigDataPrivate *priv_old, *priv_new;

    g_return_val_if_fail(NM_IS_CONFIG_DATA(old_data), NM_CONFIG_CHANGE_NONE);
    g_return_val_if_fail(NM_IS_CONFIG_DATA(new_data), NM_CONFIG_CHANGE_NONE);

    priv_old = NM_CONFIG_DATA_GET_PRIVATE(old_data);
    priv_new = NM_CONFIG_DATA_GET_PRIVATE(new_data);

    if (!_nm_keyfile_equal(priv_old->keyfile_user, priv_new->keyfile_user, TRUE))
        changes |= NM_CONFIG_CHANGE_VALUES | NM_CONFIG_CHANGE_VALUES_USER;

    if (!_nm_keyfile_equal(priv_old->keyfile_intern, priv_new->keyfile_intern, TRUE))
        changes |= NM_CONFIG_CHANGE_VALUES | NM_CONFIG_CHANGE_VALUES_INTERN;

    if (!nm_streq0(nm_config_data_get_config_main_file(old_data),
                   nm_config_data_get_config_main_file(new_data))
        || !nm_streq0(nm_config_data_get_config_description(old_data),
                      nm_config_data_get_config_description(new_data)))
        changes |= NM_CONFIG_CHANGE_CONFIG_FILES;

    if (nm_config_data_get_connectivity_enabled(old_data)
            != nm_config_data_get_connectivity_enabled(new_data)
        || nm_config_data_get_connectivity_interval(old_data)
               != nm_config_data_get_connectivity_interval(new_data)
        || !nm_streq0(nm_config_data_get_connectivity_uri(old_data),
                      nm_config_data_get_connectivity_uri(new_data))
        || !nm_streq0(nm_config_data_get_connectivity_response(old_data),
                      nm_config_data_get_connectivity_response(new_data)))
        changes |= NM_CONFIG_CHANGE_CONNECTIVITY;

    if (nm_utils_g_slist_strlist_cmp(priv_old->no_auto_default.specs,
                                     priv_new->no_auto_default.specs)
            != 0
        || nm_utils_g_slist_strlist_cmp(priv_old->no_auto_default.specs_config,
                                        priv_new->no_auto_default.specs_config)
               != 0)
        changes |= NM_CONFIG_CHANGE_NO_AUTO_DEFAULT;

    if (!nm_streq0(nm_config_data_get_dns_mode(old_data), nm_config_data_get_dns_mode(new_data)))
        changes |= NM_CONFIG_CHANGE_DNS_MODE;

    if (!nm_streq0(nm_config_data_get_rc_manager(old_data),
                   nm_config_data_get_rc_manager(new_data)))
        changes |= NM_CONFIG_CHANGE_RC_MANAGER;

    if (!global_dns_equal(priv_old->global_dns, priv_new->global_dns))
        changes |= NM_CONFIG_CHANGE_GLOBAL_DNS_CONFIG;

    nm_assert(!NM_FLAGS_ANY(changes, NM_CONFIG_CHANGE_CAUSES));

    return changes;
}

/*****************************************************************************/

void
nm_config_data_get_warnings(const NMConfigData *self, GPtrArray *warnings)
{
    gboolean invalid;

    nm_assert(NM_IS_CONFIG_DATA(self));
    nm_assert(warnings);

    _config_data_get_main_auth_polkit(self, &invalid);
    if (invalid) {
        g_ptr_array_add(
            warnings,
            g_strdup_printf(
                "invalid setting for %s.%s (should be one of \"true\", \"false\", \"root-only\")",
                NM_CONFIG_KEYFILE_GROUP_MAIN,
                NM_CONFIG_KEYFILE_KEY_MAIN_AUTH_POLKIT));
    }
}

/*****************************************************************************/

static void
get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    NMConfigData *self = NM_CONFIG_DATA(object);

    switch (prop_id) {
    case PROP_CONFIG_MAIN_FILE:
        g_value_set_string(value, nm_config_data_get_config_main_file(self));
        break;
    case PROP_CONFIG_DESCRIPTION:
        g_value_set_string(value, nm_config_data_get_config_description(self));
        break;
    case PROP_CONNECTIVITY_ENABLED:
        g_value_set_boolean(value, nm_config_data_get_connectivity_enabled(self));
        break;
    case PROP_CONNECTIVITY_URI:
        g_value_set_string(value, nm_config_data_get_connectivity_uri(self));
        break;
    case PROP_CONNECTIVITY_INTERVAL:
        g_value_set_uint(value, nm_config_data_get_connectivity_interval(self));
        break;
    case PROP_CONNECTIVITY_RESPONSE:
        g_value_set_string(value, nm_config_data_get_connectivity_response(self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    NMConfigData        *self = NM_CONFIG_DATA(object);
    NMConfigDataPrivate *priv = NM_CONFIG_DATA_GET_PRIVATE(self);

    switch (prop_id) {
    case PROP_CONFIG_MAIN_FILE:
        /* construct-only */
        priv->config_main_file = g_value_dup_string(value);
        break;
    case PROP_CONFIG_DESCRIPTION:
        /* construct-only */
        priv->config_description = g_value_dup_string(value);
        break;
    case PROP_KEYFILE_USER:
        /* construct-only */
        priv->keyfile_user = g_value_dup_boxed(value);
        if (priv->keyfile_user && !_nm_keyfile_has_values(priv->keyfile_user)) {
            g_key_file_unref(priv->keyfile_user);
            priv->keyfile_user = NULL;
        }
        break;
    case PROP_KEYFILE_INTERN:
        /* construct-only */
        priv->keyfile_intern = g_value_dup_boxed(value);
        if (priv->keyfile_intern && !_nm_keyfile_has_values(priv->keyfile_intern)) {
            g_key_file_unref(priv->keyfile_intern);
            priv->keyfile_intern = NULL;
        }
        break;
    case PROP_NO_AUTO_DEFAULT:
        /* construct-only */
        {
            const char *const   *value_arr_orig = g_value_get_boxed(value);
            gs_free const char **value_arr      = NULL;
            GSList              *specs          = NULL;
            gsize                i, j;
            gsize                len;

            len = NM_PTRARRAY_LEN(value_arr_orig);

            /* sort entries, remove duplicates and empty words. */
            value_arr =
                len == 0 ? NULL : nm_memdup(value_arr_orig, sizeof(const char *) * (len + 1));
            nm_strv_sort(value_arr, len);
            nm_strv_cleanup((char **) value_arr, FALSE, TRUE, TRUE);

            len = NM_PTRARRAY_LEN(value_arr);
            j   = 0;
            for (i = 0; i < len; i++) {
                const char *s = value_arr[i];
                gboolean    is_mac;
                char       *spec;

                if (NM_STR_HAS_PREFIX(s, NM_MATCH_SPEC_INTERFACE_NAME_TAG "="))
                    is_mac = FALSE;
                else if (nm_utils_hwaddr_valid(s, -1))
                    is_mac = TRUE;
                else {
                    /* we drop all lines that we don't understand. */
                    continue;
                }

                value_arr[j++] = s;

                spec  = is_mac ? g_strdup_printf(NM_MATCH_SPEC_MAC_TAG "%s", s) : g_strdup(s);
                specs = g_slist_prepend(specs, spec);
            }

            priv->no_auto_default.arr   = nm_strv_dup(value_arr, j, TRUE);
            priv->no_auto_default.specs = g_slist_reverse(specs);
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

/*****************************************************************************/

static void
nm_config_data_init(NMConfigData *self)
{}

static void
constructed(GObject *object)
{
    NMConfigData        *self = NM_CONFIG_DATA(object);
    NMConfigDataPrivate *priv = NM_CONFIG_DATA_GET_PRIVATE(self);
    char                *str;

    priv->keyfile = _merge_keyfiles(priv->keyfile_user, priv->keyfile_intern);

    priv->connection_infos = _match_section_infos_construct(priv->keyfile, FALSE);
    priv->device_infos     = _match_section_infos_construct(priv->keyfile, TRUE);

    priv->connectivity.enabled =
        nm_config_keyfile_get_boolean(priv->keyfile,
                                      NM_CONFIG_KEYFILE_GROUP_CONNECTIVITY,
                                      NM_CONFIG_KEYFILE_KEY_CONNECTIVITY_ENABLED,
                                      TRUE);
    priv->connectivity.uri =
        nm_strstrip(g_key_file_get_string(priv->keyfile,
                                          NM_CONFIG_KEYFILE_GROUP_CONNECTIVITY,
                                          NM_CONFIG_KEYFILE_KEY_CONNECTIVITY_URI,
                                          NULL));
    priv->connectivity.response       = g_key_file_get_string(priv->keyfile,
                                                        NM_CONFIG_KEYFILE_GROUP_CONNECTIVITY,
                                                        NM_CONFIG_KEYFILE_KEY_CONNECTIVITY_RESPONSE,
                                                        NULL);
    str                               = nm_config_keyfile_get_value(priv->keyfile,
                                      NM_CONFIG_KEYFILE_GROUP_MAIN,
                                      NM_CONFIG_KEYFILE_KEY_MAIN_AUTOCONNECT_RETRIES_DEFAULT,
                                      NM_CONFIG_GET_VALUE_NONE);
    priv->autoconnect_retries_default = _nm_utils_ascii_str_to_int64(str, 10, 0, G_MAXINT32, 4);
    g_free(str);

    /* On missing config value, fallback to 300. On invalid value, disable connectivity checking by setting
     * the interval to zero. */
    str = g_key_file_get_string(priv->keyfile,
                                NM_CONFIG_KEYFILE_GROUP_CONNECTIVITY,
                                NM_CONFIG_KEYFILE_KEY_CONNECTIVITY_INTERVAL,
                                NULL);
    priv->connectivity.interval =
        _nm_utils_ascii_str_to_int64(str,
                                     10,
                                     0,
                                     G_MAXUINT,
                                     NM_CONFIG_DEFAULT_CONNECTIVITY_INTERVAL);
    g_free(str);

    priv->dns_mode   = nm_strstrip(g_key_file_get_string(priv->keyfile,
                                                       NM_CONFIG_KEYFILE_GROUP_MAIN,
                                                       NM_CONFIG_KEYFILE_KEY_MAIN_DNS,
                                                       NULL));
    priv->rc_manager = nm_strstrip(g_key_file_get_string(priv->keyfile,
                                                         NM_CONFIG_KEYFILE_GROUP_MAIN,
                                                         NM_CONFIG_KEYFILE_KEY_MAIN_RC_MANAGER,
                                                         NULL));
    priv->systemd_resolved =
        nm_config_keyfile_get_boolean(priv->keyfile,
                                      NM_CONFIG_KEYFILE_GROUP_MAIN,
                                      NM_CONFIG_KEYFILE_KEY_MAIN_SYSTEMD_RESOLVED,
                                      TRUE);
    priv->ignore_carrier = nm_config_get_match_spec(priv->keyfile,
                                                    NM_CONFIG_KEYFILE_GROUP_MAIN,
                                                    NM_CONFIG_KEYFILE_KEY_MAIN_IGNORE_CARRIER,
                                                    NULL);
    priv->assume_ipv6ll_only =
        nm_config_get_match_spec(priv->keyfile,
                                 NM_CONFIG_KEYFILE_GROUP_MAIN,
                                 NM_CONFIG_KEYFILE_KEY_MAIN_ASSUME_IPV6LL_ONLY,
                                 NULL);
    priv->no_auto_default.specs_config =
        nm_config_get_match_spec(priv->keyfile,
                                 NM_CONFIG_KEYFILE_GROUP_MAIN,
                                 NM_CONFIG_KEYFILE_KEY_MAIN_NO_AUTO_DEFAULT,
                                 NULL);

    priv->global_dns = load_global_dns(priv->keyfile_user, FALSE);
    if (!priv->global_dns)
        priv->global_dns = load_global_dns(priv->keyfile_intern, TRUE);

    priv->iwd_config_path =
        nm_strstrip(g_key_file_get_string(priv->keyfile,
                                          NM_CONFIG_KEYFILE_GROUP_MAIN,
                                          NM_CONFIG_KEYFILE_KEY_MAIN_IWD_CONFIG_PATH,
                                          NULL));

    G_OBJECT_CLASS(nm_config_data_parent_class)->constructed(object);
}

NMConfigData *
nm_config_data_new(const char        *config_main_file,
                   const char        *config_description,
                   const char *const *no_auto_default,
                   GKeyFile          *keyfile_user,
                   GKeyFile          *keyfile_intern)
{
    return g_object_new(NM_TYPE_CONFIG_DATA,
                        NM_CONFIG_DATA_CONFIG_MAIN_FILE,
                        config_main_file,
                        NM_CONFIG_DATA_CONFIG_DESCRIPTION,
                        config_description,
                        NM_CONFIG_DATA_KEYFILE_USER,
                        keyfile_user,
                        NM_CONFIG_DATA_KEYFILE_INTERN,
                        keyfile_intern,
                        NM_CONFIG_DATA_NO_AUTO_DEFAULT,
                        no_auto_default,
                        NULL);
}

NMConfigData *
nm_config_data_new_update_keyfile_intern(const NMConfigData *base, GKeyFile *keyfile_intern)
{
    const NMConfigDataPrivate *priv = NM_CONFIG_DATA_GET_PRIVATE(base);

    return g_object_new(NM_TYPE_CONFIG_DATA,
                        NM_CONFIG_DATA_CONFIG_MAIN_FILE,
                        priv->config_main_file,
                        NM_CONFIG_DATA_CONFIG_DESCRIPTION,
                        priv->config_description,
                        NM_CONFIG_DATA_KEYFILE_USER,
                        priv->keyfile_user, /* the keyfile is unchanged. It's safe to share it. */
                        NM_CONFIG_DATA_KEYFILE_INTERN,
                        keyfile_intern,
                        NM_CONFIG_DATA_NO_AUTO_DEFAULT,
                        priv->no_auto_default.arr,
                        NULL);
}

NMConfigData *
nm_config_data_new_update_no_auto_default(const NMConfigData *base,
                                          const char *const  *no_auto_default)
{
    const NMConfigDataPrivate *priv = NM_CONFIG_DATA_GET_PRIVATE(base);

    return g_object_new(NM_TYPE_CONFIG_DATA,
                        NM_CONFIG_DATA_CONFIG_MAIN_FILE,
                        priv->config_main_file,
                        NM_CONFIG_DATA_CONFIG_DESCRIPTION,
                        priv->config_description,
                        NM_CONFIG_DATA_KEYFILE_USER,
                        priv->keyfile_user, /* the keyfile is unchanged. It's safe to share it. */
                        NM_CONFIG_DATA_KEYFILE_INTERN,
                        priv->keyfile_intern,
                        NM_CONFIG_DATA_NO_AUTO_DEFAULT,
                        no_auto_default,
                        NULL);
}

static void
finalize(GObject *gobject)
{
    NMConfigDataPrivate *priv = NM_CONFIG_DATA_GET_PRIVATE(gobject);

    g_free(priv->config_main_file);
    g_free(priv->config_description);

    g_free(priv->connectivity.uri);
    g_free(priv->connectivity.response);

    g_slist_free_full(priv->no_auto_default.specs, g_free);
    g_slist_free_full(priv->no_auto_default.specs_config, g_free);
    g_strfreev(priv->no_auto_default.arr);

    g_free(priv->dns_mode);
    g_free(priv->rc_manager);

    g_slist_free_full(priv->ignore_carrier, g_free);
    g_slist_free_full(priv->assume_ipv6ll_only, g_free);

    nm_global_dns_config_free(priv->global_dns);

    g_free(priv->iwd_config_path);

    _match_section_infos_free(priv->connection_infos);
    _match_section_infos_free(priv->device_infos);

    g_key_file_unref(priv->keyfile);
    if (priv->keyfile_user)
        g_key_file_unref(priv->keyfile_user);
    if (priv->keyfile_intern)
        g_key_file_unref(priv->keyfile_intern);

    G_OBJECT_CLASS(nm_config_data_parent_class)->finalize(gobject);
}

static void
nm_config_data_class_init(NMConfigDataClass *config_class)
{
    GObjectClass *object_class = G_OBJECT_CLASS(config_class);

    object_class->constructed  = constructed;
    object_class->finalize     = finalize;
    object_class->get_property = get_property;
    object_class->set_property = set_property;

    obj_properties[PROP_CONFIG_MAIN_FILE] =
        g_param_spec_string(NM_CONFIG_DATA_CONFIG_MAIN_FILE,
                            "",
                            "",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_CONFIG_DESCRIPTION] =
        g_param_spec_string(NM_CONFIG_DATA_CONFIG_DESCRIPTION,
                            "",
                            "",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_KEYFILE_USER] =
        g_param_spec_boxed(NM_CONFIG_DATA_KEYFILE_USER,
                           "",
                           "",
                           G_TYPE_KEY_FILE,
                           G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_KEYFILE_INTERN] =
        g_param_spec_boxed(NM_CONFIG_DATA_KEYFILE_INTERN,
                           "",
                           "",
                           G_TYPE_KEY_FILE,
                           G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_CONNECTIVITY_ENABLED] =
        g_param_spec_string(NM_CONFIG_DATA_CONNECTIVITY_ENABLED,
                            "",
                            "",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_CONNECTIVITY_URI] =
        g_param_spec_string(NM_CONFIG_DATA_CONNECTIVITY_URI,
                            "",
                            "",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_CONNECTIVITY_INTERVAL] =
        g_param_spec_uint(NM_CONFIG_DATA_CONNECTIVITY_INTERVAL,
                          "",
                          "",
                          0,
                          G_MAXUINT,
                          0,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_CONNECTIVITY_RESPONSE] =
        g_param_spec_string(NM_CONFIG_DATA_CONNECTIVITY_RESPONSE,
                            "",
                            "",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_NO_AUTO_DEFAULT] =
        g_param_spec_boxed(NM_CONFIG_DATA_NO_AUTO_DEFAULT,
                           "",
                           "",
                           G_TYPE_STRV,
                           G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST, obj_properties);
}
