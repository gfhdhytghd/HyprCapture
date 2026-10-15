"""Private notification daemon with delayed Notify acknowledgement."""
import threading

import gi
from gi.repository import Gio, GLib


class NotificationFixture:
    def __enter__(self):
        self.bus = Gio.TestDBus.new(Gio.TestDBusFlags.NONE)
        self.bus.up()
        self.address = self.bus.get_bus_address()
        self.connection = Gio.DBusConnection.new_for_address_sync(
            self.address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        self.connection.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
                                  'org.freedesktop.DBus', 'RequestName',
                                  GLib.Variant('(su)', ('org.freedesktop.Notifications', 0)),
                                  GLib.VariantType.new('(u)'), Gio.DBusCallFlags.NONE, 1000, None)
        self.node = Gio.DBusNodeInfo.new_for_xml('''
          <node><interface name="org.freedesktop.Notifications">
            <method name="GetCapabilities"><arg type="as" direction="out"/></method>
            <method name="GetServerInformation">
              <arg type="s" direction="out"/><arg type="s" direction="out"/>
              <arg type="s" direction="out"/><arg type="s" direction="out"/>
            </method>
            <method name="Notify">
              <arg type="s" direction="in"/><arg type="u" direction="in"/>
              <arg type="s" direction="in"/><arg type="s" direction="in"/>
              <arg type="s" direction="in"/><arg type="as" direction="in"/>
              <arg type="a{sv}" direction="in"/><arg type="i" direction="in"/>
              <arg type="u" direction="out"/>
            </method>
          </interface></node>''')
        self.acknowledged = []
        self.registration = self.connection.register_object(
            '/org/freedesktop/Notifications', self.node.interfaces[0], self.handle, None, None)
        self.loop = GLib.MainLoop()
        self.worker = threading.Thread(target=self.loop.run)
        self.worker.start()
        return self

    def handle(self, connection, sender, path, interface, method, parameters, invocation):
        if method == 'GetCapabilities':
            invocation.return_value(GLib.Variant('(as)', ([],)))
        elif method == 'GetServerInformation':
            invocation.return_value(GLib.Variant('(ssss)', ('fixture', 'HyprCapture', '1', '1.2')))
        elif method == 'Notify':
            def acknowledge():
                self.acknowledged.append(parameters.unpack())
                invocation.return_value(GLib.Variant('(u)', (1,)))
                return False
            GLib.timeout_add(250, acknowledge)

    def __exit__(self, *_):
        self.loop.quit()
        self.worker.join(timeout=2)
        self.connection.unregister_object(self.registration)
        self.connection.close_sync(None)
        self.bus.down()
