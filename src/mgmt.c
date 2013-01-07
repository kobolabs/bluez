/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2010  Nokia Corporation
 *  Copyright (C) 2010  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <glib.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <bluetooth/mgmt.h>

#include "log.h"
#include "adapter.h"
#include "device.h"
#include "eir.h"
#include "storage.h"
#include "mgmt.h"

#define MGMT_BUF_SIZE 1024

static int mgmt_sock = -1;
static guint mgmt_watch = 0;

static bool get_adapter_and_device(uint16_t index,
					struct mgmt_addr_info *addr,
					struct btd_adapter **adapter,
					struct btd_device **device,
					bool create)
{
	char peer_addr[18];

	*adapter = adapter_find_by_id(index);
	if (!*adapter) {
		error("Unable to find matching adapter");
		return false;
	}

	ba2str(&addr->bdaddr, peer_addr);

	if (create)
		*device = adapter_get_device(*adapter, peer_addr, addr->type);
	else
		*device = adapter_find_device(*adapter, peer_addr);

	if (create && !*device) {
		error("Unable to get device object!");
		return false;
	}

	return true;
}

static void bonding_complete(uint16_t index, const struct mgmt_addr_info *addr,
								uint8_t status)
{
	struct btd_adapter *adapter;

	adapter = adapter_find_by_id(index);
	if (adapter != NULL)
		adapter_bonding_complete(adapter, &addr->bdaddr, addr->type,
								status);
}

int mgmt_pincode_reply(int index, const bdaddr_t *bdaddr, const char *pin,
								size_t pin_len)
{
	char buf[MGMT_HDR_SIZE + sizeof(struct mgmt_cp_pin_code_reply)];
	struct mgmt_hdr *hdr = (void *) buf;
	size_t buf_len;
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s pinlen %zu", index, addr, pin_len);

	memset(buf, 0, sizeof(buf));

	if (pin == NULL) {
		struct mgmt_cp_pin_code_neg_reply *cp;

		hdr->opcode = htobs(MGMT_OP_PIN_CODE_NEG_REPLY);
		hdr->len = htobs(sizeof(*cp));
		hdr->index = htobs(index);

		cp = (void *) &buf[sizeof(*hdr)];
		bacpy(&cp->addr.bdaddr, bdaddr);
		cp->addr.type = BDADDR_BREDR;

		buf_len = sizeof(*hdr) + sizeof(*cp);
	} else {
		struct mgmt_cp_pin_code_reply *cp;

		if (pin_len > 16)
			return -EINVAL;

		hdr->opcode = htobs(MGMT_OP_PIN_CODE_REPLY);
		hdr->len = htobs(sizeof(*cp));
		hdr->index = htobs(index);

		cp = (void *) &buf[sizeof(*hdr)];
		bacpy(&cp->addr.bdaddr, bdaddr);
		cp->addr.type = BDADDR_BREDR;
		cp->pin_len = pin_len;
		memcpy(cp->pin_code, pin, pin_len);

		buf_len = sizeof(*hdr) + sizeof(*cp);
	}

	if (write(mgmt_sock, buf, buf_len) < 0)
		return -errno;

	return 0;
}

static void mgmt_pin_code_request(uint16_t index, void *buf, size_t len)
{
	struct mgmt_ev_pin_code_request *ev = buf;
	struct btd_adapter *adapter;
	struct btd_device *device;
	gboolean display = FALSE;
	char pin[17];
	ssize_t pinlen;
	char addr[18];
	int err;

	if (len < sizeof(*ev)) {
		error("Too small pin_code_request event");
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);

	DBG("hci%u %s", index, addr);

	if (!get_adapter_and_device(index, &ev->addr, &adapter, &device, true))
		return;

	memset(pin, 0, sizeof(pin));
	pinlen = btd_adapter_get_pin(adapter, device, pin, &display);
	if (pinlen > 0 && (!ev->secure || pinlen == 16)) {
		if (display && device_is_bonding(device, NULL)) {
			err = device_notify_pincode(device, ev->secure, pin);
			if (err < 0) {
				error("device_notify_pin: %s", strerror(-err));
				mgmt_pincode_reply(index, &ev->addr.bdaddr,
								NULL, 0);
			}
		} else {
			mgmt_pincode_reply(index, &ev->addr.bdaddr, pin, pinlen);
		}
		return;
	}

	err = device_request_pincode(device, ev->secure);
	if (err < 0) {
		error("device_request_pin: %s", strerror(-err));
		mgmt_pincode_reply(index, &ev->addr.bdaddr, NULL, 0);
	}
}

