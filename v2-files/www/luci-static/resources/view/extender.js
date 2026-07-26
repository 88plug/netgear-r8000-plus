'use strict';
'require view';
'require dom';
'require poll';
'require fs';
'require ui';
'require rpc';
'require uci';
'require network';

// WiFi Extender page: "repeat any scanned network", GL.iNet-style - not a
// new scan/join UI (stock LuCI's Network > Wireless > Scan > Join Network
// already does that, unchanged, reused as-is here). This page's only job:
// take an ALREADY-JOINED STA-mode wifi-iface (created by stock Join
// Network, or manually) and provision its missing half - the AP-side
// repeat companion (etc/init.d/extender's generic repeater_mode='1'
// mechanism, unchanged - see docs/FINDINGS.md §15/§26/§28 for why it's a
// raw iw+hostapd+relayd vif rather than a normal wifi-scripts AP, and why
// its BSSID is cloned from the upstream AP rather than derived-unique).
//
// Dual-band: on "Enable Repeating", scans the OTHER band's radio for the
// SAME SSID (confirmed live this session: a real neighboring network,
// "American", broadcasts identically on both 2.4GHz radio1 and 5GHz
// radio2) and if found, provisions a second STA+AP pair there too -
// this is the operator's own "auto repeat 2.4 and 5ghz for the same
// ssid/pass if found" request, done as a one-click extension of the
// existing per-band mechanism rather than a special case.
//
// Every repeat target also gets its own `network` interface
// (proto=dhcp, defaultroute='0', peerdns='0') and firewall zone covering
// both that interface and its raw AP device (`rpt_<base>_ap`) -
// confirmed-necessary, not optional: docs/FINDINGS.md §19 addendum found,
// on this router's very first hand-configured repeat target, that
// omitting defaultroute/peerdns lets the repeated network's own DHCP
// silently win this router's own default route (~50% loss to real
// internet, invisible to a 1-hop gateway ping), and that omitting the AP
// device from the firewall zone leaves every repeated client's traffic
// silently dropped by the default REJECT forward policy while
// hostapd/relayd/STA association all still look completely healthy. An
// earlier version of this page created the wireless sections for a new
// (dual-band) target without either of these - a real bug, not a
// hypothetical, caught by re-reading this exact history before shipping.
//
// No automatic service restart on save: this router's brcmfmac has a
// confirmed, repeated failure mode where `iw dev <if> del` on an
// already-in-use AP/STA interface fails (-52) and the ONLY reliable
// recovery found this whole project is a clean reboot (see
// docs/FINDINGS.md, "the interface-deletion wedge", multiple
// occurrences). Auto-restarting extender on every wireless save (the
// generic ucitrack "affects" pattern LuCI uses for e.g. firewall) would
// hit that wedge unpredictably. This page saves UCI only and asks for an
// explicit reboot instead - slower, but it's the operation this project
// has actually verified is reliable, not a guess.

var callReboot = rpc.declare({
	object: 'system',
	method: 'reboot',
	expect: { result: 0 }
});

// radio1 = the only 2.4GHz radio. radio0/radio2 are both 5GHz (paired for
// R8000's own 802.11k/v/r roaming today - docs/RUNBOOK.md §5); radio2 is
// the one this project repurposes for a second band's repeat, leaving
// radio0 as R8000's sole 5GHz AP (an accepted tradeoff - the operator's
// own stated direction is "all 3 radios will eventually repeat different
// networks").
var BAND24_RADIO = 'radio1';
var BAND5_RADIO = 'radio2';

function otherBandDevice(device) {
	if (device === BAND24_RADIO)
		return BAND5_RADIO;
	if (device === BAND5_RADIO || device === 'radio0')
		return BAND24_RADIO;
	return null;
}

function findCompanion(sections, device, ssid) {
	for (var i = 0; i < sections.length; i++) {
		var s = sections[i];
		if (s.mode === 'ap' && s.device === device && s.ssid === ssid && s.repeater_mode === '1')
			return s;
	}
	return null;
}

function nextFreeBaseName(sections) {
	var n = 0, name;
	for (;;) {
		name = 'ext' + n;
		var taken = sections.some(function(s) {
			return s['.name'] === (name + '_sta') || s['.name'] === (name + '_ap');
		});
		if (!taken)
			return name;
		n++;
	}
}

