#!/usr/bin/python3
# Test NetworkManager on simulated network devices
# For an interactive shell test, run "nm ColdplugWifi.shell", see below

__author__ = "Martin Pitt <martin.pitt@ubuntu.com>"
__copyright__ = "(C) 2013 Canonical Ltd."
__license__ = "GPL v2 or later"

import sys
import os
import os.path
import re
import time
import subprocess
import socket
import netaddr
import unittest
import ctypes
import shutil

try:
    from dbusmock import DBusTestCase
except ImportError:
    DBusTestCase = object  # dummy so that the class declaration works

import gi

gi.require_version("NM", "1.0")
from gi.repository import NM, GLib, Gio

sys.path.append(os.path.dirname(__file__))
import network_test_base

SSID = "fake net"

# If True, NetworkManager logs directly to stdout, to watch logs in real time
NM_LOG_STDOUT = os.getenv("NM_LOG_STDOUT", False)

# avoid accidentally destroying any real config
os.environ["GSETTINGS_BACKEND"] = "memory"

# we currently get a lot of WARNINGs/CRITICALs from GI (leaked objects from
# previous test runs/main loops?) Redirect them to stdout, to avoid failing
# autopkgtests
os.dup2(sys.stdout.fileno(), sys.stderr.fileno())


class WifiAuthentication():
    def __init__(self):
        self.wpa_settings = None
        self.wpa_eap_settings = None
        self.setting_name = None
        self.needed_secrets = None

        self.tmpdir = '/tmp/hostapd'
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    @property
    def auth_settings(self):
        return self.wpa_settings

    @property
    def auth_eap_settings(self):
        return self.wpa_eap_settings