int mgmt_confirm_reply(int index, const bdaddr_t *bdaddr, uint8_t bdaddr_type,
							gboolean success)
{
	char buf[MGMT_HDR_SIZE + sizeof(struct mgmt_cp_user_confirm_reply)];
	struct mgmt_hdr *hdr = (void *) buf;
	struct mgmt_cp_user_confirm_reply *cp;
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s success %d", index, addr, success);

	memset(buf, 0, sizeof(buf));

	if (success)
		hdr->opcode = htobs(MGMT_OP_USER_CONFIRM_REPLY);
	else
		hdr->opcode = htobs(MGMT_OP_USER_CONFIRM_NEG_REPLY);

	hdr->len = htobs(sizeof(*cp));
	hdr->index = htobs(index);

	cp = (void *) &buf[sizeof(*hdr)];
	bacpy(&cp->addr.bdaddr, bdaddr);
	cp->addr.type = bdaddr_type;

	if (write(mgmt_sock, buf, sizeof(buf)) < 0)
		return -errno;

	return 0;
}

int mgmt_passkey_reply(int index, const bdaddr_t *bdaddr, uint8_t bdaddr_type,
							uint32_t passkey)
{
	char buf[MGMT_HDR_SIZE + sizeof(struct mgmt_cp_user_passkey_reply)];
	struct mgmt_hdr *hdr = (void *) buf;
	size_t buf_len;
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s passkey %06u", index, addr, passkey);

	memset(buf, 0, sizeof(buf));

	hdr->index = htobs(index);
	if (passkey == INVALID_PASSKEY) {
		struct mgmt_cp_user_passkey_neg_reply *cp;

		hdr->opcode = htobs(MGMT_OP_USER_PASSKEY_NEG_REPLY);
		hdr->len = htobs(sizeof(*cp));

		cp = (void *) &buf[sizeof(*hdr)];
		bacpy(&cp->addr.bdaddr, bdaddr);
		cp->addr.type = bdaddr_type;

		buf_len = sizeof(*hdr) + sizeof(*cp);
	} else {
		struct mgmt_cp_user_passkey_reply *cp;

		hdr->opcode = htobs(MGMT_OP_USER_PASSKEY_REPLY);
		hdr->len = htobs(sizeof(*cp));

		cp = (void *) &buf[sizeof(*hdr)];
		bacpy(&cp->addr.bdaddr, bdaddr);
		cp->addr.type = bdaddr_type;
		cp->passkey = htobl(passkey);

		buf_len = sizeof(*hdr) + sizeof(*cp);
	}

	if (write(mgmt_sock, buf, buf_len) < 0)
		return -errno;

	return 0;
}

static void mgmt_passkey_request(uint16_t index, void *buf, size_t len)
{
	struct mgmt_ev_user_passkey_request *ev = buf;
	struct btd_adapter *adapter;
	struct btd_device *device;
	char addr[18];
	int err;

	if (len < sizeof(*ev)) {
		error("Too small passkey_request event");
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);

	DBG("hci%u %s", index, addr);

	if (!get_adapter_and_device(index, &ev->addr, &adapter, &device, true))
		return;

	err = device_request_passkey(device);
	if (err < 0) {
		error("device_request_passkey: %s", strerror(-err));
		mgmt_passkey_reply(index, &ev->addr.bdaddr, ev->addr.type,
							INVALID_PASSKEY);
	}
}

