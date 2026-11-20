// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Corsair Bragi cooling devices (iCUE LINK System Hub): hwmon driver and
 * command mediator.
 *
 * Bragi is the command protocol of Corsair's current devices. A command is
 * one output report; the device answers each one with one input report that
 * echoes the command byte and carries a status byte. Speeds, temperatures,
 * duties, colours and the stored fan tables are resources, opened on a
 * numbered handle and then read or written.
 *
 * The device needs one owner: it runs in software mode only while it sees
 * traffic, one mode command resets every fan, and replies carry no sender.
 * The driver is that owner. It hides the device's own hidraw node and puts a
 * client HID device with the same identity in front of it (the layout of
 * hid-steam). Every report a program writes to the client's hidraw node
 * arrives here and is sent as one exchange at a time, so each reply has a
 * known owner. Programs keep the whole protocol, with three exceptions:
 *
 *  - mode changes are answered, not sent: the driver holds software mode;
 *  - fan control writes follow the foreign_fan_control parameter;
 *  - the handle the driver uses for itself is refused.
 *
 * Fans without a host duty run the curve stored in the device, also in
 * software mode, and their speed stays readable. That is pwmN_enable = 2;
 * the stored curve is pwmN_auto_point*. A fan goes back to its stored curve
 * only at a mode change, which also closes every handle; the driver then
 * sends the remaining host duties again and reopens the handles programs
 * had open.
 *
 * Protocol notes and measurements: docs/hid-protocol.md and
 * docs/hid-probe.md beside this file.
 */

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define BRAGI_MAX_CH		25	/* LINK channels 1..25; record 0 is the hub */
#define BRAGI_RECORDS		(BRAGI_MAX_CH + 1)
#define BRAGI_MAX_POINTS	7
#define BRAGI_POLL_MS		1000
#define BRAGI_REPLY_TIMEOUT_MS	500
#define BRAGI_STALE_MS		3000
#define BRAGI_COMMIT_MS		500	/* collects a burst of curve edits into one write */
#define BRAGI_ENUM_BUF		1024
#define BRAGI_FLAG_REPLY		BIT(0)	/* in a command's flags byte: answer it */

/*
 * The handle the driver opens its own resources on. The hub accepts every
 * handle number and keeps handles independent, also for the same resource;
 * the known programs use 0, 1 and 2.
 */
#define BRAGI_HANDLE		0xfd

#define BRAGI_ST_OK		0x00
#define BRAGI_ST_REFUSED	0x03	/* hardware mode, or the handle is not open */
#define BRAGI_ST_UNSUPPORTED	0x05

/* opcodes */
#define BRAGI_SET		0x01
#define BRAGI_GET		0x02
#define BRAGI_CLOSE		0x05
#define BRAGI_WRITE		0x06
#define BRAGI_CONTINUE		0x07
#define BRAGI_READ		0x08
#define BRAGI_OPEN		0x0d

/* properties */
#define BRAGI_PROP_MODE		0x03
#define BRAGI_PROP_FIRMWARE	0x13
#define BRAGI_MODE_HARDWARE	0x01
#define BRAGI_MODE_SOFTWARE	0x02

/* resources: two id bytes as sent, low byte first */
#define BRAGI_RES_SPEEDS	0x0017
#define BRAGI_RES_SET_SPEED	0x0018
#define BRAGI_RES_TEMPS		0x0021
#define BRAGI_RES_SUBDEVICES	0x0036
#define BRAGI_RES_HW_MODES	0x6d63	/* stored: fixed duty or curve, per channel */
#define BRAGI_RES_HW_FIXED	0x6d64	/* stored: fixed duty per channel */
#define BRAGI_RES_HW_CURVES	0x6d65	/* stored: curve per channel */

#define BRAGI_HW_MODE_CURVE	0x02

/* reply layout after the echo and status bytes */
#define BRAGI_REPLY_DTYPE	4	/* 2-byte data type of a read */
#define BRAGI_REPLY_DATA	6

/* a write command: 06 <handle> LE16(len + 2) 00 00 <dtype> <data> */
#define BRAGI_WRITE_HDR		8

static const u8 bragi_dt_subdevices[2] = { 0x21, 0x00 };
static const u8 bragi_dt_speeds[2] = { 0x25, 0x00 };
static const u8 bragi_dt_temps[2] = { 0x10, 0x00 };
static const u8 bragi_dt_set_speed[2] = { 0x07, 0x00 };
static const u8 bragi_dt_hw_modes[2] = { 0x03, 0x00 };
static const u8 bragi_dt_hw_curves[2] = { 0x05, 0x00 };

enum {
	BRAGI_FOREIGN_REFUSE,
	BRAGI_FOREIGN_IGNORE,
	BRAGI_FOREIGN_APPLY,
};

static int foreign_fan_control = BRAGI_FOREIGN_REFUSE;
module_param(foreign_fan_control, int, 0644);
MODULE_PARM_DESC(foreign_fan_control,
		 "Fan control writes from programs on the hidraw node: 0 = refuse (default), 1 = accept and ignore, 2 = apply");

struct bragi_variant {
	const char *hwmon_name;
	u16 out_len;		/* including the report id byte */
	u16 in_len;
	u8 addr;		/* out[1]: the device itself; other values, devices behind it */
	u8 flags;		/* out[2] */
	u8 cmd_off;
	u8 echo_off;
	u8 status_off;
};

static const struct bragi_variant bragi_link_hub = {
	.hwmon_name = "corsair_link_hub",
	.out_len = 513,
	.in_len = 512,
	.addr = 0x00,
	.flags = BRAGI_FLAG_REPLY,
	.cmd_off = 3,
	.echo_off = 2,
	.status_off = 3,
};

struct bragi_record {
	bool present;
	u8 type;
	u8 model;
	bool pump;
	char label[24];
	bool has_fan;
	bool has_pwm;		/* a type the driver sets a duty for */
	bool has_temp;
	bool fan_ok;
	bool temp_ok;
	u16 rpm;
	s16 temp_dc;		/* 0.1 degC */
	u8 duty;		/* host duty, percent */
	bool host;		/* the host duty is in force; else the stored curve */
};

struct bragi_client_handle {
	bool open;
	u8 res[2];
};

struct bragi_point_attr {
	struct device_attribute dattr;
	char name[32];
	u8 ch;
	u8 point;
	bool temp;
};

struct bragi_dev {
	struct hid_device *hdev;
	struct hid_device *client;	/* the hidraw node programs use */
	const struct bragi_variant *var;
	struct device *hwmon_dev;

	/* Lock order: seq_lock, client_lock, io_lock. state_lock is a leaf. */
	struct mutex seq_lock;		/* the driver's command sequences; reply, scratch */
	struct mutex client_lock;	/* one client command; chandle, synth */
	struct mutex io_lock;		/* one exchange on the wire; out, in */
	spinlock_t reply_lock;		/* waiting, match, expect, in_size */
	spinlock_t state_lock;		/* duty, host, flags, curves, curve_off */

	struct completion reply_done;
	bool waiting;
	bool match;
	u8 expect;
	int in_size;
	u8 *out;
	u8 *in;
	u8 *reply;			/* the driver's copy of its last reply */
	int reply_size;
	u8 *scratch;
	u8 *synth;
	struct delayed_work poll;