class WifiAuthenticationWPAPSK(WifiAuthentication):
    def __init__(self, psk):
        super().__init__()
        self.wpa_settings = NM.SettingWirelessSecurity.new()
        self.wpa_settings.set_property(NM.SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-psk")
        self.wpa_settings.set_property(NM.SETTING_WIRELESS_SECURITY_PSK, psk)

        self.setting_name = NM.SETTING_WIRELESS_SECURITY_SETTING_NAME
        self.needed_secrets = [NM.SETTING_WIRELESS_SECURITY_PSK]


class WifiAuthenticationWPAEAP(WifiAuthentication):
    def __init__(self, modes, phase2, identity, password, client_cert=False):
        super().__init__()

        self.make_certs()

        self.wpa_settings = NM.SettingWirelessSecurity.new()
        self.wpa_settings.set_property(NM.SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-eap")

        self.wpa_eap_settings = NM.Setting8021x.new()
        self.wpa_eap_settings.set_property(NM.SETTING_802_1X_EAP, modes)

        self.wpa_eap_settings.set_property(NM.SETTING_802_1X_IDENTITY, identity)

        if password:
            self.wpa_eap_settings.set_property(NM.SETTING_802_1X_PASSWORD, password)

        if phase2 == 'tls':
            self.wpa_eap_settings.set_property(NM.SETTING_802_1X_PHASE2_AUTHEAP, phase2)
        else:
            self.wpa_eap_settings.set_property(NM.SETTING_802_1X_PHASE2_AUTH, phase2)

        if client_cert:
            # Certificate paths must start with file:// and be null terminated
            # See src/libnmc-setting/settings-docs.h
            self.wpa_eap_settings.set_property(
                    NM.SETTING_802_1X_CA_CERT,
                    GLib.Bytes(f'file://{self.tmpdir}/pki/ca.crt\0'.encode()))
            self.wpa_eap_settings.set_property(
                    NM.SETTING_802_1X_CLIENT_CERT,
                    GLib.Bytes(f'file://{self.tmpdir}/pki/issued/client.crt\0'.encode()))
            self.wpa_eap_settings.set_property(
                    NM.SETTING_802_1X_PRIVATE_KEY,
                    GLib.Bytes(f'file://{self.tmpdir}/pki/private/client.key\0'.encode()))
            self.wpa_eap_settings.set_property(
                    NM.SETTING_802_1X_PRIVATE_KEY_PASSWORD, 'passw0rd')

        self.setting_name = NM.SETTING_802_1X_SETTING_NAME
        self.needed_secrets = [NM.SETTING_802_1X_PASSWORD, NM.SETTING_802_1X_PRIVATE_KEY_PASSWORD]

    def make_certs(self):
        os.mkdir(self.tmpdir)

        with open(f'{self.tmpdir}/hostapd.eap_user', 'w') as f:
            f.write('''* PEAP,TLS

"account1" MSCHAPV2 "password1" [2]
"account2" MSCHAPV2 "password2" [2]
''')

        # Create a CA, server and client certificates protected by passw0rd and the DH parameters
        create_ca_script = """/usr/share/easy-rsa/easyrsa init-pki
EASYRSA_BATCH=1 /usr/share/easy-rsa/easyrsa build-ca nopass
EASYRSA_PASSOUT=pass:passw0rd EASYRSA_BATCH=1 /usr/share/easy-rsa/easyrsa build-server-full server
EASYRSA_PASSOUT=pass:passw0rd EASYRSA_BATCH=1 /usr/share/easy-rsa/easyrsa build-client-full client
/usr/share/easy-rsa/easyrsa gen-dh
"""

        with open(f'{self.tmpdir}/make_certs.sh', 'w') as f:
            f.write(create_ca_script)

        cmd = ['bash', 'make_certs.sh']
        subprocess.call(cmd, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL, cwd=self.tmpdir)


class NetworkManagerTest(network_test_base.NetworkTestBase):
    """Provide common functionality for NM tests"""

    def filtered_active_connections(self) -> list:
        # Ignore the 'lo' connection, active since NM 1.42:
        # https://networkmanager.dev/blog/networkmanager-1-42/#managing-the-loopback-interface
        active_connections = [c for c in self.nmclient.get_active_connections() if c.get_id() != 'lo']
        return active_connections

    def start_nm(self, wait_iface=None, auto_connect=True):
        """Start NetworkManager and initialize client object

        If wait_iface is given, wait until NM recognizes that interface.
        Otherwise, just wait until NM has initialized (for coldplug mode).

        If auto_connect is False, set the "no-auto-default=*" option to avoid
        auto-connecting to wired devices.
        """
        # mount tmpfses over system directories, to avoid destroying the
        # production configuration, and isolating tests from each other
        if not os.path.exists("/run/NetworkManager"):
            os.mkdir("/run/NetworkManager")
        for d in [
            "/etc/NetworkManager",
            "/var/lib/NetworkManager",
            "/run/NetworkManager",
            "/etc/netplan",
        ]:
            subprocess.check_call(["mount", "-n", "-t", "tmpfs", "none", d])
            self.addCleanup(subprocess.call, ["umount", d])
        os.mkdir("/etc/NetworkManager/system-connections")

        # create local configuration; this allows us to have full control, and
        # we also need to blacklist the AP device so that NM does not tear it
        # down; we also blacklist any existing real interface to avoid
        # interfering with it, and for getting predictable results
        blacklist = ""
        for iface in os.listdir("/sys/class/net"):
            if iface == "bonding_masters":
                continue
            if iface != self.dev_w_client and iface != self.dev_e_client:
                with open("/sys/class/net/%s/address" % iface) as f:
                    if blacklist:
                        blacklist += ";"
                    blacklist += "mac:%s" % f.read().strip()

        conf = os.path.join(self.workdir, "NetworkManager.conf")
        extra_main = ""
        if not auto_connect:
            extra_main += "no-auto-default=*\n"

        with open(conf, "w") as f:
            f.write(
                "[main]\nplugins=keyfile\n%s\n[keyfile]\nunmanaged-devices=%s\n"
                % (extra_main, blacklist)
            )

        if NM_LOG_STDOUT:
            f_log = None
        else:
            log = os.path.join(self.workdir, "NetworkManager.log")
            f_log = os.open(log, os.O_CREAT | os.O_WRONLY | os.O_SYNC)

        # build NM command line
        argv = ["NetworkManager", "--log-level=debug", "--debug", "--config=" + conf]
        # allow specifying extra arguments
        argv += os.environ.get("NM_TEST_DAEMON_ARGS", "").strip().split()

        p = subprocess.Popen(argv, stdout=f_log, stderr=subprocess.STDOUT)
        network_test_base.wait_nm_online()
        # automatically terminate process at end of test case
        self.addCleanup(p.wait)
        self.addCleanup(p.terminate)
        self.addCleanup(self.shutdown_connections)

        if NM_LOG_STDOUT:
            # let it initialize, then print a marker
            time.sleep(1)
            print("******* NM initialized *********\n\n")
        else:
            self.addCleanup(os.close, f_log)

            # this should be fast, give it 2 s to initialize
            if wait_iface:
                self.poll_text(log, "manager: (%s): new" % wait_iface, timeout=100)

        self.nmclient = NM.Client.new()
        self.assertTrue(self.nmclient.networking_get_enabled())

        # FIXME: This certainly ought to be true, but isn't
        # self.assertTrue(self.nmclient.get_manager_running())

        # determine device objects
        for d in self.nmclient.get_devices():
            if d.props.interface == self.dev_w_ap:
                self.assertEqual(d.get_device_type(), NM.DeviceType.WIFI)
                self.assertEqual(d.get_driver(), "mac80211_hwsim")
                self.assertEqual(d.get_hw_address(), self.mac_w_ap)
                self.nmdev_w_ap = d
            elif d.props.interface == self.dev_w_client:
                self.assertEqual(d.get_device_type(), NM.DeviceType.WIFI)
                self.assertEqual(d.get_driver(), "mac80211_hwsim")
                # NM ≥ 1.4 randomizes MAC addresses by default, so we can't
                # test for equality, just make sure it's not our AP
                self.assertNotEqual(d.get_hw_address(), self.mac_w_ap)
                self.nmdev_w = d
            elif d.props.interface == self.dev_e_client:
                self.assertEqual(d.get_device_type(), NM.DeviceType.VETH)
                self.assertEqual(d.get_driver(), "veth")
                self.assertEqual(d.get_hw_address(), self.mac_e_client)
                self.nmdev_e = d

        self.assertTrue(
            hasattr(self, "nmdev_w_ap"), "Could not determine wifi AP NM device"
        )
        self.assertTrue(
            hasattr(self, "nmdev_w"), "Could not determine wifi client NM device"
        )
        self.assertTrue(
            hasattr(self, "nmdev_e"), "Could not determine eth client NM device"
        )

        self.process_glib_events()

    def shutdown_connections(self):
        """Shut down all active NM connections."""

        if NM_LOG_STDOUT:
            print("\n\n******* Shutting down NM connections *********")

        # remove all created connections
        for active_conn in self.nmclient.get_active_connections():
            self.nmclient.deactivate_connection(active_conn)
        self.assertEventually(
            lambda: self.nmclient.get_active_connections() == [], timeout=20
        )

        # verify that NM properly deconfigures the devices
        self.assert_iface_down(self.dev_w_client)
        self.assert_iface_down(self.dev_e_client)

    @classmethod
    def process_glib_events(klass):
        """Process pending GLib main loop events"""

        context = GLib.MainContext.default()
        while context.iteration(False):
            pass

    def assertEventually(self, condition, message=None, timeout=50):
        """Assert that condition function eventually returns True.

        timeout is in deciseconds, defaulting to 50 (5 seconds). message is
        printed on failure.
        """
        while timeout >= 0:
            self.process_glib_events()
            if condition():
                break
            timeout -= 1
            time.sleep(0.1)
        else:
            self.fail(message or "timed out waiting for " + str(condition))

    def assert_iface_down(self, iface):
        """Assert that client interface is down"""

        out = subprocess.check_output(
            ["ip", "a", "show", "dev", iface], universal_newlines=True
        )
        self.assertNotIn("inet 192", out)
        self.assertNotIn("inet6 2600", out)

        if iface == self.dev_w_client:
            out = subprocess.check_output(
                ["iw", "dev", iface, "link"], universal_newlines=True
            )
            self.assertIn("Not connected", out)

            # but AP device should never be touched by NM
            out = subprocess.check_output(
                ["ip", "a", "show", "dev", self.dev_w_ap], universal_newlines=True
            )
            self.assertIn("state UP", out)

    def assert_iface_up(self, iface, expected_ip_a=None, unexpected_ip_a=None):
        """Assert that client interface is up"""

        out = subprocess.check_output(
            ["ip", "a", "show", "dev", iface], universal_newlines=True
        )
        self.assertIn("state UP", out)
        if expected_ip_a:
            for r in expected_ip_a:
                self.assertRegex(out, r)
        if unexpected_ip_a:
            for r in unexpected_ip_a:
                self.assertNotRegex(out, r)

        if iface == self.dev_w_client:
            out = subprocess.check_output(
                ["iw", "dev", iface, "link"], universal_newlines=True
            )
            self.assertIn("Connected to " + self.mac_w_ap, out)
            self.assertIn("SSID: " + SSID, out)

    def wait_ap(self, timeout):
        """Wait for AccessPoint NM object to appear, and return it"""

        self.assertEventually(
            lambda: len(self.nmdev_w.get_access_points()) > 0,
            "timed out waiting for AP to be detected",
            timeout=timeout,
        )

        return self.nmdev_w.get_access_points()[0]

    def connect_to_ap(self, ap, auth_settings, ipv6_mode, ip6_privacy):
        """Connect to an NMAccessPoint.

        auth_settings should be None for open networks and an instance of
        WifiAuthentication for WEP/WPA-PSK/WPA-EAP.

        ip6_privacy is a NM.SettingIP6ConfigPrivacy flag.

        Return (NMConnection, NMActiveConnection) objects.
        """

        ip4_method = NM.SETTING_IP4_CONFIG_METHOD_DISABLED
        ip6_method = NM.SETTING_IP6_CONFIG_METHOD_IGNORE
        if ipv6_mode is None:
            ip4_method = NM.SETTING_IP4_CONFIG_METHOD_AUTO
        else:
            ip6_method = NM.SETTING_IP6_CONFIG_METHOD_AUTO

        # If we have a secret, supply it to the new connection right away;
        # adding it afterwards with update_secrets() does not work, and we
        # can't implement a SecretAgent as get_secrets() would need to build a
        # map of a map of gpointers to gpointers which is too much for PyGI
        partial_conn = NM.SimpleConnection.new()
        partial_conn.add_setting(NM.SettingIP4Config(method=ip4_method))
        if auth_settings:
            partial_conn.add_setting(auth_settings.auth_settings)
            if isinstance(auth_settings, WifiAuthenticationWPAEAP):
                partial_conn.add_setting(auth_settings.auth_eap_settings)
        if ip6_privacy is not None:
            partial_conn.add_setting(
                NM.SettingIP6Config(ip6_privacy=ip6_privacy, method=ip6_method)
            )

        ml = GLib.MainLoop()
        self.cb_conn = None
        self.cancel = Gio.Cancellable()
        self.timeout_tag = 0

        def add_activate_cb(client, res, data):
            if self.timeout_tag > 0:
                GLib.source_remove(self.timeout_tag)
                self.timeout_tag = 0
            try:
                self.cb_conn = self.nmclient.add_and_activate_connection_finish(res)
            except gi.repository.GLib.Error as e:
                # Check if the error is "Operation was cancelled"
                if e.domain != "g-io-error-quark" or e.code != 19:
                    self.fail(
                        "add_and_activate_connection failed: %s (%s, %d)"
                        % (e.message, e.domain, e.code)
                    )
            ml.quit()

        def timeout_cb():
            self.timeout_tag = -1
            self.cancel.cancel()
            ml.quit()
            return GLib.SOURCE_REMOVE

        self.nmclient.add_and_activate_connection_async(
            partial_conn,
            self.nmdev_w,
            ap.get_path(),
            self.cancel,
            add_activate_cb,
            None,
        )
        self.timeout_tag = GLib.timeout_add_seconds(300, timeout_cb)
        ml.run()
        if self.timeout_tag < 0:
            self.timeout_tag = 0
            self.fail("Main loop for adding connection timed out!")
        self.assertNotEqual(self.cb_conn, None)
        active_conn = self.cb_conn
        self.cb_conn = None

        conn = self.conn_from_active_conn(active_conn)
        self.assertTrue(conn.verify())

        # verify need_secrets()
        needed_secrets = conn.need_secrets()
        if auth_settings is None:
            self.assertEqual(needed_secrets, (None, []))
        else:
            self.assertEqual(
                needed_secrets[0], auth_settings.setting_name
            )
            self.assertEqual(type(needed_secrets[1]), list)
            self.assertGreaterEqual(len(needed_secrets[1]), 1)
            self.assertIn(needed_secrets[1][0], auth_settings.needed_secrets)

        # we are usually ACTIVATING at this point; wait for completion
        # TODO: 5s is not enough, argh slow DHCP client
        self.assertEventually(
            lambda: active_conn.get_state() == NM.ActiveConnectionState.ACTIVATED,
            "timed out waiting for %s to get activated" % active_conn.get_connection(),
            timeout=600,
        )
        self.assertEqual(self.nmdev_w.get_state(), NM.DeviceState.ACTIVATED)
        return (conn, active_conn)

    def conn_from_active_conn(self, active_conn):
        """Get NMConnection object for an NMActiveConnection object"""

        # this sometimes takes a second try, when the corresponding
        # NMConnection object is not yet available
        tries = 3
        while tries > 0:
            self.process_glib_events()
            path = active_conn.get_connection().get_path()
            for dev in active_conn.get_devices():
                for c in dev.get_available_connections():
                    if c.get_path() == path:
                        return c
            time.sleep(0.1)
            tries -= 1

        self.fail("Could not find NMConnection object for %s" % path)

    def check_low_level_config(self, iface, ipv6_mode, ip6_privacy):
        """Check actual hardware state with ip/iw after being connected"""

        # list of expected regexps in "ip a" output
        expected_ip_a = []
        unexpected_ip_a = []

        if ipv6_mode is not None:
            if ipv6_mode in ("", "slaac"):
                # has global address from our DHCP server
                expected_ip_a.append("inet6 2600::[0-9a-f]+/")
            else:
                # has address with our prefix and MAC
                expected_ip_a.append(
                    r"inet6 2600::[0-9a-f:]+/64 scope global (?:tentative )?(?:mngtmpaddr )?(?:noprefixroute )?(dynamic|\n\s*valid_lft forever preferred_lft forever)"
                )
                # has address with our prefix and random IP (Privacy
                # Extension), if requested
                priv_re = r"inet6 2600:[0-9a-f:]+/64 scope global temporary (?:tentative )?(?:mngtmpaddr )?dynamic"
                if ip6_privacy in (
                    NM.SettingIP6ConfigPrivacy.PREFER_TEMP_ADDR,
                    NM.SettingIP6ConfigPrivacy.PREFER_PUBLIC_ADDR,
                ):
                    expected_ip_a.append(priv_re)
                else:
                    # FIXME: add a negative test here
                    pass
                    # unexpected_ip_a.append(priv_re)

            # has a link-local address
            expected_ip_a.append(r"inet6 fe80::[0-9a-f:]+/64 scope link")
        else:
            expected_ip_a.append(r"inet 192.168.5.\d+/24")

        self.assert_iface_up(iface, expected_ip_a, unexpected_ip_a)


class ColdplugWifi(NetworkManagerTest):
    """Wifi: In these tests NM starts after setting up the AP"""

    # not run by default; run "nm-wifi ColdplugWifi.shell" to get this
    @network_test_base.run_in_subprocess
    def shell(self):
        """Start AP and NM, then run a shell (for debugging)"""

        self.setup_ap("hw_mode=b\nchannel=1\nssid=" + SSID, None)
        self.start_nm(self.dev_w_client)
        print(
            """

client interface: %s, access point interface: %s, AP SSID: "%s"

You can now run commands like "nmcli dev" or "nmcli dev wifi connect '%s'".
Logs are in '%s'. When done, exit the shell.

"""
            % (self.dev_w_client, self.dev_w_ap, SSID, SSID, self.workdir)
        )
        subprocess.call(["bash", "-i"])

    @network_test_base.run_in_subprocess
    def test_no_ap(self):
        """no available access point"""

        self.start_nm(self.dev_w_client)
        # Give the interfaces a bit more time to intialized; it may be needed
        # on ppc64el.
        time.sleep(30)
        self.assertEventually(self.nmclient.networking_get_enabled, timeout=20)

        # state independent properties
        self.assertEqual(self.nmdev_w.props.device_type, NM.DeviceType.WIFI)
        self.assertTrue(self.nmdev_w.props.managed)
        self.assertFalse(self.nmdev_w.props.firmware_missing)
        self.assertTrue(
            self.nmdev_w.props.udi.startswith("/sys/devices/"), self.nmdev_w.props.udi
        )

        # get_version() plausibility check
        out = subprocess.check_output(["nmcli", "--version"], universal_newlines=True)
        cli_version = out.split()[-1]
        self.assertTrue(cli_version[0].isdigit())
        self.assertEqual(self.nmclient.get_version(), cli_version)

        # state dependent properties (disconnected)
        self.assertIn(
            self.nmdev_w.get_state(),
            [NM.DeviceState.DISCONNECTED, NM.DeviceState.UNAVAILABLE],
        )
        self.assertEqual(self.nmdev_w.get_access_points(), [])
        self.assertEqual(self.nmdev_w.get_available_connections(), [])

    def test_open_b_ip4(self):
        """Open network, 802.11b, IPv4"""

        self.do_test("hw_mode=b\nchannel=1\nssid=" + SSID, None, 11000)

    def test_open_b_ip6_raonly_tmpaddr(self):
        """Open network, 802.11b, IPv6 with only RA, preferring temp address"""

        self.do_test(
            "hw_mode=b\nchannel=1\nssid=" + SSID,
            "ra-only",
            11000,
            ip6_privacy=NM.SettingIP6ConfigPrivacy.PREFER_TEMP_ADDR,
        )

    def test_open_b_ip6_raonly_pubaddr(self):
        """Open network, 802.11b, IPv6 with only RA, preferring public address"""

        self.do_test(
            "hw_mode=b\nchannel=1\nssid=" + SSID,
            "ra-only",
            11000,
            ip6_privacy=NM.SettingIP6ConfigPrivacy.PREFER_PUBLIC_ADDR,
        )

    def test_open_b_ip6_raonly_no_pe(self):
        """Open network, 802.11b, IPv6 with only RA, PE disabled"""

        self.do_test(
            "hw_mode=b\nchannel=1\nssid=" + SSID,
            "ra-only",
            11000,
            ip6_privacy=NM.SettingIP6ConfigPrivacy.DISABLED,
        )

    def test_open_b_ip6_dhcp(self):
        """Open network, 802.11b, IPv6 with DHCP, preferring temp address"""

        self.do_test(
            "hw_mode=b\nchannel=1\nssid=" + SSID,
            "",
            11000,
            ip6_privacy=NM.SettingIP6ConfigPrivacy.UNKNOWN,
        )

    def test_open_g_ip4(self):
        """Open network, 802.11g, IPv4"""

        self.do_test("hw_mode=g\nchannel=1\nssid=" + SSID, None, 54000)

    def test_wpa1_ip4(self):
        """WPA1, 802.11g, IPv4"""

        self.do_test(
            """hw_mode=g
channel=1
ssid=%s
wpa=1
wpa_key_mgmt=WPA-PSK
wpa_pairwise=TKIP
wpa_passphrase=12345678
"""
            % SSID,
            None,
            54000,
            WifiAuthenticationWPAPSK('12345678'),
        )

    def test_wpa2_ip4(self):
        """WPA2, 802.11g, IPv4"""

        self.do_test(
            """hw_mode=g
channel=1
ssid=%s
wpa=2
wpa_key_mgmt=WPA-PSK
wpa_pairwise=CCMP
wpa_passphrase=12345678
"""
            % SSID,
            None,
            54000,
            WifiAuthenticationWPAPSK('12345678'),
        )

    def test_wpa2_ip6(self):
        """WPA2, 802.11g, IPv6 with only RA"""

        self.do_test(
            """hw_mode=g
channel=1
ssid=%s
wpa=2
wpa_key_mgmt=WPA-PSK
wpa_pairwise=CCMP
wpa_passphrase=12345678
"""
            % SSID,
            "ra-only",
            54000,
            WifiAuthenticationWPAPSK('12345678'),
            ip6_privacy=NM.SettingIP6ConfigPrivacy.PREFER_TEMP_ADDR,
        )

    def test_wpa_enterprise_authentication_ip4(self):
        """WPA2 + 802.1x, 802.11g, IPv4"""

        self.do_test(
            """hw_mode=g
channel=1
ssid=%s
auth_algs=1
eap_server=1
ieee8021x=1
eapol_version=2
wpa=2
wpa_key_mgmt=WPA-EAP
wpa_pairwise=TKIP
rsn_pairwise=CCMP
eap_user_file=/tmp/hostapd/hostapd.eap_user
ca_cert=/tmp/hostapd/pki/ca.crt
server_cert=/tmp/hostapd/pki/issued/server.crt
private_key=/tmp/hostapd/pki/private/server.key
private_key_passwd=passw0rd
dh_file=/tmp/hostapd/pki/dh.pem
ctrl_interface=/var/run/hostapd
ctrl_interface_group=0
"""
            % SSID,
            None,
            54000,
            WifiAuthenticationWPAEAP(['peap'], 'mschapv2', 'account1', 'password1')
        )

    def test_wpa_enterprise_authentication_with_client_certificate_ip4(self):
        """WPA2 + 802.1x with client certificate, 802.11g, IPv4"""

        self.do_test(
            """hw_mode=g
channel=1
ssid=%s
auth_algs=1
eap_server=1
ieee8021x=1
eapol_version=2
wpa=2
wpa_key_mgmt=WPA-EAP
wpa_pairwise=TKIP CCMP
rsn_pairwise=TKIP CCMP
eap_user_file=/tmp/hostapd/hostapd.eap_user
ca_cert=/tmp/hostapd/pki/ca.crt
server_cert=/tmp/hostapd/pki/issued/server.crt
private_key=/tmp/hostapd/pki/private/server.key
private_key_passwd=passw0rd
dh_file=/tmp/hostapd/pki/dh.pem
ctrl_interface=/var/run/hostapd
ctrl_interface_group=0
"""
            % SSID,
            None,
            54000,
            WifiAuthenticationWPAEAP(['tls'], 'tls', 'client', None, client_cert=True)
        )

    @network_test_base.run_in_subprocess
    def test_rfkill(self):
        """shut down connection on killswitch, restore it on unblock"""

        self.setup_ap("hw_mode=b\nchannel=1\nssid=" + SSID, None)
        self.start_nm(self.dev_w_client)
        ap = self.wait_ap(timeout=1800)
        (conn, active_conn) = self.connect_to_ap(ap, None, None, None)

        self.assertFalse(self.get_rfkill(self.dev_w_client))
        self.assertFalse(self.get_rfkill(self.dev_w_ap))

        # now block the client interface
        self.set_rfkill(self.dev_w_client, True)
        # disabling should be fast, give it ten seconds
        self.assertEventually(
            lambda: self.nmdev_w.get_state() == NM.DeviceState.UNAVAILABLE, timeout=100
        )

        # dev_w_client should be down now
        self.assert_iface_down(self.dev_w_client)

        # turn it back on
        self.set_rfkill(self.dev_w_client, False)
        # this involves DHCP, use same timeout as for regular connection
        self.assertEventually(
            lambda: self.nmdev_w.get_state() == NM.DeviceState.ACTIVATED, timeout=200
        )

        # dev_w_client should be back up
        self.assert_iface_up(self.dev_w_client, [r"inet 192.168.5.\d+/24"])

    #
    # Common test code
    #

    # libnm-glib has a lot of internal persistent state (private D-BUS
    # connections and such); as it is very brittle and hard to track down
    # all remaining references to any NM* object after a test, we rather
    # run each test in a separate subprocess
    @network_test_base.run_in_subprocess
    def do_test(
        self,
        hostapd_conf,
        ipv6_mode,
        expected_max_bitrate,
        auth_settings=None,
        ip6_privacy=None,
    ):
        """Actual test code, parameterized for the particular test case"""

        self.setup_ap(hostapd_conf, ipv6_mode)
        self.start_nm(self.dev_w_client)

        # on coldplug we expect the AP to be picked out fast
        ap = self.wait_ap(timeout=100)
        self.assertTrue(ap.get_path().startswith("/org/freedesktop/NetworkManager"))
        self.assertEqual(ap.get_mode(), getattr(NM, "80211Mode").INFRA)
        self.assertEqual(ap.get_max_bitrate(), expected_max_bitrate)
        # self.assertEqual(ap.get_flags(), )

        # should not auto-connect
        self.assertEqual(self.filtered_active_connections(), [])

        # connect to that AP
        (conn, active_conn) = self.connect_to_ap(ap, auth_settings, ipv6_mode, ip6_privacy)

        # check NMActiveConnection object
        self.assertIn(
            active_conn.get_uuid(),
            [c.get_uuid() for c in self.filtered_active_connections()],
        )
        self.assertEqual(
            [d.get_udi() for d in active_conn.get_devices()], [self.nmdev_w.get_udi()]
        )

        # check corresponding NMConnection object
        wireless_setting = conn.get_setting_wireless()
        self.assertEqual(wireless_setting.get_ssid().get_data(), SSID.encode())
        self.assertEqual(wireless_setting.get_hidden(), False)
        if auth_settings:
            self.assertEqual(
                conn.get_setting_wireless_security().get_name(),
                NM.SETTING_WIRELESS_SECURITY_SETTING_NAME,
            )
        else:
            self.assertEqual(conn.get_setting_wireless_security(), None)
        # for debugging
        # conn.dump()

        # for IPv6, check privacy setting
        if ipv6_mode is not None and ip6_privacy != NM.SettingIP6ConfigPrivacy.UNKNOWN:
            assert (
                ip6_privacy is not None
            ), "for IPv6 tests you need to specify ip6_privacy flag"
            ip6_setting = conn.get_setting_ip6_config()
            self.assertEqual(ip6_setting.props.ip6_privacy, ip6_privacy)

        self.check_low_level_config(self.dev_w_client, ipv6_mode, ip6_privacy)


class ColdplugEthernet(NetworkManagerTest):
    """Ethernet: In these tests NM starts after setting up the router"""

    # not run by default; run "nm-wifi ColdplugEthernet.shell" to get this
    @network_test_base.run_in_subprocess
    def shell(self):
        """Start router and NM, then run a shell (for debugging)"""

        self.setup_eth(None)
        self.start_nm(self.dev_e_client)
        print(
            """

client interface: %s, router interface: %s

You can now run commands like "nmcli dev".
Logs are in '%s'. When done, exit the shell.

"""
            % (self.dev_e_client, self.dev_e_ap, self.workdir)
        )
        subprocess.call(["bash", "-i"])

    def test_auto_ip4(self):
        """ethernet: auto-connection, IPv4"""

        self.do_test(None, auto_connect=True)

    def test_auto_ip6_raonly_no_pe(self):
        """ethernet: auto-connection, IPv6 with only RA, PE disabled"""

        self.do_test(
            "ra-only",
            auto_connect=True,
            ip6_privacy=NM.SettingIP6ConfigPrivacy.DISABLED,
        )

    def test_auto_ip6_dhcp(self):
        """ethernet: auto-connection, IPv6 with DHCP"""

        self.do_test(
            "", auto_connect=True, ip6_privacy=NM.SettingIP6ConfigPrivacy.UNKNOWN
        )

    def test_manual_ip4(self):
        """ethernet: manual connection, IPv4"""

        self.do_test(None, auto_connect=False)

    def test_manual_ip6_raonly_tmpaddr(self):
        """ethernet: manual connection, IPv6 with only RA, preferring temp address"""

        self.do_test(
            "ra-only",
            auto_connect=False,
            ip6_privacy=NM.SettingIP6ConfigPrivacy.PREFER_TEMP_ADDR,
        )

    def test_manual_ip6_raonly_pubaddr(self):
        """ethernet: manual connection, IPv6 with only RA, preferring public address"""

        self.do_test(
            "ra-only",
            auto_connect=False,
            ip6_privacy=NM.SettingIP6ConfigPrivacy.PREFER_PUBLIC_ADDR,
        )

    #
    # Common test code
    #

    @network_test_base.run_in_subprocess
    def do_test(self, ipv6_mode, ip6_privacy=None, auto_connect=True):
        """Actual test code, parameterized for the particular test case"""

        self.setup_eth(ipv6_mode)
        self.start_nm(self.dev_e_client, auto_connect=auto_connect)

        ip4_method = NM.SETTING_IP4_CONFIG_METHOD_DISABLED
        ip6_method = NM.SETTING_IP6_CONFIG_METHOD_IGNORE
        if ipv6_mode is None:
            ip4_method = NM.SETTING_IP4_CONFIG_METHOD_AUTO
        else:
            ip6_method = NM.SETTING_IP6_CONFIG_METHOD_AUTO

        if auto_connect:
            # ethernet should auto-connect quickly without an existing defined connection
            self.assertEventually(
                lambda: len(self.filtered_active_connections()) > 0,
                "timed out waiting for active connections",
                timeout=100,
            )
            active_conn = self.filtered_active_connections()[0]
        else:
            # auto-connection was disabled, set up manual connection
            partial_conn = NM.SimpleConnection.new()
            partial_conn.add_setting(NM.SettingIP4Config(method=ip4_method))
            if ip6_privacy is not None:
                partial_conn.add_setting(
                    NM.SettingIP6Config(ip6_privacy=ip6_privacy, method=ip6_method)
                )

            ml = GLib.MainLoop()
            self.cb_conn = None
            self.cancel = Gio.Cancellable()
            self.timeout_tag = 0

            def add_activate_cb(client, res, data):
                if self.timeout_tag > 0:
                    GLib.source_remove(self.timeout_tag)
                    self.timeout_tag = 0
                try:
                    self.cb_conn = self.nmclient.add_and_activate_connection_finish(res)
                except gi.repository.GLib.Error as e:
                    # Check if the error is "Operation was cancelled"
                    if e.domain != "g-io-error-quark" or e.code != 19:
                        self.fail(
                            "add_and_activate_connection failed: %s (%s, %d)"
                            % (e.message, e.domain, e.code)
                        )
                ml.quit()

            def timeout_cb():
                self.timeout_tag = -1
                self.cancel.cancel()
                ml.quit()
                return GLib.SOURCE_REMOVE

            self.nmclient.add_and_activate_connection_async(
                partial_conn, self.nmdev_e, None, self.cancel, add_activate_cb, None
            )
            self.timeout_tag = GLib.timeout_add_seconds(300, timeout_cb)
            ml.run()
            if self.timeout_tag < 0:
                self.timeout_tag = 0
                self.fail("Main loop for adding connection timed out!")
            self.assertNotEqual(self.cb_conn, None)
            active_conn = self.cb_conn
            self.cb_conn = None

        # we are usually ACTIVATING at this point; wait for completion
        # TODO: 5s is not enough, argh slow DHCP client
        self.assertEventually(
            lambda: active_conn.get_state() == NM.ActiveConnectionState.ACTIVATED,
            "timed out waiting for %s to get activated" % active_conn.get_connection(),
            timeout=150,
        )
        self.assertEqual(self.nmdev_e.get_state(), NM.DeviceState.ACTIVATED)

        conn = self.conn_from_active_conn(active_conn)
        self.assertTrue(conn.verify())

        # check NMActiveConnection object
        self.assertIn(
            active_conn.get_uuid(),
            [c.get_uuid() for c in self.filtered_active_connections()],
        )
        self.assertEqual(
            [d.get_udi() for d in active_conn.get_devices()], [self.nmdev_e.get_udi()]
        )

        # for IPv6, check privacy setting
        if ipv6_mode is not None:
            assert (
                ip6_privacy is not None
            ), "for IPv6 tests you need to specify ip6_privacy flag"
            if ip6_privacy not in (
                NM.SettingIP6ConfigPrivacy.UNKNOWN,
                NM.SettingIP6ConfigPrivacy.DISABLED,
            ):
                ip6_setting = conn.get_setting_ip6_config()
                self.assertEqual(ip6_setting.props.ip6_privacy, ip6_privacy)

        self.check_low_level_config(self.dev_e_client, ipv6_mode, ip6_privacy)


class Hotplug(NetworkManagerTest):
    """In these tests APs are set up while NM is already running"""

    @network_test_base.run_in_subprocess
    def test_auto_detect_ap(self):
        """new AP is being detected automatically within 30s"""

        self.start_nm()
        self.setup_ap("hw_mode=b\nchannel=1\nssid=" + SSID, None)
        ap = self.wait_ap(timeout=300)
        # get_ssid returns a byte array
        self.assertEqual(ap.get_ssid().get_data(), SSID.encode())
        self.assertEqual(self.nmdev_w.get_active_access_point(), None)

    @network_test_base.run_in_subprocess
    def test_auto_detect_eth(self):
        """new eth router is being detected automatically within 30s"""

        self.start_nm()
        self.setup_eth(None)
        self.assertEventually(
            lambda: len(self.filtered_active_connections()) > 0, timeout=300
        )
        active_conn = self.filtered_active_connections()[0]

        self.assertEventually(
            lambda: active_conn.get_state() == NM.ActiveConnectionState.ACTIVATED,
            "timed out waiting for %s to get activated" % active_conn.get_connection(),
            timeout=80,
        )
        self.assertEqual(self.nmdev_e.get_state(), NM.DeviceState.ACTIVATED)

        conn = self.conn_from_active_conn(active_conn)
        self.assertTrue(conn.verify())


@unittest.skipIf(
    DBusTestCase is object,
    "WARNING: python-dbusmock not installed, skipping suspend tests; get it from https://pypi.python.org/pypi/python-dbusmock",
)
class Suspend(NetworkManagerTest, DBusTestCase):
    """These tests run under a mock logind on a private system D-BUS"""

    @classmethod
    def setUpClass(klass):
        klass.start_system_bus()
        NetworkManagerTest.setUpClass()

    @classmethod
    def tearDownClass(klass):
        NetworkManagerTest.tearDownClass()
        DBusTestCase.tearDownClass()

    def setUp(self):
        NetworkManagerTest.setUp(self)

        # start mock polkit and logind processes, so that we can
        # intercept/control suspend
        (p_polkit, self.obj_polkit) = self.spawn_server_template(
            "polkitd", {}, stdout=subprocess.PIPE
        )
        # by default we are not concerned about restricting access in the tests
        self.obj_polkit.AllowUnknown(True)
        self.addCleanup(p_polkit.wait)
        self.addCleanup(p_polkit.terminate)

        (p_logind, self.obj_logind) = self.spawn_server_template(
            "logind", {}, stdout=subprocess.PIPE
        )
        self.addCleanup(p_logind.wait)
        self.addCleanup(p_logind.terminate)

        # we have to manually start wpa_supplicant, as D-BUS activation does
        # not happen for the fake D-BUS
        log = os.path.join(self.workdir, "wpasupplicant.log")
        p_wpasupp = subprocess.Popen(
            ["wpa_supplicant", "-u", "-d", "-e", "-K", self.entropy_file, "-f", log]
        )
        self.addCleanup(p_wpasupp.wait)
        self.addCleanup(p_wpasupp.terminate)

    def fixme_test_active_ip4(self):
        """suspend during active IPv4 connection"""

        self.do_test(
            "hw_mode=b\nchannel=1\nssid=" + SSID, None, [r"inet 192.168.5.\d+/24"]
        )

    def fixme_test_active_ip6(self):
        """suspend during active IPv6 connection"""

        self.do_test("hw_mode=b\nchannel=1\nssid=" + SSID, "ra-only", [r"inet6 2600::"])

    #
    # Common test code
    #

    @network_test_base.run_in_subprocess
    def do_test(self, hostapd_conf, ipv6_mode, expected_ip_a):
        """Actual test code, parameterized for the particular test case"""

        self.setup_ap(hostapd_conf, ipv6_mode)
        self.start_nm(self.dev_w_client)
        ap = self.wait_ap(timeout=1800)
        (conn, active_conn) = self.connect_to_ap(ap, None, ipv6_mode, None)

        # send logind signal that we are about to suspend
        self.obj_logind.EmitSignal("", "PrepareForSleep", "b", [True])

        # disabling should be fast, give it one second
        self.assertEventually(
            lambda: self.nmdev_w.get_state() == NM.DeviceState.UNMANAGED, timeout=10
        )
        self.assert_iface_down(self.dev_w_client)

        # send logind signal that we resumed
        self.obj_logind.EmitSignal("", "PrepareForSleep", "b", [False])

        # this involves DHCP, use same timeout as for regular connection
        self.assertEventually(
            lambda: self.nmdev_w.get_state() == NM.DeviceState.ACTIVATED, timeout=100
        )

        # dev_w_client should be back up
        self.assert_iface_up(self.dev_w_client, expected_ip_a)


def setUpModule():
    # AppArmor currently does not allow us to access the system D-BUS from an
    # unshared file system. Hack the policy to allow that until that gets fixed
    # properly. See https://launchpad.net/bugs/1244157
    subprocess.check_call(
        "sed '/nm-dhcp-client.action {/ s/{/flags=(attach_disconnected) {/'"
        " /etc/apparmor.d/sbin.dhclient > $ADTTMP/sbin.dhclient",
        shell=True,
    )
    subprocess.check_call("apparmor_parser -Kr $ADTTMP/sbin.dhclient", shell=True)

    # unshare the mount namespace, so that our tmpfs mounts are guaranteed to get
    # cleaned up, and don't influence the production system
    libc6 = ctypes.cdll.LoadLibrary("libc.so.6")
    assert (
        libc6.unshare(ctypes.c_int(0x00020000)) == 0
    ), "failed to unshare mount namespace"

    # stop system-wide NetworkManager to avoid interfering with tests
    nm_running = subprocess.call("service NetworkManager stop 2>&1", shell=True) == 0


def tearDownModule():
    subprocess.call("dhclient eth0", shell=True)
    subprocess.call("sleep 10", shell=True)


if __name__ == "__main__":
    # avoid unintelligible error messages, and breaking "make check" when not being
    # root
    if os.getuid() != 0:
        sys.stderr.write("This integration test suite needs to be run as root\n")
        sys.exit(1)

    if re.search(
        b"s390",
        subprocess.run(["dpkg", "--print-architecture"], capture_output=True).stdout,
    ):
        print("s390 arch has no wireless support, skipping")
        sys.exit(77)

    # write to stdout, not stderr
    runner = unittest.TextTestRunner(stream=sys.stdout, verbosity=2)
    unittest.main(testRunner=runner)