static void mgmt_passkey_notify(uint16_t index, void *buf, size_t len)
{
	struct mgmt_ev_passkey_notify *ev = buf;
	struct btd_adapter *adapter;
	struct btd_device *device;
	uint32_t passkey;
	char addr[18];
	int err;

	if (len < sizeof(*ev)) {
		error("Too small passkey_notify event");
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);

	DBG("hci%u %s", index, addr);

	if (!get_adapter_and_device(index, &ev->addr, &adapter, &device, true))
		return;

	passkey = bt_get_le32(&ev->passkey);

	DBG("passkey %06u entered %u", passkey, ev->entered);

	err = device_notify_passkey(device, passkey, ev->entered);
	if (err < 0)
		error("device_notify_passkey: %s", strerror(-err));
}

static void mgmt_user_confirm_request(uint16_t index, void *buf,
								size_t len)
{
	struct mgmt_ev_user_confirm_request *ev = buf;
	struct btd_adapter *adapter;
	struct btd_device *device;
	char addr[18];
	int err;

	if (len < sizeof(*ev)) {
		error("Too small user_confirm_request event");
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);

	DBG("hci%u %s confirm_hint %u", index, addr, ev->confirm_hint);

	if (!get_adapter_and_device(index, &ev->addr,  &adapter, &device, true))
		return;

	err = device_confirm_passkey(device, btohl(ev->value),
							ev->confirm_hint);
	if (err < 0) {
		error("device_confirm_passkey: %s", strerror(-err));
		mgmt_confirm_reply(index, &ev->addr.bdaddr, ev->addr.type,
									FALSE);
	}
}

static void mgmt_cmd_complete(uint16_t index, void *buf, size_t len)
{
	struct mgmt_ev_cmd_complete *ev = buf;
	uint16_t opcode;

	if (len < sizeof(*ev)) {
		error("Too small management command complete event packet");
		return;
	}

	opcode = bt_get_le16(&ev->opcode);

	len -= sizeof(*ev);

	DBG("opcode 0x%04x status 0x%02x len %zu", opcode, ev->status, len);

	switch (opcode) {
	case MGMT_OP_READ_VERSION:
		DBG("read_version complete");
		break;
	case MGMT_OP_READ_INDEX_LIST:
		DBG("read_index_list complete");
		break;
	case MGMT_OP_READ_INFO:
		DBG("read_info complete");
		break;
	case MGMT_OP_SET_POWERED:
		DBG("set_powered complete");
		break;
	case MGMT_OP_SET_DISCOVERABLE:
		DBG("set_discoverable complete");
		break;
	case MGMT_OP_SET_CONNECTABLE:
		DBG("set_connectable complete");
		break;
	case MGMT_OP_SET_PAIRABLE:
		DBG("set_pairable complete");
		break;
	case MGMT_OP_SET_SSP:
		DBG("set_ssp complete");
		break;
	case MGMT_OP_SET_LE:
		DBG("set_le complete");
		break;
	case MGMT_OP_ADD_UUID:
		DBG("add_uuid complete");
		break;
	case MGMT_OP_REMOVE_UUID:
		DBG("remove_uuid complete");
		break;
	case MGMT_OP_SET_DEV_CLASS:
		DBG("set_dev_class complete");
		break;
	case MGMT_OP_LOAD_LINK_KEYS:
		DBG("load_link_keys complete");
		break;
	case MGMT_OP_CANCEL_PAIR_DEVICE:
		DBG("cancel_pair_device complete");
		break;
	case MGMT_OP_UNPAIR_DEVICE:
		DBG("unpair_device complete");
		break;
	case MGMT_OP_DISCONNECT:
		DBG("disconnect complete event");
		break;
	case MGMT_OP_GET_CONNECTIONS:
		DBG("get_connections complete");
		break;
	case MGMT_OP_PIN_CODE_REPLY:
		DBG("pin_code_reply complete");
		break;
	case MGMT_OP_PIN_CODE_NEG_REPLY:
		DBG("pin_code_neg_reply complete");
		break;
	case MGMT_OP_SET_IO_CAPABILITY:
		DBG("set_io_capability complete");
		break;
	case MGMT_OP_PAIR_DEVICE:
		DBG("pair_device complete");
		break;
	case MGMT_OP_USER_CONFIRM_REPLY:
		DBG("user_confirm_reply complete");
		break;
	case MGMT_OP_USER_CONFIRM_NEG_REPLY:
		DBG("user_confirm_net_reply complete");
		break;
	case MGMT_OP_SET_LOCAL_NAME:
		DBG("set_local_name complete");
		break;
	case MGMT_OP_READ_LOCAL_OOB_DATA:
		DBG("read_local_oob_data complete");
		break;
	case MGMT_OP_ADD_REMOTE_OOB_DATA:
		DBG("add_remote_oob_data complete");
		break;
	case MGMT_OP_REMOVE_REMOTE_OOB_DATA:
		DBG("remove_remote_oob_data complete");
		break;
	case MGMT_OP_BLOCK_DEVICE:
		DBG("block_device complete");
		break;
	case MGMT_OP_UNBLOCK_DEVICE:
		DBG("unblock_device complete");
		break;
	case MGMT_OP_SET_FAST_CONNECTABLE:
		DBG("set_fast_connectable complete");
		break;
	case MGMT_OP_START_DISCOVERY:
		DBG("start_discovery complete");
		break;
	case MGMT_OP_STOP_DISCOVERY:
		DBG("stop_discovery complete");
		break;
	case MGMT_OP_SET_DEVICE_ID:
		DBG("set_did complete");
		break;
	default:
		error("Unknown command complete for opcode %u", opcode);
		break;
	}
}

