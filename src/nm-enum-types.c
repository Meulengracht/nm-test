


/* Generated by glib-mkenums. Do not edit */

#include "config.h"

#include "nm-enum-types.h"

#include "nm-device-bond.h" 
#include "nm-device-bridge.h" 
#include "nm-device-ethernet.h" 
#include "nm-device-infiniband.h" 
#include "nm-device-ip-tunnel.h" 
#include "nm-device-macvlan.h" 
#include "nm-device-tun.h" 
#include "nm-device-veth.h" 
#include "nm-device-vlan.h" 
#include "nm-device-vxlan.h" 
#include "nm-dhcp-dhclient.h" 
#include "nm-dhcp-dhclient-utils.h" 
#include "nm-dhcp-dhcpcd.h" 
#include "nm-dhcp-systemd.h" 
#include "nm-device.h" 
#include "nm-lldp-listener.h" 
#include "nm-arping-manager.h" 
#include "nm-device-ethernet-utils.h" 
#include "nm-device-factory.h" 
#include "nm-device-generic.h" 
#include "nm-device-logging.h" 
#include "nm-dhcp-client.h" 
#include "nm-dhcp-utils.h" 
#include "nm-dhcp-listener.h" 
#include "nm-dhcp-manager.h" 
#include "nm-dns-dnsmasq.h" 
#include "nm-dns-unbound.h" 
#include "nm-dns-manager.h" 
#include "nm-dns-plugin.h" 
#include "nm-dns-utils.h" 
#include "nm-dnsmasq-manager.h" 
#include "nm-dnsmasq-utils.h" 
#include "nm-fake-platform.h" 
#include "nm-linux-platform.h" 
#include "nm-platform.h" 
#include "nm-platform-utils.h" 
#include "nmp-object.h" 
#include "wifi-utils-nl80211.h" 
#include "wifi-utils.h" 
#include "nm-fake-rdisc.h" 
#include "nm-lndp-rdisc.h" 
#include "nm-rdisc.h" 
#include "nm-ppp-manager.h" 
#include "nm-ppp-status.h" 
#include "nm-agent-manager.h" 
#include "nm-inotify-helper.h" 
#include "nm-secret-agent.h" 
#include "nm-settings-connection.h" 
#include "nm-settings-plugin.h" 
#include "nm-settings.h" 
#include "nm-keyfile-connection.h" 
#include "plugin.h" 
#include "reader.h" 
#include "utils.h" 
#include "writer.h" 
#include "nm-supplicant-config.h" 
#include "nm-supplicant-interface.h" 
#include "nm-supplicant-manager.h" 
#include "nm-supplicant-settings-verify.h" 
#include "nm-supplicant-types.h" 
#include "nm-vpn-connection.h" 
#include "nm-vpn-manager.h" 
#include "nm-activation-request.h" 
#include "nm-active-connection.h" 
#include "nm-audit-manager.h" 
#include "nm-bus-manager.h" 
#include "nm-config.h" 
#include "nm-config-data.h" 
#include "nm-connection-provider.h" 
#include "nm-connectivity.h" 
#include "nm-dcb.h" 
#include "nm-route-manager.h" 
#include "nm-default-route-manager.h" 
#include "nm-dhcp4-config.h" 
#include "nm-dhcp6-config.h" 
#include "nm-dispatcher.h" 
#include "nm-exported-object.h" 
#include "nm-firewall-manager.h" 
#include "nm-ip4-config.h" 
#include "nm-ip6-config.h" 
#include "nm-logging.h" 
#include "nm-auth-manager.h" 
#include "nm-auth-subject.h" 
#include "nm-auth-utils.h" 
#include "nm-manager.h" 
#include "nm-multi-index.h" 
#include "nm-policy.h" 
#include "nm-rfkill-manager.h" 
#include "nm-session-monitor.h" 
#include "nm-sleep-monitor.h" 
#include "nm-types.h" 
#include "nm-core-utils.h" 
#include "NetworkManagerUtils.h" 
#include "wifi-utils-wext.h"

