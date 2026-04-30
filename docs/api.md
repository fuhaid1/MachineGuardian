# API Reference

The ESP32 serves a REST API on port 80. All responses are JSON unless noted.

## GET /api/status

Live system state. Called every 2 seconds by the dashboard.

**Response:**
```json
{
  "state": 0,
  "machine_on": false,
  "employee": "",
  "raw_amps": 4.218,
  "ambient_amps": 4.102,
  "net_amps": 0.000,
  "midpoint": 1834.6,
  "threshold": 1.0,
  "session_runtime_sec": 0
}
```

| Field | Type | Description |
|---|---|---|
| `state` | int | 0=idle, 1=authenticated, 2=running, 3=unauthorized |
| `machine_on` | bool | Whether net amps ≥ threshold |
| `employee` | string | Name of authenticated employee, or "" |
| `raw_amps` | float | RMS amps before ambient subtraction |
| `ambient_amps` | float | Saved ambient baseline |
| `net_amps` | float | EMA-filtered net amps (raw − ambient) |
| `midpoint` | float | Current ADC DC midpoint (debug) |
| `threshold` | float | Current detection threshold |
| `session_runtime_sec` | int | Seconds current session has been running |

---

## GET /api/power

Power log for the current 24h window. Only samples captured during active (authorised) sessions are stored.

**Response:**
```json
[
  { "ts": 1748600000, "amps": 3.82, "watts": 840.0 },
  { "ts": 1748600005, "amps": 3.91, "watts": 860.2 }
]
```

Maximum 288 entries (circular buffer). Sampled every 5 seconds during running state.

---

## GET /api/runtime

Session runtime log grouped by date and employee.

**Response:**
```json
[
  { "date": "2025-05-28", "employee": "Ahmed Al-Rashid", "seconds": 14400, "unauthorized": false },
  { "date": "2025-05-28", "employee": "UNAUTHORIZED",    "seconds": 1200,  "unauthorized": true  }
]
```

Up to 100 entries. Combined per employee per day (multiple sessions on the same day are summed).

---

## GET /api/notifications

Event log. Most recent 50 events.

**Response:**
```json
[
  { "timestamp": 1748600100, "message": "Ahmed Al-Rashid authenticated", "type": "info" },
  { "timestamp": 1748600400, "message": "Unauthorized machine start detected!", "type": "alarm" }
]
```

| `type` | Meaning |
|---|---|
| `info` | Normal events (login, machine start/stop) |
| `warn` | Non-critical alerts (unknown card, auth timeout) |
| `alarm` | Unauthorized machine start |

---

## GET /api/employees

Returns employee list. UIDs are intentionally excluded from this response.

**Response:**
```json
[
  { "name": "Ahmed Al-Rashid", "active": true },
  { "name": "Sara Mohammed",   "active": false }
]
```

---

## POST /api/employees/add

Add a new employee and persist to NVS.

**Body:**
```json
{ "name": "Khalid Hassan", "uid": "A1B2C3D4" }
```

**Response:** `{"ok":true}` or HTTP 400/507 on error.

UID must be uppercase hex string (no spaces, no dashes). Maximum 20 employees.

---

## POST /api/employees/remove

Remove employee at index N (0-based, from `/api/employees` order).

**Body:**
```json
{ "index": 2 }
```

**Response:** `{"ok":true}`

This permanently deletes the employee from NVS.

---

## POST /api/employees/toggle

Toggle active/inactive state for employee at index N.

**Body:**
```json
{ "index": 0 }
```

**Response:** `{"ok":true}`

Deactivated employees' cards are rejected at the reader but their records are kept.

---

## POST /api/employees/clear

Delete all employees from NVS. Cannot be undone.

**Body:** (empty)

**Response:** `{"ok":true}`

---

## GET /api/set_ambient

Captures a new ambient baseline. Takes ~3 seconds.

**Important:** call this only with the monitored machine completely off.

**Response:**
```json
{
  "raw_avg": 3.729,
  "ambient_amps": 4.102,
  "threshold": 1.0
}
```

| Field | Description |
|---|---|
| `raw_avg` | Average of 30 raw RMS samples |
| `ambient_amps` | Saved value (raw_avg × 1.1 — includes 10% safety margin) |
| `threshold` | Current detection threshold (unchanged) |

---

## POST /api/set_threshold

Update the detection threshold. Persisted to NVS.

**Body:**
```json
{ "threshold": 1.5 }
```

Valid range: 0.1 – 50.0 amps.

**Response:** `{"ok":true}` or HTTP 400 if out of range.