	struct bragi_record rec[BRAGI_RECORDS];
	bool duty_dirty;		/* host duties to send */
	bool reload;			/* a fan went back to its curve: mode change due */
	bool curves_dirty;		/* curves differs from the device's table */
	bool curves_stale;		/* a program wrote the stored tables */
	unsigned long updated;
	bool valid;
	u8 fw[2];
	u16 fw_build;

	/* The stored curve table as read: count, then per record sensor, points, 4 bytes a point. */
	u8 *curves;
	size_t curves_len;
	u16 curve_off[BRAGI_RECORDS];	/* offset of a channel's record; 0: no usable curve */

	struct bragi_client_handle chandle[256];	/* by handle number */

	struct attribute_group point_group;
	const struct attribute_group *groups[2];
};

struct bragi_type {
	u8 type;
	s8 model;		/* -1: every model */
	const char *name;
	bool duty;		/* a fan or pump: the driver sets its duty */
	bool pump;
};

/*
 * LINK device types. A duty is offered only for the types known to take
 * one; any other device with a speed is read only.
 */
static const struct bragi_type bragi_types[] = {
	{ 0x01, -1, "QX RGB", true },
	{ 0x02, -1, "LX RGB", true },
	{ 0x03, -1, "RX RGB MAX", true },
	{ 0x04, -1, "RX MAX", true },
	{ 0x05, -1, "adapter" },
	{ 0x07, -1, "H-series AIO", true, true },
	{ 0x09, -1, "XC7" },
	{ 0x0a, -1, "XG3", true },
	{ 0x0b, -1, "HXi PSU" },
	{ 0x0c, -1, "XD5", true, true },
	{ 0x0d, -1, "XG7" },
	{ 0x0f, -1, "RX RGB", true },
	{ 0x10, -1, "VRM fan", true },
	{ 0x11, -1, "TITAN AIO", true, true },
	{ 0x13, -1, "RX", true },
	{ 0x19, -1, "XD6", true, true },
	{ 0x1a, 0, "fan", true },
	{ 0x1a, 1, "AIO pump", true, true },
	{ 0x1a, 8, "LCD module" },
	{ 0x1b, -1, "Commander Duo", true },
};

static const struct bragi_type *bragi_type(u8 type, u8 model)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(bragi_types); i++)
		if (bragi_types[i].type == type &&
		    (bragi_types[i].model < 0 || bragi_types[i].model == model))
			return &bragi_types[i];
	return NULL;
}

/* transport */

/*
 * One exchange: send the report in bd->out and wait for its reply, which
 * lands in bd->in. io_lock held. Only this function sends, so the next
 * report is the reply; the echo byte is still checked to drop the late reply
 * of an exchange that timed out. Returns the status byte or a negative errno.
 */
static int bragi_io(struct bragi_dev *bd, size_t len, bool match, u8 echo)
{
	const struct bragi_variant *var = bd->var;
	int ret;

	spin_lock_irq(&bd->reply_lock);
	bd->match = match;
	bd->expect = echo;
	bd->waiting = true;
	reinit_completion(&bd->reply_done);
	spin_unlock_irq(&bd->reply_lock);

	ret = hid_hw_output_report(bd->hdev, bd->out, len);
	if (ret >= 0 && !wait_for_completion_timeout(&bd->reply_done,
						     msecs_to_jiffies(BRAGI_REPLY_TIMEOUT_MS)))
		ret = -ETIMEDOUT;
	if (ret < 0) {
		spin_lock_irq(&bd->reply_lock);
		bd->waiting = false;
		spin_unlock_irq(&bd->reply_lock);
		return ret;
	}

	return bd->in_size > var->status_off ? bd->in[var->status_off] : BRAGI_ST_OK;
}

static int bragi_raw_event(struct hid_device *hdev, struct hid_report *report,
			   u8 *data, int size)
{
	struct bragi_dev *bd = hid_get_drvdata(hdev);
	const struct bragi_variant *var;
	unsigned long flags;

	/* the client device and the interface without commands */
	if (!bd)
		return 0;

	var = bd->var;
	spin_lock_irqsave(&bd->reply_lock, flags);
	if (bd->waiting &&
	    (!bd->match || (size > var->status_off && data[var->echo_off] == bd->expect))) {
		bd->in_size = min_t(int, size, var->in_len);
		memcpy(bd->in, data, bd->in_size);
		bd->waiting = false;
		complete(&bd->reply_done);
	}
	spin_unlock_irqrestore(&bd->reply_lock, flags);

	return 0;
}

/*
 * A command of the driver. seq_lock held. The reply goes to bd->reply,
 * unless @keep: then the reply of an earlier command stays there.
 * Returns the status byte or a negative errno.
 */
static int bragi_cmd_keep(struct bragi_dev *bd, const u8 *cmd, size_t len, bool keep)
{
	const struct bragi_variant *var = bd->var;
	int ret;

	if (WARN_ON(var->cmd_off + len > var->out_len))
		return -EINVAL;

	mutex_lock(&bd->io_lock);
	memset(bd->out, 0, var->out_len);
	bd->out[1] = var->addr;
	bd->out[2] = var->flags;
	memcpy(bd->out + var->cmd_off, cmd, len);
	ret = bragi_io(bd, var->out_len, true, cmd[0]);
	if (ret >= 0 && !keep) {
		memset(bd->reply, 0, var->in_len);
		memcpy(bd->reply, bd->in, bd->in_size);
		bd->reply_size = bd->in_size;
	}
	mutex_unlock(&bd->io_lock);

	return ret;
}

static int bragi_cmd(struct bragi_dev *bd, const u8 *cmd, size_t len)
{
	return bragi_cmd_keep(bd, cmd, len, false);
}

static int bragi_status_errno(int st)
{
	if (st < 0)
		return st;
	return st == BRAGI_ST_OK ? 0 : -EIO;
}

/* resources on the driver's handle; seq_lock held */

/* The reply of a read before it stays in bd->reply: the callers parse it after the close. */
static void bragi_res_close(struct bragi_dev *bd)
{
	const u8 cmd[] = { BRAGI_CLOSE, 0x01, BRAGI_HANDLE };

	bragi_cmd_keep(bd, cmd, sizeof(cmd), true);
}

/* -EAGAIN: the device is in hardware mode */
static int bragi_res_open(struct bragi_dev *bd, u16 res)
{
	const u8 cmd[] = { BRAGI_OPEN, BRAGI_HANDLE, res & 0xff, res >> 8 };
	int st;

	bragi_res_close(bd);
	st = bragi_cmd(bd, cmd, sizeof(cmd));
	if (st == BRAGI_ST_REFUSED)
		return -EAGAIN;
	return bragi_status_errno(st);
}

/* Read one resource into bd->reply. */
static int bragi_res_read(struct bragi_dev *bd, u16 res, const u8 *dtype)
{
	const u8 cmd[] = { BRAGI_READ, BRAGI_HANDLE };
	int ret;

	ret = bragi_res_open(bd, res);
	if (ret)
		return ret;
	ret = bragi_status_errno(bragi_cmd(bd, cmd, sizeof(cmd)));
	if (!ret && (bd->reply_size < bd->var->in_len ||
		     memcmp(bd->reply + BRAGI_REPLY_DTYPE, dtype, 2)))
		ret = -EIO;
	bragi_res_close(bd);
	return ret;
}