static void mgmt_cmd_status(uint16_t index, void *buf, size_t len)
{
	struct mgmt_ev_cmd_status *ev = buf;
	uint16_t opcode;

	if (len < sizeof(*ev)) {
		error("Too small management command status event packet");
		return;
	}

	opcode = bt_get_le16(&ev->opcode);

	if (!ev->status) {
		DBG("%s (0x%04x) cmd_status %u", mgmt_opstr(opcode), opcode,
								ev->status);
		return;
	}

	switch (opcode) {
	case MGMT_OP_READ_LOCAL_OOB_DATA:
		DBG("read_local_oob_data failed");
		break;
	}

	error("hci%u: %s (0x%04x) failed: %s (0x%02x)", index,
			mgmt_opstr(opcode), opcode, mgmt_errstr(ev->status),
			ev->status);
}

static void mgmt_device_blocked(uint16_t index, void *buf, size_t len)
{
	struct btd_adapter *adapter;
	struct btd_device *device;
	struct mgmt_ev_device_blocked *ev = buf;
	char addr[18];

	if (len < sizeof(*ev)) {
		error("Too small mgmt_device_blocked event packet");
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);
	DBG("Device blocked, index %u, addr %s", index, addr);

	if (!get_adapter_and_device(index, &ev->addr, &adapter, &device, false))
		return;

	if (device)
		device_block(device, TRUE);
}

static void mgmt_device_unblocked(uint16_t index, void *buf, size_t len)
{
	struct btd_adapter *adapter;
	struct btd_device *device;
	struct mgmt_ev_device_unblocked *ev = buf;
	char addr[18];

	if (len < sizeof(*ev)) {
		error("Too small mgmt_device_unblocked event packet");
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);
	DBG("Device unblocked, index %u, addr %s", index, addr);

	if (!get_adapter_and_device(index, &ev->addr, &adapter, &device, false))
		return;

	if (device)
		device_unblock(device, FALSE, TRUE);
}