// Ensures a `network` interface + firewall zone exist for a repeat
// target's STA link, creating them if missing and fixing the
// defaultroute/peerdns protection if an existing interface lacks it
// (e.g. one created by stock Join Network, which has no way to know
// about this router's specific default-route-hijack history).
function ensureNetworkAndZone(networkName, apIfname) {
	if (!uci.get('network', networkName)) {
		uci.add('network', 'interface', networkName);
		uci.set('network', networkName, 'proto', 'dhcp');
	}
	uci.set('network', networkName, 'defaultroute', '0');
	uci.set('network', networkName, 'peerdns', '0');

	var zones = uci.sections('firewall', 'zone');
	var zone = zones.filter(function(z) {
		return (z.network || []).indexOf(networkName) !== -1;
	})[0];
	if (!zone) {
		var zoneName = networkName.replace(/_wwan$/, '');
		uci.add('firewall', 'zone', zoneName);
		uci.set('firewall', zoneName, 'name', zoneName);
		uci.set('firewall', zoneName, 'network', [networkName]);
		uci.set('firewall', zoneName, 'device', [apIfname]);
		uci.set('firewall', zoneName, 'input', 'ACCEPT');
		uci.set('firewall', zoneName, 'output', 'ACCEPT');
		uci.set('firewall', zoneName, 'forward', 'ACCEPT');
	} else {
		var devices = zone.device || [];
		if (devices.indexOf(apIfname) === -1) {
			devices = devices.concat([apIfname]);
			uci.set('firewall', zone['.name'], 'device', devices);
		}
	}
}