/* Write one resource; the data must fit one report. */
static int bragi_res_write(struct bragi_dev *bd, u16 res, const u8 *dtype,
			   const u8 *data, size_t len)
{
	const struct bragi_variant *var = bd->var;
	u8 *cmd = bd->scratch;
	int ret;

	if (var->cmd_off + BRAGI_WRITE_HDR + len > var->out_len)
		return -EINVAL;
	memmove(cmd + BRAGI_WRITE_HDR, data, len);
	cmd[0] = BRAGI_WRITE;
	cmd[1] = BRAGI_HANDLE;
	put_unaligned_le16(len + 2, cmd + 2);
	cmd[4] = 0x00;
	cmd[5] = 0x00;
	cmd[6] = dtype[0];
	cmd[7] = dtype[1];

	ret = bragi_res_open(bd, res);
	if (ret)
		return ret;
	ret = bragi_status_errno(bragi_cmd(bd, cmd, BRAGI_WRITE_HDR + len));
	bragi_res_close(bd);
	return ret;
}

/* Returns the status byte or a negative errno. */
static int bragi_set_mode(struct bragi_dev *bd, u8 mode)
{
	const u8 cmd[] = { BRAGI_SET, BRAGI_PROP_MODE, 0x00, mode };

	return bragi_cmd(bd, cmd, sizeof(cmd));
}

/* Send the duty of every host-driven fan. */
static int bragi_write_duties(struct bragi_dev *bd)
{
	u8 data[1 + 4 * BRAGI_MAX_CH];
	size_t len = 1;
	unsigned int ch;
	int ret;

	spin_lock(&bd->state_lock);
	for (ch = 1; ch <= BRAGI_MAX_CH; ch++) {
		if (!bd->rec[ch].has_pwm || !bd->rec[ch].host)
			continue;
		data[len++] = ch;
		data[len++] = 0x00;
		data[len++] = bd->rec[ch].duty;
		data[len++] = 0x00;
	}
	bd->duty_dirty = false;
	spin_unlock(&bd->state_lock);

	data[0] = (len - 1) / 4;
	if (!data[0])
		return 0;

	ret = bragi_res_write(bd, BRAGI_RES_SET_SPEED, bragi_dt_set_speed, data, len);
	if (ret) {
		spin_lock(&bd->state_lock);
		bd->duty_dirty = true;
		spin_unlock(&bd->state_lock);
	}
	return ret;
}

/*
 * Reopen what programs had open before a mode change closed it. client_lock
 * held. A handle stays followed unless the device refuses it.
 */
static void bragi_client_restore(struct bragi_dev *bd)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(bd->chandle); i++) {
		struct bragi_client_handle *h = &bd->chandle[i];
		const u8 cmd[] = { BRAGI_OPEN, i, h->res[0], h->res[1] };

		if (h->open && bragi_cmd(bd, cmd, sizeof(cmd)) > BRAGI_ST_OK)
			h->open = false;
	}
}

/*
 * Take the device to software mode. With @reload through hardware mode
 * first: that returns every fan to its stored curve and makes the device
 * take stored tables written since the last mode change. Either way the
 * device then has no host duty and no open handle, so both are restored.
 * A reload that did not get through is due again.
 */
static int bragi_enter(struct bragi_dev *bd, bool reload)
{
	bool again = false;
	int ret;

	mutex_lock(&bd->client_lock);
	if (reload && bragi_set_mode(bd, BRAGI_MODE_HARDWARE) < 0)
		again = true;
	ret = bragi_status_errno(bragi_set_mode(bd, BRAGI_MODE_SOFTWARE));
	if (!ret) {
		ret = bragi_write_duties(bd);
		bragi_client_restore(bd);
	} else if (reload) {
		again = true;
	}
	mutex_unlock(&bd->client_lock);

	if (again) {
		spin_lock(&bd->state_lock);
		bd->reload = true;
		spin_unlock(&bd->state_lock);
	}
	return ret;
}

static int bragi_get_firmware(struct bragi_dev *bd)
{
	const u8 cmd[] = { BRAGI_GET, BRAGI_PROP_FIRMWARE };
	int ret = bragi_status_errno(bragi_cmd(bd, cmd, sizeof(cmd)));

	if (ret)
		return ret;
	bd->fw[0] = bd->reply[4];
	bd->fw[1] = bd->reply[5];
	bd->fw_build = get_unaligned_le16(bd->reply + 6);
	return 0;
}

/*
 * buf: last channel, then per channel an 8-byte header (type at 2, model at
 * 3, id length at 7) and the id.
 */
static void bragi_parse_subdevices(struct bragi_dev *bd, const u8 *buf, size_t len)
{
	unsigned int last, ch;
	size_t pos = 1, idlen;

	if (!len)
		return;
	last = buf[0];
	for (ch = 1; ch <= last; ch++) {
		if (pos + 8 > len)
			break;
		idlen = buf[pos + 7];
		if (idlen && pos + 8 + idlen > len)
			break;
		if (idlen && ch <= BRAGI_MAX_CH) {
			bd->rec[ch].present = true;
			bd->rec[ch].type = buf[pos + 2];
			bd->rec[ch].model = buf[pos + 3];
		} else if (idlen) {
			hid_warn(bd->hdev, "LINK channel %u is beyond the %u this driver handles\n",
				 ch, BRAGI_MAX_CH);
		}
		pos += 8 + idlen;
	}
}