static void store_longtermkey(const bdaddr_t *local, bdaddr_t *peer,
				uint8_t bdaddr_type, unsigned char *key,
				uint8_t master, uint8_t authenticated,
				uint8_t enc_size, uint16_t ediv,
				uint8_t rand[8])
{
	char adapter_addr[18];
	char device_addr[18];
	char filename[PATH_MAX + 1];
	GKeyFile *key_file;
	char key_str[35];
	char rand_str[19];
	char *str;
	int i;
	gsize length = 0;

	ba2str(local, adapter_addr);
	ba2str(peer, device_addr);

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s/info", adapter_addr,
								device_addr);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);

	key_str[0] = '0';
	key_str[1] = 'x';
	for (i = 0; i < 16; i++)
		sprintf(key_str + 2 + (i * 2), "%2.2X", key[i]);

	g_key_file_set_string(key_file, "LongTermKey", "Key", key_str);

	g_key_file_set_integer(key_file, "LongTermKey", "Authenticated",
				authenticated);
	g_key_file_set_integer(key_file, "LongTermKey", "Master", master);
	g_key_file_set_integer(key_file, "LongTermKey", "EncSize", enc_size);
	g_key_file_set_integer(key_file, "LongTermKey", "EDiv", ediv);

	rand_str[0] = '0';
	rand_str[1] = 'x';
	for (i = 0; i < 8; i++)
		sprintf(rand_str + 2 + (i * 2), "%2.2X", rand[i]);

	g_key_file_set_string(key_file, "LongTermKey", "Rand", rand_str);

	create_file(filename, S_IRUSR | S_IWUSR);

	str = g_key_file_to_data(key_file, &length, NULL);
	g_file_set_contents(filename, str, length, NULL);
	g_free(str);

	g_key_file_free(key_file);
}

static void mgmt_new_ltk(uint16_t index, void *buf, size_t len)
{
	struct mgmt_ev_new_long_term_key *ev = buf;
	struct btd_adapter *adapter;
	struct btd_device *device;

	if (len != sizeof(*ev)) {
		error("mgmt_new_ltk event size mismatch (%zu != %zu)",
							len, sizeof(*ev));
		return;
	}

	DBG("Controller %u new LTK authenticated %u enc_size %u", index,
				ev->key.authenticated, ev->key.enc_size);

	if (!get_adapter_and_device(index, &ev->key.addr,
						&adapter, &device, true))
		return;

	if (ev->store_hint) {
		struct mgmt_ltk_info *key = &ev->key;
		const bdaddr_t *bdaddr = adapter_get_address(adapter);

		store_longtermkey(bdaddr, &key->addr.bdaddr,
					key->addr.type, key->val, key->master,
					key->authenticated, key->enc_size,
					key->ediv, key->rand);

		device_set_bonded(device, TRUE);

		if (device_is_temporary(device))
			device_set_temporary(device, FALSE);
	}

	if (ev->key.master)
		bonding_complete(index, &ev->key.addr, 0);
}