function addRepeaterPair(base, device, network_name, ssid, encryption, key, isNewSta) {
	// AP side: matches etc/init.d/extender's expectations exactly -
	// disabled='1' so wifi-scripts/netifd never touches it, repeater_mode='1'
	// so the init script's own config_foreach picks it up and brings it up
	// via raw iw + hostapd + relayd with a cloned upstream BSSID.
	uci.add('wireless', 'wifi-iface', base + '_ap');
	uci.set('wireless', base + '_ap', 'device', device);
	uci.set('wireless', base + '_ap', 'mode', 'ap');
	uci.set('wireless', base + '_ap', 'ssid', ssid);
	uci.set('wireless', base + '_ap', 'encryption', encryption || 'psk2');
	if (key)
		uci.set('wireless', base + '_ap', 'key', key);
	uci.set('wireless', base + '_ap', 'disabled', '1');
	uci.set('wireless', base + '_ap', 'repeater_mode', '1');

	if (isNewSta) {
		uci.add('wireless', 'wifi-iface', base + '_sta');
		uci.set('wireless', base + '_sta', 'device', device);
		uci.set('wireless', base + '_sta', 'network', network_name);
		uci.set('wireless', base + '_sta', 'mode', 'sta');
		uci.set('wireless', base + '_sta', 'ssid', ssid);
		uci.set('wireless', base + '_sta', 'encryption', encryption || 'psk2');
		if (key)
			uci.set('wireless', base + '_sta', 'key', key);
	}

	ensureNetworkAndZone(network_name, 'rpt_' + base + '_ap');
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('wireless'),
			uci.load('network'),
			uci.load('firewall'),
			network.getWifiDevices()
		]);
	},

	scanForSSID: function(deviceName, ssid) {
		return network.getWifiDevice(deviceName).then(function(dev) {
			if (!dev)
				return null;
			return dev.getScanList().then(function(results) {
				for (var i = 0; i < results.length; i++)
					if (results[i].ssid === ssid)
						return results[i];
				return null;
			}).catch(function() { return null; });
		});
	},

	// Live state of an AP companion. NOT queryable via network.getWifiNetwork()/
	// isUp() - those reflect netifd's OWN view of wireless devices, and the
	// whole point of this repeater architecture (docs/FINDINGS.md §15) is
	// that its AP side is deliberately created OUTSIDE netifd (disabled='1',
	// brought up by etc/init.d/extender's own raw iw+hostapd instead) -
	// confirmed live while building this: isUp() reported false for an AP
	// that was, at that exact moment, actually running with a real BSSID
	// clone and PROMISC/ALLMULTI set. hostapd's own ctrl_interface is the
	// only thing that actually knows this interface's real state, so ask
	// it directly. Falls back to "configured only" on any lookup failure -
	// a status page should never throw, only under-report.
	getCompanionStatus: function(companion) {
		var name = companion['.name'];
		var apIfname = 'rpt_' + name;
		return fs.exec('/usr/sbin/hostapd_cli', ['-p', '/var/run/hostapd-' + name, 'status'])
			.then(function(res) {
				var out = (res && res.code === 0) ? (res.stdout || '') : '';
				var active = /(^|\n)state=ENABLED/.test(out);
				var m = out.match(/num_sta\[0\]=(\d+)/);
				var clients = m ? parseInt(m[1], 10) : 0;
				return { active: active, clients: clients, ifname: apIfname };
			}).catch(function() { return { active: false, clients: 0, ifname: apIfname }; });
	},

	handleEnableRepeat: function(sta, ev) {
		var btn = ev.currentTarget;
		btn.disabled = true;
		btn.classList.add('spinning');
		btn.firstChild.data = _('Working…');

		var sections = uci.sections('wireless', 'wifi-iface');
		var base = nextFreeBaseName(sections);

		// sta.network may point at an interface stock Join Network created
		// with no idea about this router's default-route history -
		// ensureNetworkAndZone (inside addRepeaterPair) fixes it either way,
		// whether it already existed or is being created fresh here.
		addRepeaterPair(base, sta.device, sta.network, sta.ssid, sta.encryption, sta.key, false);

		var otherDev = otherBandDevice(sta.device);
		var self = this;
		var scanPromise = otherDev ? this.scanForSSID(otherDev, sta.ssid) : Promise.resolve(null);

		return scanPromise.then(function(found) {
			var dualBandMsg = '';
			if (found) {
				var sections2 = uci.sections('wireless', 'wifi-iface');
				var existing = findCompanion(sections2, otherDev, sta.ssid);
				if (!existing) {
					var base2 = nextFreeBaseName(sections2);
					addRepeaterPair(base2, otherDev, base2 + '_wwan', sta.ssid, sta.encryption, sta.key, true);
					dualBandMsg = _('Also found "%s" on the other band - repeating both.').format(sta.ssid);
				}
			}
			return uci.save().then(function() {
				return dualBandMsg;
			});
		}).then(function(dualBandMsg) {
			ui.addNotification(null, E('p', [
				E('strong', {}, _('Repeater configured for "%s".').format(sta.ssid)),
				dualBandMsg ? E('p', {}, dualBandMsg) : '',
				E('p', {}, _('A reboot is required to bring this up reliably - this router\'s driver does not reliably hot-swap AP/STA interfaces on the same radio (confirmed repeatedly this project). Use the "Reboot Now" button below when ready.'))
			]), 'info');
			return self.render();
		}).catch(function(err) {
			ui.addNotification(null, E('p', _('Failed: %s').format(err.message || err)), 'error');
			btn.disabled = false;
			btn.classList.remove('spinning');
		});
	},

	handleRemoveRepeat: function(companion, ev) {
		if (!confirm(_('Remove the repeater for "%s"? This deletes its wireless, network and firewall configuration.').format(companion.ssid)))
			return;

		var base = companion['.name'].replace(/_ap$/, '');
		var staSection = uci.get('wireless', base + '_sta');
		var networkName = staSection ? staSection.network : null;

		uci.remove('wireless', companion['.name']);
		// Only remove the STA side (and its network/firewall zone) if this
		// page created it as part of a dual-band pair (base + '_sta'
		// matching our own naming) - a STA joined independently via stock
		// Join Network, later turned into a repeater, stays joined.
		if (staSection && staSection['.name'] === base + '_sta' && /^ext\d+$/.test(base)) {
			uci.remove('wireless', base + '_sta');
			if (networkName) {
				// Only remove a zone if THIS network is its only member -
				// never delete a zone shared with something else just
				// because it happens to also list this network. Narrow in
				// practice (a dual-band-created network's name can't
				// pre-date this same save transaction), but a delete path
				// should never rely on "can't happen" alone.
				var zones = uci.sections('firewall', 'zone').filter(function(z) {
					var nets = z.network || [];
					return nets.indexOf(networkName) !== -1 && nets.length === 1;
				});
				zones.forEach(function(z) { uci.remove('firewall', z['.name']); });
				if (uci.get('network', networkName))
					uci.remove('network', networkName);
			}
		}

		var self = this;
		return uci.save().then(function() {
			ui.addNotification(null, E('p', _('Repeater for "%s" removed. Reboot to fully apply.').format(companion.ssid)), 'info');
			return self.render();
		});
	},

	handleReboot: function(ev) {
		ui.showModal(_('Rebooting…'), [
			E('p', { 'class': 'spinning' }, _('The system is rebooting. This may take a minute.'))
		]);
		return callReboot();
	},

	render: function() {
		var sections = uci.sections('wireless', 'wifi-iface');
		var stas = sections.filter(function(s) { return s.mode === 'sta'; });
		var companions = sections.filter(function(s) {
			return s.mode === 'ap' && s.repeater_mode === '1';
		});

		var self = this;
		return Promise.all(companions.map(function(c) { return self.getCompanionStatus(c); }))
			.then(function(statuses) {
				var statusByName = {};
				companions.forEach(function(c, i) { statusByName[c['.name']] = statuses[i]; });

				var rows = stas.map(function(sta) {
					var companion = findCompanion(sections, sta.device, sta.ssid);
					var band = (sta.device === BAND24_RADIO) ? '2.4GHz' : '5GHz';
					var actionCell;
					if (companion) {
						var status = statusByName[companion['.name']] || { active: false };
						var statusEl = status.active
							? E('span', { 'style': 'color:#5c5' }, [
									_('Repeating'), ' (', status.clients, ' ',
									(status.clients === 1 ? _('client') : _('clients')), ')'
								])
							: E('span', { 'style': 'color:#c95' }, _('Configured - reboot to activate'));
						actionCell = E('div', { 'style': 'display:flex;gap:.5em;align-items:center;justify-content:flex-end' }, [
							statusEl,
							E('button', {
								'class': 'cbi-button cbi-button-remove',
								'click': ui.createHandlerFn(this, 'handleRemoveRepeat', companion)
							}, _('Remove'))
						]);
					} else {
						actionCell = E('button', {
							'class': 'cbi-button cbi-button-action',
							'click': ui.createHandlerFn(this, 'handleEnableRepeat', sta)
						}, _('Enable Repeating'));
					}
					return E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' }, sta.ssid || E('em', {}, _('(hidden)'))),
						E('td', { 'class': 'td' }, band),
						E('td', { 'class': 'td' }, sta.device || ''),
						E('td', { 'class': 'td right' }, actionCell)
					]);
				}, self);

				var table = E('table', { 'class': 'table cbi-section-table' }, [
					E('tr', { 'class': 'tr table-titles' }, [
						E('th', { 'class': 'th' }, _('Network')),
						E('th', { 'class': 'th' }, _('Band')),
						E('th', { 'class': 'th' }, _('Radio')),
						E('th', { 'class': 'th right' }, _('Repeater'))
					])
				].concat(rows));

				if (rows.length === 0) {
					table.appendChild(E('tr', { 'class': 'tr placeholder' }, [
						E('td', { 'class': 'td', 'colspan': 4 }, [
							E('em', {}, _('No joined networks yet. Use Network › Wireless › Scan › Join Network to connect to one first, then come back here to turn it into a repeater.'))
						])
					]));
				}

				return E('div', {}, [
					E('h2', {}, _('Extender')),
					E('p', {}, _('Turn any network this router has joined into a WiFi repeater/extender - same SSID and password as the original, transparent to clients. If the same network is also found on the other band, both get repeated automatically.')),
					table,
					E('div', { 'class': 'cbi-page-actions right', 'style': 'margin-top:1em' }, [
						E('button', {
							'class': 'cbi-button cbi-button-positive important',
							'click': ui.createHandlerFn(self, 'handleReboot')
						}, _('Reboot Now (apply repeater changes)'))
					])
				]);
			});
	}
});