GType
nm_vlan_error_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_VLAN_ERROR_CONNECTION_NOT_VLAN, "NM_VLAN_ERROR_CONNECTION_NOT_VLAN", "ConnectionNotVlan" },
        { NM_VLAN_ERROR_CONNECTION_INVALID, "NM_VLAN_ERROR_CONNECTION_INVALID", "ConnectionInvalid" },
        { NM_VLAN_ERROR_CONNECTION_INCOMPATIBLE, "NM_VLAN_ERROR_CONNECTION_INCOMPATIBLE", "ConnectionIncompatible" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMVlanError"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_unman_flag_op_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_UNMAN_FLAG_OP_SET_MANAGED, "NM_UNMAN_FLAG_OP_SET_MANAGED", "set-managed" },
        { NM_UNMAN_FLAG_OP_SET_UNMANAGED, "NM_UNMAN_FLAG_OP_SET_UNMANAGED", "set-unmanaged" },
        { NM_UNMAN_FLAG_OP_FORGET, "NM_UNMAN_FLAG_OP_FORGET", "forget" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMUnmanFlagOp"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_dhcp_state_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_DHCP_STATE_UNKNOWN, "NM_DHCP_STATE_UNKNOWN", "nm-dhcp-state-unknown" },
        { NM_DHCP_STATE_BOUND, "NM_DHCP_STATE_BOUND", "nm-dhcp-state-bound" },
        { NM_DHCP_STATE_TIMEOUT, "NM_DHCP_STATE_TIMEOUT", "nm-dhcp-state-timeout" },
        { NM_DHCP_STATE_DONE, "NM_DHCP_STATE_DONE", "nm-dhcp-state-done" },
        { NM_DHCP_STATE_EXPIRE, "NM_DHCP_STATE_EXPIRE", "nm-dhcp-state-expire" },
        { NM_DHCP_STATE_FAIL, "NM_DHCP_STATE_FAIL", "nm-dhcp-state-fail" },
        { __NM_DHCP_STATE_MAX, "__NM_DHCP_STATE_MAX", "--nm-dhcp-state-max" },
        { NM_DHCP_STATE_MAX, "NM_DHCP_STATE_MAX", "nm-dhcp-state-max" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMDhcpState"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_dns_ip_config_type_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_DNS_IP_CONFIG_TYPE_DEFAULT, "NM_DNS_IP_CONFIG_TYPE_DEFAULT", "default" },
        { NM_DNS_IP_CONFIG_TYPE_BEST_DEVICE, "NM_DNS_IP_CONFIG_TYPE_BEST_DEVICE", "best-device" },
        { NM_DNS_IP_CONFIG_TYPE_VPN, "NM_DNS_IP_CONFIG_TYPE_VPN", "vpn" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMDnsIPConfigType"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_dns_manager_resolv_conf_mode_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_DNS_MANAGER_RESOLV_CONF_UNMANAGED, "NM_DNS_MANAGER_RESOLV_CONF_UNMANAGED", "unmanaged" },
        { NM_DNS_MANAGER_RESOLV_CONF_EXPLICIT, "NM_DNS_MANAGER_RESOLV_CONF_EXPLICIT", "explicit" },
        { NM_DNS_MANAGER_RESOLV_CONF_PROXY, "NM_DNS_MANAGER_RESOLV_CONF_PROXY", "proxy" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMDnsManagerResolvConfMode"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_dns_manager_resolv_conf_manager_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_DNS_MANAGER_RESOLV_CONF_MAN_NONE, "NM_DNS_MANAGER_RESOLV_CONF_MAN_NONE", "none" },
        { NM_DNS_MANAGER_RESOLV_CONF_MAN_RESOLVCONF, "NM_DNS_MANAGER_RESOLV_CONF_MAN_RESOLVCONF", "resolvconf" },
        { NM_DNS_MANAGER_RESOLV_CONF_MAN_NETCONFIG, "NM_DNS_MANAGER_RESOLV_CONF_MAN_NETCONFIG", "netconfig" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMDnsManagerResolvConfManager"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_dns_masq_status_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_DNSMASQ_STATUS_UNKNOWN, "NM_DNSMASQ_STATUS_UNKNOWN", "unknown" },
        { NM_DNSMASQ_STATUS_DEAD, "NM_DNSMASQ_STATUS_DEAD", "dead" },
        { NM_DNSMASQ_STATUS_RUNNING, "NM_DNSMASQ_STATUS_RUNNING", "running" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMDnsMasqStatus"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_platform_signal_change_type_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_PLATFORM_SIGNAL_NONE, "NM_PLATFORM_SIGNAL_NONE", "none" },
        { NM_PLATFORM_SIGNAL_ADDED, "NM_PLATFORM_SIGNAL_ADDED", "added" },
        { NM_PLATFORM_SIGNAL_CHANGED, "NM_PLATFORM_SIGNAL_CHANGED", "changed" },
        { NM_PLATFORM_SIGNAL_REMOVED, "NM_PLATFORM_SIGNAL_REMOVED", "removed" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMPlatformSignalChangeType"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_rdisc_dhcp_level_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_RDISC_DHCP_LEVEL_UNKNOWN, "NM_RDISC_DHCP_LEVEL_UNKNOWN", "unknown" },
        { NM_RDISC_DHCP_LEVEL_NONE, "NM_RDISC_DHCP_LEVEL_NONE", "none" },
        { NM_RDISC_DHCP_LEVEL_OTHERCONF, "NM_RDISC_DHCP_LEVEL_OTHERCONF", "otherconf" },
        { NM_RDISC_DHCP_LEVEL_MANAGED, "NM_RDISC_DHCP_LEVEL_MANAGED", "managed" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMRDiscDHCPLevel"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_rdisc_preference_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_RDISC_PREFERENCE_INVALID, "NM_RDISC_PREFERENCE_INVALID", "invalid" },
        { NM_RDISC_PREFERENCE_LOW, "NM_RDISC_PREFERENCE_LOW", "low" },
        { NM_RDISC_PREFERENCE_MEDIUM, "NM_RDISC_PREFERENCE_MEDIUM", "medium" },
        { NM_RDISC_PREFERENCE_HIGH, "NM_RDISC_PREFERENCE_HIGH", "high" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMRDiscPreference"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_rdisc_config_map_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GFlagsValue values[] = {
        { NM_RDISC_CONFIG_DHCP_LEVEL, "NM_RDISC_CONFIG_DHCP_LEVEL", "dhcp-level" },
        { NM_RDISC_CONFIG_GATEWAYS, "NM_RDISC_CONFIG_GATEWAYS", "gateways" },
        { NM_RDISC_CONFIG_ADDRESSES, "NM_RDISC_CONFIG_ADDRESSES", "addresses" },
        { NM_RDISC_CONFIG_ROUTES, "NM_RDISC_CONFIG_ROUTES", "routes" },
        { NM_RDISC_CONFIG_DNS_SERVERS, "NM_RDISC_CONFIG_DNS_SERVERS", "dns-servers" },
        { NM_RDISC_CONFIG_DNS_DOMAINS, "NM_RDISC_CONFIG_DNS_DOMAINS", "dns-domains" },
        { NM_RDISC_CONFIG_HOP_LIMIT, "NM_RDISC_CONFIG_HOP_LIMIT", "hop-limit" },
        { NM_RDISC_CONFIG_MTU, "NM_RDISC_CONFIG_MTU", "mtu" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_flags_register_static (g_intern_static_string ("NMRDiscConfigMap"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_ppp_status_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_PPP_STATUS_UNKNOWN, "NM_PPP_STATUS_UNKNOWN", "unknown" },
        { NM_PPP_STATUS_DEAD, "NM_PPP_STATUS_DEAD", "dead" },
        { NM_PPP_STATUS_INITIALIZE, "NM_PPP_STATUS_INITIALIZE", "initialize" },
        { NM_PPP_STATUS_SERIALCONN, "NM_PPP_STATUS_SERIALCONN", "serialconn" },
        { NM_PPP_STATUS_DORMANT, "NM_PPP_STATUS_DORMANT", "dormant" },
        { NM_PPP_STATUS_ESTABLISH, "NM_PPP_STATUS_ESTABLISH", "establish" },
        { NM_PPP_STATUS_AUTHENTICATE, "NM_PPP_STATUS_AUTHENTICATE", "authenticate" },
        { NM_PPP_STATUS_CALLBACK, "NM_PPP_STATUS_CALLBACK", "callback" },
        { NM_PPP_STATUS_NETWORK, "NM_PPP_STATUS_NETWORK", "network" },
        { NM_PPP_STATUS_RUNNING, "NM_PPP_STATUS_RUNNING", "running" },
        { NM_PPP_STATUS_TERMINATE, "NM_PPP_STATUS_TERMINATE", "terminate" },
        { NM_PPP_STATUS_DISCONNECT, "NM_PPP_STATUS_DISCONNECT", "disconnect" },
        { NM_PPP_STATUS_HOLDOFF, "NM_PPP_STATUS_HOLDOFF", "holdoff" },
        { NM_PPP_STATUS_MASTER, "NM_PPP_STATUS_MASTER", "master" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMPPPStatus"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_settings_connection_flags_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GFlagsValue values[] = {
        { NM_SETTINGS_CONNECTION_FLAGS_NONE, "NM_SETTINGS_CONNECTION_FLAGS_NONE", "nm-settings-connection-flags-none" },
        { NM_SETTINGS_CONNECTION_FLAGS_UNSAVED, "NM_SETTINGS_CONNECTION_FLAGS_UNSAVED", "nm-settings-connection-flags-unsaved" },
        { NM_SETTINGS_CONNECTION_FLAGS_NM_GENERATED, "NM_SETTINGS_CONNECTION_FLAGS_NM_GENERATED", "nm-settings-connection-flags-nm-generated" },
        { NM_SETTINGS_CONNECTION_FLAGS_NM_GENERATED_ASSUMED, "NM_SETTINGS_CONNECTION_FLAGS_NM_GENERATED_ASSUMED", "nm-settings-connection-flags-nm-generated-assumed" },
        { __NM_SETTINGS_CONNECTION_FLAGS_LAST, "__NM_SETTINGS_CONNECTION_FLAGS_LAST", "--nm-settings-connection-flags-last" },
        { NM_SETTINGS_CONNECTION_FLAGS_ALL, "NM_SETTINGS_CONNECTION_FLAGS_ALL", "nm-settings-connection-flags-all" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_flags_register_static (g_intern_static_string ("NMSettingsConnectionFlags"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_settings_plugin_capabilities_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_SETTINGS_PLUGIN_CAP_NONE, "NM_SETTINGS_PLUGIN_CAP_NONE", "none" },
        { NM_SETTINGS_PLUGIN_CAP_MODIFY_CONNECTIONS, "NM_SETTINGS_PLUGIN_CAP_MODIFY_CONNECTIONS", "modify-connections" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMSettingsPluginCapabilities"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_settings_plugin_prop_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_SETTINGS_PLUGIN_PROP_FIRST, "NM_SETTINGS_PLUGIN_PROP_FIRST", "first" },
        { NM_SETTINGS_PLUGIN_PROP_NAME, "NM_SETTINGS_PLUGIN_PROP_NAME", "name" },
        { NM_SETTINGS_PLUGIN_PROP_INFO, "NM_SETTINGS_PLUGIN_PROP_INFO", "info" },
        { NM_SETTINGS_PLUGIN_PROP_CAPABILITIES, "NM_SETTINGS_PLUGIN_PROP_CAPABILITIES", "capabilities" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMSettingsPluginProp"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_opt_type_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { TYPE_INVALID, "TYPE_INVALID", "invalid" },
        { TYPE_INT, "TYPE_INT", "int" },
        { TYPE_BYTES, "TYPE_BYTES", "bytes" },
        { TYPE_UTF8, "TYPE_UTF8", "utf8" },
        { TYPE_KEYWORD, "TYPE_KEYWORD", "keyword" },
        { TYPE_STRING, "TYPE_STRING", "string" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("OptType"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_supplicant_feature_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_SUPPLICANT_FEATURE_UNKNOWN, "NM_SUPPLICANT_FEATURE_UNKNOWN", "unknown" },
        { NM_SUPPLICANT_FEATURE_NO, "NM_SUPPLICANT_FEATURE_NO", "no" },
        { NM_SUPPLICANT_FEATURE_YES, "NM_SUPPLICANT_FEATURE_YES", "yes" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMSupplicantFeature"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_supplicant_error_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_SUPPLICANT_ERROR_UNKNOWN, "NM_SUPPLICANT_ERROR_UNKNOWN", "Unknown" },
        { NM_SUPPLICANT_ERROR_CONFIG, "NM_SUPPLICANT_ERROR_CONFIG", "Config" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMSupplicantError"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_config_get_value_flags_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GFlagsValue values[] = {
        { NM_CONFIG_GET_VALUE_NONE, "NM_CONFIG_GET_VALUE_NONE", "none" },
        { NM_CONFIG_GET_VALUE_RAW, "NM_CONFIG_GET_VALUE_RAW", "raw" },
        { NM_CONFIG_GET_VALUE_STRIP, "NM_CONFIG_GET_VALUE_STRIP", "strip" },
        { NM_CONFIG_GET_VALUE_NO_EMPTY, "NM_CONFIG_GET_VALUE_NO_EMPTY", "no-empty" },
        { NM_CONFIG_GET_VALUE_TYPE_SPEC, "NM_CONFIG_GET_VALUE_TYPE_SPEC", "type-spec" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_flags_register_static (g_intern_static_string ("NMConfigGetValueFlags"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_config_change_flags_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GFlagsValue values[] = {
        { NM_CONFIG_CHANGE_NONE, "NM_CONFIG_CHANGE_NONE", "nm-config-change-none" },
        { NM_CONFIG_CHANGE_SIGHUP, "NM_CONFIG_CHANGE_SIGHUP", "nm-config-change-sighup" },
        { NM_CONFIG_CHANGE_SIGUSR1, "NM_CONFIG_CHANGE_SIGUSR1", "nm-config-change-sigusr1" },
        { NM_CONFIG_CHANGE_SIGUSR2, "NM_CONFIG_CHANGE_SIGUSR2", "nm-config-change-sigusr2" },
        { NM_CONFIG_CHANGE_CONFIG_FILES, "NM_CONFIG_CHANGE_CONFIG_FILES", "nm-config-change-config-files" },
        { NM_CONFIG_CHANGE_VALUES, "NM_CONFIG_CHANGE_VALUES", "nm-config-change-values" },
        { NM_CONFIG_CHANGE_VALUES_USER, "NM_CONFIG_CHANGE_VALUES_USER", "nm-config-change-values-user" },
        { NM_CONFIG_CHANGE_VALUES_INTERN, "NM_CONFIG_CHANGE_VALUES_INTERN", "nm-config-change-values-intern" },
        { NM_CONFIG_CHANGE_CONNECTIVITY, "NM_CONFIG_CHANGE_CONNECTIVITY", "nm-config-change-connectivity" },
        { NM_CONFIG_CHANGE_NO_AUTO_DEFAULT, "NM_CONFIG_CHANGE_NO_AUTO_DEFAULT", "nm-config-change-no-auto-default" },
        { NM_CONFIG_CHANGE_DNS_MODE, "NM_CONFIG_CHANGE_DNS_MODE", "nm-config-change-dns-mode" },
        { NM_CONFIG_CHANGE_RC_MANAGER, "NM_CONFIG_CHANGE_RC_MANAGER", "nm-config-change-rc-manager" },
        { NM_CONFIG_CHANGE_GLOBAL_DNS_CONFIG, "NM_CONFIG_CHANGE_GLOBAL_DNS_CONFIG", "nm-config-change-global-dns-config" },
        { _NM_CONFIG_CHANGE_LAST, "_NM_CONFIG_CHANGE_LAST", "-nm-config-change-last" },
        { NM_CONFIG_CHANGE_ALL, "NM_CONFIG_CHANGE_ALL", "nm-config-change-all" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_flags_register_static (g_intern_static_string ("NMConfigChangeFlags"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_dispatcher_action_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { DISPATCHER_ACTION_HOSTNAME, "DISPATCHER_ACTION_HOSTNAME", "hostname" },
        { DISPATCHER_ACTION_PRE_UP, "DISPATCHER_ACTION_PRE_UP", "pre-up" },
        { DISPATCHER_ACTION_UP, "DISPATCHER_ACTION_UP", "up" },
        { DISPATCHER_ACTION_PRE_DOWN, "DISPATCHER_ACTION_PRE_DOWN", "pre-down" },
        { DISPATCHER_ACTION_DOWN, "DISPATCHER_ACTION_DOWN", "down" },
        { DISPATCHER_ACTION_VPN_PRE_UP, "DISPATCHER_ACTION_VPN_PRE_UP", "vpn-pre-up" },
        { DISPATCHER_ACTION_VPN_UP, "DISPATCHER_ACTION_VPN_UP", "vpn-up" },
        { DISPATCHER_ACTION_VPN_PRE_DOWN, "DISPATCHER_ACTION_VPN_PRE_DOWN", "vpn-pre-down" },
        { DISPATCHER_ACTION_VPN_DOWN, "DISPATCHER_ACTION_VPN_DOWN", "vpn-down" },
        { DISPATCHER_ACTION_DHCP4_CHANGE, "DISPATCHER_ACTION_DHCP4_CHANGE", "dhcp4-change" },
        { DISPATCHER_ACTION_DHCP6_CHANGE, "DISPATCHER_ACTION_DHCP6_CHANGE", "dhcp6-change" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("DispatcherAction"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_auth_subject_type_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_AUTH_SUBJECT_TYPE_INVALID, "NM_AUTH_SUBJECT_TYPE_INVALID", "invalid" },
        { NM_AUTH_SUBJECT_TYPE_INTERNAL, "NM_AUTH_SUBJECT_TYPE_INTERNAL", "internal" },
        { NM_AUTH_SUBJECT_TYPE_UNIX_PROCESS, "NM_AUTH_SUBJECT_TYPE_UNIX_PROCESS", "unix-process" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMAuthSubjectType"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_auth_call_result_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_AUTH_CALL_RESULT_UNKNOWN, "NM_AUTH_CALL_RESULT_UNKNOWN", "unknown" },
        { NM_AUTH_CALL_RESULT_YES, "NM_AUTH_CALL_RESULT_YES", "yes" },
        { NM_AUTH_CALL_RESULT_AUTH, "NM_AUTH_CALL_RESULT_AUTH", "auth" },
        { NM_AUTH_CALL_RESULT_NO, "NM_AUTH_CALL_RESULT_NO", "no" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMAuthCallResult"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_ip_config_source_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_IP_CONFIG_SOURCE_UNKNOWN, "NM_IP_CONFIG_SOURCE_UNKNOWN", "nm-ip-config-source-unknown" },
        { _NM_IP_CONFIG_SOURCE_RTM_F_CLONED, "_NM_IP_CONFIG_SOURCE_RTM_F_CLONED", "-nm-ip-config-source-rtm-f-cloned" },
        { NM_IP_CONFIG_SOURCE_RTPROT_KERNEL, "NM_IP_CONFIG_SOURCE_RTPROT_KERNEL", "nm-ip-config-source-rtprot-kernel" },
        { NM_IP_CONFIG_SOURCE_KERNEL, "NM_IP_CONFIG_SOURCE_KERNEL", "nm-ip-config-source-kernel" },
        { NM_IP_CONFIG_SOURCE_SHARED, "NM_IP_CONFIG_SOURCE_SHARED", "nm-ip-config-source-shared" },
        { NM_IP_CONFIG_SOURCE_IP4LL, "NM_IP_CONFIG_SOURCE_IP4LL", "nm-ip-config-source-ip4ll" },
        { NM_IP_CONFIG_SOURCE_PPP, "NM_IP_CONFIG_SOURCE_PPP", "nm-ip-config-source-ppp" },
        { NM_IP_CONFIG_SOURCE_WWAN, "NM_IP_CONFIG_SOURCE_WWAN", "nm-ip-config-source-wwan" },
        { NM_IP_CONFIG_SOURCE_VPN, "NM_IP_CONFIG_SOURCE_VPN", "nm-ip-config-source-vpn" },
        { NM_IP_CONFIG_SOURCE_DHCP, "NM_IP_CONFIG_SOURCE_DHCP", "nm-ip-config-source-dhcp" },
        { NM_IP_CONFIG_SOURCE_RDISC, "NM_IP_CONFIG_SOURCE_RDISC", "nm-ip-config-source-rdisc" },
        { NM_IP_CONFIG_SOURCE_USER, "NM_IP_CONFIG_SOURCE_USER", "nm-ip-config-source-user" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMIPConfigSource"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_link_type_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_LINK_TYPE_NONE, "NM_LINK_TYPE_NONE", "none" },
        { NM_LINK_TYPE_UNKNOWN, "NM_LINK_TYPE_UNKNOWN", "unknown" },
        { NM_LINK_TYPE_ETHERNET, "NM_LINK_TYPE_ETHERNET", "ethernet" },
        { NM_LINK_TYPE_INFINIBAND, "NM_LINK_TYPE_INFINIBAND", "infiniband" },
        { NM_LINK_TYPE_OLPC_MESH, "NM_LINK_TYPE_OLPC_MESH", "olpc-mesh" },
        { NM_LINK_TYPE_WIFI, "NM_LINK_TYPE_WIFI", "wifi" },
        { NM_LINK_TYPE_WWAN_ETHERNET, "NM_LINK_TYPE_WWAN_ETHERNET", "wwan-ethernet" },
        { NM_LINK_TYPE_WIMAX, "NM_LINK_TYPE_WIMAX", "wimax" },
        { NM_LINK_TYPE_DUMMY, "NM_LINK_TYPE_DUMMY", "dummy" },
        { NM_LINK_TYPE_GRE, "NM_LINK_TYPE_GRE", "gre" },
        { NM_LINK_TYPE_GRETAP, "NM_LINK_TYPE_GRETAP", "gretap" },
        { NM_LINK_TYPE_IFB, "NM_LINK_TYPE_IFB", "ifb" },
        { NM_LINK_TYPE_IP6TNL, "NM_LINK_TYPE_IP6TNL", "ip6tnl" },
        { NM_LINK_TYPE_IPIP, "NM_LINK_TYPE_IPIP", "ipip" },
        { NM_LINK_TYPE_LOOPBACK, "NM_LINK_TYPE_LOOPBACK", "loopback" },
        { NM_LINK_TYPE_MACVLAN, "NM_LINK_TYPE_MACVLAN", "macvlan" },
        { NM_LINK_TYPE_MACVTAP, "NM_LINK_TYPE_MACVTAP", "macvtap" },
        { NM_LINK_TYPE_OPENVSWITCH, "NM_LINK_TYPE_OPENVSWITCH", "openvswitch" },
        { NM_LINK_TYPE_SIT, "NM_LINK_TYPE_SIT", "sit" },
        { NM_LINK_TYPE_TAP, "NM_LINK_TYPE_TAP", "tap" },
        { NM_LINK_TYPE_TUN, "NM_LINK_TYPE_TUN", "tun" },
        { NM_LINK_TYPE_VETH, "NM_LINK_TYPE_VETH", "veth" },
        { NM_LINK_TYPE_VLAN, "NM_LINK_TYPE_VLAN", "vlan" },
        { NM_LINK_TYPE_VXLAN, "NM_LINK_TYPE_VXLAN", "vxlan" },
        { NM_LINK_TYPE_BNEP, "NM_LINK_TYPE_BNEP", "bnep" },
        { NM_LINK_TYPE_BRIDGE, "NM_LINK_TYPE_BRIDGE", "bridge" },
        { NM_LINK_TYPE_BOND, "NM_LINK_TYPE_BOND", "bond" },
        { NM_LINK_TYPE_TEAM, "NM_LINK_TYPE_TEAM", "team" },
        { NM_LINK_TYPE_ANY, "NM_LINK_TYPE_ANY", "any" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMLinkType"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_pobject_type_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NMP_OBJECT_TYPE_UNKNOWN, "NMP_OBJECT_TYPE_UNKNOWN", "nmp-object-type-unknown" },
        { NMP_OBJECT_TYPE_LINK, "NMP_OBJECT_TYPE_LINK", "nmp-object-type-link" },
        { NMP_OBJECT_TYPE_IP4_ADDRESS, "NMP_OBJECT_TYPE_IP4_ADDRESS", "nmp-object-type-ip4-address" },
        { NMP_OBJECT_TYPE_IP6_ADDRESS, "NMP_OBJECT_TYPE_IP6_ADDRESS", "nmp-object-type-ip6-address" },
        { NMP_OBJECT_TYPE_IP4_ROUTE, "NMP_OBJECT_TYPE_IP4_ROUTE", "nmp-object-type-ip4-route" },
        { NMP_OBJECT_TYPE_IP6_ROUTE, "NMP_OBJECT_TYPE_IP6_ROUTE", "nmp-object-type-ip6-route" },
        { NMP_OBJECT_TYPE_LNK_GRE, "NMP_OBJECT_TYPE_LNK_GRE", "nmp-object-type-lnk-gre" },
        { NMP_OBJECT_TYPE_LNK_INFINIBAND, "NMP_OBJECT_TYPE_LNK_INFINIBAND", "nmp-object-type-lnk-infiniband" },
        { NMP_OBJECT_TYPE_LNK_IP6TNL, "NMP_OBJECT_TYPE_LNK_IP6TNL", "nmp-object-type-lnk-ip6tnl" },
        { NMP_OBJECT_TYPE_LNK_IPIP, "NMP_OBJECT_TYPE_LNK_IPIP", "nmp-object-type-lnk-ipip" },
        { NMP_OBJECT_TYPE_LNK_MACVLAN, "NMP_OBJECT_TYPE_LNK_MACVLAN", "nmp-object-type-lnk-macvlan" },
        { NMP_OBJECT_TYPE_LNK_MACVTAP, "NMP_OBJECT_TYPE_LNK_MACVTAP", "nmp-object-type-lnk-macvtap" },
        { NMP_OBJECT_TYPE_LNK_SIT, "NMP_OBJECT_TYPE_LNK_SIT", "nmp-object-type-lnk-sit" },
        { NMP_OBJECT_TYPE_LNK_VLAN, "NMP_OBJECT_TYPE_LNK_VLAN", "nmp-object-type-lnk-vlan" },
        { NMP_OBJECT_TYPE_LNK_VXLAN, "NMP_OBJECT_TYPE_LNK_VXLAN", "nmp-object-type-lnk-vxlan" },
        { __NMP_OBJECT_TYPE_LAST, "__NMP_OBJECT_TYPE_LAST", "--nmp-object-type-last" },
        { NMP_OBJECT_TYPE_MAX, "NMP_OBJECT_TYPE_MAX", "nmp-object-type-max" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMPObjectType"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_ip_config_merge_flags_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GFlagsValue values[] = {
        { NM_IP_CONFIG_MERGE_DEFAULT, "NM_IP_CONFIG_MERGE_DEFAULT", "default" },
        { NM_IP_CONFIG_MERGE_NO_ROUTES, "NM_IP_CONFIG_MERGE_NO_ROUTES", "no-routes" },
        { NM_IP_CONFIG_MERGE_NO_DNS, "NM_IP_CONFIG_MERGE_NO_DNS", "no-dns" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_flags_register_static (g_intern_static_string ("NMIPConfigMergeFlags"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_utils_error_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_UTILS_ERROR_UNKNOWN, "NM_UTILS_ERROR_UNKNOWN", "Unknown" },
        { NM_UTILS_ERROR_CANCELLED_DISPOSING, "NM_UTILS_ERROR_CANCELLED_DISPOSING", "CancelledDisposing" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMUtilsError"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_match_spec_match_type_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GEnumValue values[] = {
        { NM_MATCH_SPEC_NO_MATCH, "NM_MATCH_SPEC_NO_MATCH", "no-match" },
        { NM_MATCH_SPEC_MATCH, "NM_MATCH_SPEC_MATCH", "match" },
        { NM_MATCH_SPEC_NEG_MATCH, "NM_MATCH_SPEC_NEG_MATCH", "neg-match" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_enum_register_static (g_intern_static_string ("NMMatchSpecMatchType"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
GType
nm_utils_test_flags_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GFlagsValue values[] = {
        { NM_UTILS_TEST_NONE, "NM_UTILS_TEST_NONE", "nm-utils-test-none" },
        { _NM_UTILS_TEST_INITIALIZED, "_NM_UTILS_TEST_INITIALIZED", "-nm-utils-test-initialized" },
        { _NM_UTILS_TEST_GENERAL, "_NM_UTILS_TEST_GENERAL", "-nm-utils-test-general" },
        { NM_UTILS_TEST_NO_KEYFILE_OWNER_CHECK, "NM_UTILS_TEST_NO_KEYFILE_OWNER_CHECK", "nm-utils-test-no-keyfile-owner-check" },
        { _NM_UTILS_TEST_LAST, "_NM_UTILS_TEST_LAST", "-nm-utils-test-last" },
        { NM_UTILS_TEST_ALL, "NM_UTILS_TEST_ALL", "nm-utils-test-all" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_flags_register_static (g_intern_static_string ("NMUtilsTestFlags"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}