/* Probe only: read the subdevice list. */
static int bragi_enumerate(struct bragi_dev *bd)
{
	const u8 rd[] = { BRAGI_READ, BRAGI_HANDLE };
	size_t data_len = bd->var->in_len - BRAGI_REPLY_DATA;
	size_t more_len = bd->var->in_len - BRAGI_REPLY_DTYPE;
	size_t len;
	u8 *buf;
	int ret;

	buf = kzalloc(BRAGI_ENUM_BUF, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = bragi_res_open(bd, BRAGI_RES_SUBDEVICES);
	if (ret)
		goto out;
	ret = bragi_status_errno(bragi_cmd(bd, rd, sizeof(rd)));
	if (!ret && memcmp(bd->reply + BRAGI_REPLY_DTYPE, bragi_dt_subdevices, 2))
		ret = -EIO;
	if (ret) {
		bragi_res_close(bd);
		goto out;
	}
	len = min(data_len, (size_t)BRAGI_ENUM_BUF);
	memcpy(buf, bd->reply + BRAGI_REPLY_DATA, len);

	/*
	 * A second read continues the list; the reference implementations
	 * issue it for more than 14 devices. Its reply is used only when its
	 * status is ok, and parsing stops at the last channel count anyway.
	 */
	if (bragi_cmd(bd, rd, sizeof(rd)) == BRAGI_ST_OK) {
		size_t n = min(more_len, (size_t)BRAGI_ENUM_BUF - len);

		memcpy(buf + len, bd->reply + BRAGI_REPLY_DTYPE, n);
		len += n;
	}
	bragi_res_close(bd);

	bragi_parse_subdevices(bd, buf, len);
out:
	kfree(buf);
	return ret;
}

/* Speeds are required; a device without the temperature resource still gets its fans. */
static int bragi_refresh(struct bragi_dev *bd)
{
	unsigned int n, i;
	const u8 *p;
	int ret;

	ret = bragi_res_read(bd, BRAGI_RES_SPEEDS, bragi_dt_speeds);
	if (ret)
		return ret;
	n = min_t(unsigned int, bd->reply[BRAGI_REPLY_DATA], BRAGI_RECORDS);
	for (i = 0; i < BRAGI_RECORDS; i++) {
		p = bd->reply + BRAGI_REPLY_DATA + 1 + 3 * i;
		bd->rec[i].fan_ok = i < n && p[0] == 0;
		if (bd->rec[i].fan_ok)
			bd->rec[i].rpm = get_unaligned_le16(p + 1);
	}

	ret = bragi_res_read(bd, BRAGI_RES_TEMPS, bragi_dt_temps);
	if (ret == -EAGAIN)
		return ret;
	n = ret ? 0 : min_t(unsigned int, bd->reply[BRAGI_REPLY_DATA], BRAGI_RECORDS);
	for (i = 0; i < BRAGI_RECORDS; i++) {
		p = bd->reply + BRAGI_REPLY_DATA + 1 + 3 * i;
		bd->rec[i].temp_ok = i < n && p[0] == 0;
		if (bd->rec[i].temp_ok)
			bd->rec[i].temp_dc = (s16)get_unaligned_le16(p + 1);
	}

	bd->updated = jiffies;
	bd->valid = true;
	return 0;
}

/* stored curves */

/*
 * Read the stored curve table and note where each channel's record is. A
 * channel has a usable curve when its stored mode is "curve" and the record
 * has 1 to 7 points. On any failure no channel has one.
 */
static void bragi_read_curves(struct bragi_dev *bd)
{
	const struct bragi_variant *var = bd->var;
	size_t avail = var->in_len - BRAGI_REPLY_DATA;
	size_t max = var->out_len - var->cmd_off - BRAGI_WRITE_HDR;
	u16 off[BRAGI_RECORDS] = { };
	u8 modes[BRAGI_RECORDS] = { };
	unsigned int count, i, np;
	const u8 *buf;
	size_t pos = 0;

	if (bragi_res_read(bd, BRAGI_RES_HW_MODES, bragi_dt_hw_modes))
		goto store;
	count = min_t(unsigned int, bd->reply[BRAGI_REPLY_DATA], BRAGI_RECORDS);
	memcpy(modes, bd->reply + BRAGI_REPLY_DATA + 1, count);

	if (bragi_res_read(bd, BRAGI_RES_HW_CURVES, bragi_dt_hw_curves))
		goto store;
	buf = bd->reply + BRAGI_REPLY_DATA;
	count = buf[0];
	pos = 1;
	for (i = 0; i < count; i++) {
		if (pos + 2 > avail)
			goto bad;
		np = buf[pos + 1];
		if (pos + 2 + 4 * np > avail)
			goto bad;
		if (i && i < BRAGI_RECORDS && np >= 1 && np <= BRAGI_MAX_POINTS &&
		    modes[i] == BRAGI_HW_MODE_CURVE)
			off[i] = pos;
		pos += 2 + 4 * np;
	}
	/* one report on the way back */
	if (pos > max)
		goto bad;
	goto store;

bad:
	memset(off, 0, sizeof(off));
	pos = 0;
store:
	spin_lock(&bd->state_lock);
	memcpy(bd->curves, bd->reply + BRAGI_REPLY_DATA, pos);
	bd->curves_len = pos;
	memcpy(bd->curve_off, off, sizeof(off));
	bd->curves_dirty = false;
	spin_unlock(&bd->state_lock);
}

/* Write the edited table to the device and check it. The device applies it at a mode change. */
static void bragi_commit_curves(struct bragi_dev *bd)
{
	u8 *copy = bd->scratch + bd->var->out_len;
	size_t len;
	int ret;

	spin_lock(&bd->state_lock);
	len = bd->curves_len;
	memcpy(copy, bd->curves, len);
	bd->curves_dirty = false;
	spin_unlock(&bd->state_lock);
	if (!len)
		return;

	ret = bragi_res_write(bd, BRAGI_RES_HW_CURVES, bragi_dt_hw_curves, copy, len);
	if (!ret)
		ret = bragi_res_read(bd, BRAGI_RES_HW_CURVES, bragi_dt_hw_curves);
	if (!ret && memcmp(bd->reply + BRAGI_REPLY_DATA, copy, len))
		ret = -EIO;
	if (ret) {
		hid_err(bd->hdev, "stored curve write failed: %d\n", ret);
		bragi_read_curves(bd);
		return;
	}

	spin_lock(&bd->state_lock);
	bd->reload = true;
	spin_unlock(&bd->state_lock);
}

static bool bragi_fresh(const struct bragi_dev *bd)
{
	return READ_ONCE(bd->valid) &&
	       !time_after(jiffies, READ_ONCE(bd->updated) + msecs_to_jiffies(BRAGI_STALE_MS));
}

/*
 * What the stored curve gives for a channel now, in percent: linear between
 * points, flat outside them (as measured). state_lock held. The sensor byte
 * is the temperature record the curve follows; 0 is not a record.
 */
static int bragi_curve_pct(const struct bragi_dev *bd, unsigned int ch)
{
	unsigned int off = bd->curve_off[ch], np, i;
	const u8 *p;
	int t, t0, t1, p0, p1;
	u8 sensor;

	if (!off || !bragi_fresh(bd))
		return -ENODATA;
	sensor = bd->curves[off];
	np = bd->curves[off + 1];
	if (!sensor || sensor > BRAGI_MAX_CH || !bd->rec[sensor].temp_ok)
		return -ENODATA;
	t = bd->rec[sensor].temp_dc;
	p = bd->curves + off + 2;

	for (i = 0; i < np; i++) {
		t1 = get_unaligned_le16(p + 4 * i);
		p1 = min_t(int, get_unaligned_le16(p + 4 * i + 2), 100);
		if (t > t1)
			continue;
		if (!i)
			return p1;
		t0 = get_unaligned_le16(p + 4 * (i - 1));
		p0 = min_t(int, get_unaligned_le16(p + 4 * (i - 1) + 2), 100);
		return p0 + DIV_ROUND_CLOSEST((p1 - p0) * (t - t0), t1 - t0);
	}
	return min_t(int, get_unaligned_le16(p + 4 * (np - 1) + 2), 100);
}

/* A pump is not run below half duty. */
static unsigned int bragi_floor(const struct bragi_record *r, unsigned int pct)
{
	return r->pump ? max(pct, 50U) : pct;
}

static void bragi_set_host_duty(struct bragi_dev *bd, unsigned int ch, unsigned int pct)
{
	struct bragi_record *r = &bd->rec[ch];

	pct = bragi_floor(r, min(pct, 100U));

	spin_lock(&bd->state_lock);
	r->duty = pct;
	r->host = true;
	bd->duty_dirty = true;
	spin_unlock(&bd->state_lock);
}

static bool bragi_take(struct bragi_dev *bd, bool *flag)
{
	bool set;

	spin_lock(&bd->state_lock);
	set = *flag;
	*flag = false;
	spin_unlock(&bd->state_lock);
	return set;
}

static void bragi_poll_work(struct work_struct *work)
{
	struct bragi_dev *bd = container_of(to_delayed_work(work), struct bragi_dev, poll);
	int ret;

	mutex_lock(&bd->seq_lock);
	if (bragi_take(bd, &bd->curves_stale))
		bragi_read_curves(bd);
	if (READ_ONCE(bd->curves_dirty))
		bragi_commit_curves(bd);
	if (bragi_take(bd, &bd->reload))
		bragi_enter(bd, true);

	ret = bragi_refresh(bd);
	if (ret == -EAGAIN) {
		dev_warn_ratelimited(&bd->hdev->dev, "hub left software mode, re-entering\n");
		if (!bragi_enter(bd, false))
			bragi_refresh(bd);
	}
	if (READ_ONCE(bd->duty_dirty))
		bragi_write_duties(bd);
	mutex_unlock(&bd->seq_lock);

	queue_delayed_work(system_percpu_wq, &bd->poll, msecs_to_jiffies(BRAGI_POLL_MS));
}

/* the client device: what programs see */

/* Answer a command the driver did not send on. client_lock held. */
static void bragi_client_reply(struct bragi_dev *bd, u8 echo, u8 status)
{
	const struct bragi_variant *var = bd->var;

	memset(bd->synth, 0, var->in_len);
	bd->synth[var->echo_off] = echo;
	bd->synth[var->status_off] = status;
	hid_input_report(bd->client, HID_INPUT_REPORT, bd->synth, var->in_len, 0);
}

/* Apply a set-speed write (count, then channel, 00, percent, 00 per fan) as host duties. */
static void bragi_client_apply_duties(struct bragi_dev *bd, const u8 *cmd, size_t len)
{
	unsigned int count, i, ch;
	const u8 *p;

	if (len < BRAGI_WRITE_HDR + 1 || memcmp(cmd + 6, bragi_dt_set_speed, 2))
		return;
	count = cmd[BRAGI_WRITE_HDR];
	for (i = 0; i < count; i++) {
		if (BRAGI_WRITE_HDR + 1 + 4 * (i + 1) > len)
			break;
		p = cmd + BRAGI_WRITE_HDR + 1 + 4 * i;
		ch = p[0];
		if (ch >= 1 && ch <= BRAGI_MAX_CH && bd->rec[ch].has_pwm)
			bragi_set_host_duty(bd, ch, p[2]);
	}
	mod_delayed_work(system_percpu_wq, &bd->poll, 0);
}

/* A program writes to a fan control resource: the status to answer, or -1 to send it on. */
static int bragi_client_fan_write(struct bragi_dev *bd, u16 res, const u8 *cmd, size_t len)
{
	switch (READ_ONCE(foreign_fan_control)) {
	case BRAGI_FOREIGN_IGNORE:
		return BRAGI_ST_OK;
	case BRAGI_FOREIGN_APPLY:
		if (res != BRAGI_RES_SET_SPEED) {
			spin_lock(&bd->state_lock);
			bd->curves_stale = true;
			bd->reload = true;
			spin_unlock(&bd->state_lock);
			mod_delayed_work(system_percpu_wq, &bd->poll,
					 msecs_to_jiffies(BRAGI_COMMIT_MS));
			return -1;
		}
		if (cmd[0] == BRAGI_WRITE)
			bragi_client_apply_duties(bd, cmd, len);
		return BRAGI_ST_OK;
	default:
		hid_info_once(bd->hdev,
			      "refused a fan control write from a program (foreign_fan_control=0)\n");
		return BRAGI_ST_UNSUPPORTED;
	}
}

static bool bragi_res_is_fan_control(u16 res)
{
	return res == BRAGI_RES_SET_SPEED || res == BRAGI_RES_HW_MODES ||
	       res == BRAGI_RES_HW_FIXED || res == BRAGI_RES_HW_CURVES;
}

/* The status to answer a program's command with, or -1 to send it on. client_lock held. */
static int bragi_client_filter(struct bragi_dev *bd, const u8 *cmd, size_t len)
{
	const struct bragi_client_handle *h;
	u8 c[4] = { };
	size_t i;

	memcpy(c, cmd, min(len, sizeof(c)));
	switch (c[0]) {
	case BRAGI_SET:
		/* The driver holds software mode; a mode change would reset every fan. */
		return get_unaligned_le16(c + 1) == BRAGI_PROP_MODE ? BRAGI_ST_OK : -1;
	case BRAGI_OPEN:
		return c[1] == BRAGI_HANDLE ? BRAGI_ST_UNSUPPORTED : -1;
	case BRAGI_READ:
		return c[1] == BRAGI_HANDLE ? BRAGI_ST_REFUSED : -1;
	case BRAGI_CLOSE:
		/* 05 <count> <handle>...: of a list with the driver's handle nothing is closed */
		for (i = 0; i < c[1] && 2 + i < len; i++)
			if (cmd[2 + i] == BRAGI_HANDLE)
				return BRAGI_ST_REFUSED;
		return -1;
	case BRAGI_WRITE:
	case BRAGI_CONTINUE:
		if (c[1] == BRAGI_HANDLE)
			return BRAGI_ST_REFUSED;
		h = &bd->chandle[c[1]];
		if (!h->open || !bragi_res_is_fan_control(get_unaligned_le16(h->res)))
			return -1;
		return bragi_client_fan_write(bd, get_unaligned_le16(h->res), cmd, len);
	default:
		return -1;
	}
}

/*
 * Follow which handles programs have open: for the fan control rule, and to
 * reopen them after a mode change. Without a status (@status < 0: the reply
 * was lost) a handle counts as open: an open adds it, a close keeps it.
 */
static void bragi_client_track(struct bragi_dev *bd, const u8 *cmd, size_t len, int status)
{
	struct bragi_client_handle *h;
	u8 c[4] = { };
	size_t i;

	memcpy(c, cmd, min(len, sizeof(c)));
	switch (c[0]) {
	case BRAGI_OPEN:
		h = &bd->chandle[c[1]];
		/* A refused open of an open handle leaves it open, on its resource. */
		if (status > BRAGI_ST_OK || (status < 0 && h->open))
			return;
		h->open = true;
		h->res[0] = c[2];
		h->res[1] = c[3];
		return;
	case BRAGI_CLOSE:
		if (status != BRAGI_ST_OK)
			return;
		for (i = 0; i < c[1] && 2 + i < len; i++)
			bd->chandle[cmd[2 + i]].open = false;
		return;
	}
}

/*
 * A report from a program: 00 <address> <flags> <command>. It returns when
 * the device has answered (about 2 ms), with the reply then in the hidraw
 * queue, or fails when no reply came.
 *
 * The rules apply to every command for the device itself, whatever its
 * flags; a command for a device behind it passes. Each command goes out with
 * the reply flag: the driver then sees its outcome, and the next exchange
 * cannot take its reply. A program that asked for no reply gets none.
 */
static int bragi_client_report(struct bragi_dev *bd, const u8 *buf, size_t len)
{
	const struct bragi_variant *var = bd->var;
	const u8 *cmd = buf + var->cmd_off;
	bool has_cmd = len > var->cmd_off;
	bool own = has_cmd && buf[1] == var->addr;
	bool reply = !has_cmd || (buf[2] & BRAGI_FLAG_REPLY);
	size_t clen = has_cmd ? len - var->cmd_off : 0;
	int st = -1, ret = len;

	if (!len || len > var->out_len)
		return -EINVAL;

	mutex_lock(&bd->client_lock);
	if (own)
		st = bragi_client_filter(bd, cmd, clen);
	if (st >= 0) {
		if (reply)
			bragi_client_reply(bd, cmd[0], st);
	} else {
		mutex_lock(&bd->io_lock);
		memcpy(bd->out, buf, len);
		if (has_cmd)
			bd->out[2] |= BRAGI_FLAG_REPLY;
		st = bragi_io(bd, len, has_cmd, has_cmd ? cmd[0] : 0);
		if (st >= 0 && reply)
			hid_input_report(bd->client, HID_INPUT_REPORT, bd->in, bd->in_size, 0);
		mutex_unlock(&bd->io_lock);

		if (own)
			bragi_client_track(bd, cmd, clen, st);
		if (st < 0)
			ret = st;
	}
	mutex_unlock(&bd->client_lock);

	return ret;
}

static int bragi_client_ll_parse(struct hid_device *hdev)
{
	struct bragi_dev *bd = hdev->driver_data;

	return hid_parse_report(hdev, bd->hdev->dev_rdesc, bd->hdev->dev_rsize);
}

static int bragi_client_ll_start(struct hid_device *hdev)
{
	return 0;
}

static void bragi_client_ll_stop(struct hid_device *hdev)
{
}

static int bragi_client_ll_open(struct hid_device *hdev)
{
	return 0;
}

static void bragi_client_ll_close(struct hid_device *hdev)
{
}

static int bragi_client_ll_raw_request(struct hid_device *hdev, unsigned char reportnum,
				       u8 *buf, size_t len, unsigned char rtype, int reqtype)
{
	struct bragi_dev *bd = hdev->driver_data;

	if (rtype == HID_OUTPUT_REPORT && reqtype == HID_REQ_SET_REPORT)
		return bragi_client_report(bd, buf, len);
	return hid_hw_raw_request(bd->hdev, reportnum, buf, len, rtype, reqtype);
}

static int bragi_client_ll_output_report(struct hid_device *hdev, u8 *buf, size_t len)
{
	return bragi_client_report(hdev->driver_data, buf, len);
}

static const struct hid_ll_driver bragi_client_ll = {
	.parse = bragi_client_ll_parse,
	.start = bragi_client_ll_start,
	.stop = bragi_client_ll_stop,
	.open = bragi_client_ll_open,
	.close = bragi_client_ll_close,
	.raw_request = bragi_client_ll_raw_request,
	.output_report = bragi_client_ll_output_report,
};

static int bragi_client_create(struct bragi_dev *bd)
{
	struct hid_device *hdev = bd->hdev, *client;
	int ret;

	client = hid_allocate_device();
	if (IS_ERR(client))
		return PTR_ERR(client);

	client->ll_driver = &bragi_client_ll;
	client->driver_data = bd;
	client->dev.parent = hdev->dev.parent;
	client->bus = hdev->bus;
	client->vendor = hdev->vendor;
	client->product = hdev->product;
	client->version = hdev->version;
	client->type = hdev->type;
	client->country = hdev->country;
	strscpy(client->name, hdev->name, sizeof(client->name));
	strscpy(client->phys, hdev->phys, sizeof(client->phys));
	strscpy(client->uniq, hdev->uniq, sizeof(client->uniq));

	bd->client = client;
	ret = hid_add_device(client);
	if (ret) {
		hid_destroy_device(client);
		bd->client = NULL;
	}
	return ret;
}

/* hwmon */

static umode_t bragi_is_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	const struct bragi_dev *bd = data;

	switch (type) {
	case hwmon_fan:
		return bd->rec[channel + 1].has_fan ? 0444 : 0;
	case hwmon_pwm:
		if (!bd->rec[channel + 1].has_pwm)
			return 0;
		if (attr == hwmon_pwm_auto_channels_temp && !bd->curve_off[channel + 1])
			return 0;
		return 0644;
	case hwmon_temp:
		/* temp1..temp25 = channels 1..25, temp26 = record 0 (hub) */
		return bd->rec[(channel + 1) % BRAGI_RECORDS].has_temp ? 0444 : 0;
	default:
		return 0;
	}
}

