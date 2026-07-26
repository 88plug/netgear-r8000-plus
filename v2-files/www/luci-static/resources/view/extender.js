'use strict';
'require view';
'require dom';
'require poll';
'require ui';
'require rpc';
'require uci';
'require network';

// WiFi Extender page: "repeat any scanned network", GL.iNet-style - not a
// new scan/join UI (stock LuCI's Network > Wireless > Scan > Join Network
// already does that, unchanged, reused as-is here). This page's only job:
// take an ALREADY-JOINED STA-mode wifi-iface (created by stock Join
// Network, or manually) and provision its missing half - the AP-side
// repeat companion (etc/init.d/eufy-repeater's generic repeater_mode='1'
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
// No automatic service restart on save: this router's brcmfmac has a
// confirmed, repeated failure mode where `iw dev <if> del` on an
// already-in-use AP/STA interface fails (-52) and the ONLY reliable
// recovery found this whole project is a clean reboot (see
// docs/FINDINGS.md, "the interface-deletion wedge", multiple
// occurrences). Auto-restarting eufy-repeater on every wireless save
// (the generic ucitrack "affects" pattern LuCI uses for e.g. firewall)
// would hit that wedge unpredictably. This page saves UCI only and asks
// for an explicit reboot instead - slower, but it's the operation this
// project has actually verified is reliable, not a guess.

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

function addRepeaterPair(base, device, network_name, ssid, encryption, key, isNewSta) {
	// AP side: matches etc/init.d/eufy-repeater's expectations exactly -
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
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('wireless'),
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

	handleEnableRepeat: function(sta, ev) {
		var btn = ev.currentTarget;
		btn.disabled = true;
		btn.classList.add('spinning');
		btn.firstChild.data = _('Working…');

		var sections = uci.sections('wireless', 'wifi-iface');
		var base = nextFreeBaseName(sections);

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

	handleReboot: function(ev) {
		ui.showModal(_('Rebooting…'), [
			E('p', { 'class': 'spinning' }, _('The system is rebooting. This may take a minute.'))
		]);
		return callReboot();
	},

	render: function() {
		var sections = uci.sections('wireless', 'wifi-iface');
		var stas = sections.filter(function(s) { return s.mode === 'sta'; });

		var rows = stas.map(function(sta) {
			var companion = findCompanion(sections, sta.device, sta.ssid);
			var band = (sta.device === BAND24_RADIO) ? '2.4GHz' : '5GHz';
			var actionCell;
			if (companion) {
				actionCell = E('em', {}, _('Repeating'));
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
		}, this);

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
					'click': ui.createHandlerFn(this, 'handleReboot')
				}, _('Reboot Now (apply repeater changes)'))
			])
		]);
	}
});