static gboolean mgmt_event(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	char buf[MGMT_BUF_SIZE];
	struct mgmt_hdr *hdr = (void *) buf;
	ssize_t ret;
	uint16_t len, opcode, index;

	if (cond & G_IO_NVAL)
		return FALSE;

	if (cond & (G_IO_ERR | G_IO_HUP)) {
		error("Error on management socket");
		return FALSE;
	}

	ret = read(mgmt_sock, buf, sizeof(buf));
	if (ret < 0) {
		error("Unable to read from management socket: %s (%d)",
						strerror(errno), errno);
		return TRUE;
	}

	if (ret < MGMT_HDR_SIZE) {
		error("Too small Management packet");
		return TRUE;
	}

	opcode = bt_get_le16(&hdr->opcode);
	len = bt_get_le16(&hdr->len);
	index = bt_get_le16(&hdr->index);

	if (ret != MGMT_HDR_SIZE + len) {
		error("Packet length mismatch. ret %zd len %u", ret, len);
		return TRUE;
	}

	switch (opcode) {
	case MGMT_EV_CMD_COMPLETE:
		mgmt_cmd_complete(index, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_CMD_STATUS:
		mgmt_cmd_status(index, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_CONTROLLER_ERROR:
		DBG("controller_error event");
		break;
	case MGMT_EV_INDEX_ADDED:
		DBG("index_added event");
		break;
	case MGMT_EV_INDEX_REMOVED:
		DBG("index_removed event");
		break;
	case MGMT_EV_NEW_SETTINGS:
		DBG("new_settings event");
		break;
	case MGMT_EV_CLASS_OF_DEV_CHANGED:
		DBG("class_of_dev_changed event");
		break;
	case MGMT_EV_LOCAL_NAME_CHANGED:
		DBG("local_name_changed event");
		break;
	case MGMT_EV_NEW_LINK_KEY:
		DBG("new_link_key event");
		break;
	case MGMT_EV_DEVICE_CONNECTED:
		DBG("device_connected event");
		break;
	case MGMT_EV_DEVICE_DISCONNECTED:
		DBG("device_disconnected event");
		break;
	case MGMT_EV_CONNECT_FAILED:
		DBG("connect_failed event");
		break;
	case MGMT_EV_PIN_CODE_REQUEST:
		mgmt_pin_code_request(index, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_USER_CONFIRM_REQUEST:
		mgmt_user_confirm_request(index, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_AUTH_FAILED:
		DBG("auth_failed event");
		break;
	case MGMT_EV_DEVICE_FOUND:
		DBG("device_found event");
		break;
	case MGMT_EV_DISCOVERING:
		DBG("discovering event");
		break;
	case MGMT_EV_DEVICE_BLOCKED:
		mgmt_device_blocked(index, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_DEVICE_UNBLOCKED:
		mgmt_device_unblocked(index, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_DEVICE_UNPAIRED:
		DBG("device_unpaired event");
		break;
	case MGMT_EV_USER_PASSKEY_REQUEST:
		mgmt_passkey_request(index, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_PASSKEY_NOTIFY:
		mgmt_passkey_notify(index, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_NEW_LONG_TERM_KEY:
		mgmt_new_ltk(index, buf + MGMT_HDR_SIZE, len);
		break;
	default:
		error("Unknown Management opcode %u (index %u)", opcode, index);
		break;
	}

	return TRUE;
}

int mgmt_setup(void)
{
	struct mgmt_hdr hdr;
	struct sockaddr_hci addr;
	GIOChannel *io;
	GIOCondition condition;
	int dd, err;

	dd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (dd < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.hci_family = AF_BLUETOOTH;
	addr.hci_dev = HCI_DEV_NONE;
	addr.hci_channel = HCI_CHANNEL_CONTROL;

	if (bind(dd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		err = -errno;
		goto fail;
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.opcode = htobs(MGMT_OP_READ_INDEX_LIST);
	hdr.index = htobs(MGMT_INDEX_NONE);
	if (write(dd, &hdr, sizeof(hdr)) < 0) {
		err = -errno;
		goto fail;
	}

	io = g_io_channel_unix_new(dd);
	condition = G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL;
	mgmt_watch = g_io_add_watch(io, condition, mgmt_event, NULL);
	g_io_channel_unref(io);

	mgmt_sock = dd;

	return 0;

fail:
	close(dd);
	return err;
}

void mgmt_cleanup(void)
{
	if (mgmt_sock >= 0) {
		close(mgmt_sock);
		mgmt_sock = -1;
	}

	if (mgmt_watch > 0) {
		g_source_remove(mgmt_watch);
		mgmt_watch = 0;
	}
}