static int bragi_read_pwm(struct bragi_dev *bd, u32 attr, unsigned int ch, long *val)
{
	const struct bragi_record *r = &bd->rec[ch];
	unsigned int off;
	int pct = 0;
	u8 sensor = 0;
	bool host;

	spin_lock(&bd->state_lock);
	host = r->host;
	off = bd->curve_off[ch];
	if (attr == hwmon_pwm_input)
		pct = host ? r->duty : bragi_curve_pct(bd, ch);
	else if (off)
		sensor = bd->curves[off];
	spin_unlock(&bd->state_lock);

	switch (attr) {
	case hwmon_pwm_input:
		/* On the stored curve the device reports no duty: this is the curve's value. */
		if (pct < 0)
			return pct;
		*val = DIV_ROUND_CLOSEST(pct * 255, 100);
		return 0;
	case hwmon_pwm_enable:
		*val = host ? 1 : 2;
		return 0;
	case hwmon_pwm_auto_channels_temp:
		*val = sensor >= 1 && sensor <= BRAGI_MAX_CH ? BIT(sensor - 1) : 0;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int bragi_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct bragi_dev *bd = dev_get_drvdata(dev);
	struct bragi_record *r;

	switch (type) {
	case hwmon_fan:
		r = &bd->rec[channel + 1];
		if (attr != hwmon_fan_input)
			return -EOPNOTSUPP;
		if (!bragi_fresh(bd) || !READ_ONCE(r->fan_ok))
			return -ENODATA;
		*val = READ_ONCE(r->rpm);
		return 0;
	case hwmon_pwm:
		return bragi_read_pwm(bd, attr, channel + 1, val);
	case hwmon_temp:
		r = &bd->rec[(channel + 1) % BRAGI_RECORDS];
		if (attr != hwmon_temp_input)
			return -EOPNOTSUPP;
		if (!bragi_fresh(bd) || !READ_ONCE(r->temp_ok))
			return -ENODATA;
		*val = READ_ONCE(r->temp_dc) * 100;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int bragi_read_string(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, const char **str)
{
	struct bragi_dev *bd = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_fan:
		*str = bd->rec[channel + 1].label;
		return 0;
	case hwmon_temp:
		*str = bd->rec[(channel + 1) % BRAGI_RECORDS].label;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int bragi_write(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long val)
{
	struct bragi_dev *bd = dev_get_drvdata(dev);
	unsigned int ch = channel + 1, off, sensor;
	struct bragi_record *r = &bd->rec[ch];
	unsigned long delay = 0;
	int pct;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_pwm_input:
		/* A duty takes the fan from its stored curve, as pwmN_enable = 1 does. */
		if (val < 0 || val > 255)
			return -EINVAL;
		bragi_set_host_duty(bd, ch, DIV_ROUND_CLOSEST(val * 100, 255));
		break;
	case hwmon_pwm_enable:
		if (val != 1 && val != 2)
			return -EINVAL;
		spin_lock(&bd->state_lock);
		if (val == 1 && !r->host) {
			/* start from where the curve has the fan */
			pct = bragi_curve_pct(bd, ch);
			if (pct >= 0)
				r->duty = bragi_floor(r, pct);
			r->host = true;
			bd->duty_dirty = true;
		} else if (val == 2 && r->host) {
			r->host = false;
			bd->reload = true;
		}
		spin_unlock(&bd->state_lock);
		break;
	case hwmon_pwm_auto_channels_temp:
		/* one temperature: bit N-1 is tempN, the sensor of channel N */
		if (val <= 0 || val >= BIT(BRAGI_MAX_CH) || !is_power_of_2(val))
			return -EINVAL;
		sensor = __ffs(val) + 1;
		if (!bd->rec[sensor].has_temp)
			return -EINVAL;
		spin_lock(&bd->state_lock);
		off = bd->curve_off[ch];
		if (off) {
			bd->curves[off] = sensor;
			bd->curves_dirty = true;
		}
		spin_unlock(&bd->state_lock);
		if (!off)
			return -ENODATA;
		delay = msecs_to_jiffies(BRAGI_COMMIT_MS);
		break;
	default:
		return -EOPNOTSUPP;
	}

	mod_delayed_work(system_percpu_wq, &bd->poll, delay);
	return 0;
}

static const struct hwmon_ops bragi_hwmon_ops = {
	.is_visible = bragi_is_visible,
	.read = bragi_read,
	.read_string = bragi_read_string,
	.write = bragi_write,
};

static const u32 bragi_fan_config[] = {
	[0 ... BRAGI_MAX_CH - 1] = HWMON_F_INPUT | HWMON_F_LABEL,
	0
};

static const u32 bragi_pwm_config[] = {
	[0 ... BRAGI_MAX_CH - 1] = HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP,
	0
};

static const u32 bragi_temp_config[] = {
	[0 ... BRAGI_RECORDS - 1] = HWMON_T_INPUT | HWMON_T_LABEL,
	0
};

static const struct hwmon_channel_info bragi_fan_info = {
	.type = hwmon_fan,
	.config = bragi_fan_config,
};

static const struct hwmon_channel_info bragi_pwm_info = {
	.type = hwmon_pwm,
	.config = bragi_pwm_config,
};

static const struct hwmon_channel_info bragi_temp_info = {
	.type = hwmon_temp,
	.config = bragi_temp_config,
};

static const struct hwmon_channel_info * const bragi_info[] = {
	&bragi_fan_info,
	&bragi_pwm_info,
	&bragi_temp_info,
	NULL
};

static const struct hwmon_chip_info bragi_chip_info = {
	.ops = &bragi_hwmon_ops,
	.info = bragi_info,
};

/* pwmN_auto_pointM_temp and pwmN_auto_pointM_pwm: the stored curve */

static ssize_t bragi_point_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const struct bragi_point_attr *pa = container_of(attr, struct bragi_point_attr, dattr);
	struct bragi_dev *bd = dev_get_drvdata(dev);
	unsigned int off, v = 0;

	spin_lock(&bd->state_lock);
	off = bd->curve_off[pa->ch];
	if (off && pa->point < bd->curves[off + 1])
		v = get_unaligned_le16(bd->curves + off + 2 + 4 * pa->point + (pa->temp ? 0 : 2));
	else
		off = 0;
	spin_unlock(&bd->state_lock);

	if (!off)
		return -ENODATA;
	if (pa->temp)
		return sysfs_emit(buf, "%u\n", v * 100);
	return sysfs_emit(buf, "%u\n", DIV_ROUND_CLOSEST(min(v, 100U) * 255, 100));
}

/* Temperatures stay in order: a point is kept between its neighbours. */
static ssize_t bragi_point_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	const struct bragi_point_attr *pa = container_of(attr, struct bragi_point_attr, dattr);
	struct bragi_dev *bd = dev_get_drvdata(dev);
	unsigned int off, np, v;
	int ret = 0;
	long val;
	u8 *p;

	if (kstrtol(buf, 10, &val))
		return -EINVAL;
	if (val < 0 || val > (pa->temp ? 150000 : 255))
		return -EINVAL;
	if (pa->temp)
		v = DIV_ROUND_CLOSEST(val, 100);
	else
		v = bragi_floor(&bd->rec[pa->ch], DIV_ROUND_CLOSEST(val * 100, 255));

	spin_lock(&bd->state_lock);
	off = bd->curve_off[pa->ch];
	np = off ? bd->curves[off + 1] : 0;
	p = bd->curves + off + 2 + 4 * pa->point;
	if (pa->point >= np) {
		ret = -ENODATA;
	} else if (!pa->temp) {
		put_unaligned_le16(v, p + 2);
	} else if ((pa->point && v < get_unaligned_le16(p - 4)) ||
		   (pa->point + 1 < np && v > get_unaligned_le16(p + 4))) {
		ret = -EINVAL;
	} else {
		put_unaligned_le16(v, p);
	}
	if (!ret)
		bd->curves_dirty = true;
	spin_unlock(&bd->state_lock);
	if (ret)
		return ret;

	mod_delayed_work(system_percpu_wq, &bd->poll, msecs_to_jiffies(BRAGI_COMMIT_MS));
	return count;
}

static int bragi_point_attrs(struct bragi_dev *bd)
{
	struct device *dev = &bd->hdev->dev;
	struct bragi_point_attr *pa;
	struct attribute **attrs;
	unsigned int ch, i, t, total = 0, n = 0;

	for (ch = 1; ch <= BRAGI_MAX_CH; ch++)
		if (bd->rec[ch].has_pwm && bd->curve_off[ch])
			total += 2 * bd->curves[bd->curve_off[ch] + 1];
	if (!total)
		return 0;

	pa = devm_kcalloc(dev, total, sizeof(*pa), GFP_KERNEL);
	attrs = devm_kcalloc(dev, total + 1, sizeof(*attrs), GFP_KERNEL);
	if (!pa || !attrs)
		return -ENOMEM;

	for (ch = 1; ch <= BRAGI_MAX_CH; ch++) {
		if (!bd->rec[ch].has_pwm || !bd->curve_off[ch])
			continue;
		for (i = 0; i < bd->curves[bd->curve_off[ch] + 1]; i++) {
			for (t = 0; t < 2; t++, pa++) {
				pa->ch = ch;
				pa->point = i;
				pa->temp = t;
				snprintf(pa->name, sizeof(pa->name), "pwm%u_auto_point%u_%s",
					 ch, i + 1, t ? "temp" : "pwm");
				sysfs_attr_init(&pa->dattr.attr);
				pa->dattr.attr.name = pa->name;
				pa->dattr.attr.mode = 0644;
				pa->dattr.show = bragi_point_show;
				pa->dattr.store = bragi_point_store;
				attrs[n++] = &pa->dattr.attr;
			}
		}
	}

	bd->point_group.attrs = attrs;
	bd->groups[0] = &bd->point_group;
	return 0;
}

/* probe and remove */

static void bragi_init_channels(struct bragi_dev *bd)
{
	unsigned int i;

	for (i = 0; i < BRAGI_RECORDS; i++) {
		struct bragi_record *r = &bd->rec[i];
		const struct bragi_type *t = NULL;

		if (i && r->present)
			t = bragi_type(r->type, r->model);
		/* A known fan or pump has its files also while its speed is not valid. */
		r->has_pwm = t && t->duty;
		r->pump = t && t->pump;
		r->has_fan = r->has_pwm || (i && r->present && r->fan_ok);
		r->has_temp = r->temp_ok;
		/* where pwmN_enable = 1 starts when the curve gives no value */
		r->duty = r->pump ? 100 : 50;
		if (i == 0)
			strscpy(r->label, "hub", sizeof(r->label));
		else if (t)
			snprintf(r->label, sizeof(r->label), "ch%u %s", i, t->name);
		else
			snprintf(r->label, sizeof(r->label), "ch%u type 0x%02x", i, r->type);
	}
}

static unsigned int bragi_present_count(const struct bragi_dev *bd)
{
	unsigned int ch, n = 0;

	for (ch = 1; ch <= BRAGI_MAX_CH; ch++)
		n += bd->rec[ch].present;
	return n;
}

static int bragi_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	const struct bragi_variant *var = (const struct bragi_variant *)id->driver_data;
	struct device *dev = &hdev->dev;
	struct bragi_dev *bd;
	int ret;

	hid_set_drvdata(hdev, NULL);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	/*
	 * The client device this driver created, and the interface without
	 * an output report: a hidraw node, nothing else.
	 */
	if (hdev->ll_driver == &bragi_client_ll ||
	    list_empty(&hdev->report_enum[HID_OUTPUT_REPORT].report_list))
		return hid_hw_start(hdev, HID_CONNECT_HIDRAW);

	bd = devm_kzalloc(dev, sizeof(*bd), GFP_KERNEL);
	if (!bd)
		return -ENOMEM;
	bd->out = devm_kmalloc(dev, var->out_len, GFP_KERNEL);
	bd->in = devm_kmalloc(dev, var->in_len, GFP_KERNEL);
	bd->reply = devm_kzalloc(dev, var->in_len, GFP_KERNEL);
	bd->synth = devm_kmalloc(dev, var->in_len, GFP_KERNEL);
	bd->curves = devm_kzalloc(dev, var->in_len, GFP_KERNEL);
	/* a command being built, then a copy of the curve table */
	bd->scratch = devm_kmalloc(dev, var->out_len + var->in_len, GFP_KERNEL);
	if (!bd->out || !bd->in || !bd->reply || !bd->synth || !bd->curves || !bd->scratch)
		return -ENOMEM;

	bd->hdev = hdev;
	bd->var = var;
	mutex_init(&bd->seq_lock);
	mutex_init(&bd->client_lock);
	mutex_init(&bd->io_lock);
	spin_lock_init(&bd->reply_lock);
	spin_lock_init(&bd->state_lock);
	init_completion(&bd->reply_done);
	INIT_DELAYED_WORK(&bd->poll, bragi_poll_work);
	hid_set_drvdata(hdev, bd);

	/* No hidraw node on the device itself: programs get the client's. */
	ret = hid_hw_start(hdev, 0);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret)
		goto out_hw_stop;

	hid_device_io_start(hdev);

	/*
	 * Through hardware mode, so that no duty and no handle of an earlier
	 * owner survives: every fan starts on its stored curve.
	 */
	mutex_lock(&bd->seq_lock);
	ret = bragi_get_firmware(bd);
	if (!ret)
		ret = bragi_enter(bd, true);
	if (!ret)
		ret = bragi_enumerate(bd);
	if (!ret)
		ret = bragi_refresh(bd);
	if (!ret) {
		bragi_init_channels(bd);
		bragi_read_curves(bd);
	}
	mutex_unlock(&bd->seq_lock);
	if (ret) {
		hid_err(hdev, "initialisation failed: %d\n", ret);
		goto out_hw_close;
	}

	ret = bragi_point_attrs(bd);
	if (ret)
		goto out_hw_close;

	bd->hwmon_dev = hwmon_device_register_with_info(dev, var->hwmon_name, bd, &bragi_chip_info,
							bd->groups[0] ? bd->groups : NULL);
	if (IS_ERR(bd->hwmon_dev)) {
		ret = PTR_ERR(bd->hwmon_dev);
		goto out_hw_close;
	}

	ret = bragi_client_create(bd);
	if (ret)
		goto out_hwmon;

	hid_info(hdev, "firmware %u.%u.%u, %u LINK devices\n",
		 bd->fw[0], bd->fw[1], bd->fw_build, bragi_present_count(bd));

	queue_delayed_work(system_percpu_wq, &bd->poll, msecs_to_jiffies(BRAGI_POLL_MS));
	return 0;

out_hwmon:
	hwmon_device_unregister(bd->hwmon_dev);
	cancel_delayed_work_sync(&bd->poll);
out_hw_close:
	hid_hw_close(hdev);
	hid_device_io_stop(hdev);
out_hw_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void bragi_remove(struct hid_device *hdev)
{
	struct bragi_dev *bd = hid_get_drvdata(hdev);

	if (!bd) {
		hid_hw_stop(hdev);
		return;
	}

	/* hid-core holds input back during remove; the exchanges below need their replies */
	hid_device_io_start(hdev);

	/* first what can still start work: hwmon writes, then programs */
	hwmon_device_unregister(bd->hwmon_dev);
	hid_destroy_device(bd->client);
	cancel_delayed_work_sync(&bd->poll);

	/* Hand every fan back to the hub's stored tables. */
	mutex_lock(&bd->seq_lock);
	bragi_set_mode(bd, BRAGI_MODE_HARDWARE);
	mutex_unlock(&bd->seq_lock);

	hid_device_io_stop(hdev);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

#ifdef CONFIG_PM
static int bragi_suspend(struct hid_device *hdev, pm_message_t message)
{
	struct bragi_dev *bd = hid_get_drvdata(hdev);

	if (bd)
		cancel_delayed_work_sync(&bd->poll);
	return 0;
}

/* The hub comes back in hardware mode; the poll's -EAGAIN path restores it. */
static int bragi_resume(struct hid_device *hdev)
{
	struct bragi_dev *bd = hid_get_drvdata(hdev);

	if (bd)
		queue_delayed_work(system_percpu_wq, &bd->poll, 0);
	return 0;
}
#endif

/*
 * One row per device variant. Siblings that use the same opcodes with other
 * framing and are not enabled here: Commander Core / Core XT / ST
 * (0c1c / 0c2a / 0c32: out[1] = 0x08, 64/96/384/1024-byte reports) and
 * Commander Duo (0c56). See docs/hid-protocol.md.
 */
static const struct hid_device_id bragi_table[] = {
	{ HID_USB_DEVICE(0x1b1c, 0x0c3f), .driver_data = (kernel_ulong_t)&bragi_link_hub },
	{ }
};
MODULE_DEVICE_TABLE(hid, bragi_table);

static struct hid_driver bragi_driver = {
	.name = "corsair-bragi",
	.id_table = bragi_table,
	.probe = bragi_probe,
	.remove = bragi_remove,
	.raw_event = bragi_raw_event,
#ifdef CONFIG_PM
	.suspend = bragi_suspend,
	.resume = bragi_resume,
	.reset_resume = bragi_resume,
#endif
};

static int __init bragi_init(void)
{
	return hid_register_driver(&bragi_driver);
}

static void __exit bragi_exit(void)
{
	hid_unregister_driver(&bragi_driver);
}

/* hwmon initcalls run before HID drivers when built in; see corsair-cpro. */
late_initcall(bragi_init);
module_exit(bragi_exit);

MODULE_DESCRIPTION("Corsair Bragi cooling devices (iCUE LINK System Hub) hwmon driver");
MODULE_LICENSE("GPL");
